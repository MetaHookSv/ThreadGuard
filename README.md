# ThreadGuard

## Wait for background threads before a module is unloaded

GoldSrc's Win32 thread code can still be running when the module that created a thread is unloaded, which leads to crashes or long hangs on shutdown and on `_restart`. ThreadGuard hooks `CreateThread` / `WaitForSingleObject` / `Sleep` inside the managed modules, records every thread handle they create, and waits for those threads to terminate before the module is released.

Managed modules:

| Module | Notes |
|---|---|
| `hw.dll` (engine) | always |
| `GameUI.dll` | skipped when it uses the `steam_api.dll` callback model and does not import `CreateThread` |
| `ServerBrowser.dll` | same as `GameUI.dll` |
| `server.dll` | Sven Co-op (`svencoop` game directory) only |

It also fixes a Valve bug where the `_restart` command did not shut the server down properly, leaking resources such as `CSteam3Server`: the hooked `_restart` runs `shutdownserver` first.

The engine's `FreeLibrary` import is hooked on both PE and BLOB engines. During
shutdown/restart it joins GameUI's managed workers before unloading GameUI;
GameUI's own `FreeLibrary` hook already does the same for ServerBrowser. This
keeps their socket-thread destructors from killing live workers under the loader
lock and abandoning CRT locks. Legacy GameUI/ServerBrowser worker `select` waits
are capped at 20 ms. After termination starts, `select` reports no ready sockets
and `recvfrom` reports `WSAEWOULDBLOCK`, so the nonblocking drain loop releases its
locks and reaches the existing shutdown-event check. Main-thread socket calls
and socket timeout options are unchanged. Both Winsock import DLL names are
supported; callback-only module exclusions remain intact.

At the engine's `ExitGame` lifecycle callback, ThreadGuard always requests and
joins its remaining managed workers before CRT detach. It does not require
`GetState()` to still report closing or restarting: HL 3266's `CEngine::Unload`
has already reset it to `DLL_INACTIVE`. Skipping this join allowed old Steam
discovery workers to access allocations from the previous engine lifetime and
leave the small-block allocator locked after an access violation. The earlier
module-unload gates and network shutdown ordering remain unchanged.

Before the ExitGame fix, the GameUI change was checked with Debug/Release builds and handler tests for
idle/continuous UDP receive and unload ordering. Real HL 3266 debugger evidence
shows its GameUI socket worker exiting before the DLL destructor; two restart
runs exited 0. Repeated runs also exposed heap corruption during reload and an
engine allocator shutdown hang after both network joins had completed, so the
full HL 3266 installation is not considered a consistently passing restart test.
HL 10210 and CoF 5936 exit tests and Sven 10257's map/restart/quit test exited 0.
Other snapshots retain catalog coverage but were not launched for this change.
Teardown diagnostics use OutputDebugStringA because the GameUI console can
already be shut down; ordinary CLI console capture does not receive them.

The ExitGame fix passed Debug/Release builds and both CTests, including a real
worker that must finish cleanup before the inactive-state lifecycle callback
returns. Five original HL 3266 BLOB map/restart/quit runs exited 0 without the
previous access violations. HL 10210 and CoF 5936 exit checks and Sven 10257's
map/restart/quit check also exited 0. All 11 catalog snapshots validated; the
other seven were not individually launched. The separate earlier heap-corruption
crash is not independently proven to have the same cause.

Network threads shut down cooperatively on all 11 supported Windows engine
snapshots (`hl-*`, including BLOB, `svencoop-*`, and `cof-*`). Killing this worker
with `TerminateThread` can abandon a Steam lock and hang shutdown. ThreadGuard
identifies each creation through `NET_ThreadFunc`, tracks its own duplicate
handle, and checks the live `dwNetThreadId` value. The engine's target
`TerminateThread` call instead requests exit and waits for the real thread to
finish. The worker calls `ExitThread(0)` from its unlocked `Sleep(1)` loop tail.
Other threads' termination calls keep their original behavior.

