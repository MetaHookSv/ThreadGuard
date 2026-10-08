#include <metahook.h>
#include "plugins.h"
#include "privatehook.h"
#include "enginedef.h"
#include "ThreadManager.h"

#include <IEngine.h>

static_assert(METAHOOK_API_VERSION >= 109, "ThreadGuard resolves the engine IEngine* slot from gamedata and requires MetaHook API 109 (ResolveGameSymbol)");

hook_t* g_pHook_FreeLibrary_Engine = NULL;
hook_t* g_pHook_FreeLibrary_GameUI = NULL;

static hook_t* g_pHook_SteamAPI_Shutdown                  = NULL;
static hook_t* g_pHook_GL_Shutdown                        = NULL;
static void(__cdecl* g_pfn_SteamAPI_Shutdown)(void)       = NULL;
static void(__cdecl* g_pfn_GL_Shutdown)(HWND, HDC, HGLRC) = NULL;
static bool g_bSteamShutdown                              = false;

static void __cdecl NewSteamAPI_Shutdown(void)
{
    // SvEngine calls this too late on quit and skips it entirely on _restart.
    // The GL_Shutdown hook owns the call for both paths.
}

static void __cdecl NewGL_Shutdown(HWND window, HDC dc, HGLRC context)
{
    if (!g_bSteamShutdown)
    {
        g_bSteamShutdown = true;
        g_pfn_SteamAPI_Shutdown();
    }

    g_pfn_GL_Shutdown(window, dc, context);
}

IThreadManager* g_ThreadManager_Engine        = NULL;
IThreadManager* g_ThreadManager_GameUI        = NULL;
IThreadManager* g_ThreadManager_ServerBrowser = NULL;
IThreadManager* g_ThreadManager_ServerDLL     = NULL;

IEngine** eng = NULL;

void ServerDLL_WaitForShutdown(HMODULE hModule);
void GameUI_WaitForShutdown(HMODULE hModule);
void ServerBrowser_WaitForShutdown(HMODULE hModule);

int GetEngineDLLState()
{
    if (eng)
        return (*eng)->GetState();

    return DLL_INACTIVE;
}

BOOL WINAPI NewFreeLibrary_Engine(HMODULE hModule)
{
    if (g_ThreadManager_GameUI && g_ThreadManager_GameUI->GetModule() == hModule)
    {
        GameUI_WaitForShutdown(hModule);
    }
    if (g_ThreadManager_ServerDLL && g_ThreadManager_ServerDLL->GetModule() == hModule)
    {
        ServerDLL_WaitForShutdown(hModule);
    }

    return FreeLibrary(hModule);
}

BOOL WINAPI NewFreeLibrary_GameUI(HMODULE hModule)
{
    if (g_ThreadManager_ServerBrowser && g_ThreadManager_ServerBrowser->GetModule() == hModule)
    {
        ServerBrowser_WaitForShutdown(hModule);
    }

    return FreeLibrary(hModule);
}

void Engine_WaitForShutdown(HMODULE hModule, BlobHandle_t hBlobModule)
{
    // ExitGame runs after EngineAPI::Run returns and before the engine's CRT
    // detach. CEngine::Unload may already have reset GetState() to DLL_INACTIVE;
    // the lifecycle callback itself requires joining every remaining worker.
    if (g_ThreadManager_Engine)
    {
        g_ThreadManager_Engine->StartTermination();
        g_ThreadManager_Engine->WaitForAliveThreadsToShutdown();
    }
}

