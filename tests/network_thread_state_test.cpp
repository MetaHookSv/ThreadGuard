#include "../src/NetworkThreadState.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(condition)                                                      \
    do                                                                        \
    {                                                                         \
        if (!(condition))                                                     \
        {                                                                     \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
            std::abort();                                                     \
        }                                                                     \
    } while (0)

static DWORD WINAPI WaitForRelease(void* event)
{
    WaitForSingleObject(event, INFINITE);
    return 0;
}

int main()
{
    NetworkThreadState state;
    const DWORD        mainThread = GetCurrentThreadId();
    CHECK(!state.IsThread(mainThread, mainThread));
    CHECK(state.PrepareCreate());
    // A failed CreateThread leaves no identity or exit request behind.
    CHECK(!state.IsThread(mainThread, mainThread));
    CHECK(state.PrepareCreate());
    HANDLE release = CreateEvent(NULL, TRUE, FALSE, NULL);
    CHECK(release != NULL);
    DWORD  id     = 0;
    HANDLE worker = CreateThread(NULL, 0, WaitForRelease, release, 0, &id);
    CHECK(worker != NULL);
    CHECK(state.Track(worker));
    CHECK(state.IsThread(id, id));
    CHECK(!state.IsThread(mainThread, id));
    CHECK(!state.IsThread(id, mainThread));
    CHECK(!state.ShouldExit(id, id));
    CHECK(!state.PrepareCreate());
    CHECK(NetworkThreadState::StopResult::NotTarget == state.RequestStop(GetCurrentThread(), id));
    CHECK(NetworkThreadState::StopResult::NotTarget == state.RequestStop(worker, mainThread));
    HANDLE waitHandle = NULL;
    CHECK(NetworkThreadState::StopResult::Failed == state.RequestTrackedStop(mainThread, &waitHandle));
    CHECK(NULL == waitHandle);
    CHECK(!state.ShouldExit(id, id));
    CHECK(NetworkThreadState::StopResult::Requested == state.RequestTrackedStop(id, &waitHandle));
    CHECK(NULL != waitHandle && waitHandle != worker);
    CHECK(NetworkThreadState::StopResult::Requested == state.RequestStop(worker, id));
    CHECK(state.ShouldExit(id, id));
    CHECK(!state.ShouldExit(mainThread, id));
    CHECK(NetworkThreadState::StopResult::Requested == state.RequestStop(worker, id));
    CHECK(!state.Reset());
    SetEvent(release);
    CHECK(WAIT_OBJECT_0 == WaitForSingleObject(worker, 5000));
    CHECK(NetworkThreadState::StopResult::Stopped == state.RequestStop(worker, id));
    // The state owns a duplicate, never the engine's original handle.
    CHECK(state.Reset());
    CHECK(WAIT_OBJECT_0 == WaitForSingleObject(waitHandle, 0));
    CloseHandle(waitHandle);
    CHECK(WAIT_OBJECT_0 == WaitForSingleObject(worker, 0));
    CloseHandle(worker);
    ResetEvent(release);
    CHECK(state.PrepareCreate());
    worker = CreateThread(NULL, 0, WaitForRelease, release, 0, &id);
    CHECK(worker != NULL && state.Track(worker));
    CHECK(!state.ShouldExit(id, id));
    CHECK(NetworkThreadState::StopResult::SelfWait == state.RequestStop(worker, id, id));
    CHECK(!state.ShouldExit(id, id));
    SetEvent(release);
    CHECK(WAIT_OBJECT_0 == WaitForSingleObject(worker, 5000));
    // Repeated cleanup is safe after the engine has cleared its public ID.
    CHECK(NetworkThreadState::StopResult::Stopped == state.RequestTrackedStop(0, &waitHandle));
    CHECK(WAIT_OBJECT_0 == WaitForSingleObject(waitHandle, 0));
    CloseHandle(waitHandle);
    CHECK(state.Reset());
    CloseHandle(worker);
    CloseHandle(release);
    std::puts("PASS network identity, requests, failed creation, repeated stop, rebuild and handle ownership");
}
