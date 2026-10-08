// Exercise the actual handlers with owned Win32 threads and a loop that either
// blocks in select or keeps finding packets. The host API provides only a fatal
// diagnostic stub. The unload routing test replaces FreeLibrary only to assert
// that joining precedes unloading. IAT installation and symbol resolution still
// require a game run.
#include "../src/ThreadManager.cpp"
#include <cstdlib>
#include <cstdarg>

static BOOL WINAPI TestFreeLibrary(HMODULE module);
#define FreeLibrary TestFreeLibrary
#include "../src/privatehook.cpp"
#undef FreeLibrary

metahook_api_t* g_pMetaHookAPI     = NULL;
HANDLE          g_MainThreadId     = NULL;
int             g_iEngineType      = 0;
DWORD           g_dwEngineBuildnum = 0;
mh_dll_info_t   g_EngineDLLInfo    = {};

#define CHECK(condition)                                                      \
    do                                                                        \
    {                                                                         \
        if (!(condition))                                                     \
        {                                                                     \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
            std::abort();                                                     \
        }                                                                     \
    } while (0)

static void Fatal(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    std::abort();
}

class TestManager : public IThreadManager
{
public:
    std::vector<HANDLE> handles;
    std::atomic<bool>   stopping{false};
    HMODULE             module = NULL;
    unsigned            joined = 0;
    void                StartTermination() override { stopping = true; }
    void                WaitForAliveThreadsToShutdown() override
    {
        CHECK(stopping);
        ++joined;
    }
    void         OnCreateThread(HANDLE thread) override { handles.push_back(thread); }
    bool         OnWaitForSingleObject(HANDLE, DWORD) override { return stopping; }
    bool         OnSleep(DWORD) override { return stopping && (HANDLE)GetCurrentThreadId() != g_MainThreadId; }
    void         InstallHook(int) override {}
    void         UnistallHook() override {}
    PVOID        GetImageBase() const override { return NULL; }
    ULONG        GetImageSize() const override { return 0; }
    PVOID        GetTextBase() const override { return NULL; }
    ULONG        GetTextSize() const override { return MAXDWORD; }
    HMODULE      GetModule() const override { return module; }
    BlobHandle_t GetBlobModule() const override { return NULL; }
    void         CloseDuplicates()
    {
        for (HANDLE h : handles)
        {
            CHECK(WAIT_OBJECT_0 == WaitForSingleObject(h, 0));
            CloseHandle(h);
        }
        handles.clear();
    }
};

class TestEngine : public IEngine
{
public:
    int    state = DLL_ACTIVE;
    bool   Load(bool, char*, char*) override { return true; }
    void   Unload() override {}
    void   SetState(int value) override { state = value; }
    int    GetState() override { return state; }
    void   SetSubState(int) override {}
    int    GetSubState() override { return 0; }
    int    Frame() override { return 0; }
    double GetFrameTime() override { return 0; }
    double GetCurTime() override { return 0; }
    void   TrapKey_Event(int, bool) override {}
    void   TrapMouse_Event(int, bool) override {}
    void   StartTrapMode() override {}
    bool   IsTrapping() override { return false; }
    bool   CheckDoneTrapping(int&, int&) override { return false; }
    int    GetQuitting() override { return 0; }
    void   SetQuitting(int) override {}
};

static TestManager* unloadManager;
static HMODULE      expectedModule;
static unsigned     expectedJoins;
static unsigned     unloadCalls;
static BOOL WINAPI  TestFreeLibrary(HMODULE module)
{
    CHECK(expectedModule == module);
    CHECK(expectedJoins == unloadManager->joined);
    ++unloadCalls;
    return TRUE;
}

