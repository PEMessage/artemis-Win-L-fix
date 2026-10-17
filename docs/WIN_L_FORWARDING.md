# 把 `Win+L` 转发到远端主机

> 一次把 Windows 键盘栈从「窗口消息」一路挖到「低级键盘钩子」的工程记录。
> 面向**没写过 Windows 输入代码**的程序员，第一视角，包含所有走过的弯路。
>
> 起因只是一句需求：*「我想按 `Win+L`，锁的是远端那台机器，不是我本机。」*
> 结论是：**可以做到，但几乎每一层都有一个反直觉的坑。**

---

## 序：一个看起来 5 分钟的问题

项目是 Artemis（Moonlight/Apollo 的 Windows 客户端）。串流时，客户端把键盘事件转发给主机。
设置里有一个选项叫 **Capture system keyboard shortcuts**（捕获系统快捷键）——打开之后，
`Alt+Tab`、`Win` 键这类「系统级」组合会被吃掉、转发给远端，而不是在本机生效。

某天有人报告：

> 「`Win+L` 转发不过去。**理论上**只用在注册表里把本地的 `Win+L` 关掉就行了，但**实际在这个项目里会出问题**：
> 不但映射不过去，而且所有热键都不再传到 client 上了。看看什么问题。」

`Win+L` = 锁定工作站。这句报告的诡异之处在于它描述了两个症状：
1. `Win+L` 出不去；
2. **其它所有热键也跟着坏了**。

一个键坏掉还能理解；一个键坏掉把整条链路带崩，说明**状态被污染了**。这是第一个线索，
也是整篇故事的主线：**问题不在「某个键」，而在「某个状态被谁持有、由谁释放」。**

---

## 0. 先给没有 Windows 输入经验的人补三块背景

在动代码之前，必须先把三个概念钉死。它们会在后面反复出现。

### 0.1 「窗口消息」不是唯一的键盘来源

在 Windows 上，一个普通程序拿键盘，靠的是**窗口消息**：系统把 `WM_KEYDOWN` / `WM_KEYUP`
投递到焦点窗口的消息队列，程序在自己的消息循环里取出来。

这条路有个致命性质：**只有焦点窗口才拿得到**。而且，在消息投递**之前**，系统会先处理它自己的
「系统热键」（`Alt+Tab`、`Win+L`……），把组合键吃掉。

所以对于一个「想把这些组合转发出去」的程序来说，**窗口消息这一层太晚了**。

### 0.2 更低的一层：低级键盘钩子（`WH_KEYBOARD_LL`）

Windows 提供了一个「更早」的拦截点：`SetWindowsHookEx(WH_KEYBOARD_LL, proc, ...)`。
它会在按键**被投递到任何队列之前**调用你的回调，并允许你返回 `1` 把事件**吞掉**。

三个必须记住的性质：

1. **钩子是按「后安装的先调用」串成一条链的。** 你 `SetWindowsHookEx` 得越晚，你越靠前。
2. **回调在「安装它的那个线程」上执行**，所以那个线程必须在泵消息（否则回调会延迟，甚至被系统摘掉）。
3. 回调里能读到 `KBDLLHOOKSTRUCT`，里面有 `vkCode`（虚拟键码）、`flags`（其中 `LLKHF_INJECTED`
   表示「这是注入的、不是人按的」）等。

> 直觉检查：如果「Hook 是全局的、又在窗口消息之前」，那它天然就是「抓系统键」的正确层。
> 是的——**后面所有的坑，都围绕『谁来装这个钩子、装了几次、顺序如何』展开。**

### 0.3 SDL 的键盘抓取，其实就是这个钩子

本项目的窗口和输入是 SDL 管的。SDL 的 `SDL_SetWindowKeyboardGrab(win, SDL_TRUE)`
在 Windows 上的实现（`SDL_windowswindow.c` 的 `WIN_GrabKeyboard`）就是：

```c
data->keyboard_hook = SetWindowsHookEx(WH_KEYBOARD_LL, WIN_KeyboardHookProc, module, 0);
```

