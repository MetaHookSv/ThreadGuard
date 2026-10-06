#pragma once

#include <Windows.h>
#include <atomic>
#include <mutex>

// Creation/reset belong to the engine's serialized start/stop lifecycle.
// Readers on the worker only touch the published atomic identity and request.
class NetworkThreadState
{
public:
	enum class StopResult { NotTarget, Requested, Stopped, SelfWait, Failed };

	bool PrepareCreate() { return Reset(); }

	bool Track(HANDLE thread)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		const DWORD id = GetThreadId(thread);
		if (m_thread || !id)
			return false;
		if (!DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(),
			&m_thread, 0, FALSE, DUPLICATE_SAME_ACCESS))
			return false;
		m_exitRequested.store(false);
		m_threadId.store(id);
		return true;
	}

	bool IsThread(DWORD currentId, DWORD engineId) const
	{
		return currentId != 0 && currentId == engineId && currentId == m_threadId.load();
	}

	bool ShouldExit(DWORD currentId, DWORD engineId) const
	{
		return IsThread(currentId, engineId) && m_exitRequested.load();
	}

	void RetireCurrentThread()
	{
		DWORD id = GetCurrentThreadId();
		m_threadId.compare_exchange_strong(id, 0);
	}

	StopResult RequestStop(HANDLE thread, DWORD engineId, DWORD currentId = GetCurrentThreadId())
	{
		std::lock_guard<std::mutex> lock(m_lock);
		return RequestStopLocked(thread, engineId, currentId);
	}

	StopResult RequestTrackedStop(DWORD engineId, HANDLE* waitHandle = NULL)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		if (waitHandle)
			*waitHandle = NULL;
		if (!m_thread)
			return StopResult::NotTarget;
		// A repeated shutdown may already have cleared the engine's ID. An
		// exited object is safe; a live identity mismatch must block cleanup.
		const auto result = WaitForSingleObject(m_thread, 0) == WAIT_OBJECT_0
			? StopResult::Stopped : RequestStopLocked(m_thread, engineId, GetCurrentThreadId());
		if (result == StopResult::NotTarget)
			return StopResult::Failed;
		if (waitHandle && (result == StopResult::Requested || result == StopResult::Stopped)
			&& !DuplicateHandle(GetCurrentProcess(), m_thread, GetCurrentProcess(),
				waitHandle, 0, FALSE, DUPLICATE_SAME_ACCESS))
			return StopResult::Failed;
		return result;
	}

	bool Reset()
	{
		std::lock_guard<std::mutex> lock(m_lock);
		if (m_thread && WaitForSingleObject(m_thread, 0) != WAIT_OBJECT_0)
			return false;
		m_threadId.store(0);
		m_exitRequested.store(false);
		if (m_thread)
			CloseHandle(m_thread);
		m_thread = NULL;
		return true;
	}

private:
	StopResult RequestStopLocked(HANDLE thread, DWORD engineId, DWORD currentId)
	{
		if (!m_thread)
			return StopResult::NotTarget;
		const DWORD id = GetThreadId(thread);
		// Keep the object identity for repeated stops even after the worker
		// retires its active ID immediately before ExitThread.
		if (!id || id != engineId || id != GetThreadId(m_thread))
			return StopResult::NotTarget;
		if (id == currentId)
			return StopResult::SelfWait;

		const DWORD state = WaitForSingleObject(m_thread, 0);
		if (state == WAIT_OBJECT_0)
		{
			// A finished thread's ID can be reused. Never request exit for a new
			// live thread merely because it has the old numeric ID.
			return WaitForSingleObject(thread, 0) == WAIT_OBJECT_0
				? StopResult::Stopped : StopResult::NotTarget;
		}
		if (state != WAIT_TIMEOUT)
			return StopResult::Failed;
		m_exitRequested.store(true);
		return StopResult::Requested;
	}

	std::mutex m_lock;
	HANDLE m_thread = NULL;
	std::atomic<DWORD> m_threadId{ 0 };
	std::atomic<bool> m_exitRequested{ false };
};