void Engine_FillAddress(void)
{
    // gamedata provides the address of the engine module's global IEngine* slot,
    // so GetEngineDLLState keeps dereferencing it exactly once.
    eng = (decltype(eng))GamedataResolvePtr(g_EngineDLLInfo.ImageBase, "engine", "eng", MH_GAMESYMBOL_KIND_GLOBAL);
    // Match the actual CreateThread entry, including engines where NET_StartThread
    // is inlined (HL25) or shared as a tail block (old HL/BLOB).
    auto networkEntry = (LPTHREAD_START_ROUTINE)GamedataResolvePtr(
        g_EngineDLLInfo.ImageBase, "engine", "NET_ThreadFunc", MH_GAMESYMBOL_KIND_FUNCTION);
    auto networkId = (DWORD*)GamedataResolvePtr(
        g_EngineDLLInfo.ImageBase, "engine", "dwNetThreadId", MH_GAMESYMBOL_KIND_GLOBAL);
    auto queuePacket = GamedataResolvePtr(
        g_EngineDLLInfo.ImageBase, "engine", "NET_QueuePacket", MH_GAMESYMBOL_KIND_FUNCTION);
    auto shutdown = GamedataResolvePtr(
        g_EngineDLLInfo.ImageBase, "engine", "NET_Shutdown", MH_GAMESYMBOL_KIND_FUNCTION);
    NetworkThread_Configure(networkEntry, networkId, queuePacket, shutdown);
    if (g_iEngineType == ENGINE_SVENGINE)
    {
        g_pfn_GL_Shutdown = (decltype(g_pfn_GL_Shutdown))GamedataResolvePtr(
            g_EngineDLLInfo.ImageBase, "engine", "GL_Shutdown", MH_GAMESYMBOL_KIND_FUNCTION);
    }
}

void Engine_InstallHook(HMODULE hModule, BlobHandle_t hBlobModule)
{
    if (hModule)
    {
        if (g_iEngineType == ENGINE_SVENGINE)
        {
            g_bSteamShutdown          = false;
            g_pHook_SteamAPI_Shutdown = g_pMetaHookAPI->IATHook(hModule,
                                                                "steam_api.dll", "SteamAPI_Shutdown", NewSteamAPI_Shutdown, (void**)&g_pfn_SteamAPI_Shutdown);
            // During LoadEngine, MetaHook fills the original pointer at transaction commit.
            if (!g_pHook_SteamAPI_Shutdown)
            {
                Sys_Error("Could not hook engine SteamAPI_Shutdown import!");
                return;
            }
            g_pHook_GL_Shutdown = g_pMetaHookAPI->InlineHook((void*)g_pfn_GL_Shutdown,
                                                             NewGL_Shutdown, (void**)&g_pfn_GL_Shutdown);
            if (!g_pHook_GL_Shutdown)
            {
                g_pMetaHookAPI->UnHook(g_pHook_SteamAPI_Shutdown);
                g_pHook_SteamAPI_Shutdown = NULL;
                Sys_Error("Could not hook engine GL_Shutdown!");
                return;
            }
        }

        g_ThreadManager_Engine = CreateThreadManagerForModule(hModule);
        g_ThreadManager_Engine->InstallHook(hookflag_CreateThread | hookflag_WaitForSingleObject | hookflag_Sleep);

        g_pHook_FreeLibrary_Engine = g_pMetaHookAPI->IATHook(hModule, "kernel32.dll", "FreeLibrary", NewFreeLibrary_Engine, NULL);
    }
    else if (hBlobModule)
    {
        g_ThreadManager_Engine = CreateThreadManagerForBlob(hBlobModule);
        g_ThreadManager_Engine->InstallHook(hookflag_CreateThread | hookflag_WaitForSingleObject | hookflag_Sleep);
        g_pHook_FreeLibrary_Engine = g_pMetaHookAPI->BlobIATHook(hBlobModule, "kernel32.dll", "FreeLibrary", NewFreeLibrary_Engine, NULL);
    }
    if (g_ThreadManager_Engine)
    {
        if (!g_pHook_FreeLibrary_Engine)
            Sys_Error("Could not install engine FreeLibrary hook!");
        NetworkThread_InstallHook(g_ThreadManager_Engine);
    }
}