而 `WIN_KeyboardHookProc`（在 `SDL_windowsevents.c`）**只吞掉一小撮键**：

```c
switch (hookData->vkCode) {
case VK_LWIN:   case VK_RWIN:   scanCode = SDL_SCANCODE_LGUI / RGUI; break;
case VK_LMENU:  case VK_RMENU:  ...ALT...
case VK_LCONTROL: case VK_RCONTROL: ...CTRL...
case VK_TAB:    case VK_ESCAPE: ...        /* 为了在 Win7 抓 Alt+Tab / Alt+Esc */
default: return CallNextHookEx(NULL, nCode, wParam, lParam);   /* ← 其余全部放行 */
}
```

**注意 `default: CallNextHookEx`。** 这句话意味着：`Win` 被 SDL 吞了，但**`L` 会继续往下走**，
最终走窗口消息那一条路。

记住这个「`Win` 走钩子、`L` 走消息」的**分裂**——它是最终 bug 的种子。

---

## 1. 最小起点：先搞清楚 `Win+L` 到底卡在哪一层

### 1.1 第一个（正确的）结论：`Win` 只在「捕获中」才转发

先看代码里真正决定「要不要发 `Win`」的地方（`keyboard.cpp`）：

```c
modifiers = 0;
...
if (event->keysym.mod & KMOD_GUI) {
    if (isSystemKeyCaptureActive()) {      /* ← 条件 */
        modifiers |= MODIFIER_META;
    }
}
...
case SDL_SCANCODE_LGUI:
    if (!isSystemKeyCaptureActive()) {     /* ← 同一个条件 */
        return;                            /* 直接丢弃 */
    }
    keyCode = 0x5B;                        /* VK_LWIN */
    break;
```

而 `isSystemKeyCaptureActive()` 要求：模式不是 OFF、窗口有输入焦点、且窗口带着
`SDL_WINDOW_KEYBOARD_GRABBED` 标志。也就是说：**「抓取」没真正装上的时候，`Win` 会被丢掉，
只有 `L` 被发出去**——远端当然不会锁。

到这里可以得到第一个「最小可用」的结论：

> **要转发 `Win+L`，前提是 `Win` 本身先被转发出去。**

### 1.2 弯路 ①：我一度写下了「这是不可能的」

`Win+L` 和 `Ctrl+Alt+Del` 常被放在一起说「保留组合，钩子挡不住」。
我据此在 README 里写下了一段「**`Win+L` 无法被客户端捕获，Moonlight/Deskflow 都一样**」的结论，
并给用户开了个「改注册表 + 关本地锁屏」的偏方。

**这是本次最大的判断失误。** 它错在：把「我当前的实现路径做不到」当成了「系统不允许」。

---

## 2. 反例：Deskflow 明明做到了

用户一句话把我顶了回来：

> 「deskflow 可以的，我用过了。在 Windows 上能抓到 `win+l`，只要把 `DisableLockWorkstation` 关掉即可。」

**方法论：先找反例，再下结论。** 我去读了 Deskflow 的 `src/lib/platform/MSWindowsHook.cpp`，
看到了关键的一段：

```c
if (g_mode == kHOOK_RELAY_EVENTS) {
  switch (wParam) {
  case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL: break;  /* 放行，保持指示灯同步 */
  default: return true;                                      /* ← 其余全部吞掉 */
  }
}
return false;
```

**Deskflow 在转发模式下把「所有按键」都吞掉。** 于是：

- `Win` 的 down 不会到系统；
- `L` 的 down 也不会到系统；
- 系统「根本没看到 `Win+L`」，自然既不锁屏、也不吃掉它；
- 两个键都被 Deskflow 自己完整地转发出去了。

而 SDL 的钩子只吞 `Win`、**放行 `L`**——于是 `L` 撞上了系统的保留组合，被系统吃掉了。

