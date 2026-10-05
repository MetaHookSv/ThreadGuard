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

对于 Windows Sven Co-op 8948/10257，ThreadGuard 还会在 `GL_Shutdown` 之前关闭 Steam 客户端，每轮引擎生命周期只调用一次。引擎原来的晚调用通过 `SteamAPI_Shutdown` IAT hook 屏蔽；否则其重启路径会跳过这次关闭，使 Steam 线程遗留到进程退出。处理顺序为 `shutdownserver -> 原始 _restart -> SteamAPI_Shutdown -> GL_Shutdown`，随后允许 launcher 重新加载引擎。参见 [issue #898](https://github.com/hzqst/MetaHookSv/issues/898)。

2026-10-05 实机验证：Windows x86 Sven 10257，MetaHook 正常退出契约为 0。旧版 ThreadGuard 在 `osprey -> _restart -> quit` 后复现 `0xC0000409`，修复后相同对照返回 0。另分别启用和禁用 HalflifeCLI，测试不重启、重启一次、连续重启三次，共六组；每次重启后均确认 `osprey` 和原生 RCON 可用，六组均以 0 退出。Renderer 因已安装版本的独立 gamedata 不匹配问题临时禁用；测试后恢复插件列表和临时 CLI 配置。Release 构建及全部 11 个 gamedata 快照校验通过。8948 仅核实 Windows 调用路径和符号产物，本次未实机验证其他引擎及 Renderer 兼容性。

## 安装

1. 下载并安装 [MetaHookSv](https://github.com/hzqst/MetaHookSv)。

2. 构建或下载 .dll，放入 `/SteamLibrary/steamapps/common/Sven Co-op/svencoop/metahook/plugins` 目录。

3. 在 `/SteamLibrary/steamapps/common/Sven Co-op/svencoop/metahook/configs/plugins.lst` 中添加 `ThreadGuard.dll`（单独占一行）。

4. 保留随插件一同分发的 `svencoop/metahook/gamedata/threadguard` 目录：其中存放着 `eng` 全局变量，以及 Sven Co-op 使用的 `GL_Shutdown` 函数。更新 DLL 时同时更新此目录。

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