static void CheckUnloadRouting()
{
    TestEngine engine;
    IEngine*   enginePointer = &engine;
    eng                      = &enginePointer;
    TestManager manager;
    manager.module         = (HMODULE)0x1234;
    unloadManager          = &manager;
    g_ThreadManager_GameUI = &manager;
    for (int state : {DLL_ACTIVE, DLL_CLOSE, DLL_RESTART})
    {
        engine.state     = state;
        manager.stopping = false;
        expectedModule   = (HMODULE)0x5678;
        expectedJoins    = manager.joined;
        CHECK(NewFreeLibrary_Engine(expectedModule));
        CHECK(!manager.stopping);
        expectedModule = manager.module;
        if (state != DLL_ACTIVE) ++expectedJoins;
        CHECK(NewFreeLibrary_Engine(expectedModule));
        CHECK((state != DLL_ACTIVE) == manager.stopping);
    }
    g_ThreadManager_GameUI        = NULL;
    g_ThreadManager_ServerBrowser = &manager;
    ++expectedJoins;
    CHECK(NewFreeLibrary_GameUI(manager.module));
    g_ThreadManager_ServerBrowser = NULL;
    CHECK(7 == unloadCalls);
    eng = NULL;
}

struct EngineExitContext
{
    IThreadManager* manager;
    HANDLE          started;
    HANDLE          cleaned;
};

static DWORD WINAPI EngineExitWorker(void* argument)
{
    auto context = static_cast<EngineExitContext*>(argument);
    SetEvent(context->started);
    while (!context->manager->OnSleep(1)) Sleep(1);
    // Resource cleanup must finish before the engine lifecycle callback returns.
    SetEvent(context->cleaned);
    return 0;
}

static void CheckEngineExit()
{
    TestEngine engine;
    IEngine*   enginePointer = &engine;
    for (int state : {DLL_INACTIVE, DLL_CLOSE, DLL_RESTART, DLL_ACTIVE, -1})
    {
        engine.state = state;
        eng          = state == -1 ? NULL : &enginePointer;
        CThreadManager manager(NULL, NULL);
        g_ThreadManager_Engine    = &manager;
        EngineExitContext context = {&manager, CreateEvent(NULL, TRUE, FALSE, NULL),
                                     CreateEvent(NULL, TRUE, FALSE, NULL)};
        CHECK(context.started && context.cleaned);
        HANDLE worker = CreateThread(NULL, 0, EngineExitWorker, &context, 0, NULL);
        CHECK(worker != NULL);
        HANDLE duplicate = NULL;
        CHECK(DuplicateHandle(GetCurrentProcess(), worker, GetCurrentProcess(), &duplicate,
                              0, FALSE, DUPLICATE_SAME_ACCESS));
        static_cast<IThreadManager&>(manager).OnCreateThread(duplicate);
        CHECK(WAIT_OBJECT_0 == WaitForSingleObject(context.started, 5000));
        Engine_WaitForShutdown(NULL, NULL);
        CHECK(WAIT_OBJECT_0 == WaitForSingleObject(context.cleaned, 0));
        CHECK(WAIT_OBJECT_0 == WaitForSingleObject(worker, 0));
        Engine_WaitForShutdown(NULL, NULL);
        CloseHandle(worker);
        CloseHandle(context.started);
        CloseHandle(context.cleaned);
    }
    eng                    = NULL;
    g_ThreadManager_Engine = NULL;
    Engine_WaitForShutdown(NULL, NULL);
}

static DWORD                 engineThreadId;
static SOCKET                socketHandle   = INVALID_SOCKET;
static HANDLE                shutdownWorker = NULL;
static unsigned              shutdownCalls  = 0;
static HANDLE                entered;
static CRITICAL_SECTION      networkLock;
static std::atomic<bool>     flood{false};
static std::atomic<unsigned> packets{0};
static int __cdecl           Packet(int)
{
    ++packets;
    return flood.load() ? 1 : 0;
}

static void OpenSocket()
{
    socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(INVALID_SOCKET != socketHandle);
    sockaddr_in address     = {};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(0 == bind(socketHandle, (sockaddr*)&address, sizeof(address)));
}

