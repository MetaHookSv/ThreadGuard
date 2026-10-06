#include <winsock2.h>
#include "metahook.h"
#include "ThreadManager.h"
#include "NetworkThreadState.h"
#include "plugins.h"
#include <vector>
#include <mutex>
#include <intrin.h>
#include <cstdio>

static std::mutex g_ThreadManagerLock;
static std::vector<IThreadManager*> g_ThreadManagers;

extern HANDLE g_MainThreadId;

static NetworkThreadState g_NetworkThread;
static IThreadManager* g_NetworkManager = NULL;
static LPTHREAD_START_ROUTINE g_NetworkEntry = NULL;
static DWORD* g_NetworkThreadId = NULL;
static int (__cdecl* g_pfn_NET_QueuePacket)(int) = NULL;
static void (__cdecl* g_pfn_NET_Shutdown)(void) = NULL;
static int (WSAAPI* g_pfn_select)(int, fd_set*, fd_set*, fd_set*, const timeval*) = NULL;
static hook_t* g_pHook_NetworkTerminate = NULL;
static hook_t* g_pHook_NetworkSelect = NULL;
static hook_t* g_pHook_NET_QueuePacket = NULL;
static hook_t* g_pHook_NET_Shutdown = NULL;
static constexpr long kNetworkPollMicroseconds = 20000;

static bool IsNetworkThread()
{
	return g_NetworkThreadId && g_NetworkThread.IsThread(GetCurrentThreadId(), *g_NetworkThreadId);
}

static bool NetworkExitRequested()
{
	return g_NetworkThreadId && g_NetworkThread.ShouldExit(GetCurrentThreadId(), *g_NetworkThreadId);
}

static void NetworkTrace(const char* event, DWORD id)
{
	char message[160];
	sprintf_s(message, "[ThreadGuard] network thread %lu: %s\n", id, event);
	OutputDebugStringA(message);
}

static bool WaitForNetworkThread(HANDLE thread)
{
	// No registry, state or manager-list lock is held here. This is the
	// real Win32 wait, never the engine's zero-timeout polling wrapper.
	const DWORD wait = WaitForSingleObject(thread, INFINITE);
	if (wait != WAIT_OBJECT_0)
	{
		Sys_Error("Waiting for network thread failed (wait %lu, error %lu)!", wait, GetLastError());
		return false;
	}
	NetworkTrace("thread signaled; engine cleanup may continue", GetThreadId(thread));
	return true;
}

static BOOL WINAPI NewTerminateThread(HANDLE thread, DWORD exitCode)
{
	if (g_NetworkThreadId && FindThreadManagerByVirtualAddress(_ReturnAddress()) == g_NetworkManager)
	{
		const auto result = g_NetworkThread.RequestStop(thread, *g_NetworkThreadId);
		if (result == NetworkThreadState::StopResult::SelfWait || result == NetworkThreadState::StopResult::Failed)
		{
			Sys_Error("Cannot safely stop network thread (self wait or invalid tracked handle)!");
			return FALSE;
		}
		if (result == NetworkThreadState::StopResult::Requested || result == NetworkThreadState::StopResult::Stopped)
		{
			NetworkTrace("cooperative stop requested", GetThreadId(thread));
			return WaitForNetworkThread(thread) ? TRUE : FALSE;
		}
	}
	return TerminateThread(thread, exitCode);
}

static int WSAAPI NewNetworkSelect(int nfds, fd_set* read, fd_set* write, fd_set* except, const timeval* timeout)
{
	// Bound this worker's waits even before a stop is requested: an already
	// blocked select cannot observe a later atomic request. Preserve shorter
	// timeouts and every other thread's original select semantics.
	const timeval poll = { 0, kNetworkPollMicroseconds };
	if (IsNetworkThread() && (!timeout || timeout->tv_sec > 0 || timeout->tv_usec > poll.tv_usec))
		timeout = &poll;
	return g_pfn_select(nfds, read, write, except, timeout);
}

static int __cdecl NewNET_QueuePacket(int source)
{
	// False takes the normal no-packet path. The loop still unlocks and hands
	// any already received message to its queue before reaching Sleep(1).
	if (NetworkExitRequested())
		return 0;
	return g_pfn_NET_QueuePacket(source);
}

