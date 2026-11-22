#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

typedef HRESULT (*QueryApi)(const GUID&, const GUID&, void**);
struct AsyncBlock { void* queue; void* context; void (*callback)(AsyncBlock*); unsigned char internal[32]; };
struct Token { uint64_t value; };

static int g_failures = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) g_failures++;
}

static QueryApi g_query;
static std::string g_table;
static int TableSlot(const char* method) {
  std::string key = std::string("\"name\": \"") + method + "\",";
  size_t at = g_table.find(key);
  if (at == std::string::npos || g_table.find(key, at + 1) != std::string::npos) return -1;
  size_t slot = g_table.find("\"slot\": ", at);
  size_t next = g_table.find("\"name\": ", at + key.size());
  if (slot == std::string::npos || (next != std::string::npos && slot > next)) return -1;
  return std::atoi(g_table.c_str() + slot + 8);
}
static std::string TableUuid(const char* iface) {
  std::string key = std::string("\"name\": \"") + iface + "\",";
  size_t at = g_table.find(key);
  if (at == std::string::npos) return "";
  size_t uuid = g_table.find("\"uuid\": \"", at);
  size_t next = g_table.find("\"name\": ", at + key.size());
  if (uuid == std::string::npos || (next != std::string::npos && uuid > next)) return "";
  return g_table.substr(uuid + 9, 36);
}
static bool LoadTable() {
  std::ifstream file("table.json", std::ios::binary);
  if (!file) return false;
  std::ostringstream text;
  text << file.rdbuf();
  g_table = text.str();
  return !g_table.empty();
}
static std::string GuidText(const GUID& g) {
  char text[40];
  std::snprintf(text, sizeof(text), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", static_cast<unsigned long>(g.Data1), g.Data2, g.Data3,
                g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
  return text;
}

struct Api {
  void* self;
  void** slots;
  template <typename Fn> Fn At(int slot, const char* method) const {
    int expected = TableSlot(method);
    if (expected != slot) {
      std::printf("FAIL slot pin: %s is slot %d in table.json, the test uses %d\n", method, expected, slot);
      std::exit(1);
    }
    return reinterpret_cast<Fn>(slots[slot]);
  }
};
static Api Get(const char* iface, const GUID& iid) {
  if (TableUuid(iface) != GuidText(iid)) {
    std::printf("FAIL guid pin: %s is %s in table.json, the test uses %s\n", iface, TableUuid(iface).c_str(), GuidText(iid).c_str());
    std::exit(1);
  }
  void* object = nullptr;
  HRESULT hr = g_query(iid, iid, &object);
  if (FAILED(hr) || !object) {
    std::printf("FAIL QueryApiImpl %s %08lx\n", iface, static_cast<unsigned long>(hr));
    std::exit(1);
  }
  return {object, *static_cast<void***>(object)};
}

static const GUID kThreading = {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};
static const GUID kGameEvent = {0xbbfbdcc7, 0xbfe7, 0x409b, {0xa5, 0xca, 0xed, 0xf0, 0x54, 0x96, 0x0b, 0x4d}};
static const GUID kUser = {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
static const GUID kSystem3 = {0x67ce4bfc, 0xb1d1, 0x4ac7, {0xbc, 0x3a, 0xcb, 0x92, 0x19, 0xa9, 0x7a, 0x85}};
static const GUID kActivation = {0x2e4f76fe, 0x0fc7, 0x461e, {0xab, 0x4d, 0xa4, 0x49, 0x94, 0x34, 0xc3, 0xcf}};
static const GUID kInvite = {0x0651aae2, 0x4012, 0x4077, {0xbf, 0x84, 0x8b, 0x90, 0x97, 0x09, 0x0e, 0x2c}};
static const GUID kInvite2 = {0x014d1cc3, 0xbcfe, 0x41ff, {0xb2, 0xf0, 0xe1, 0xef, 0x07, 0x15, 0x58, 0x28}};
static const GUID kProtocol = {0x026b010c, 0x06c3, 0x4cdd, {0xbb, 0xcb, 0x43, 0xf2, 0x29, 0xdb, 0x1c, 0xff}};
static const GUID kStore = {0x0dd112ac, 0x7c24, 0x448c, {0xb9, 0x2b, 0x39, 0x60, 0xfb, 0x5b, 0xd3, 0x0c}};
static const GUID kError = {0x8ca467f7, 0x22e8, 0x4096, {0x84, 0x56, 0xbb, 0x8a, 0xa1, 0x3f, 0x79, 0xd8}};

constexpr uint32_t kManual = 0;
constexpr uint32_t kThreadPool = 1;
constexpr uint32_t kWork = 0;
constexpr uint32_t kCompletion = 1;

typedef void(CALLBACK* TaskCallback)(void*, bool);
struct Threading {
  Api api;
  HRESULT Create(uint32_t work, uint32_t completion, void** queue) const {
    return api.At<HRESULT (*)(void*, uint32_t, uint32_t, void**)>(12, "XTaskQueueCreate")(api.self, work, completion, queue);
  }
  HRESULT CreateComposite(void* work, void* completion, void** queue) const {
    return api.At<HRESULT (*)(void*, void*, void*, void**)>(13, "XTaskQueueCreateComposite")(api.self, work, completion, queue);
  }
  HRESULT GetPort(void* queue, uint32_t port, void** handle) const {
    return api.At<HRESULT (*)(void*, void*, uint32_t, void**)>(14, "XTaskQueueGetPort")(api.self, queue, port, handle);
  }
  bool Dispatch(void* queue, uint32_t port, uint32_t timeout) const {
    return api.At<bool (*)(void*, void*, uint32_t, uint32_t)>(16, "XTaskQueueDispatch")(api.self, queue, port, timeout);
  }
  void Close(void* queue) const { api.At<void (*)(void*, void*)>(17, "XTaskQueueCloseHandle")(api.self, queue); }
  HRESULT Submit(void* queue, uint32_t port, void* context, TaskCallback callback) const {
    return api.At<HRESULT (*)(void*, void*, uint32_t, void*, TaskCallback)>(18, "XTaskQueueSubmitCallback")(api.self, queue, port, context, callback);
  }
  HRESULT SubmitDelayed(void* queue, uint32_t port, uint32_t delay, void* context, TaskCallback callback) const {
    return api.At<HRESULT (*)(void*, void*, uint32_t, uint32_t, void*, TaskCallback)>(19, "XTaskQueueSubmitDelayedCallback")(
        api.self, queue, port, delay, context, callback);
  }
  HRESULT RegisterWaiter(void* queue, uint32_t port, HANDLE handle, void* context, TaskCallback callback, Token* token) const {
    return api.At<HRESULT (*)(void*, void*, uint32_t, HANDLE, void*, TaskCallback, Token*)>(20, "XTaskQueueRegisterWaiter")(
        api.self, queue, port, handle, context, callback, token);
  }
  void UnregisterWaiter(void* queue, Token token) const {
    api.At<void (*)(void*, void*, Token)>(21, "XTaskQueueUnregisterWaiter")(api.self, queue, token);
  }
  HRESULT Terminate(void* queue, bool wait) const {
    return api.At<HRESULT (*)(void*, void*, bool, void*, void*)>(22, "XTaskQueueTerminate")(api.self, queue, wait, nullptr, nullptr);
  }
  bool IsEmpty(void* queue, uint32_t port) const {
    return api.At<bool (*)(void*, void*, uint32_t)>(28, "XTaskQueueIsEmpty")(api.self, queue, port);
  }
  HRESULT WaitAsync(AsyncBlock* block) const {
    return api.At<HRESULT (*)(void*, AsyncBlock*, BOOLEAN)>(3, "XAsyncGetStatus")(api.self, block, TRUE);
  }
};
static Threading g_tq;

struct Counter {
  std::atomic<int> calls{0};
  std::atomic<int> canceled{0};
  void* queue = nullptr;
  uint32_t port = 0;
  std::atomic<int> sawNonEmpty{0};
  HANDLE gate = nullptr;
};
static void CALLBACK Count(void* context, bool canceled) {
  auto* c = static_cast<Counter*>(context);
  if (canceled) c->canceled++;
  c->calls++;
}
static void CALLBACK CountAndProbe(void* context, bool canceled) {
  auto* c = static_cast<Counter*>(context);
  if (!g_tq.IsEmpty(c->queue, c->port)) c->sawNonEmpty++;
  Count(context, canceled);
}
static void CALLBACK WaitForGate(void* context, bool canceled) {
  auto* c = static_cast<Counter*>(context);
  WaitForSingleObject(c->gate, 5000);
  Count(context, canceled);
}

static bool WaitEmpty(void* queue, uint32_t port, DWORD timeoutMs) {
  ULONGLONG until = GetTickCount64() + timeoutMs;
  while (!g_tq.IsEmpty(queue, port)) {
    if (GetTickCount64() > until) return false;
    Sleep(5);
  }
  return true;
}

static void CheckManualQueue() {
  void* q = nullptr;
  Check(g_tq.Create(kManual, kManual, &q) == S_OK && q, "XTaskQueueCreate makes a manual queue");
  Check(g_tq.IsEmpty(q, kWork) && g_tq.IsEmpty(q, kCompletion), "XTaskQueueIsEmpty is true on both ports of a new queue");

  Counter c;
  c.queue = q;
  c.port = kWork;
  g_tq.Submit(q, kWork, &c, CountAndProbe);
  Check(!g_tq.IsEmpty(q, kWork), "XTaskQueueIsEmpty is false for the work port while a callback waits for dispatch");
  Check(g_tq.IsEmpty(q, kCompletion), "XTaskQueueIsEmpty keeps the completion port empty when only work was queued");
  Check(g_tq.Dispatch(q, kWork, 0) && c.calls == 1, "XTaskQueueDispatch runs the queued work callback");
  Check(c.sawNonEmpty == 1, "XTaskQueueIsEmpty is false from inside the running callback");
  Check(g_tq.IsEmpty(q, kWork), "XTaskQueueIsEmpty is true again after the callback was dispatched");

  Counter d;
  d.queue = q;
  d.port = kCompletion;
  g_tq.Submit(q, kCompletion, &d, CountAndProbe);
  g_tq.Submit(q, kCompletion, &d, CountAndProbe);
  Check(!g_tq.IsEmpty(q, kCompletion) && g_tq.IsEmpty(q, kWork), "XTaskQueueIsEmpty tracks the completion port separately");
  g_tq.Dispatch(q, kCompletion, 0);
  Check(!g_tq.IsEmpty(q, kCompletion), "XTaskQueueIsEmpty stays false while one of two callbacks is left");
  g_tq.Dispatch(q, kCompletion, 0);
  Check(g_tq.IsEmpty(q, kCompletion) && d.calls == 2, "XTaskQueueIsEmpty is true once every completion callback ran");

  Counter delayed;
  g_tq.SubmitDelayed(q, kWork, 150, &delayed, Count);
  Check(!g_tq.IsEmpty(q, kWork), "XTaskQueueIsEmpty counts a delayed callback that is not due yet");
  Check(!g_tq.Dispatch(q, kWork, 0), "XTaskQueueDispatch has nothing to run before the delay passes");
  Check(g_tq.Dispatch(q, kWork, 5000) && delayed.calls == 1 && g_tq.IsEmpty(q, kWork),
        "XTaskQueueIsEmpty is true after the delayed callback was dispatched");

  Check(g_tq.IsEmpty(q, 2), "XTaskQueueIsEmpty reports an unknown port as empty");

  HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Counter waited;
  Token token{};
  Check(g_tq.RegisterWaiter(q, kWork, never, &waited, Count, &token) == S_OK && g_tq.IsEmpty(q, kWork),
        "XTaskQueueIsEmpty does not count a registered waiter whose handle is unsignaled");
  g_tq.UnregisterWaiter(q, token);
  CloseHandle(never);
  g_tq.Close(q);
}

static void CheckTermination() {
  void* q = nullptr;
  g_tq.Create(kManual, kManual, &q);
  Counter c;
  g_tq.SubmitDelayed(q, kWork, 60000, &c, Count);
  Check(!g_tq.IsEmpty(q, kWork), "XTaskQueueIsEmpty counts a long-delayed callback");
  g_tq.Terminate(q, false);
  Check(!g_tq.IsEmpty(q, kWork), "XTaskQueueIsEmpty still counts the canceled callback until it is dispatched");
  while (g_tq.Dispatch(q, kWork, 1000)) {
  }
  Check(c.calls == 1 && c.canceled == 1 && g_tq.IsEmpty(q, kWork), "XTaskQueueIsEmpty is true once the canceled callback was delivered");
  g_tq.Close(q);
}

struct DispatchLoop {
  void* queue;
  uint32_t port;
  uint32_t timeout;
  std::atomic<int> dispatched{0};
  HANDLE thread = nullptr;
};
static DWORD WINAPI RunDispatchLoop(void* context) {
  auto* loop = static_cast<DispatchLoop*>(context);
  while (g_tq.Dispatch(loop->queue, loop->port, loop->timeout)) loop->dispatched++;
  return 0;
}
static void StartDispatchLoop(DispatchLoop* loop) { loop->thread = CreateThread(nullptr, 0, RunDispatchLoop, loop, 0, nullptr); }
static bool DispatchLoopEnds(DispatchLoop* loop, DWORD ms) {
  if (!loop->thread || WaitForSingleObject(loop->thread, ms) != WAIT_OBJECT_0) return false;
  CloseHandle(loop->thread);
  loop->thread = nullptr;
  return true;
}
static bool WaitCalls(const Counter& c, int calls, DWORD ms) {
  ULONGLONG until = GetTickCount64() + ms;
  while (c.calls < calls) {
    if (GetTickCount64() > until) return false;
    Sleep(5);
  }
  return true;
}

static void CheckDispatchAfterTerminate() {
  void* q = nullptr;
  g_tq.Create(kManual, kManual, &q);
  Counter c;
  DispatchLoop loop{q, kWork, INFINITE};
  StartDispatchLoop(&loop);
  g_tq.Submit(q, kWork, &c, Count);
  Check(WaitCalls(c, 1, 5000) && c.canceled == 0, "XTaskQueueDispatch(INFINITE) runs work submitted while it waits");
  g_tq.Terminate(q, false);
  bool ended = DispatchLoopEnds(&loop, 5000);
  Check(ended && loop.dispatched == 1, "XTaskQueueDispatch(INFINITE) blocked on an empty port returns false when the queue is terminated");
  if (ended) g_tq.Close(q);

  q = nullptr;
  g_tq.Create(kManual, kManual, &q);
  Counter pending;
  g_tq.Submit(q, kWork, &pending, Count);
  g_tq.Submit(q, kWork, &pending, Count);
  g_tq.Terminate(q, false);
  DispatchLoop drain{q, kWork, INFINITE};
  StartDispatchLoop(&drain);
  ended = DispatchLoopEnds(&drain, 5000);
  Check(ended && drain.dispatched == 2 && pending.calls == 2 && pending.canceled == 2,
        "XTaskQueueDispatch(INFINITE) delivers the callbacks pending at termination as canceled, then returns false");
  DispatchLoop completion{q, kCompletion, INFINITE};
  StartDispatchLoop(&completion);
  bool completionEnded = DispatchLoopEnds(&completion, 5000);
  Check(completionEnded && completion.dispatched == 0, "XTaskQueueDispatch(INFINITE) on the empty completion port of a terminated queue returns false");
  ULONGLONG started = GetTickCount64();
  bool dispatched = g_tq.Dispatch(q, kWork, 3000);
  Check(!dispatched && GetTickCount64() - started < 1000, "XTaskQueueDispatch with a timeout returns false at once on a terminated, drained queue");
  if (ended && completionEnded) g_tq.Close(q);
}

static void CALLBACK ResetGateAndCount(void* context, bool canceled) {
  auto* c = static_cast<Counter*>(context);
  ResetEvent(c->gate);
  Count(context, canceled);
}

static void CheckWaiters() {
  void* q = nullptr;
  g_tq.Create(kThreadPool, kThreadPool, &q);

  HANDLE autoReset = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  Counter a;
  Token ta{};
  Check(g_tq.RegisterWaiter(q, kWork, autoReset, &a, Count, &ta) == S_OK, "XTaskQueueRegisterWaiter registers an auto-reset event");
  SetEvent(autoReset);
  Check(WaitCalls(a, 1, 5000) && a.canceled == 0, "XTaskQueueRegisterWaiter invokes the callback when the event is signaled");
  SetEvent(autoReset);
  Check(WaitCalls(a, 2, 5000), "XTaskQueueRegisterWaiter invokes the callback again when an auto-reset event is signaled a second time");
  Sleep(100);
  Check(a.calls == 2, "XTaskQueueRegisterWaiter invokes the callback once per signal of an auto-reset event");
  g_tq.UnregisterWaiter(q, ta);
  SetEvent(autoReset);
  Sleep(100);
  Check(a.calls == 2, "XTaskQueueUnregisterWaiter: the callback is no longer invoked when the event is signaled");
  CloseHandle(autoReset);

  HANDLE manualReset = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  Counter m;
  Token tm{};
  g_tq.RegisterWaiter(q, kWork, manualReset, &m, Count, &tm);
  Check(WaitCalls(m, 5, 5000), "XTaskQueueRegisterWaiter invokes the callback repeatedly while a manual-reset event stays signaled");
  ResetEvent(manualReset);
  Sleep(50);
  int settled = m.calls;
  Sleep(100);
  Check(m.calls == settled, "XTaskQueueRegisterWaiter stops repeating once the manual-reset event is reset");
  g_tq.UnregisterWaiter(q, tm);
  CloseHandle(manualReset);

  Counter r;
  r.gate = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  Token tr{};
  g_tq.RegisterWaiter(q, kWork, r.gate, &r, ResetGateAndCount, &tr);
  WaitCalls(r, 1, 5000);
  Sleep(100);
  Check(r.calls == 1, "XTaskQueueRegisterWaiter invokes the callback once when it resets the manual-reset event itself");
  g_tq.UnregisterWaiter(q, tr);
  CloseHandle(r.gate);
  g_tq.Close(q);

  void* manualQueue = nullptr;
  g_tq.Create(kManual, kManual, &manualQueue);
  HANDLE signaled = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  Counter d;
  Token td{};
  g_tq.RegisterWaiter(manualQueue, kWork, signaled, &d, Count, &td);
  bool dispatched = g_tq.Dispatch(manualQueue, kWork, 5000) && g_tq.Dispatch(manualQueue, kWork, 5000) && g_tq.Dispatch(manualQueue, kWork, 5000);
  Check(dispatched && d.calls == 3, "XTaskQueueRegisterWaiter on a manual queue submits a new callback after each dispatched one while the event stays signaled");
  g_tq.UnregisterWaiter(manualQueue, td);
  while (g_tq.Dispatch(manualQueue, kWork, 0)) {
  }
  int drained = d.calls;
  Check(!g_tq.Dispatch(manualQueue, kWork, 100) && d.calls == drained, "XTaskQueueUnregisterWaiter on a manual queue: no callback is submitted afterwards");
  CloseHandle(signaled);

  void* terminated = nullptr;
  g_tq.Create(kThreadPool, kThreadPool, &terminated);
  HANDLE later = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  Counter t;
  Token tt{};
  g_tq.RegisterWaiter(terminated, kWork, later, &t, Count, &tt);
  g_tq.Terminate(terminated, true);
  SetEvent(later);
  Sleep(100);
  Check(t.calls == 0, "XTaskQueueTerminate unregisters the waiters: a later signal invokes nothing");
  CloseHandle(later);
  g_tq.Close(terminated);
  g_tq.Close(manualQueue);
}

static void CheckComposite() {
  void* q = nullptr;
  g_tq.Create(kManual, kManual, &q);
  void* work = nullptr;
  void* completion = nullptr;
  g_tq.GetPort(q, kWork, &work);
  g_tq.GetPort(q, kCompletion, &completion);
  void* composite = nullptr;
  Check(g_tq.CreateComposite(work, completion, &composite) == S_OK, "XTaskQueueCreateComposite shares the ports");
  Counter c;
  g_tq.Submit(composite, kWork, &c, Count);
  Check(!g_tq.IsEmpty(q, kWork) && !g_tq.IsEmpty(composite, kWork), "XTaskQueueIsEmpty sees work on a shared port from both queues");
  g_tq.Dispatch(q, kWork, 0);
  Check(c.calls == 1 && g_tq.IsEmpty(q, kWork) && g_tq.IsEmpty(composite, kWork), "XTaskQueueIsEmpty is true on both queues after dispatch");
  g_tq.Close(composite);
  g_tq.Close(q);
}

static void CheckThreadPool() {
  void* q = nullptr;
  g_tq.Create(kThreadPool, kThreadPool, &q);
  Counter c;
  c.gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_tq.Submit(q, kWork, &c, WaitForGate);
  Sleep(20);
  Check(!g_tq.IsEmpty(q, kWork), "XTaskQueueIsEmpty is false while a thread-pool callback is running");
  SetEvent(c.gate);
  Check(WaitEmpty(q, kWork, 5000) && c.calls == 1, "XTaskQueueIsEmpty turns true only after the thread-pool callback returned");
  CloseHandle(c.gate);

  Counter many;
  std::vector<std::thread> submitters;
  for (int t = 0; t < 4; t++)
    submitters.emplace_back([&] {
      for (int i = 0; i < 250; i++) g_tq.Submit(q, kCompletion, &many, Count);
    });
  for (auto& t : submitters) t.join();
  Check(WaitEmpty(q, kCompletion, 10000) && many.calls == 1000, "XTaskQueueIsEmpty stays consistent with 1000 callbacks submitted from four threads");
  g_tq.Close(q);
}

static void CheckGameEvent() {
  Api user = Get("IXUserImpl", kUser);
  AsyncBlock add{};
  user.At<HRESULT (*)(void*, uint32_t, AsyncBlock*)>(7, "XUserAddAsync")(user.self, 1, &add);
  g_tq.WaitAsync(&add);
  void* handle = nullptr;
  HRESULT hr = user.At<HRESULT (*)(void*, AsyncBlock*, void**)>(8, "XUserAddResult")(user.self, &add, &handle);
  Check(hr == S_OK && handle, "XUserAddAsync gives the local user");

  Api events = Get("IXGameEventImpl", kGameEvent);
  auto write = events.At<HRESULT (*)(void*, void*, const char*, const char*, const char*, const char*, const char*)>(3, "XGameEventWrite");
  const char* scid = "00000000-0000-0000-0000-000000000000";
  const char* session = "{11111111-2222-3333-4444-555555555555}";
  Check(write(events.self, handle, scid, session, "TestEvent", "{\"mode\":1}", "{\"score\":2}") == S_OK,
        "XGameEventWrite accepts an event (dropped: no telemetry service offline)");
  Check(write(events.self, handle, scid, session, "TestEvent", nullptr, nullptr) == S_OK, "XGameEventWrite accepts missing optional dimensions and measurements");
  Check(write(events.self, nullptr, scid, session, "TestEvent", nullptr, nullptr) == E_INVALIDARG, "XGameEventWrite rejects a null user");
  Check(write(events.self, handle, nullptr, session, "TestEvent", nullptr, nullptr) == E_INVALIDARG, "XGameEventWrite rejects a null service config id");
  Check(write(events.self, handle, scid, nullptr, "TestEvent", nullptr, nullptr) == E_INVALIDARG, "XGameEventWrite rejects a null play session id");
  Check(write(events.self, handle, scid, session, nullptr, nullptr, nullptr) == E_INVALIDARG, "XGameEventWrite rejects a null event name");
  user.At<void (*)(void*, void*)>(4, "XUserCloseHandle")(user.self, handle);
}

struct HandleEvent {
  void* handle;
  uint32_t type;
  uint32_t reason;
};
struct HandleRecorder {
  std::mutex lock;
  std::vector<HandleEvent> events;
  size_t Count(void* handle, uint32_t type, uint32_t reason) {
    std::lock_guard<std::mutex> guard(lock);
    size_t n = 0;
    for (const HandleEvent& e : events) n += e.handle == handle && e.type == type && e.reason == reason;
    return n;
  }
  size_t Size() {
    std::lock_guard<std::mutex> guard(lock);
    return events.size();
  }
};
static void CALLBACK RecordHandle(void* handle, uint32_t type, uint32_t reason, void* context) {
  auto* recorder = static_cast<HandleRecorder*>(context);
  std::lock_guard<std::mutex> guard(recorder->lock);
  recorder->events.push_back({handle, type, reason});
}

struct ErrorReports {
  std::mutex lock;
  std::vector<std::string> messages;
};
static bool CALLBACK RecordError(HRESULT, const char* message, void* context) {
  auto* reports = static_cast<ErrorReports*>(context);
  std::lock_guard<std::mutex> guard(reports->lock);
  reports->messages.emplace_back(message ? message : "");
  return false;
}

constexpr uint32_t kStoreContextHandle = 9;
constexpr uint32_t kTaskQueueHandle = 12;
constexpr uint32_t kUserHandle = 13;
constexpr uint32_t kCreated = 0;
constexpr uint32_t kDestroyed = 1;

static void CheckHandleTracking() {
  Api system3 = Get("IXSystemImpl3", kSystem3);
  auto track = system3.At<HRESULT (*)(void*, void (CALLBACK*)(void*, uint32_t, uint32_t, void*), void*)>(6, "XSystemHandleTrack");
  auto valid = system3.At<BOOLEAN (*)(void*, void*)>(7, "XSystemIsHandleValid");
  auto duplicate = g_tq.api.At<HRESULT (*)(void*, void*, void**)>(15, "XTaskQueueDuplicateHandle");
  auto processQueue = g_tq.api.At<bool (*)(void*, void**)>(25, "XTaskQueueGetCurrentProcessTaskQueue");

  HandleRecorder recorder;
  Check(track(system3.self, RecordHandle, &recorder) == S_OK, "XSystemHandleTrack accepts a callback");
  void* q = nullptr;
  Check(g_tq.Create(kManual, kManual, &q) == S_OK && recorder.Count(q, kTaskQueueHandle, kCreated) == 1 && valid(system3.self, q),
        "XTaskQueueCreate reports a created TaskQueue handle and XSystemIsHandleValid accepts it");
  void* copy = nullptr;
  Check(duplicate(g_tq.api.self, q, &copy) == S_OK && copy == q && recorder.Count(q, kTaskQueueHandle, kCreated) == 1,
        "XTaskQueueDuplicateHandle adds a reference without a second Created event");
  g_tq.Close(copy);
  Check(valid(system3.self, q) && recorder.Count(q, kTaskQueueHandle, kDestroyed) == 0, "closing one of two references keeps the handle valid");
  g_tq.Close(q);
  Check(!valid(system3.self, q) && recorder.Count(q, kTaskQueueHandle, kDestroyed) == 1,
        "closing the last reference reports Destroyed and XSystemIsHandleValid rejects the handle");

  void* process = nullptr;
  Check(processQueue(g_tq.api.self, &process) && process && valid(system3.self, process), "XTaskQueueGetCurrentProcessTaskQueue hands out a tracked handle");
  g_tq.Close(process);

  Api user = Get("IXUserImpl", kUser);
  AsyncBlock add{};
  user.At<HRESULT (*)(void*, uint32_t, AsyncBlock*)>(7, "XUserAddAsync")(user.self, 1, &add);
  g_tq.WaitAsync(&add);
  void* handle = nullptr;
  user.At<HRESULT (*)(void*, AsyncBlock*, void**)>(8, "XUserAddResult")(user.self, &add, &handle);
  Check(handle && recorder.Count(handle, kUserHandle, kCreated) == 1 && valid(system3.self, handle), "XUserAddResult reports a created User handle");
  void* userCopy = nullptr;
  user.At<HRESULT (*)(void*, void*, void**)>(3, "XUserDuplicateHandle")(user.self, handle, &userCopy);
  user.At<void (*)(void*, void*)>(4, "XUserCloseHandle")(user.self, userCopy);
  Check(valid(system3.self, handle) && recorder.Count(handle, kUserHandle, kDestroyed) == 0, "a duplicated User handle stays valid until its last close");
  user.At<void (*)(void*, void*)>(4, "XUserCloseHandle")(user.self, handle);
  Check(!valid(system3.self, handle) && recorder.Count(handle, kUserHandle, kDestroyed) == 1, "closing the last User handle reports Destroyed");

  Api store = Get("IXStoreImpl", kStore);
  void* context = nullptr;
  store.At<HRESULT (*)(void*, void*, void**)>(3, "XStoreCreateContext")(store.self, nullptr, &context);
  Check(context && recorder.Count(context, kStoreContextHandle, kCreated) == 1 && valid(system3.self, context),
        "XStoreCreateContext reports a created StoreContext handle");
  store.At<void (*)(void*, void*)>(4, "XStoreCloseContextHandle")(store.self, context);
  Check(!valid(system3.self, context) && recorder.Count(context, kStoreContextHandle, kDestroyed) == 1,
        "XStoreCloseContextHandle reports the StoreContext handle destroyed");

  Check(!valid(system3.self, nullptr), "XSystemIsHandleValid rejects a null handle");
  int notAHandle = 0;
  Check(!valid(system3.self, &notAHandle), "XSystemIsHandleValid rejects a pointer the runtime never handed out");

  Check(track(system3.self, nullptr, nullptr) == S_OK, "XSystemHandleTrack with a null callback turns the callback off");
  size_t before = recorder.Size();
  void* untracked = nullptr;
  g_tq.Create(kManual, kManual, &untracked);
  Check(valid(system3.self, untracked), "handles stay tracked for XSystemIsHandleValid with the callback off");
  g_tq.Close(untracked);
  Check(recorder.Size() == before, "no callback runs after tracking is turned off");

  Api error = Get("IXErrorImpl", kError);
  auto setCallback = error.At<void (*)(void*, bool (CALLBACK*)(HRESULT, const char*, void*), void*)>(4, "XErrorSetCallback");
  ErrorReports reports;
  setCallback(error.self, RecordError, &reports);
  void* leaked = nullptr;
  g_tq.Create(kManual, kManual, &leaked);
  auto uninitialize = reinterpret_cast<void (*)()>(GetProcAddress(GetModuleHandleA("xgameruntime.dll"), "UninitializeApiImpl"));
  uninitialize();
  bool reported = false;
  {
    std::lock_guard<std::mutex> guard(reports.lock);
    for (const std::string& m : reports.messages) reported = reported || m.find("TaskQueue handle") != std::string::npos;
  }
  Check(reported, "UninitializeApiImpl reports a handle that was never closed through XErrorReport");
  setCallback(error.self, nullptr, nullptr);
  g_tq.Close(leaked);
}

static void CheckPendingInvites() {
  const HRESULT kNotFound = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  Api activation = Get("IXGameActivationImpl", kActivation);
  auto acceptActivation = activation.At<HRESULT (*)(void*, const char*)>(5, "XGameActivationAcceptPendingInvite");
  Check(acceptActivation(activation.self, "ms-xbl-0://invite") == kNotFound,
        "XGameActivationAcceptPendingInvite reports no such pending invite (none is ever delivered offline)");
  Check(acceptActivation(activation.self, nullptr) == E_INVALIDARG && acceptActivation(activation.self, "") == E_INVALIDARG,
        "XGameActivationAcceptPendingInvite rejects a null or empty invite URI");
  Api invite = Get("IXGameInviteImpl2", kInvite2);
  auto acceptInvite = invite.At<HRESULT (*)(void*, const char*)>(7, "XGameInviteAcceptPendingInvite");
  Check(acceptInvite(invite.self, "ms-xbl-0://invite") == kNotFound, "XGameInviteAcceptPendingInvite reports no such pending invite");
  Check(acceptInvite(invite.self, nullptr) == E_INVALIDARG && acceptInvite(invite.self, "") == E_INVALIDARG,
        "XGameInviteAcceptPendingInvite rejects a null or empty invite URI");
}

static void CheckSilentUnregisters() {
  Token never{};
  Api activation = Get("IXGameActivationImpl", kActivation);
  auto unActivation = activation.At<BOOLEAN (*)(void*, Token, BOOLEAN)>(4, "XGameActivationUnregisterForEvent");
  Check(unActivation(activation.self, never, FALSE) && unActivation(activation.self, never, TRUE),
        "XGameActivationUnregisterForEvent answers true for a token it never issued, with or without wait");
  Api protocol = Get("IXGameProtocolImpl", kProtocol);
  auto unProtocol = protocol.At<BOOLEAN (*)(void*, Token, BOOLEAN)>(4, "XGameProtocolUnregisterForActivation");
  Check(unProtocol(protocol.self, never, FALSE) && unProtocol(protocol.self, never, TRUE),
        "XGameProtocolUnregisterForActivation answers true for a token it never issued, with or without wait");
  Api invite = Get("IXGameInviteImpl", kInvite);
  auto unInvite = invite.At<BOOLEAN (*)(void*, Token, BOOLEAN)>(4, "XGameInviteUnregisterForEvent");
  Check(unInvite(invite.self, never, FALSE) && unInvite(invite.self, never, TRUE),
        "XGameInviteUnregisterForEvent answers true for a token it never issued, with or without wait");
}

int main() {
  HMODULE module = LoadLibraryA("xgameruntime.dll");
  if (!module) {
    std::printf("FAIL LoadLibrary\n");
    return 1;
  }
  g_query = reinterpret_cast<QueryApi>(GetProcAddress(module, "QueryApiImpl"));
  if (!LoadTable()) {
    std::printf("FAIL table.json not found next to the test (run it from the build directory)\n");
    return 1;
  }
  g_tq.api = Get("IXThreadingImpl", kThreading);
  CheckManualQueue();
  CheckTermination();
  CheckDispatchAfterTerminate();
  CheckWaiters();
  CheckComposite();
  CheckThreadPool();
  CheckGameEvent();
  CheckHandleTracking();
  CheckPendingInvites();
  CheckSilentUnregisters();
  std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
  return g_failures ? 1 : 0;
}