static void __cdecl ShutdownResources()
{
    // Match the binary's ordering: resource cleanup precedes its late stop.
    // The hook must have joined before ANY original cleanup starts.
    if (shutdownWorker)
        CHECK(WAIT_OBJECT_0 == WaitForSingleObject(shutdownWorker, 0));
    CHECK(TryEnterCriticalSection(&networkLock));
    if (socketHandle != INVALID_SOCKET)
    {
        CHECK(0 == closesocket(socketHandle));
        socketHandle = INVALID_SOCKET;
    }
    LeaveCriticalSection(&networkLock);
    if (shutdownWorker)
        CHECK(NewTerminateThread(shutdownWorker, 0));
    ++shutdownCalls;
}

static DWORD WINAPI Worker(void*)
{
    SetEvent(entered);
    for (;;)
    {
        fd_set read;
        FD_ZERO(&read);
        FD_SET(socketHandle, &read);
        if (!flood.load()) NewNetworkSelect(0, &read, NULL, NULL, NULL);
        bool more;
        do
        {
            EnterCriticalSection(&networkLock);
            more = NewNET_QueuePacket(0) != 0;
            LeaveCriticalSection(&networkLock);
        } while (more);
        NewSleep(1);
    }
}

static DWORD WINAPI OtherWorker(void*)
{
    Sleep(INFINITE);
    return 0;
}

struct SocketContext
{
    SOCKET           socket;
    sockaddr_in      address;
    HANDLE           started;
    HANDLE           stop;
    CRITICAL_SECTION lock;
    bool             flood;
};

static DWORD WINAPI SocketWorker(void* argument)
{
    auto context = static_cast<SocketContext*>(argument);
    while (WAIT_OBJECT_0 != NewWaitForSingleObject(context->stop, 0))
    {
        fd_set read;
        FD_ZERO(&read);
        FD_SET(context->socket, &read);
        if (!context->flood) SetEvent(context->started);
        if (NewSocketSelect(0, &read, NULL, NULL, NULL) > 0)
        {
            EnterCriticalSection(&context->lock);
            char packet;
            while (NewSocketRecvFrom(context->socket, &packet, 1, 0, NULL, NULL) == 1)
            {
                // Always leave another datagram queued: draining must observe stop.
                CHECK(1 == sendto(context->socket, &packet, 1, 0, (sockaddr*)&context->address, sizeof(context->address)));
                SetEvent(context->started);
            }
            CHECK(WSAEWOULDBLOCK == WSAGetLastError());
            LeaveCriticalSection(&context->lock);
        }
    }
    return 0;
}

static void CheckSocketShutdown(TestManager& manager)
{
    for (bool flood : {false, true})
    {
        SocketContext context = {};
        context.flood         = flood;
        context.socket        = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        CHECK(INVALID_SOCKET != context.socket);
        context.address.sin_family      = AF_INET;
        context.address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(0 == bind(context.socket, (sockaddr*)&context.address, sizeof(context.address)));
        int size = sizeof(context.address);
        CHECK(0 == getsockname(context.socket, (sockaddr*)&context.address, &size));
        u_long nonblocking = 1;
        CHECK(0 == ioctlsocket(context.socket, FIONBIO, &nonblocking));
        context.started = CreateEvent(NULL, TRUE, FALSE, NULL);
        context.stop    = CreateEvent(NULL, TRUE, FALSE, NULL);
        InitializeCriticalSection(&context.lock);
        if (flood) CHECK(1 == sendto(context.socket, "x", 1, 0, (sockaddr*)&context.address, size));
        manager.stopping = false;
        HANDLE worker    = CreateThread(NULL, 0, SocketWorker, &context, 0, NULL);
        CHECK(WAIT_OBJECT_0 == WaitForSingleObject(context.started, 5000));
        manager.StartTermination();
        CHECK(WAIT_OBJECT_0 == WaitForSingleObject(worker, 2000));
        CHECK(TryEnterCriticalSection(&context.lock));
        LeaveCriticalSection(&context.lock);
        // The main thread's socket calls still forward even during termination.
        if (flood)
        {
            char packet;
            CHECK(1 == NewSocketRecvFrom(context.socket, &packet, 1, 0, NULL, NULL));
        }
        CloseHandle(worker);
        CloseHandle(context.started);
        CloseHandle(context.stop);
        DeleteCriticalSection(&context.lock);
        closesocket(context.socket);
    }
    manager.stopping = false;
}
static DWORD WINAPI FinishedWorker(void*) { return 0; }
struct SpawnContext
{
    IThreadManager* manager;
    HANDLE          childFinished;
};
static DWORD WINAPI ChildWorker(void* event)
{
    Sleep(100);
    SetEvent((HANDLE)event);
    return 0;
}
static DWORD WINAPI SpawningWorker(void* context)
{
    auto spawn = (SpawnContext*)context;
    Sleep(30);
    HANDLE child = CreateThread(NULL, 0, ChildWorker, spawn->childFinished, 0, NULL);
    CHECK(child != NULL);
    HANDLE duplicate = NULL;
    CHECK(DuplicateHandle(GetCurrentProcess(), child, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS));
    spawn->manager->OnCreateThread(duplicate);
    CloseHandle(child);
    return 0;
}