> **结论修正：** 不是「系统不允许」，而是「**我们在错误的层读 `L`**」。
> 只要在**钩子层**把整个组合吞下来，`Win+L` 就能出去。

这里引入第一个真正的设计概念：**「拥有」（ownership）**。
一个组合键的 down 和 up，必须由**同一个 owner** 完整地接管——要么系统，要么我们，不能一人一半。

---

## 3. 第二次尝试：让应用自己拥有 `Win` 键

### 3.1 目标与最小改动

目标很清晰：**在捕获期间，由我们（应用）拥有 `Win` 键**——吞掉它、自己转发，绝不留给系统。

设计上我定了一个**不变量**（后面所有 bug 都是在违反它）：

> **`Win` 的 down 和 up，必须由同一个 owner 发出，且顺序相对组合键正确（先 `Win` down，再组合键）。**

于是新增一对文件 `app/streaming/input/win_keyboard_hook.{h,cpp}`，并在
`SdlInputHandler::updateKeyboardGrabState()` 里、**SDL 装完它的钩子之后**再装我们的钩子
（后装先调 ⇒ 我们排在最前）。核心回调长这样（简化）：

```c
if (vk == VK_LWIN || vk == VK_RWIN) {
    if (!capturing) return CallNextHookEx(...);   /* 不捕获时放行给 SDL/系统 */
    if (down) { s_winKeyVk = vk; forwardKey(vk, true); }   /* 拥有它 */
    else if (up && s_winKeyVk == vk) { s_winKeyVk = 0; forwardKey(vk, false); }
    return 1;                                     /* 系统永远看不到 Win */
}
if (capturing && s_winKeyVk != 0 && !isHandledBySdl(vk) && down) {
    s_capturedKeys[vk] = true;                    /* 拥有这个组合键 */
    forwardKey(vk, true);
    return 1;
}
```

可以先把 `s_winKeyVk`（当前我们正持有的那个 Win）和 `s_capturedKeys[256]` 当成两个「所有权登记表」。

### 3.2 弯路 ②：用 `GetAsyncKeyState` 判断 `Win` 是否按住

我担心「钩子顺序万一不是我们最前、看不到 `Win` 的 down」，就加了个「物理状态兜底」：

```c
bool isWinKeyDown() { return s_winKeyVk != 0 || winKeyPhysicallyDown(); } /* GetAsyncKeyState */
```

结果**整件事变差了**：`Ctrl+Alt+Shift+Z`（切换捕获的快捷键）时灵时不灵。

原因值得单独记住：

> **在低级钩子回调里，`GetAsyncKeyState` 可能返回「过期」的值。**
> 它反映的是系统维护的「异步键状态」，而这个状态的更新时机与钩子拿到的键状态**并不一致**；
> 对被吞掉的键尤其不可靠。

它一旦误报「`Win` 还按着」，我的钩子就会把后面的 `Z` 也当成 `Win+Z` **吞掉**——
而 `Z` 恰恰是「关闭捕获」的开关，被吞掉就意味着**用户失去了唯一的手动恢复手段**。

**教训：不要用「第二信源」去推断所有权。** 所有权只应由**我们亲眼看到的事件**驱动。
我把 `GetAsyncKeyState` 从状态机里彻底删掉了，`Win` 状态变成**纯事件驱动**，从此这部分确定性了。

> 顺带一个必须记录的细节：被吞掉的键（组合键）我们**自己**发 up；
> 而 `Win` 是「不吞、转发」的键——所以 `Win` 的 up **永远要发**，不能因为
> 「此刻似乎没在捕获」就 `return` 掉。这一条在 `keyboard.cpp` 里也补了：
> ```c
> case SDL_SCANCODE_LGUI:
>     if (event->state == SDL_PRESSED && !isSystemKeyCaptureActive()) return; /* 只挡按下 */
>     ...
> ```

### 3.3 单个概念：`0x8000` 与「修饰键发 0」