static void __cdecl NewNET_Shutdown(void)
{
	// All supported Windows engines close sockets before their late thread
	// stop. Their worker's select is outside net_cs, so join before entering
	// the original shutdown (and before it acquires net_cs to clear lag data).
	HANDLE thread = NULL;
	const auto result = g_NetworkThread.RequestTrackedStop(*g_NetworkThreadId, &thread);
	if (result == NetworkThreadState::StopResult::SelfWait || result == NetworkThreadState::StopResult::Failed)
	{
		Sys_Error("Cannot safely stop network thread before resource shutdown!");
		return;
	}
	if (thread)
	{
		NetworkTrace("NET_Shutdown: cooperative stop requested", GetThreadId(thread));
		const bool stopped = WaitForNetworkThread(thread);
		CloseHandle(thread);
		if (!stopped)
			return;
		NetworkTrace("NET_Shutdown: network thread stopped before resource cleanup", *g_NetworkThreadId);
	}
	// Preserve the engine's resource cleanup and its later (now idempotent)
	// TerminateThread call; do not clear IDs, flags, sockets or engine handles.
	g_pfn_NET_Shutdown();
}

void NetworkThread_Configure(LPTHREAD_START_ROUTINE entry, DWORD* threadId, void* queuePacket, void* shutdown)
{
	g_NetworkEntry = entry;
	g_NetworkThreadId = threadId;
	g_pfn_NET_QueuePacket = (decltype(g_pfn_NET_QueuePacket))queuePacket;
	g_pfn_NET_Shutdown = (decltype(g_pfn_NET_Shutdown))shutdown;
}

void NetworkThread_InstallHook(IThreadManager* manager)
{
	g_NetworkManager = manager;
	const auto module = manager->GetModule();
	const auto blob = manager->GetBlobModule();
	const char* socketModule = NULL;
	for (const auto name : { "ws2_32.dll", "wsock32.dll" })
	{
		if (module ? g_pMetaHookAPI->ModuleHasImportEx(module, name, "select")
			: g_pMetaHookAPI->BlobHasImportEx(blob, name, "select"))
		{
			socketModule = name;
			break;
		}
	}
	if (!socketModule)
	{
		Sys_Error("Could not find engine select import for cooperative network shutdown!");
		return;
	}
	if (module)
	{
		g_pHook_NetworkTerminate = g_pMetaHookAPI->IATHook(module, "kernel32.dll", "TerminateThread", NewTerminateThread, NULL);
		g_pHook_NetworkSelect = g_pMetaHookAPI->IATHook(module, socketModule, "select", NewNetworkSelect, (void**)&g_pfn_select);
	}
	else
	{
		g_pHook_NetworkTerminate = g_pMetaHookAPI->BlobIATHook(blob, "kernel32.dll", "TerminateThread", NewTerminateThread, NULL);
		g_pHook_NetworkSelect = g_pMetaHookAPI->BlobIATHook(blob, socketModule, "select", NewNetworkSelect, (void**)&g_pfn_select);
	}
	g_pHook_NET_QueuePacket = g_pMetaHookAPI->InlineHook((void*)g_pfn_NET_QueuePacket, NewNET_QueuePacket, (void**)&g_pfn_NET_QueuePacket);
	g_pHook_NET_Shutdown = g_pMetaHookAPI->InlineHook((void*)g_pfn_NET_Shutdown, NewNET_Shutdown, (void**)&g_pfn_NET_Shutdown);
	if (!g_pHook_NetworkTerminate || !g_pHook_NetworkSelect || !g_pHook_NET_QueuePacket || !g_pHook_NET_Shutdown)
		Sys_Error("Could not install cooperative network shutdown hooks!");
}

void NetworkThread_UninstallHook()
{
	if (!g_NetworkThread.Reset())
	{
		Sys_Error("Network thread is still running during engine unload!");
		return;
	}
	for (auto hook : { g_pHook_NET_Shutdown, g_pHook_NET_QueuePacket, g_pHook_NetworkSelect, g_pHook_NetworkTerminate })
		if (hook)
			g_pMetaHookAPI->UnHook(hook);
	g_pHook_NET_QueuePacket = g_pHook_NetworkSelect = g_pHook_NetworkTerminate = NULL;
	g_pHook_NET_Shutdown = NULL;
	g_pfn_NET_QueuePacket = NULL;
	g_pfn_NET_Shutdown = NULL;
	g_pfn_select = NULL;
	g_NetworkEntry = NULL;
	g_NetworkThreadId = NULL;
	g_NetworkManager = NULL;
}

