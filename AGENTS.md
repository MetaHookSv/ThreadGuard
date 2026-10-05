# AGENTS.md - ThreadGuard Project Guide

## Project Overview

**ThreadGuard** is a MetaHookSV plugin that intercepts the Win32 thread APIs (`CreateThread`, `WaitForSingleObject`, `Sleep`) inside the modules that use them and centrally waits for those threads to terminate during shutdown. It exists because GoldSrc's Win32 thread code can still be running after the module that created a thread is unloaded, which leads to crashes or long hangs on shutdown and on `_restart`.

It also fixes a Valve bug: the engine's `_restart` command did not shut the server down properly and leaked resources such as `CSteam3Server` — the hooked `_restart` runs `shutdownserver` first.

For SvEngine, Steam client shutdown is moved before `GL_Shutdown`, once per
engine lifetime. The engine's `SteamAPI_Shutdown` IAT call is suppressed because
it runs late on normal exit and is skipped on restart (MetaHookSv issue #898).

- **Project type**: Native C++ plugin (Windows DLL), MSVC x86 only
- **Engine**: GoldSrc / SvEngine, with Sven Co-op specific gates (see Engine Compatibility)
- **Framework**: MetaHookSV Plugin API (`IPluginsV4`, API 109 or newer; `LoadEngine` also rejects a launcher whose `MetaHookAPIVersion` is lower than the SDK constant)
- **Main dependencies**: MetaHook SDK (public API, `include/HLSDK`, `include/Interface`), the engine's `IEngine` interface, and the Win32 threading/module APIs. **No third-party library is linked and no Capstone headers are needed**

## Project Structure

```
ThreadGuard/
├── src/
│   ├── plugins.cpp            # IPluginsV4 lifecycle: Init / LoadEngine / LoadClient / ExitGame / Shutdown
│   ├── plugins.h              # Shared globals, MHPluginName, GamedataResolvePtr, legacy search macros
│   ├── privatehook.cpp        # Module routing, per-module hook install/uninstall, termination gates
│   ├── privatehook.h          # Engine_FillAddress / Engine_WaitForShutdown / DllLoadNotification
│   ├── ThreadManager.cpp      # Manager registry, the three API wrappers, CThreadManager
│   ├── ThreadManager.h        # IThreadManager interface and the hookflag_* values
│   ├── exportfuncs.cpp        # EngineCommand_InstallHook and the `_restart` fix
│   ├── exportfuncs.h          # EngineCommand_InstallHook declaration
│   └── enginedef.h            # IEngine state constants (DLL_INACTIVE … DLL_RESTART)
├── cmake/
│   ├── Sources.cmake          # Explicit compile list (4 plugin units + the SDK's interface.cpp)
│   ├── Dependencies.cmake     # Source-path resolution and FetchContent fallback
│   ├── LaunchGame.cmake       # Optional F5 deploy support
│   └── VCLTL.cmake            # VC-LTL 5.3.1
├── scripts/
│   ├── build-ThreadGuard-x86-{Debug,Release}.bat
│   ├── manifests/threadguard.json      # eng global + SvEngine GL_Shutdown
│   ├── sync-gamedata.py                # Prunes the upstream catalog into the build tree
│   └── validate-gamedata.py            # Validates it before the plugin target builds
├── thirdparty/cache/          # Ignored VC-LTL binary cache
├── build/x86/<configuration>/    # Ignored build output
├── install/x86/<configuration>/  # Ignored install output
├── README.md, README.zh-CN.md # Bilingual install / managed-module / build documentation
└── CMakeLists.txt             # Windows MSVC x86 build and install rules
```

There is no `docs/` directory, no assets and no test suite.

## Core Modules

### 1. Plugin lifecycle (`src/plugins.cpp`)

`IPluginsV4` exported through `EXPOSE_SINGLE_INTERFACE(IPluginsV4, IPluginsV4, METAHOOK_PLUGIN_API_VERSION_V4)`:

- `LoadEngine`: rejects a mismatched host with `Sys_Error("MetaHookAPIVersion too low! expect %d, got %d !")`, collects the file system and engine type/buildnum, copies `cl_enginefunc_t`, records `g_MainThreadId` with `GetCurrentThreadId()`, resolves `eng` and (SvEngine only) `GL_Shutdown`, then registers `DllLoadNotification`. It installs **no** hooks directly
- `LoadClient`: copies the export table and calls `EngineCommand_InstallHook()` (the `_restart` fix)
- `ExitGame`: calls `Engine_WaitForShutdown(GetEngineModule(), GetBlobEngineModule())`
- `Shutdown`: unregisters the DLL-notification callback
- `GetVersion`: returns the build timestamp baked in by CMake

### 2. Module routing (`src/privatehook.cpp`)

All hooks are installed and removed from the DLL load/unload notification — nothing is hooked at load time. `DllLoadNotification` routes on `LOAD_DLL_NOTIFICATION_IS_ENGINE` first, then on `_wcsicmp` of `BaseDllName`; the unload path matches by comparing `ctx->hModule` with the recorded manager module.

| Managed module | Hook flags | Extra |
| --- | --- | --- |
| engine (`hw.dll`, or the blob engine) | `CreateThread \| WaitForSingleObject \| Sleep` | `FreeLibrary` IAT hook on the **engine's** import table, only when the game directory is `svencoop` |
| `GameUI.dll` | `CreateThread \| WaitForSingleObject` | `FreeLibrary` IAT hook on **GameUI's** import table |
| `ServerBrowser.dll` | `CreateThread \| WaitForSingleObject` | — |
| `server.dll` | `CreateThread` only | enabled only when the game directory is `svencoop` |

Two deliberate gates:

- `GameUI.dll` and `ServerBrowser.dll` are skipped entirely when the module imports `steam_api.dll` but not `kernel32.dll!CreateThread` (the code comment is "It use callback instead of creating stupid thread")
- `server.dll` is tracked for `CreateThread` only — the comment is "Fuck off the CPlayerDatabase_RunThread" — and only under `svencoop`

`InstallHook` uses `IATHook` for real modules and `BlobIATHook` for the blob engine, so only calls that go through that module's import table are intercepted.

SvEngine additionally installs an engine `SteamAPI_Shutdown` IAT hook and a
gamedata-resolved `GL_Shutdown(HWND, HDC, HGLRC)` cdecl inline hook. The former
suppresses the engine call; the latter calls the saved Steam API original once
before forwarding all three arguments. MetaHook fills saved original pointers
at hook transaction commit, so they must not be checked for null immediately
after enqueueing the hook. Both handles and per-engine state are cleared during
engine unload. Other engine families retain their existing shutdown behavior.

The `FreeLibrary` hooks are how the plugin catches the unload of modules it does not own: `NewFreeLibrary_Engine` waits for the **`server.dll`** manager when the module being freed is the tracked one, and `NewFreeLibrary_GameUI` does the same for the **`ServerBrowser.dll`** manager. Both then forward to the real `FreeLibrary`.

### 3. Thread managers (`src/ThreadManager.cpp`)

The registry is a `std::vector<IThreadManager*>` behind `g_ThreadManagerLock`; `FindThreadManagerByVirtualAddress` takes that lock while it scans. Each `CThreadManager` owns one module (or blob module), its `.text` range, a fixed `HANDLE m_hAliveThread[MAXIMUM_WAIT_OBJECTS]` pool and the three hook handles.

The three wrappers are installed into `kernel32.dll` import slots:

- `NewCreateThread` calls the real `CreateThread` **first**, then attributes the call with `FindThreadManagerByVirtualAddress(_ReturnAddress())` — the `.text` range decides ownership. The returned handle is duplicated (`DuplicateHandle(..., THREAD_ALL_ACCESS, FALSE, DUPLICATE_SAME_ACCESS)`) and the duplicate is handed to `OnCreateThread`, so the manager owns a handle whose lifetime it controls; the **original** handle is returned to the caller unchanged. An unused `originalCreationFlags` local remains in the function
- `NewWaitForSingleObject` only intercepts `dwMilliseconds == 0` (the polling shape). While the manager is terminating, `OnWaitForSingleObject` returns true and the wrapper reports `WAIT_OBJECT_0` immediately instead of polling; every other timeout forwards to the real call
- `NewSleep` only intercepts `dwMilliseconds == 1`. While terminating, `OnSleep` returns true for any thread except `g_MainThreadId`, and the wrapper calls `ExitThread(0)`

Both "while terminating" behaviours are gated on `m_bStartTermination`, a plain `bool` set by `StartTermination` (no atomics; the flag is one-way).

`OnCreateThread` takes `m_ThreadListLock`, tries `AddAliveThread` for a free slot, and on a full pool calls `FindAndRemoveSignaledAliveThread` (a `WaitForSingleObject(handle, 0)` scan) to reclaim a slot whose thread has already exited. If that also fails it calls `g_pMetaHookAPI->SysError("Failed to insert thread to thread manager!")` — a full pool fails loudly rather than dropping a thread.

`WaitForAliveThreadsToShutdown` snapshots the alive handles under `m_ThreadListLock`, then calls `WaitForMultipleObjects(numThreads, hThreads, TRUE, INFINITE)`, clears `m_hAliveThread` (outside the lock) and `CloseHandle`s each snapshot handle. A `#if 0` block that used to move them into `m_hClosedThread` is disabled.

### 4. Termination gates

Actual waiting only happens when the engine reports `DLL_CLOSE` or `DLL_RESTART`:

- `GetEngineDLLState()` returns `(*eng)->GetState()` and `DLL_INACTIVE` when the slot is null — the slot is dereferenced exactly once
- `Engine_WaitForShutdown` (from `ExitGame`) applies that gate to the engine manager; its `hModule` / `hBlobModule` parameters are unused in the body
- `ServerDLL_WaitForShutdown` and `ServerBrowser_WaitForShutdown` apply the same gate and are triggered by the `FreeLibrary` hooks above

Each path calls `StartTermination()` and then `WaitForAliveThreadsToShutdown()` on its own manager.

### 5. The `_restart` fix (`src/exportfuncs.cpp`)

`EngineCommand_InstallHook` (from `LoadClient`) looks up `shutdownserver` with `FindCmd`, stores its function pointer, and installs `Host_Quit_Restart_f` over `_restart` with `HookCmd`. The replacement runs `shutdownserver` and then the original `_restart` function. A missing `shutdownserver` command is fatal: `Command "shutdownserver" not found!`.

### 6. Disabled-by-design code

- The `#if 0` closed-thread path is intentionally disabled in both `UnistallHook` and `WaitForAliveThreadsToShutdown`: `m_hClosedThread`, `FindClosedThread` / `AddClosedThread` / `FindAndRemoveSignaledClosedThread`, `WaitForClosedThreadsToShutdown` and the related `DllMain` / `CloseHandle` / `TerminateThread` hook members. Only the alive-thread path is maintained
- `IThreadManager::UnistallHook` and `GameUI_UnistallHook` carry a misspelling ("Unistall") that the other module-level functions (`Engine_UninstallHook`, `ServerDLL_UninstallHook`, `ServerBrowser_UninstallHook`) do not — keep the names as-is
- The `Search_Pattern*` macros in `src/plugins.h` are retained but no scan path remains

## Key Code Flow

```
LoadEngine: register DllLoadNotification, resolve `eng` from gamedata, record g_MainThreadId
    ↓
DllLoadNotification (engine load)   → Engine_InstallHook   → manager + CreateThread/Wait/Sleep hooks
DllLoadNotification (module load)   → GameUI_ / ServerBrowser_ / ServerDLL_InstallHook
    ↓
Module calls CreateThread through its import table
    ↓
NewCreateThread → real CreateThread → attribute by _ReturnAddress → DuplicateHandle → OnCreateThread
    ↓
Module polls with WaitForSingleObject(h, 0) / Sleep(1) through the same table
    ↓
NewWaitForSingleObject / NewSleep → manager gate → WAIT_OBJECT_0 / ExitThread(0) while terminating
    ↓
Shutdown path (ExitGame, or a hooked FreeLibrary)
    ↓
gate on DLL_CLOSE / DLL_RESTART → StartTermination → WaitForAliveThreadsToShutdown
    ↓
WaitForMultipleObjects(..., TRUE, INFINITE) → clear the pool → CloseHandle each

LoadClient → EngineCommand_InstallHook → HookCmd("_restart", ...)
    ↓
`_restart` → Host_Quit_Restart_f → shutdownserver → original `_restart`
```

## Build Instructions

Requirements: Windows, Visual Studio 2022, CMake 3.21 or newer, Git, Python 3.8 or newer, MSVC x86 (`-A Win32`), static CRT (`MultiThreaded`), `_MBCS`, `NO_MALLOC_OVERRIDE` and VC-LTL 5.3.1. Neither `CMAKE_CXX_STANDARD` nor a `cxx_std_*` compile feature is set, so the MSVC default language level applies.

```bat
scripts\build-ThreadGuard-x86-Release.bat
scripts\build-ThreadGuard-x86-Debug.bat
```

The scripts configure, build and install. Debug compiles at `/W0`, Release at `/W3`; both suppress `/wd4311 /wd4312 /wd4819 /wd4996` and pass `/permissive`. Release also enables interprocedural optimization and `/OPT:REF /OPT:ICF`; the DLL links with `/SUBSYSTEM:WINDOWS`. Output stays in `build/x86/<configuration>/`; the DLL, its PDB and the gamedata catalog are installed to `install/x86/<configuration>/svencoop/metahook/`. Nothing is deployed to the game automatically.

### Dependencies

- **MetaHook SDK**: fetched automatically at a pinned commit; pass `-DMETAHOOK_SOURCE_PATH=D:\MetaHook` or export the same environment variable to build against a local tree. The path is the repository root providing `include/metahook.h`, `include/HLSDK` and `include/Interface`
- **API surface used**: `ResolveGameSymbol` / `GetGameSymbolStatusString` (through `GamedataResolvePtr`), `RegisterLoadDllNotificationCallback` / `UnregisterLoadDllNotificationCallback`, `IATHook` / `BlobIATHook` / `UnHook`, `HookCmd` / `FindCmd`, `GetModuleBase` / `GetBlobModuleImageBase` / `GetSectionByName` / `GetEngineModule` / `GetBlobEngineModule`, `GetGameDirectory`, `ModuleHasImport` / `ModuleHasImportEx`, `SysError`; from the engine interface, `IEngine::GetState()`
- **VC-LTL 5.3.1**: downloaded once into `thirdparty/cache`
- **Nothing else**: no third-party library is linked and no Capstone headers are needed

Keep `cmake/Sources.cmake` as the explicit compile list (4 plugin units); `include/HLSDK/common/interface.cpp` is compiled in because `EXPOSE_SINGLE_INTERFACE` (which exports `CreateInterface`) lives there.

### gamedata

`scripts/manifests/threadguard.json` declares the `engine` / **`eng`** global — the catalog symbol name is `eng`, not `engine` — across 11 engine snapshots (`cof-5936`, `hl-10210`, `hl-3248`, `hl-3266`, `hl-3329`, `hl-3647`, `hl-4554`, `hl-6153`, `hl-8684`, `svencoop-10257`, `svencoop-8948`). A conditional group additionally requires the `GL_Shutdown` function for both Sven snapshots. There are no patch records.

`scripts/manifests/threadguard.json` → `scripts/sync-gamedata.py` → pruned catalog under `build/x86/<Configuration>/assets/svencoop/metahook/gamedata/threadguard`, validated by `scripts/validate-gamedata.py` before the plugin target builds. Disable with `-DTHREADGUARD_SYNC_GAMEDATA=OFF`. When gamedata usage changes, update the manifest in the same change.

### Optional F5 debugging

```powershell
cmake -S . -B build/launch -G "Visual Studio 17 2022" -A Win32 -DMETAHOOKSV_ENABLE_LAUNCH_GAME=ON
```

Select **LaunchGame** and press F5; **DeployGame** builds, stages and copies the plugin DLL/PDB/resources into an existing MetaHook installation before the debugger attaches. The feature defaults OFF. See `README.md` for `METAHOOKSV_GAME_*` options.

### CI

`.github/workflows/livebuild.yml` and `release.yml` build and package the plugin. There is no test suite to run.

## Engine Compatibility

`eng` must exist in the gamedata catalog for the running engine build:

| Game build | Support |
| --- | --- |
| `hl-3248`, `hl-3266`, `hl-3329`, `hl-3647`, `hl-4554` | ✅ |
| `hl-6153` | ✅ |
| `hl-8684`, `hl-10210` | ✅ |
| `svencoop-8948`, `svencoop-10257` | ✅ |
| `cof-5936` (Cry of Fear) | ✅ |
| Any build absent from the catalog | ❌ fatal at `LoadEngine` |

Catalog coverage is not a correctness statement. Two further Sven Co-op specific gates are applied at runtime: the engine `FreeLibrary` hook and `server.dll` tracking are both limited to a `svencoop` game directory, and `GameUI.dll` / `ServerBrowser.dll` are skipped when they use the Steam callback model.

## Important Constants, Macros and Types

```cpp
static_assert(METAHOOK_API_VERSION >= 109, ...);  // in src/privatehook.cpp: ResolveGameSymbol
#define MHPluginName "ThreadGuard"
#define Sys_Error(msg, ...) g_pMetaHookAPI->SysError("[" MHPluginName "] " msg, __VA_ARGS__);

// src/ThreadManager.h — per-manager hook selection
hookflag_CreateThread = 1, hookflag_WaitForSingleObject = 2, hookflag_Sleep = 4

// src/enginedef.h — IEngine::GetState() values; only DLL_CLOSE / DLL_RESTART wait
DLL_INACTIVE 0, DLL_ACTIVE 1, DLL_PAUSED 2, DLL_CLOSE 3, DLL_TRANS 4, DLL_RESTART 5

// The only intercepted call shapes
WaitForSingleObject(hObject, 0)   // polling loops
Sleep(1)                          // busy waits

// Fixed-size pools, sized by the Win32 constant (64)
HANDLE m_hAliveThread[MAXIMUM_WAIT_OBJECTS];   // live threads, duplicated handles
HANDLE m_hClosedThread[MAXIMUM_WAIT_OBJECTS];  // #if 0, not maintained

// Globals
IEngine** eng;                    // gamedata GLOBAL, dereferenced exactly once
HANDLE g_MainThreadId;            // GetCurrentThreadId() at LoadEngine; never ExitThread'd
```

Runtime configuration: `ThreadGuard.dll` must be listed in the host's `metahook/configs/plugins.lst`, and `metahook/gamedata/threadguard` must stay next to it.

## Debugging Tips

1. **Console output**: `Sys_Error` reports a gamedata miss (`Could not resolve gamedata symbol: eng (module engine, <status>)` plus the engine buildnum — no CRC64), a full thread pool (`Failed to insert thread to thread manager!`), a missing `shutdownserver` command, or a too-low launcher API version
2. **Breakpoint locations**: `DllLoadNotification()` (hook routing), `NewCreateThread()` (attribution via `_ReturnAddress`, handle duplication), `NewWaitForSingleObject()` / `NewSleep()` (the two short-circuits), `WaitForAliveThreadsToShutdown()` (the actual wait), `Host_Quit_Restart_f()` (the `_restart` fix)
3. **Attribution is by return address**: a breakpoint in `FindThreadManagerByVirtualAddress` that misses means the thread was created from outside the managed module's `.text`, so the thread is not tracked at all
4. **Nothing waits unless the engine state gate passes** — check `GetEngineDLLState()` before concluding that termination handling is broken

## Repository Rules

- Preserve the MetaHook API, plugin exports and calling conventions. Match the naming, indentation and comment style of the files you touch
- Resolve engine symbols only through the host gamedata contract. Do not add a signature-scan fallback for `eng`; keep the catalog name `eng` (not `engine`) and the fatal `Could not resolve gamedata symbol: ...` diagnostic
- Keep the wrapper semantics explicit: `NewCreateThread` must still duplicate the handle before handing it to a manager and must return the original handle, and the `Sleep(1)` / `WaitForSingleObject(0)` short-circuits must stay gated on `StartTermination` and, for `Sleep`, on the non-main-thread check
- Keep the termination gate (`DLL_CLOSE` / `DLL_RESTART` via `(*eng)->GetState()`) and the per-module hook flags intact; the `svencoop` game-directory gates for `server.dll` and the engine `FreeLibrary` hook are deliberate
- The fixed `MAXIMUM_WAIT_OBJECTS` handle pool and its "already signaled slot" reclamation are part of the design; a full pool must keep failing loudly rather than silently dropping a thread
- The `#if 0` closed-thread path is intentionally disabled. Do not re-enable it as part of an unrelated change
- When gamedata usage changes, update `scripts/manifests/threadguard.json` in the same change
- Do not modify external sources or third-party sources; MetaHook is a read-only build input
- MSVC x86 only. Keep the static CRT / VC-LTL and warning-level settings in `CMakeLists.txt` in sync with the other standalone plugin repositories
- `README.md` and `README.zh-CN.md` are a pair: keep the managed-module table and build options consistent in both
- Verification distinguishes build checks from a real game run: there is no test suite here, and the hook routing, the shutdown wait and the `_restart` fix can only be observed in a live game. Claims about in-game behavior must not be made without evidence. Documentation changes need content, path and format checks, not a plugin rebuild

## Related Links

- **MetaHookSV**: https://github.com/hzqst/MetaHookSv
- **Gamedata symbol catalog**: https://hlnd2t.github.io/GoldSrc_VibeSignatures/
