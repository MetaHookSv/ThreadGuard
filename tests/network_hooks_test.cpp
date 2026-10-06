// Exercise the actual handlers with owned Win32 threads and a loop that either
// blocks in select or keeps finding packets. The host API provides only a fatal
// diagnostic stub. IAT installation and symbol resolution require a game run.
#include "../src/ThreadManager.cpp"
#include <cstdlib>
#include <cstdarg>

metahook_api_t* g_pMetaHookAPI = NULL;
HANDLE g_MainThreadId = NULL;

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); std::abort(); } } while (0)

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
	void StartTermination() override {}
	void WaitForAliveThreadsToShutdown() override {}
	void OnCreateThread(HANDLE thread) override { handles.push_back(thread); }
	bool OnWaitForSingleObject(HANDLE, DWORD) override { return false; }
	bool OnSleep(DWORD) override { return false; }
	void InstallHook(int) override {}
	void UnistallHook() override {}
	PVOID GetImageBase() const override { return NULL; }
	ULONG GetImageSize() const override { return 0; }
	PVOID GetTextBase() const override { return NULL; }
	ULONG GetTextSize() const override { return MAXDWORD; }
	HMODULE GetModule() const override { return NULL; }
	BlobHandle_t GetBlobModule() const override { return NULL; }
	void CloseDuplicates()
	{
		for (HANDLE h : handles) { CHECK(WAIT_OBJECT_0 == WaitForSingleObject(h, 0)); CloseHandle(h); }
		handles.clear();
	}
};

static DWORD engineThreadId;
static SOCKET socketHandle = INVALID_SOCKET;
static HANDLE shutdownWorker = NULL;
static unsigned shutdownCalls = 0;
static HANDLE entered;
static CRITICAL_SECTION networkLock;
static std::atomic<bool> flood{ false };
static std::atomic<unsigned> packets{ 0 };
static int __cdecl Packet(int) { ++packets; return flood.load() ? 1 : 0; }

static void OpenSocket()
{
	socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	CHECK(INVALID_SOCKET != socketHandle);
	sockaddr_in address = {};
	address.sin_family = AF_INET;
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

static DWORD WINAPI OtherWorker(void*) { Sleep(INFINITE); return 0; }
static DWORD WINAPI FinishedWorker(void*) { return 0; }
struct SpawnContext { IThreadManager* manager; HANDLE childFinished; };
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
	api.SysError = Fatal;
	g_pMetaHookAPI = &api;
	g_MainThreadId = (HANDLE)GetCurrentThreadId();
	TestManager manager;
	AddThreadManager(&manager);
	g_NetworkManager = &manager;
	NetworkThread_Configure(Worker, &engineThreadId, (void*)Packet, (void*)ShutdownResources);
	g_pfn_select = select;
	WSADATA data;
	CHECK(0 == WSAStartup(MAKEWORD(2, 2), &data));
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
	CHECK(NULL == NewCreateThread(NULL, SIZE_T(-1), Worker, NULL,
		STACK_SIZE_PARAM_IS_A_RESERVATION, &engineThreadId));
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
			shutdownWorker = worker;
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
	DWORD otherId;
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
	CThreadManager pool(NULL, NULL);
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
	SpawnContext spawn = { &poolApi, childFinished };
	HANDLE parent = CreateThread(NULL, 0, SpawningWorker, &spawn, CREATE_SUSPENDED, NULL);
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