IThreadManager * FindThreadManagerByVirtualAddress(PVOID VirtualAddress)
{
	std::lock_guard<std::mutex> lock(g_ThreadManagerLock);

	for (auto p : g_ThreadManagers)
	{
		if (VirtualAddress >= p->GetTextBase() && VirtualAddress < (PUCHAR)p->GetTextBase() + p->GetTextSize())
		{
			return p;
		}
	}

	return NULL;
}

HANDLE WINAPI NewCreateThread(
	LPSECURITY_ATTRIBUTES   lpThreadAttributes,
	SIZE_T                  dwStackSize,
	LPTHREAD_START_ROUTINE  lpStartAddress,
	LPVOID					lpParameter,
	DWORD                   dwCreationFlags,
	LPDWORD                 lpThreadId
)
{
	auto ThreadManager = FindThreadManagerByVirtualAddress(_ReturnAddress());
	const bool network = g_NetworkEntry && lpStartAddress == g_NetworkEntry && ThreadManager == g_NetworkManager;
	if (network && !g_NetworkThread.PrepareCreate())
	{
		Sys_Error("Cannot recreate a network thread before its predecessor exits!");
		return NULL;
	}

	// Publish identity before the worker can enter its first blocking select.
	// Preserve the caller's suspended state, entry point and original handle.
	auto hThreadHandle = CreateThread(lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter,
		network ? dwCreationFlags | CREATE_SUSPENDED : dwCreationFlags, lpThreadId);

	if (hThreadHandle)
	{
		if (network && !g_NetworkThread.Track(hThreadHandle))
		{
			Sys_Error("Could not retain network thread identity (error %lu)!", GetLastError());
			return hThreadHandle;
		}

		if (ThreadManager)
		{
			HANDLE hNewThreadHandle = 0;
			if (DuplicateHandle((HANDLE)(-1), hThreadHandle, (HANDLE)(-1), &hNewThreadHandle, THREAD_ALL_ACCESS, FALSE, DUPLICATE_SAME_ACCESS))
			{
				ThreadManager->OnCreateThread(hNewThreadHandle);
			}
		}
		if (network && !(dwCreationFlags & CREATE_SUSPENDED) && ResumeThread(hThreadHandle) == (DWORD)-1)
			Sys_Error("Could not resume network thread (error %lu)!", GetLastError());
	}

	return hThreadHandle;
}

DWORD WINAPI NewWaitForSingleObject(HANDLE hHandle,  DWORD dwMilliseconds)
{
	//Fuck valve
	if (dwMilliseconds == 0)
	{
		auto ThreadManager = FindThreadManagerByVirtualAddress(_ReturnAddress());

		if (ThreadManager)
		{
			if (ThreadManager->OnWaitForSingleObject(hHandle, dwMilliseconds))
			{
				return WAIT_OBJECT_0;
			}
		}
	}

	return WaitForSingleObject(hHandle, dwMilliseconds);
}

void WINAPI NewSleep(DWORD dwMilliseconds)
{
	if (dwMilliseconds == 1)
	{
		if (IsNetworkThread())
		{
			if (NetworkExitRequested())
			{
				// Verified for all supported Windows engines: this worker calls
				// Sleep(1) only at the unlocked loop tail. No RAII scope is active.
				NetworkTrace("safe Sleep(1); exiting", GetCurrentThreadId());
				g_NetworkThread.RetireCurrentThread();
				ExitThread(0);
			}
			return Sleep(dwMilliseconds);
		}
		auto ThreadManager = FindThreadManagerByVirtualAddress(_ReturnAddress());

		if (ThreadManager)
		{
			if (ThreadManager->OnSleep(dwMilliseconds))
			{
				ExitThread(0);
				return;
			}
		}
	}

	return Sleep(dwMilliseconds);
}

class CThreadManager : public IThreadManager
{

private:
	std::mutex m_ThreadListLock;

