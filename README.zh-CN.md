# ThreadGuard

## 在模块卸载前等待后台线程结束

GoldSrc 的 Win32 线程代码在创建它的模块被卸载时可能仍在运行，这会导致游戏退出以及执行 `_restart` 时崩溃或长时间卡住。ThreadGuard 会 hook 被接管模块内部的 `CreateThread` / `WaitForSingleObject` / `Sleep`，记录它们创建的每一个线程句柄，并在模块被释放之前等待这些线程结束。

[英文README](README.md)

接管的模块：

| 模块 | 说明 |
|---|---|
| `hw.dll` (引擎) | 始终接管 |
| `GameUI.dll` | 当其使用 `steam_api.dll` 回调模型、且未导入 `CreateThread` 时跳过 |
| `ServerBrowser.dll` | 同 `GameUI.dll` |
| `server.dll` | 仅限 Sven Co-op（`svencoop` 游戏目录） |

该插件还修复了 Valve 的一个 bug：`_restart` 命令没有正确关闭服务器，导致 `CSteam3Server` 对象出现资源泄漏。被 hook 后的 `_restart` 会先执行 `shutdownserver`以正确释放这些资源。

普通 PE 与 BLOB 引擎都安装 `FreeLibrary` hook，在关闭或重启时，先等待 GameUI 的受管线程退出，再卸载 GameUI；GameUI 自身的 `FreeLibrary` hook 保留对 ServerBrowser 的同类保护，避免 socket 线程在持有 loader lock 的析构阶段被强杀、遗留 CRT 锁。旧版 GameUI/ServerBrowser 工作线程的 `select` 等待限制为 20 ms；收到终止请求后，`select` 返回无就绪 socket，`recvfrom` 返回 `WSAEWOULDBLOCK`，使非阻塞收包循环正常释放锁，再到达原有的退出事件检查。主线程的 socket 调用及 socket 超时选项不变，兼容两种 Winsock DLL 导入名称，并保留仅使用回调模型的模块排除规则。

GameUI 修复的 Debug / Release 构建和测试通过，覆盖空闲及持续 UDP 收包、卸载前等待顺序。真实 HL 3266 调试记录确认 GameUI socket 线程在 DLL 析构前退出，两轮重启测试返回 0；重复测试也出现重新加载期间的堆损坏，以及两轮网络线程均已完成等待后发生的引擎分配器退出卡住，因此不能视为该完整安装的重启已稳定通过。HL 10210、CoF 5936 的退出测试及 Sven 10257 的加载地图、重启、退出测试返回 0。其他快照保留目录覆盖，本次未逐一启动。关闭阶段的诊断使用 `OutputDebugStringA`，因为 GameUI 控制台可能已经关闭；普通 CLI 控制台捕获不会收到这些日志。

全部 11 个受支持的 Windows 引擎快照（`hl-*`，包括 BLOB；`svencoop-*`；`cof-*`）使用网络线程协作退出。直接 `TerminateThread` 可能使工作线程遗留 Steam 锁，从而挂起退出。ThreadGuard 通过 `NET_ThreadFunc` 识别每次创建，持有独立的 duplicate 句柄，并核对 `dwNetThreadId` 的运行时值。引擎针对该线程的 `TerminateThread` 调用改为发出退出请求并等待真实线程结束；网络线程在已经解锁的循环末尾 `Sleep(1)` 中调用 `ExitThread(0)`。其他线程的终止调用保留原行为。