转发用 `LiSendKeyboardEvent2(0x8000 | vk, DOWN/UP, modifiers, flags)`。
`0x8000` 是 Moonlight 协议里「这是 VK 码」的约定；主机侧会 `& 0x00FF` 取低字节。

`modifiers` 我们**特意发 0**，两个原因：

1. 修饰键（Shift/Ctrl/Alt）本身会被当作**独立按键事件**转发，主机已经知道它们按着；
2. 所有已知 GFE 版本会**丢弃带 META 标志的键盘事件**，带上反而会把 `Win+X` 弄丢。

> 这是「精确抽象」的一个小例子：**能用一个不变量（修饰键作为独立事件存在）替代的冗余信息，就不要发。**

---

## 4. 真正的大 bug：SDL 会在焦点变化时**重装**它的钩子

修完 `GetAsyncKeyState` 之后，一个 bug 稳定复现：

> 「从别的窗口切回 Artemis 后，`Win+L` 失效（`Win+H` 却正常）；
> 此时按 `Win+L`，捕获会失效，**要手动 `Ctrl+Alt+Shift+Z` 拨两次才能恢复**。」

「**拨两次 Z 能恢复**」是决定性线索。`setCaptureActive(false/true)` 会**卸载再安装**我们的钩子——
如果它能修好，那问题一定是**「钩子顺序被换掉了」**。

去读 SDL 源码，一切就通了（`SDL_video.c`）：

```c
void SDL_OnWindowFocusGained(SDL_Window *window) { ... SDL_UpdateWindowGrab(window); }

void SDL_UpdateWindowGrab(SDL_Window *window) {
    if (window->flags & SDL_WINDOW_INPUT_FOCUS) keyboard_grabbed = TRUE;
    else                                         keyboard_grabbed = FALSE;   /* 失焦 */
    ...
    _this->SetWindowKeyboardGrab(_this, window, keyboard_grabbed);            /* ← 重装/卸载 */
}
```

**每次窗口重新获得焦点，SDL 都会重新 `SetWindowsHookEx` 它自己的钩子 → 它变成「最新」→ 排到我们前面。**

后果：切回 Artemis 后，`Win` 的 down 被 SDL 的钩子先吞掉，**我们的钩子根本看不到它**，
于是 `s_winKeyVk == 0`，我们**不认为这是 `Win` 组合**，`Win+L` 就漏给了系统的保留组合。
而 `Win+H` 看起来「正常」，只是因为漏抓时它还能走普通窗口消息路径转发出去。

**修复**：加一个 `reinstall()`——只重排顺序、**不释放按键状态**；
在窗口重新获得焦点时（`SDL_WINDOWEVENT_FOCUS_GAINED` → `m_InputHandler->notifyFocusGained()`）调用它，
把我们的钩子重新顶到链首。

```c
void WinKeyboardHook::reinstall() {
    if (s_hook == nullptr) return;
    UnhookWindowsHookEx(s_hook);
    s_hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHookProc, nullptr, 0);
}
```

> 修完这个，「聚焦回来后 hotkey 失效」彻底没了。
> 但还剩最后一个 bug——而且它是**镜像**的。

---

## 5. 最后一个 bug：`down` 和 `up` 走了**两条不同的路**

剩下的现象是：

> 「在界面里按 `Win+xxx`，**切到本地其它窗口**，本地会一直保持 `Win` 按下。
> 手动按一下再松开 `Win` 就好了。」

「手动按一下再松开能治好」= 补发了一次 `Win` 的 down+up。这几乎坐实了：**有一次 `Win` 的 down 发了，但 up 没发。**

问题出在我的钩子对 `Win` 的处理是**不对称**的：

- 看到 `Win` 的 down 时，**只有**在我没拥有它的时候才转发（`s_winKeyVk == 0` 分支）；
- 但看到 `Win` 的 up 时，只要 `capturing` 为真就**无条件吞掉**。

于是出现了「**down 是别人发的、up 被我吞掉**」的错配：