	HANDLE m_hAliveThread[MAXIMUM_WAIT_OBJECTS];
	HANDLE m_hClosedThread[MAXIMUM_WAIT_OBJECTS];

	HMODULE m_hModule;
	BlobHandle_t m_hBlobModule;

	PVOID m_ImageBase;
	ULONG m_ImageSize;

	PVOID m_TextBase;
	ULONG m_TextSize;

	//for __beginthread
	hook_t* m_pHook_CreateThread;

	//for CSocketThread
	hook_t* m_pHook_WaitForSingleObject;

	//for hw.dll
	hook_t* m_pHook_Sleep;

	std::atomic<bool> m_bStartTermination;
public:
	CThreadManager(HMODULE hModule, BlobHandle_t hBlob)
	{
		m_hModule = hModule;
		m_hBlobModule = hBlob;

		m_ImageBase = 0;
		m_ImageSize = 0;
		m_TextBase = 0;
		m_TextSize = 0;

		if (m_hModule)
		{
			m_ImageBase = g_pMetaHookAPI->GetModuleBase(m_hModule);
			m_TextBase = g_pMetaHookAPI->GetSectionByName(m_ImageBase, ".text\0\0\0", &m_TextSize);
		}

		if (m_hBlobModule)
		{
			m_ImageBase = g_pMetaHookAPI->GetBlobModuleImageBase(m_hBlobModule);
			m_TextBase = g_pMetaHookAPI->GetSectionByName(m_ImageBase, ".text\0\0\0", &m_TextSize);
		}

		memset(m_hAliveThread, 0, sizeof(m_hAliveThread));
		memset(m_hClosedThread, 0, sizeof(m_hClosedThread));

		m_pHook_CreateThread = NULL;
		m_pHook_WaitForSingleObject = NULL;
		m_pHook_Sleep = NULL;

		m_bStartTermination = false;
	}

	HMODULE GetModule(void) const
	{
		return m_hModule;
	}

	BlobHandle_t GetBlobModule(void) const
	{
		return m_hBlobModule;
	}

	PVOID GetImageBase(void) const
	{
		return m_ImageBase;
	}

	ULONG GetImageSize(void) const
	{
		return m_ImageSize;
	}

	PVOID GetTextBase(void) const
	{
		return m_TextBase;
	}

	ULONG GetTextSize(void) const
	{
		return m_TextSize;
	}

	bool OnWaitForSingleObject(HANDLE hObject, DWORD dwMilliseconds) override
	{
		if (m_bStartTermination)
		{
			return true;
		}
		return false;
	}

	bool OnSleep(DWORD dwMilliseconds) override
	{
		if (m_bStartTermination)
		{
			if((HANDLE)GetCurrentThreadId() != g_MainThreadId)
				return true;
		}
		return false;
	}

	void InstallHook(int flags) override
	{
		if (m_hModule)
		{
			if(flags & hookflag_CreateThread)
				m_pHook_CreateThread = g_pMetaHookAPI->IATHook(m_hModule, "kernel32.dll", "CreateThread", NewCreateThread, NULL);

			if (flags & hookflag_WaitForSingleObject)
				m_pHook_WaitForSingleObject = g_pMetaHookAPI->IATHook(m_hModule, "kernel32.dll", "WaitForSingleObject", NewWaitForSingleObject, NULL);

			if (flags & hookflag_Sleep)
				m_pHook_Sleep = g_pMetaHookAPI->IATHook(m_hModule, "kernel32.dll", "Sleep", NewSleep, NULL);
		}
		else if (m_hBlobModule)
		{
			if (flags & hookflag_CreateThread)
				m_pHook_CreateThread = g_pMetaHookAPI->BlobIATHook(m_hBlobModule, "kernel32.dll", "CreateThread", NewCreateThread, NULL);

			if (flags & hookflag_WaitForSingleObject)
				m_pHook_WaitForSingleObject = g_pMetaHookAPI->BlobIATHook(m_hBlobModule, "kernel32.dll", "WaitForSingleObject", NewWaitForSingleObject, NULL);

			if (flags & hookflag_Sleep)
				m_pHook_Sleep = g_pMetaHookAPI->BlobIATHook(m_hBlobModule, "kernel32.dll", "Sleep", NewSleep, NULL);
		}
	}