void Engine_UninstallHook(HMODULE hModule, BlobHandle_t hBlobModule)
{
    NetworkThread_UninstallHook();
    eng = NULL;
    if (g_pHook_GL_Shutdown)
    {
        g_pMetaHookAPI->UnHook(g_pHook_GL_Shutdown);
        g_pHook_GL_Shutdown = NULL;
    }
    if (g_pHook_SteamAPI_Shutdown)
    {
        g_pMetaHookAPI->UnHook(g_pHook_SteamAPI_Shutdown);
        g_pHook_SteamAPI_Shutdown = NULL;
    }
    g_pfn_GL_Shutdown       = NULL;
    g_pfn_SteamAPI_Shutdown = NULL;
    g_bSteamShutdown        = false;

    if (g_pHook_FreeLibrary_Engine)
    {
        g_pMetaHookAPI->UnHook(g_pHook_FreeLibrary_Engine);
        g_pHook_FreeLibrary_Engine = NULL;
    }

    if (g_ThreadManager_Engine)
    {
        g_ThreadManager_Engine->UnistallHook();
        DeleteThreadManager(g_ThreadManager_Engine);
        delete g_ThreadManager_Engine;
        g_ThreadManager_Engine = NULL;
    }
}

void GameUI_InstallHook(HMODULE hModule)
{
    //It use callback instead of creating stupid thread.
    if (g_pMetaHookAPI->ModuleHasImport(hModule, "steam_api.dll") && !g_pMetaHookAPI->ModuleHasImportEx(hModule, "kernel32.dll", "CreateThread"))
        return;

    g_ThreadManager_GameUI = CreateThreadManagerForModule(hModule);
    g_ThreadManager_GameUI->InstallHook(hookflag_CreateThread | hookflag_WaitForSingleObject | hookflag_Socket);

    g_pHook_FreeLibrary_GameUI = g_pMetaHookAPI->IATHook(hModule, "kernel32.dll", "FreeLibrary", NewFreeLibrary_GameUI, NULL);
}

void GameUI_WaitForShutdown(HMODULE hModule)
{
    // Join before FreeLibrary enters the loader lock and the socket destructor.
    if (g_ThreadManager_GameUI && (GetEngineDLLState() == DLL_CLOSE || GetEngineDLLState() == DLL_RESTART))
    {
        g_ThreadManager_GameUI->StartTermination();
        g_ThreadManager_GameUI->WaitForAliveThreadsToShutdown();
        // GameUI's console may already be shut down; keep teardown diagnostics
        // independent of engine/VGUI callbacks, even on the main thread.
        OutputDebugStringA("[ThreadGuard] GameUI: threads stopped before FreeLibrary\n");
    }
}

void GameUI_UnistallHook(HMODULE hModule)
{
    if (g_pHook_FreeLibrary_GameUI)
    {
        g_pMetaHookAPI->UnHook(g_pHook_FreeLibrary_GameUI);
        g_pHook_FreeLibrary_GameUI = NULL;
    }

    if (g_ThreadManager_GameUI)
    {
        g_ThreadManager_GameUI->UnistallHook();
        DeleteThreadManager(g_ThreadManager_GameUI);
        delete g_ThreadManager_GameUI;
        g_ThreadManager_GameUI = NULL;
    }
}

void ServerDLL_WaitForShutdown(HMODULE hModule)
{
    if (g_ThreadManager_ServerDLL && (GetEngineDLLState() == DLL_CLOSE || GetEngineDLLState() == DLL_RESTART))
    {
        g_ThreadManager_ServerDLL->StartTermination();
        g_ThreadManager_ServerDLL->WaitForAliveThreadsToShutdown();
    }
}

void ServerDLL_InstallHook(HMODULE hModule)
{
    //It use callback instead of creating stupid thread.
    if (0 != stricmp(g_pMetaHookAPI->GetGameDirectory(), "svencoop"))
        return;

    //Fuck off the CPlayerDatabase_RunThread
    g_ThreadManager_ServerDLL = CreateThreadManagerForModule(hModule);
    g_ThreadManager_ServerDLL->InstallHook(hookflag_CreateThread);
}