1. 某次 `Win` 的 down 恰好走的是 SDL/窗口路径（`capturing` 翻转的瞬间）；
2. 等 `up` 到来时 `capturing` 已经是真 → 我的钩子把它吞了、又不转发；
3. 那个「别人」永远等不到 up → `Win` 卡住。

**修复（对称性）**：

```c
/* Win up：只有我拥有过它的 down，我才吞并转发；否则放行给正常路径补发 release */
if (s_winKeyVk == vk) { forwardKey(vk, false); return 1; }
else                  return CallNextHookEx(...);   /* 别吞，让 owner 自己收尾 */
```

### 5.1 再加一层兜底：`releaseKeys()` 与「本地 resync」

即便如此，跨组件边界的状态仍可能有残留。于是我加了**兜底释放**——任何
「停止捕获 / 失焦 / 卸载」都调用 `releaseKeys()`，它做三件事：

1. 释放我们捕获的所有组合键；
2. 给**主机**无条件补发 `Win` 抬起（多发的 up 主机端会忽略，因为 Apollo 对「未按下的键的 up」是 no-op）；
3. **给本地注入一次 `Win` 抬起**：

```c
void releaseLocalWinKeys() {          /* 用 SendInput 注入，KEYEVENTF_KEYUP */
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD; inputs[0].ki.wVk = VK_LWIN; inputs[0].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[1].type = INPUT_KEYBOARD; inputs[1].ki.wVk = VK_RWIN; inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}
```

第 3 步能生效，靠的正是 SDL 的一个行为（`SDL_OnWindowFocusLost` → `SDL_UpdateWindowGrab`）：
**失焦时 SDL 会自己摘掉它的钩子**——所以那一刻链上只剩我们，注入的 up 能真正到达 Windows，
把本地卡住的状态清掉。（我们自己注入的事件带 `LLKHF_INJECTED`，钩子里主动跳过，不会自产自销。）

到这里，用户回复：**「好的，现在完全修好了。」**

---

## 6. 调试方法论（这一段的价值不亚于代码）

这一路踩过三个「非代码」的坑，值得单独记下来。

### 6.1 先加日志，再谈修复

Windows 输入栈的状态是**不可见**的。不要靠猜。我们在这些地方打了点（前缀统一）：

- `Windows keyboard hook: installed / uninstalled / re-installed`
- `forward Windows key down/up (vk=0x..)`
- `forward key down/up (vk=0x..)`（组合键）
- `pass Windows key up (vk=..) to normal path`（那个「错配」分支）
- `release local Windows keys`

日志一出来，很多争论当场结束——比如「`Win` 的 down/up 是否成对」「失焦时有没有释放」。

### 6.2 陷阱一：**你在测旧的 exe，而构建是「静默成功」的**

我改了几轮，用户每次都说「还是一样」。查下去发现：

- 用户开着 Artemis，`build\deploy\Artemis.exe` **被占用**；
- 部署脚本里的 `copy /y` **失败但不报错**（输出被重定向到了 `nul`）；
- 于是用户一直在测几轮之前的那份 exe。

**教训：** 部署脚本要检查拷贝结果；验证「跑的是不是最新」要靠**客观证据**，不是自觉：

```powershell
(Get-FileHash build\deploy\Artemis.exe).Hash     # 和构建产物对比
Select-String <log> -Pattern "release local Windows keys"   # 新版才有的特征行
```

### 6.3 陷阱二：同时开了**两个**实例

有段时间用户同时跑了两个 Artemis（PID 71596 / 53136）。两个实例各自装一份**全局**钩子、
各自连主机，键盘行为互相污染——这种「环境噪声」会让任何单点修复看起来都无效。

**教训：** 复现实验前先确保环境干净（`Get-Process Artemis` 应为空、只启动一个）。

---

## 7. 构建环境：只装「最少依赖」

为了让改动能真正编译验证，环境用的是**最小集合**（也呼应「less is more」）：

```powershell
scoop install aqtinstall
aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 `
    --archives qtbase qtdeclarative qtsvg `
    --outputdir C:\Qt
```