	void UnistallHook(void) override
	{
#if 0
		if (m_pHook_DllMain)
		{
			g_pMetaHookAPI->UnHook(m_pHook_DllMain);
			m_pHook_DllMain = NULL;
		}
#endif
		if (m_pHook_CreateThread)
		{
			g_pMetaHookAPI->UnHook(m_pHook_CreateThread);
			m_pHook_CreateThread = NULL;
		}
#if 0
		if (m_pHook_CloseHandle)
		{
			g_pMetaHookAPI->UnHook(m_pHook_CloseHandle);
			m_pHook_CloseHandle = NULL;
		}

		if (m_pHook_TerminateThread)
		{
			g_pMetaHookAPI->UnHook(m_pHook_TerminateThread);
			m_pHook_TerminateThread = NULL;
		}
#endif
		if (m_pHook_WaitForSingleObject)
		{
			g_pMetaHookAPI->UnHook(m_pHook_WaitForSingleObject);
			m_pHook_WaitForSingleObject = NULL;
		}

		if (m_pHook_Sleep)
		{
			g_pMetaHookAPI->UnHook(m_pHook_Sleep);
			m_pHook_Sleep = NULL;
		}
	}
private:

	bool FindAliveThread(HANDLE hThread)
	{
		for (DWORD i = 0; i < _ARRAYSIZE(m_hAliveThread); ++i)
		{
			if (m_hAliveThread[i] == hThread)
			{
				return true;
			}
		}
		return false;
	}

	bool FindAndRemoveAliveThread(HANDLE hThread)
	{
		for (DWORD i = 0; i < _ARRAYSIZE(m_hAliveThread); ++i)
		{
			if (m_hAliveThread[i] == hThread)
			{
				m_hAliveThread[i] = 0;
				return true;
			}
		}
		return false;
	}

	bool AddAliveThread(HANDLE hThread)
	{
		for (DWORD i = 0; i < _ARRAYSIZE(m_hAliveThread); ++i)
		{
			if (m_hAliveThread[i] == 0)
			{
				m_hAliveThread[i] = hThread;
				return true;
			}
		}
		return false;
	}

	bool FindClosedThread(HANDLE hThread)
	{
		for (DWORD i = 0; i < _ARRAYSIZE(m_hClosedThread); ++i)
		{
			if (m_hClosedThread[i] == hThread)
			{
				return true;
			}
		}
		return false;
	}

	bool AddClosedThread(HANDLE hThread)
	{
		for (DWORD i = 0; i < _ARRAYSIZE(m_hClosedThread); ++i)
		{
			if (m_hClosedThread[i] == 0)
			{
				m_hClosedThread[i] = hThread;
				return true;
			}
		}
		return false;
	}

	bool FindAndRemoveSignaledAliveThread(DWORD* pIndex)
	{
		for (DWORD i = 0; i < _ARRAYSIZE(m_hAliveThread); ++i)
		{
			if (m_hAliveThread[i])
			{
					if (WAIT_OBJECT_0 == WaitForSingleObject(m_hAliveThread[i], 0))
					{
						CloseHandle(m_hAliveThread[i]);
						m_hAliveThread[i] = 0;
					if (pIndex)
						*pIndex = i;
					return true;
				}
			}
		}

		return false;
	}

	bool FindAndRemoveSignaledClosedThread(DWORD* pIndex)
	{
		for (DWORD i = 0; i < _ARRAYSIZE(m_hClosedThread); ++i)
		{
			if (m_hClosedThread[i])
			{
				if (WAIT_OBJECT_0 == WaitForSingleObject(m_hClosedThread[i], 0))
				{
					m_hClosedThread[i] = 0;
					if (pIndex)
						*pIndex = i;
					return true;
				}
			}
		}

		return false;
	}

	void StartTermination(void) override
	{
		if (this == g_NetworkManager && g_NetworkThreadId)
		{
			// Preserve the old ExitGame fallback when an engine shutdown path
			// reaches the manager without first calling TerminateThread.
			const auto result = g_NetworkThread.RequestTrackedStop(*g_NetworkThreadId);
			if (result == NetworkThreadState::StopResult::SelfWait || result == NetworkThreadState::StopResult::Failed)
				Sys_Error("Cannot request network thread exit during engine termination!");
		}
		m_bStartTermination = true;
	}