void ServerDLL_UninstallHook(HMODULE hModule)
{
    if (g_ThreadManager_ServerDLL)
    {
        g_ThreadManager_ServerDLL->UnistallHook();
        DeleteThreadManager(g_ThreadManager_ServerDLL);
        delete g_ThreadManager_ServerDLL;
        g_ThreadManager_ServerDLL = NULL;
    }
}

void ServerBrowser_WaitForShutdown(HMODULE hModule)
{
    if (g_ThreadManager_ServerBrowser && (GetEngineDLLState() == DLL_CLOSE || GetEngineDLLState() == DLL_RESTART))
    {
        g_ThreadManager_ServerBrowser->StartTermination();
        g_ThreadManager_ServerBrowser->WaitForAliveThreadsToShutdown();
    }
}

void ServerBrowser_InstallHook(HMODULE hModule)
{
    //It use callback instead of creating stupid thread.
    if (g_pMetaHookAPI->ModuleHasImport(hModule, "steam_api.dll") && !g_pMetaHookAPI->ModuleHasImportEx(hModule, "kernel32.dll", "CreateThread"))
        return;

    g_ThreadManager_ServerBrowser = CreateThreadManagerForModule(hModule);
    g_ThreadManager_ServerBrowser->InstallHook(hookflag_CreateThread | hookflag_WaitForSingleObject | hookflag_Socket);
}

void ServerBrowser_UninstallHook(HMODULE hModule)
{
    if (g_ThreadManager_ServerBrowser)
    {
        g_ThreadManager_ServerBrowser->UnistallHook();
        DeleteThreadManager(g_ThreadManager_ServerBrowser);
        delete g_ThreadManager_ServerBrowser;
        g_ThreadManager_ServerBrowser = NULL;
    }
}

void DllLoadNotification(mh_load_dll_notification_context_t* ctx)
{
    if (ctx->flags & LOAD_DLL_NOTIFICATION_IS_LOAD)
    {
        if (ctx->flags & LOAD_DLL_NOTIFICATION_IS_ENGINE)
        {
            Engine_InstallHook(ctx->hModule, ctx->hBlob);
        }
        else if (ctx->BaseDllName && ctx->hModule && !_wcsicmp(ctx->BaseDllName, L"GameUI.dll"))
        {
            GameUI_InstallHook(ctx->hModule);
        }
        else if (ctx->BaseDllName && ctx->hModule && !_wcsicmp(ctx->BaseDllName, L"ServerBrowser.dll"))
        {
            ServerBrowser_InstallHook(ctx->hModule);
        }
        else if (ctx->BaseDllName && ctx->hModule && !_wcsicmp(ctx->BaseDllName, L"server.dll"))
        {
            ServerDLL_InstallHook(ctx->hModule);
        }
    }
    else if (ctx->flags & LOAD_DLL_NOTIFICATION_IS_UNLOAD)
    {
        if (ctx->flags & LOAD_DLL_NOTIFICATION_IS_ENGINE)
        {
            Engine_UninstallHook(ctx->hModule, ctx->hBlob);
        }
        else if (g_ThreadManager_GameUI && ctx->hModule == g_ThreadManager_GameUI->GetModule())
        {
            GameUI_UnistallHook(ctx->hModule);
        }
        else if (g_ThreadManager_ServerBrowser && ctx->hModule == g_ThreadManager_ServerBrowser->GetModule())
        {
            ServerBrowser_UninstallHook(ctx->hModule);
        }
        else if (g_ThreadManager_ServerDLL && ctx->hModule == g_ThreadManager_ServerDLL->GetModule())
        {
            ServerDLL_UninstallHook(ctx->hModule);
        }
    }
}