几点说明：

- **版本对齐 CI**：仓库的 `dev-build.yml` 用 `6.8.3` / `win64_msvc2022_64`，照抄最省事。
- **只装三个 archive**：`app.pro` 是 `QT += core quick network quickcontrols2 svg`，
  代码里**没有**用到 `QtMultimedia`（CI 装它只是保险），也没用到 Qt5 兼容层 `qt5compat`。
  所以只装 `qtbase / qtdeclarative / qtsvg` 就够——这正是「精确抽象」在依赖上的体现：
  **按实际使用面裁剪，而不是照搬整个 SDK。**
- 依赖（SDL2 / FFmpeg / OpenSSL / Opus…）来自 `libs` 子模块（`libs/windows/{include,lib}/x64`），
  构建前 `git submodule update --init --recursive` 一次即可。
- 构建：`vcvarsall amd64` + 仓库自带的 `scripts/jom.exe release`。
- **运行**前必须部署：把 `libs/windows/lib/x64/*.dll`、`AntiHooking.dll`、
  `gamecontrollerdb.txt`，以及 `windeployqt` 出来的 Qt 运行时都放到 exe 旁边——
  否则就是那句熟悉的「找不到 SDL2.dll / avcodec-61.dll」。

> 「只编不部署」是新手最常撞的墙：编译产物 ≠ 可运行产物。Windows 上它们是两件事。

---

## 8. 最终形态（代码地图）

```
app/streaming/input/win_keyboard_hook.h        # 对外：install / reinstall / uninstall / releaseKeys / setCaptureCheck
app/streaming/input/win_keyboard_hook.cpp      # 钩子本体/状态机（仅 win32:!winrt 编译）
app/streaming/input/input.cpp                  # 接线：updateKeyboardGrabState() 装/卸；notifyFocusLost() 释放
app/streaming/input/input.h                    # 声明 notifyFocusGained()
app/streaming/session.cpp                      # 焦点变化 → notifyFocusGained / notifyFocusLost
app/streaming/input/keyboard.cpp               # Win 键 up 永不丢弃（不捕获时也能收尾）
app/app.pro                                    # win32:!winrt 增加源文件 + DEFINES += HAVE_WIN_KEYBOARD_HOOK
```

钩子内部的**状态机**（这才是复杂度的真正所在，全部围绕「所有权」）：

| 状态 | 含义 | 何时置位 / 清除 |
|---|---|---|
| `s_winKeyVk` | 我们当前「拥有」并转发中的 Win 键（0=无） | 捕获中看到 Win down 时置位；Win up / 释放时清零 |
| `s_capturedKeys[256]` | 我们吞掉并转发过的组合键 | 捕获 Win+key down 时置位；其 up / 释放时清零 |
| `s_captureCheck` | 由输入层注入的「现在还能捕获吗」判定 | 等价于 `isSystemKeyCaptureActive()` |

**三条不变量**（违反任意一条都会出现「卡键」）：

1. **同源**：某个键的 down 与 up 必须由同一 owner 发出；
2. **有序**：`Win` 的 down 必须先于组合键的 down 到达主机；
3. **可回滚**：任何「停止捕获 / 失焦 / 卸载」都必须把登记表清空并补发 release（幂等）。

---

## 9. 复盘：设计原则是如何体现在这场 debug 里的

### 9.1 Gall 定律

> *A complex system that works has evolved from a simple system that worked.*

这次完全印证了它：

- 起点是**最小的**「`Win` 只在捕获时转发」；
- 每一步只引入**一个新概念**：先「吞掉整个组合」，再「拥有 Win 键」，再「钩子顺序」，
  最后才是「跨组件边界的 down/up 对称性」；
- 这些概念**不是提前设计出来的**，而是被症状一步步逼出来的：
  「拨两次 Z 能恢复」逼出了「顺序」，「本地卡 / 主机卡」逼出了「所有权边界」，
  「`Ctrl+Alt+Shift+Z` 时灵时不灵」逼出了「不要用第二信源」。