	void WaitForAliveThreadsToShutdown(void) override
	{
		for (;;)
		{
			HANDLE hThreads[MAXIMUM_WAIT_OBJECTS] = { 0 };
			DWORD numThreads = 0;
			{
				std::lock_guard<std::mutex> lock(m_ThreadListLock);
				for (DWORD i = 0; i < _ARRAYSIZE(m_hAliveThread); ++i)
				{
					if (m_hAliveThread[i] != 0)
					{
						hThreads[numThreads++] = m_hAliveThread[i];
						// Transfer ownership to this batch before dropping the lock.
						m_hAliveThread[i] = NULL;
					}
				}
			}
			if (!numThreads)
				return;
			if (WaitForMultipleObjects(numThreads, hThreads, TRUE, INFINITE) != WAIT_OBJECT_0)
			{
				Sys_Error("Waiting for managed threads failed (error %lu)!", GetLastError());
				return;
			}

#if 0
			{
				std::lock_guard<std::mutex> lock(m_ThreadListLock);
				for (DWORD i = 0; i < numThreads; ++i)
				{
					if (hThreads[i] && !FindClosedThread(hThreads[i]))
						AddClosedThread(hThreads[i]);
				}
			}
#endif
			for (DWORD i = 0; i < numThreads; ++i)
				CloseHandle(hThreads[i]);
			// A tracked parent can create a child while we are joining it.
			// Drain that newly registered batch before permitting module unload.
		}
	}

#if 0
	void WaitForClosedThreadsToShutdown(void) override
	{
		HANDLE hThreads[MAXIMUM_WAIT_OBJECTS] = { 0 };
		DWORD numThreads = 0;

		if (1)
		{
			std::lock_guard<std::mutex> lock(m_ThreadListLock);
			for (DWORD i = 0; i < _ARRAYSIZE(m_hClosedThread); ++i)
			{
				if (m_hClosedThread[i] != 0)
				{
					hThreads[numThreads] = m_hClosedThread[i];
					numThreads++;
				}
			}
		}

		if (numThreads != 0)
			WaitForMultipleObjects(numThreads, hThreads, TRUE, INFINITE);

		memset(m_hClosedThread, 0, sizeof(m_hClosedThread));

		for (DWORD i = 0; i < numThreads; ++i)
		{
			if (hThreads[i])
			{
				CloseHandle(hThreads[i]);
			}
		}
	}

#endif

	void OnCreateThread(HANDLE hThreadHandle) override
	{
		std::lock_guard<std::mutex> lock(m_ThreadListLock);

		bool bAdded = AddAliveThread(hThreadHandle);

		if (!bAdded)
		{
			DWORD NewIndex = 0;
			if (FindAndRemoveSignaledAliveThread(&NewIndex))
			{
				m_hAliveThread[NewIndex] = hThreadHandle;
				bAdded = true;
			}
		}

		if (!bAdded)
		{
			g_pMetaHookAPI->SysError("Failed to insert thread to thread manager!");
		}
	}
};

void AddThreadManager(IThreadManager* p)
{
	std::lock_guard<std::mutex> lock(g_ThreadManagerLock);

	g_ThreadManagers.emplace_back(p);
}

void DeleteThreadManager(IThreadManager* p)
{
	std::lock_guard<std::mutex> lock(g_ThreadManagerLock);

	for (auto itor = g_ThreadManagers.begin(); itor != g_ThreadManagers.end();)
	{
		if ((*itor) == p)
		{
			itor = g_ThreadManagers.erase(itor);
			return;
		}
		itor++;
	}
}

IThreadManager* CreateThreadManagerForModule(HMODULE hModule)
{
	auto p = new CThreadManager(hModule, NULL);
	if (p)
	{
		AddThreadManager(p);
		return p;
	}

	return NULL;
}

IThreadManager* CreateThreadManagerForBlob(BlobHandle_t hBlob)
{
	auto p = new CThreadManager(NULL, hBlob);
	if (p)
	{
		AddThreadManager(p);
		return p;
	}

	return NULL;
}