仅将该网络线程的 `select` 等待上限设为 20 ms，更短的超时保持原值。收到退出请求后，`NET_QueuePacket` 返回无包，使持续收包的循环也能正常解锁并走到 Sleep。不会提前清零网络状态，不关闭调用方拥有的句柄，也不在等待超时后回退强杀。创建时先发布身份再恢复线程，重建或重新加载引擎时复位退出请求。参见 [issue #4](https://github.com/MetaHookSv/ThreadGuard/issues/4)。

对于 Windows Sven Co-op 8948/10257，ThreadGuard 还会在 `GL_Shutdown` 之前关闭 Steam 客户端，每轮引擎生命周期只调用一次。引擎原来的晚调用通过 `SteamAPI_Shutdown` IAT hook 屏蔽；否则其重启路径会跳过这次关闭，使 Steam 线程遗留到进程退出。处理顺序为 `shutdownserver -> 原始 _restart -> SteamAPI_Shutdown -> GL_Shutdown`，随后允许 launcher 重新加载引擎。参见 [issue #898](https://github.com/hzqst/MetaHookSv/issues/898)。

2026-10-05 实机验证：Windows x86 Sven 10257，MetaHook 正常退出契约为 0。旧版 ThreadGuard 在 `osprey -> _restart -> quit` 后复现 `0xC0000409`，修复后相同对照返回 0。另分别启用和禁用 HalflifeCLI，测试不重启、重启一次、连续重启三次，共六组；每次重启后均确认 `osprey` 和原生 RCON 可用，六组均以 0 退出。Renderer 因已安装版本的独立 gamedata 不匹配问题临时禁用；测试后恢复插件列表和临时 CLI 配置。Release 构建及全部 11 个 gamedata 快照校验通过。8948 仅核实 Windows 调用路径和符号产物，本次未实机验证其他引擎及 Renderer 兼容性。

2026-10-06 网络线程修复验证：Debug / Release 定向测试均通过，包含真实线程的 100 次停止/重建、无限 select、持续收包及句柄回收。Sven 10257 的 50 次输入命令退出和五组对照均返回 0；HL 10210、HL 3266 BLOB 和 CoF 5936 的网络线程退出调试记录均包含请求、安全 Sleep 退出和等待 signaled，进程返回 0。CoF 输入命令场景在 quit 前失去 RCON 响应，已保留 dump，未计为该场景通过。其余快照仅完成符号生成、目录校验及静态调用路径核验，未逐一实机运行。

BLOB 引擎需要包含 ordinal 导入修复的 MetaHook：加载器记录 ordinal 导入，并通过原始导出地址支持 `BlobIATHook` / `BlobHasImportEx` 按名称查询 `select`。插件不依赖硬编码 ordinal 或 `select_import` gamedata。发布本修复前，还需发布上游全部 11 个快照的 `NET_QueuePacket` 符号；本地验证使用已生成并通过校验的目录。

`NET_Shutdown` 入口 hook 会先请求并等待该网络线程结束，再进入原关闭函数。实际核验的 11 个 Windows 二进制均先关闭 socket、后停止线程，而工作线程的 `select` 在网络锁外；提前等待可防止该关闭路径与在途 `select` 并发，也避免清理期间重新填入 lag 队列。队列清理、socket 关闭和锁销毁仍由原函数负责。重复关闭及未启用网络线程时，原资源清理仍正常执行。

补充关闭入口 hook 后，顺序回归测试先复现“原资源回调执行时线程仍存活”，修复后 Debug / Release 均通过。100 次 handler 循环覆盖空闲与持续收包、等待后关闭真实 socket、重复关闭和禁用网络线程。随后实测 Sven 10257 共 8 组（含断线、重启、禁用网络线程），以及 HL 10210、HL 3266 BLOB、CoF 5936 各一次退出，全部返回 0；启用网络线程的场景均记录到等待结束后才进入原资源清理。测试用 DLL、启动器、目录和临时配置均已恢复。

## 安装

1. 下载并安装 [MetaHookSv](https://github.com/hzqst/MetaHookSv)。

2. 构建或下载 .dll，放入 `/SteamLibrary/steamapps/common/Sven Co-op/svencoop/metahook/plugins` 目录。

3. 在 `/SteamLibrary/steamapps/common/Sven Co-op/svencoop/metahook/configs/plugins.lst` 中添加 `ThreadGuard.dll`（单独占一行）。

4. 保留随插件一同分发的 `svencoop/metahook/gamedata/threadguard` 目录：其中包含 `eng`、`NET_ThreadFunc`、`dwNetThreadId`、`NET_QueuePacket`、`NET_Shutdown`，以及 Sven Co-op 使用的 `GL_Shutdown`。更新 DLL 时同时更新此目录。HL25 已内联 `NET_StartThread`，因此不要求该符号。

5. 开始游戏。

## F5 调试（可选）

先安装 MetaHook，并在游戏的 `plugins.lst` 中启用本插件，然后配置独立的 Visual Studio Win32 解决方案：

```powershell
cmake -S . -B build/launch -G "Visual Studio 17 2022" -A Win32 -DMETAHOOKSV_ENABLE_LAUNCH_GAME=ON
```

打开解决方案，选择 **LaunchGame** 后按 **F5**。**DeployGame** 编译本插件及依赖，暂存 Install，再复制插件 DLL、PDB 和资源，最后由原生调试器启动已有游戏 launcher。不会修改根目录启动器/运行库或插件列表。VS 应开启运行前构建，并将构建失败策略设为 **不启动**；重新部署前请退出游戏。普通构建不会部署。

`METAHOOKSV_GAME_DIRECTORY` 默认通过 Steam 自动查找，`METAHOOKSV_GAME_APPID` 默认为 `225840`。自定义 mod 使用 `METAHOOKSV_GAME_MOD`，附加参数使用 `METAHOOKSV_GAME_ARGUMENTS`；支持 Debug 和 Release。

共享模块依次从 `METAHOOKSV_LAUNCH_GAME_MODULE_DIR`、所在 MetaHookSv 聚合仓库或固定提交的源码包获取。缺少 Installer 源码时自动下载 GitHub `latest` 的自包含 CLI，无需安装 .NET；可用 `METAHOOKSV_INSTALLER_RELEASE` 固定 tag，或用 `METAHOOKSV_INSTALLER_CLI_EXECUTABLE` 指定离线 EXE。插件模式要求 v20261004c 或之后版本。`build/launch/launch-game/installer/<release>` 下的有效缓存直接复用，不自动升级；切换 tag 或清理该私有缓存后重新下载。首次下载若触发 GitHub API 限流，可通过环境变量 `GH_TOKEN`/`GITHUB_TOKEN` 提供凭据。功能默认 OFF，关闭时不新增下载。

## 构建

构建需求：Windows、Visual Studio 2022、CMake 3.21 或更高版本、Python 3.8 或更高版本。首次 configure 会下载 VC-LTL 5.3.1 到 `thirdparty/cache`，并同步 gamedata 目录。

1. 运行 `scripts\build-ThreadGuard-x86-Release.bat`（或 `scripts\build-ThreadGuard-x86-Debug.bat`）。

2. 插件、其 PDB 以及 gamedata 目录会被安装到 `install\x86\<Configuration>\svencoop\metahook`。

3. 按照“安装”一节的说明，将 `ThreadGuard.dll` 和 `gamedata\threadguard` 复制到 `svencoop/metahook`。

MetaHook SDK 会在 configure 时自动拉取最新的 `main` 分支。如需改为针对本地 MetaHook 源码树构建，可在命令行传入，或在 configure 前导出同名环境变量：

```
scripts\build-ThreadGuard-x86-Release.bat -DMETAHOOK_SOURCE_PATH=D:\MetaHook
```

该路径为仓库根目录，需提供 `include/metahook.h`、`include/HLSDK` 与 `include/Interface`。

ThreadGuard 从 MetaHook 的 gamedata 目录解析其符号，不链接任何第三方库，也不需要 Capstone 头文件。传入 `-DTHREADGUARD_SYNC_GAMEDATA=OFF` 可在不下载 gamedata 的情况下构建。

## 定向测试

```powershell
cmake -S tests -B build/tests -A Win32 -DMETAHOOK_SOURCE_PATH=D:/MetaHookSv/MetaHook
cmake --build build/tests --config Release
ctest --test-dir build/tests -C Release --output-on-failure
```

将 `Release` 换成 `Debug` 可测试调试配置。独立测试使用真实 Win32 线程、socket、锁和等待函数，但只提供宿主的致命错误回调，用于验证 handler 行为；真实 IAT 安装及 gamedata 解析仍需游戏实测。handler 测试包含空闲 socket 和持续取包条件下的 100 次停止/重建，以及创建失败、挂起创建、非目标转发和句柄计数检查。实机调试输出记录退出请求、安全 Sleep 退出及真实等待 signaled 三个阶段。