- 如果我一开始就试图「写一个完整的、正确的钩子」，大概率会写成一个更复杂、更难调的系统。

### 9.2 五条原则的落点

| 原则 | 在这里的体现 |
|---|---|
| **最小起步** | 先只想清楚「`Win` 什么时候才被转发」，而不是一上来设计钩子框架 |
| **精确抽象** | 「所有权」这一个抽象，统一解释了所有卡键现象；`0x8000`/修饰键 0 是精确的信息裁剪 |
| **单步单概念** | 每轮只动一处：吞组合 → 拥有 Win → 修顺序 → 修对称性 → 兜底 resync |
| **动机优先** | 先讲「为什么必须由同一个人发 down/up」，再给代码；先讲「为什么 GetAsyncKeyState 不能用」 |
| **工程细节浮现** | 生命周期（`uninstall` 释放）、幂等（`releaseKeys` 可重复调用）、调试接口（日志）都是被问题逼出来的 |

### 9.3 「Good Design」的几个影子

- **可组合 / 无状态 / 幂等**：`releaseKeys()` 可以被任意次调用而不产生副作用；
  `install()` / `reinstall()` / `uninstall()` 都是幂等的。
- **正交**：`setCaptureCheck` 把「要不要捕获」的**策略**从钩子的**机制**里剥离，
  钩子只关心「当前能不能捕获」这个布尔值，不关心串流/焦点/全屏的细节。
- **plumbing vs porcelain**：`win_keyboard_hook` 是 plumbing（只管吞/发/登记），
  `SdlInputHandler` 是 porcelain（决定何时启用）。
- **less is more**：Qt 只装三个 archive；修饰键干脆不发；`0x8000|vk` 用最低位约定表达类型。

---

## 10. 如果只带走 5 句话

1. **「做不到」和「我没在正确的层做」是两件事**——先找反例（Deskflow）再下结论。
2. **在低级钩子里，不要用 `GetAsyncKeyState` 之类的旁路信息去推断状态**；只信你亲眼看到的事件。
3. **低级钩子是「后装先调」的链**；而 SDL 会在**窗口每次获得焦点时重装它的钩子**——
   任何和它共存的钩子都要考虑顺序被翻转。
4. **一个键卡住，通常是 down 和 up 走了两条不同的路**；修法不是「多发几次」，
   而是让**同一个 owner** 收尾，并在边界处**幂等兜底**。
5. **Windows 上「卡键」可以是本地也可以是远端**；先把「卡在哪台机器」问清楚，
   否则会在错误的机器上修半天。以及——**先确认你测的是不是最新的 exe。**

---

## 附录 A：关键取材位置

- SDL 键盘抓取实现：`SDL_windowswindow.c::WIN_GrabKeyboard` / `WIN_SetWindowKeyboardGrab`
- SDL 钩子回调：`SDL_windowsevents.c::WIN_KeyboardHookProc`
- SDL 焦点与抓取：`SDL_video.c::SDL_UpdateWindowGrab` / `SDL_OnWindowFocusGained` / `SDL_OnWindowFocusLost`
- Deskflow 参考实现：`src/lib/platform/MSWindowsHook.cpp`
- Moonlight 协议：`LiSendKeyboardEvent2`（`0x8000` 约定、META 事件被 GFE 丢弃）
- 主机侧（Apollo）：`src/platform/windows/input.cpp`（`keyCode & 0x00FF`、`key_press` 记账）

## 附录 B：典型日志片段（最终形态）

```
Windows keyboard hook: installed
Windows keyboard hook: re-installed              ← 焦点回来后重排到链首
Windows keyboard hook: forward Windows key down (vk=0x5B)
Windows keyboard hook: forward key down (vk=0x4C)   ← L
Windows keyboard hook: forward key up   (vk=0x4C)
Windows keyboard hook: forward Windows key up (vk=0x5B)
Windows keyboard hook: release local Windows keys   ← 失焦兜底
```