int main()
{
    metahook_api_t api = {};
    api.SysError       = Fatal;
    g_pMetaHookAPI     = &api;
    g_MainThreadId     = (HANDLE)GetCurrentThreadId();
    CheckUnloadRouting();
    CheckEngineExit();
    TestManager manager;
    AddThreadManager(&manager);
    g_NetworkManager = &manager;
    NetworkThread_Configure(Worker, &engineThreadId, (void*)Packet, (void*)ShutdownResources);
    g_pfn_select = select;
    WSADATA data;
    CHECK(0 == WSAStartup(MAKEWORD(2, 2), &data));
    CheckSocketShutdown(manager);
    entered = CreateEvent(NULL, TRUE, FALSE, NULL);
    InitializeCriticalSection(&networkLock);
    // Networking disabled: still forward resource cleanup on each invocation.
    NewNET_Shutdown();
    NewNET_Shutdown();
    CHECK(2 == shutdownCalls);
    OpenSocket();
    DWORD handlesBefore = 0, handlesAfter = 0;
    NetworkTrace("handler test initialized", GetCurrentThreadId());
    GetProcessHandleCount(GetCurrentProcess(), &handlesBefore);
    CHECK(NULL == NewCreateThread(NULL, SIZE_T(-1), Worker, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, &engineThreadId));
    for (unsigned iteration = 0; iteration < 100; ++iteration)
    {
        if (socketHandle == INVALID_SOCKET) OpenSocket();
        flood.store((iteration & 1) != 0);
        ResetEvent(entered);
        HANDLE worker = NewCreateThread(NULL, 0, Worker, NULL,
                                        iteration == 0 ? CREATE_SUSPENDED : 0, &engineThreadId);
        CHECK(worker != NULL);
        if (iteration == 0)
        {
            CHECK(WAIT_TIMEOUT == WaitForSingleObject(entered, 20));
            CHECK(1 == ResumeThread(worker));
        }
        CHECK(WAIT_OBJECT_0 == WaitForSingleObject(entered, 5000));
        Sleep(5);
        CHECK(WAIT_TIMEOUT == WaitForSingleObject(worker, 0));
        const ULONGLONG start = GetTickCount64();
        if (iteration == 99)
        {
            CThreadManager shutdownManager(NULL, NULL);
            g_NetworkManager = &shutdownManager;
            static_cast<IThreadManager&>(shutdownManager).StartTermination();
            CHECK(WAIT_OBJECT_0 == WaitForSingleObject(worker, 2000));
            g_NetworkManager = &manager;
        }
        else if (iteration % 3 == 0)
        {
            shutdownWorker        = worker;
            const unsigned before = shutdownCalls;
            NewNET_Shutdown();
            NewNET_Shutdown();
            CHECK(before + 2 == shutdownCalls);
            shutdownWorker = NULL;
        }
        else
            CHECK(NewTerminateThread(worker, 0));
        CHECK(GetTickCount64() - start < 2000);
        CHECK(WAIT_OBJECT_0 == WaitForSingleObject(worker, 0));
        CHECK(!g_NetworkThread.IsThread(engineThreadId, engineThreadId));
        CHECK(!g_NetworkThread.ShouldExit(engineThreadId, engineThreadId));
        CHECK(NewTerminateThread(worker, 0));
        CHECK(TryEnterCriticalSection(&networkLock));
        LeaveCriticalSection(&networkLock);
        CHECK(g_NetworkThread.Reset());
        CloseHandle(worker);
        manager.CloseDuplicates();
    }
    // Match the handle-count baseline even if the last cycle closed its socket.
    if (socketHandle == INVALID_SOCKET) OpenSocket();
    // Non-network TerminateThread calls still reach Win32 with their exit code.
    DWORD  otherId;
    HANDLE other = NewCreateThread(NULL, 0, OtherWorker, NULL, 0, &otherId);
    CHECK(NewTerminateThread(other, 42));
    CHECK(WAIT_OBJECT_0 == WaitForSingleObject(other, 5000));
    DWORD code = 0;
    CHECK(GetExitCodeThread(other, &code) && code == 42);
    CloseHandle(other);
    manager.CloseDuplicates();
    GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
    std::fprintf(stderr, "handles: before=%lu after=%lu\n", handlesBefore, handlesAfter);
    CHECK(handlesBefore == handlesAfter);
    // More than a pool's worth of stopped/recreated workers reuses signaled
    // slots. The displaced manager duplicate must be closed, too.
    CThreadManager  pool(NULL, NULL);
    IThreadManager& poolApi = pool;
    for (unsigned i = 0; i <= MAXIMUM_WAIT_OBJECTS; ++i)
    {
        HANDLE h = CreateThread(NULL, 0, FinishedWorker, NULL, 0, NULL);
        CHECK(h != NULL && WAIT_OBJECT_0 == WaitForSingleObject(h, 5000));
        HANDLE duplicate = NULL;
        CHECK(DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS));
        poolApi.OnCreateThread(duplicate);
        CloseHandle(h);
    }
    poolApi.WaitForAliveThreadsToShutdown();
    GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
    CHECK(handlesBefore == handlesAfter);
    HANDLE childFinished = CreateEvent(NULL, TRUE, FALSE, NULL);
    CHECK(childFinished != NULL);
    SpawnContext spawn  = {&poolApi, childFinished};
    HANDLE       parent = CreateThread(NULL, 0, SpawningWorker, &spawn, CREATE_SUSPENDED, NULL);
    CHECK(parent != NULL);
    HANDLE parentDuplicate = NULL;
    CHECK(DuplicateHandle(GetCurrentProcess(), parent, GetCurrentProcess(), &parentDuplicate, 0, FALSE, DUPLICATE_SAME_ACCESS));
    poolApi.OnCreateThread(parentDuplicate);
    CHECK(1 == ResumeThread(parent));
    poolApi.WaitForAliveThreadsToShutdown();
    CHECK(WAIT_OBJECT_0 == WaitForSingleObject(childFinished, 0));
    CloseHandle(parent);
    CloseHandle(childFinished);
    GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
    CHECK(handlesBefore == handlesAfter);
    CHECK(packets.load() > 0);
    DeleteCriticalSection(&networkLock);
    CloseHandle(entered);
    closesocket(socketHandle);
    WSACleanup();
    DeleteThreadManager(&manager);
    std::puts("PASS 100 handler stop/recreate cycles: idle select, continuous packets, unlocked exit, forwarding, no handle growth");
}
