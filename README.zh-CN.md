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

该插件还修复了 Valve 的一个 bug：`_restart` 命令没有正确关闭服务器，导致 `CSteam3Server` 之类的资源泄漏。被 hook 后的 `_restart` 会先执行 `shutdownserver`。

## 安装

1. 下载并安装 [MetaHookSv](https://github.com/hzqst/MetaHookSv)。

2. 构建或下载 .dll，放入 `/SteamLibrary/steamapps/common/Sven Co-op/svencoop/metahook/plugins` 目录。

3. 在 `/SteamLibrary/steamapps/common/Sven Co-op/svencoop/metahook/configs/plugins.lst` 中添加 `ThreadGuard.dll`（单独占一行）。

4. 保留随插件一同分发的 `svencoop/metahook/gamedata/threadguard` 目录：其中存放着插件在加载时解析的 `eng` 全局变量。

5. 开始游戏。

## 构建

构建需求：Windows、Visual Studio 2022、CMake 3.21 或更高版本、Python 3.8 或更高版本。首次 configure 会下载 VC-LTL 5.3.1 到 `thirdparty/cache`，并同步 gamedata 目录。

1. 运行 `scripts\build-ThreadGuard-x86-Release.bat`（或 `scripts\build-ThreadGuard-x86-Debug.bat`）。

2. 插件、其 PDB 以及 gamedata 目录会被安装到 `install\x86\<Configuration>\svencoop\metahook`。

3. 按照“安装”一节的说明，将 `ThreadGuard.dll` 和 `gamedata\threadguard` 复制到 `svencoop/metahook`。

MetaHook SDK 会在 configure 时自动拉取固定 commit。如需改为针对本地 MetaHook 源码树构建，可在命令行传入，或在 configure 前导出同名环境变量：

```
scripts\build-ThreadGuard-x86-Release.bat -DMETAHOOK_SOURCE_PATH=D:\MetaHook
```

该路径为仓库根目录，需提供 `include/metahook.h`、`include/HLSDK` 与 `include/Interface`。

ThreadGuard 从 MetaHook 的 gamedata 目录解析其符号，不链接任何第三方库，也不需要 Capstone 头文件。传入 `-DTHREADGUARD_SYNC_GAMEDATA=OFF` 可在不下载 gamedata 的情况下构建。