The `NET_Shutdown` entry hook requests and joins this worker before calling the
original shutdown. All 11 Windows binaries close sockets before their original
thread stop, and the worker calls `select` outside the network lock. Waiting at
entry prevents that shutdown from closing sockets under an in-flight `select`
and prevents the worker from repopulating the lag queues during cleanup. The
original function still owns queue cleanup, socket closure and lock destruction.
Repeated shutdown and disabled threaded networking retain original cleanup.

Only this worker's `select` waits are capped at 20 ms (shorter waits are kept).
After an exit request, `NET_QueuePacket` returns no packet, allowing even a busy
receive loop to unlock and reach that sleep. No network flags are cleared early,
no caller-owned handles are closed, and there is no timeout fallback to a forced
kill. Creation publishes the identity before resuming the worker, and recreation
or engine reload resets the request. See [issue #4](https://github.com/MetaHookSv/ThreadGuard/issues/4).

On Windows Sven Co-op 8948/10257, ThreadGuard also closes the Steam client before
`GL_Shutdown`, once per engine lifetime. SvEngine's original late
`SteamAPI_Shutdown` import call is suppressed; its restart path otherwise skips
that call and can leave Steam threads alive until process teardown. This keeps
the order `shutdownserver -> original _restart -> SteamAPI_Shutdown -> GL_Shutdown`
without exiting the launcher. See [issue #898](https://github.com/hzqst/MetaHookSv/issues/898).

Verified on 2026-10-05 with Windows x86 Sven 10257 and MetaHook's normal exit
code 0 contract: the old ThreadGuard reproduced `0xC0000409` after
`osprey -> _restart -> quit`; the fixed DLL returned 0 for the same control.
Six further runs covered zero, one and three restarts, with HalflifeCLI both
enabled and disabled. Every restart restored `osprey` and native RCON, and all
six processes exited with 0. Renderer was temporarily disabled because of its
unrelated installed gamedata mismatch; plugin lists and temporary CLI config
were restored afterward. The Release build and all 11 gamedata snapshots passed
validation. Sven 8948 has Windows code-path and catalog verification only;
other engines and Renderer compatibility were not runtime-tested in this change.

# Install

1. Download and install [MetaHookSv](https://github.com/hzqst/MetaHookSv).

2. Build or download .dll, put it into `/SteamLibrary/steamapps/common/Sven Co-op/svencoop/metahook/plugins` directory.

3. Add `ThreadGuard.dll` in `/SteamLibrary/steamapps/common/Sven Co-op/svencoop/metahook/configs/plugins.lst` as a newline.

4. Keep the `svencoop/metahook/gamedata/threadguard` directory shipped next to the plugin: it carries `eng`, `NET_ThreadFunc`, `dwNetThreadId`, `NET_QueuePacket`, `NET_Shutdown`, and, for Sven Co-op, `GL_Shutdown`. Update this catalog together with the DLL. `NET_StartThread` is not required because HL25 inlines it.

5. Enjoy.

Verified on 2026-10-06: Debug and Release targeted tests passed, including 100
real-thread stop/recreate cycles, infinite select, continuous receive and handle
reclamation. Sven 10257 passed 50 input-command exits and five controls. HL 10210,
HL 3266 BLOB and CoF 5936 recorded the request, safe Sleep exit and signaled wait,
and exited with code 0. The CoF input-command scenario lost RCON before quit; its
dump was retained and that scenario is not counted as passing. Other snapshots
have symbol generation, catalog validation and static call-path coverage only.

After adding the pre-cleanup entry hook, the ordering regression first failed
with a live worker in the original resource callback, then passed in Debug and
Release. The 100-cycle handler test includes idle/busy workers, real socket
closure after join, repeated shutdown and disabled networking. Live verification
passed eight Sven 10257 cases (including disconnect, restart and no-network)
and one shutdown each on HL 10210, HL 3266 BLOB and CoF 5936. All exited with 0;
threaded runs logged the completed wait before original resource cleanup.
Game DLLs, launcher, catalogs and temporary configuration were restored.

BLOB engines require MetaHook's ordinal-import fix: the loader records ordinal
imports and matches their original export addresses for named `BlobIATHook` and
`BlobHasImportEx` queries. No hardcoded ordinal or `select_import` gamedata is
needed. Publishing this change also requires upstream `NET_QueuePacket` symbols
for all 11 snapshots; local verification used the generated, validated catalog.

## F5 debugging (optional)

Install MetaHook and enable this plugin in the game's `plugins.lst` first. Configure a standalone Visual Studio Win32 solution:

```powershell
cmake -S . -B build/launch -G "Visual Studio 17 2022" -A Win32 -DMETAHOOKSV_ENABLE_LAUNCH_GAME=ON
```

Open the solution, select **LaunchGame** and press **F5**. **DeployGame** builds this plugin and its dependencies, stages Install, and copies plugin DLLs/PDBs/resources before the native debugger starts the existing game launcher. Root launchers/runtime files and plugin lists remain unchanged. Set VS to build before running and **Do not launch** on build errors; stop the game before redeploying. Ordinary builds do not deploy.

`METAHOOKSV_GAME_DIRECTORY` defaults to Steam discovery; `METAHOOKSV_GAME_APPID` defaults to `225840`. Set `METAHOOKSV_GAME_MOD` for a custom mod and `METAHOOKSV_GAME_ARGUMENTS` for extra arguments. Debug and Release are supported.

The shared module uses `METAHOOKSV_LAUNCH_GAME_MODULE_DIR`, the surrounding MetaHookSv checkout, or a pinned source archive. Without Installer sources, it downloads the self-contained CLI from GitHub `latest` (no .NET required); `METAHOOKSV_INSTALLER_RELEASE` selects a fixed tag, and `METAHOOKSV_INSTALLER_CLI_EXECUTABLE` supplies an offline EXE. Plugin mode requires v20261004c or later. Valid caches under `build/launch/launch-game/installer/<release>` are reused without update checks; select another tag or clear that private cache to upgrade. `GH_TOKEN`/`GITHUB_TOKEN` may be supplied through the environment if GitHub API rate limits prevent the first download. The feature defaults OFF and performs no extra downloads when disabled.

# Build

Requirements: Windows, Visual Studio 2022, CMake 3.21 or newer, Python 3.8 or newer. The first configure downloads VC-LTL 5.3.1 into `thirdparty/cache` and synchronizes the gamedata catalog.

1. Run `scripts\build-ThreadGuard-x86-Release.bat` (or `scripts\build-ThreadGuard-x86-Debug.bat`).

2. The plugin, its PDB and the gamedata catalog are installed to `install\x86\<Configuration>\svencoop\metahook`.

3. Copy `ThreadGuard.dll` and `gamedata\threadguard` into `svencoop/metahook`, as described in Install.

The MetaHook SDK is fetched automatically from the latest `main`. To build against a local MetaHook source tree instead, pass it on the command line or export the same environment variable before configuring:

```
scripts\build-ThreadGuard-x86-Release.bat -DMETAHOOK_SOURCE_PATH=D:\MetaHook
```

The path is the repository root that provides `include/metahook.h`, `include/HLSDK` and `include/Interface`.

ThreadGuard resolves its symbols from the MetaHook gamedata catalog, links no third-party library and needs no Capstone headers. Pass `-DTHREADGUARD_SYNC_GAMEDATA=OFF` to build without downloading gamedata.

## Targeted tests

```powershell
cmake -S tests -B build/tests -A Win32 -DMETAHOOK_SOURCE_PATH=D:/MetaHookSv/MetaHook
cmake --build build/tests --config Release
ctest --test-dir build/tests -C Release --output-on-failure
```

Repeat with `Debug` for the debug configuration. These standalone tests use real
Win32 threads, sockets, locks and waits, but provide only the host's fatal-error
callback. They cover handler behavior, not MetaHook's IAT installation or gamedata
resolution; those require live game verification. The handler test includes 100
stop/recreate cycles with idle sockets and continuous packets, creation failure,
suspended creation, non-target forwarding and handle counts. Debugger output
records the real stop request, safe sleep exit and signaled wait in a game run.
