// ThreadManForUser, InterruptManager, Kernel_Library and the scheduler behind them.
//
// Guest threads are real host threads, but only the one holding the "CPU token" runs. The token
// moves when the running thread blocks, yields, exits, or when a higher priority thread becomes
// ready (checked at every import call). That keeps the guest's single-core, priority based
// semantics without needing preemption inside recompiled code.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "kernel_internal.h"
#include "platform.h"

namespace psp {
namespace k {

namespace {

struct ThreadExit {
  uint32_t status;
};
struct ThreadKilled {};
struct GameExitSignal {};

enum class TState { Running, Ready, Waiting, Dormant };
enum WaitType { WT_None = 0, WT_Sleep = 1, WT_Delay = 2, WT_Sema = 3, WT_EventFlag = 4, WT_Fpl = 7,
                WT_ThreadEnd = 9, WT_Mutex = 12 };

struct Run {
  std::condition_variable cv;
  bool killed = false;
};

struct Thread {
  int32_t uid = 0;
  std::string name;
  uint32_t entry = 0, attr = 0, stackSize = 0, stackAddr = 0, gp = 0;
  uint32_t initPrio = 0, prio = 0, argSize = 0, argAddr = 0;
  TState state = TState::Dormant;
  bool suspended = false;
  int waitType = 0;
  int32_t waitId = 0;
  bool hasTimeout = false;
  uint64_t wakeAt = 0;
  uint32_t timeoutAddr = 0;
  uint32_t timeoutRet = E_WAIT_TIMEOUT;
  bool waitDone = false;
  uint32_t waitRet = 0;
  bool cbWait = false, cbWake = false;
  std::list<Thread*>* waitList = nullptr;
  uint32_t need = 0, efBits = 0, efMode = 0, efOut = 0, ptrOut = 0;
  int wakeupCount = 0;
  uint32_t exitStatus = 0;
  bool intrOff = false;
  bool deleteOnExit = false;
  bool isMain = false;
  std::shared_ptr<Run> run;
  std::list<Thread*> endWaiters;
};

struct Sema {
  int32_t uid;
  std::string name;
  uint32_t attr, init;
  int32_t count, max;
  std::list<Thread*> waiters;
};
struct EventFlag {
  int32_t uid;
  std::string name;
  uint32_t attr, init, pattern;
  std::list<Thread*> waiters;
};
struct Mutex {
  int32_t uid;
  std::string name;
  uint32_t attr;
  Thread* owner = nullptr;
  int32_t count = 0;
  std::list<Thread*> waiters;
};
struct Fpl {
  int32_t uid;
  std::string name;
  uint32_t attr, blockSize, numBlocks, base;
  std::vector<uint32_t> freeBlocks;
  std::list<Thread*> waiters;
};
struct Callback {
  int32_t uid;
  std::string name;
  int32_t thread;
  uint32_t func, arg;
  uint32_t notifyCount = 0, notifyArg = 0;
};
struct Alarm {
  int32_t uid;
  uint64_t at;
  uint32_t handler, common;
};
struct IntrHandler {
  int intno, sub;
  uint32_t func, arg;
  bool enabled;
};
struct PendingCall {
  uint32_t func, a[4];
};

std::mutex g_m;
std::condition_variable g_doneCv;
bool g_done = false;
int g_exitCode = 0;

std::map<int32_t, std::unique_ptr<Thread>> g_threads;
std::vector<std::unique_ptr<Thread>> g_graveyard;
std::map<int32_t, std::unique_ptr<Sema>> g_semas;
std::map<int32_t, std::unique_ptr<EventFlag>> g_flags;
std::map<int32_t, std::unique_ptr<Mutex>> g_mutexes;
std::map<int32_t, std::unique_ptr<Fpl>> g_fpls;
std::map<int32_t, std::unique_ptr<Callback>> g_callbacks;
std::map<int32_t, Alarm> g_alarms;
std::vector<IntrHandler> g_intr;
std::deque<PendingCall> g_pending;

std::list<Thread*> g_ready;
Thread* g_cur = nullptr;
uint32_t g_gp = 0;
Cpu g_intrCpu;
uint32_t g_intrSp = 0;
uint32_t g_intrK0 = 0;

// $k0 points at a small per-thread block the game uses as thread-local storage.
uint32_t thread_local_block() {
  uint32_t a = sysmem_alloc(0x100, 0x40, false, "tls");
  if (a) std::memset(ptr(a), 0, 0x100);
  return a;
}
bool g_inIntr = false;
int32_t g_exitCb = 0;
bool g_exitNotified = false;
uint64_t g_quitDeadline = 0;

[[noreturn]] void block_forever() {
  std::mutex m;
  std::condition_variable cv;
  std::unique_lock<std::mutex> lk(m);
  cv.wait(lk, [] { return false; });
  for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

void signal_done() {
  std::lock_guard<std::mutex> lk(g_m);
  g_done = true;
  g_doneCv.notify_all();
}

template <class T>
T* find(std::map<int32_t, std::unique_ptr<T>>& m, uint32_t id) {
  auto it = m.find(static_cast<int32_t>(id));
  return it == m.end() ? nullptr : it->second.get();
}

Thread* thread_arg(uint32_t id) {
  if (id == 0) return g_cur;
  return find(g_threads, id);
}

// ---- ready queue ----

Thread* pick_ready() {
  Thread* best = nullptr;
  for (Thread* t : g_ready) {
    if (t->suspended) continue;
    if (!best || t->prio < best->prio) best = t;
  }
  return best;
}

void make_ready(Thread* t) {
  t->state = TState::Ready;
  t->waitType = 0;
  g_ready.push_back(t);
}

void enqueue_waiter(std::list<Thread*>& list, Thread* t, bool byPriority) {
  t->waitList = &list;
  if (byPriority) {
    auto it = list.begin();
    while (it != list.end() && (*it)->prio <= t->prio) ++it;
    list.insert(it, t);
  } else {
    list.push_back(t);
  }
}

void release_wait(Thread* t, uint32_t ret) {
  if (t->waitList) {
    t->waitList->remove(t);
    t->waitList = nullptr;
  }
  if (t->hasTimeout && t->timeoutAddr && ret != t->timeoutRet) {
    uint64_t n = now_us();
    put32(t->timeoutAddr, t->wakeAt > n ? static_cast<uint32_t>(t->wakeAt - n) : 0);
  }
  t->waitRet = ret;
  t->waitDone = true;
  t->hasTimeout = false;
  if (t->state == TState::Waiting) make_ready(t);
}

// ---- interrupt context ----

uint32_t run_intr_call(uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
  bool was = g_inIntr;
  g_inIntr = true;
  Cpu& ic = g_intrCpu;
  ic.r[28] = g_gp;
  ic.r[26] = g_intrK0;
  ic.r[29] = g_intrSp;
  ic.r[31] = 0;
  ic.r[4] = a0;
  ic.r[5] = a1;
  ic.r[6] = a2;
  ic.r[7] = a3;
  try {
    call(ic, func);
  } catch (...) {
    g_inIntr = was;
    throw;
  }
  g_inIntr = was;
  return ic.r[2];
}

void handle_quit(uint64_t now) {
  if (!g_exitNotified) {
    g_exitNotified = true;
    if (g_exitCb) {
      notify_callback(g_exitCb, 0);
      g_quitDeadline = now + 3000000;
      return;
    }
    game_exit();
  }
  if (g_quitDeadline && now >= g_quitDeadline) game_exit();
}

void expire_timers(uint64_t now) {
  for (auto& kv : g_threads) {
    Thread* t = kv.second.get();
    if (t->state == TState::Waiting && t->hasTimeout && t->wakeAt <= now) {
      if (t->timeoutAddr) put32(t->timeoutAddr, 0);
      release_wait(t, t->timeoutRet);
    }
  }
  if (g_alarms.empty() || g_inIntr || (g_cur && g_cur->intrOff)) return;
  std::vector<int32_t> due;
  for (auto& kv : g_alarms)
    if (kv.second.at <= now) due.push_back(kv.first);
  for (int32_t id : due) {
    auto it = g_alarms.find(id);
    if (it == g_alarms.end()) continue;
    Alarm a = it->second;
    uint32_t r = run_intr_call(a.handler, a.common, 0, 0, 0);
    it = g_alarms.find(id);
    if (it == g_alarms.end()) continue;
    if (static_cast<int32_t>(r) > 0) it->second.at = now_us() + r;
    else g_alarms.erase(it);
  }
}

}  // namespace

void service_events() {
  uint64_t now = now_us();
  if (plat::quit_requested()) handle_quit(now);
  expire_timers(now);
  if (g_inIntr || (g_cur && g_cur->intrOff)) return;
  while (!g_pending.empty()) {
    PendingCall p = g_pending.front();
    g_pending.pop_front();
    run_intr_call(p.func, p.a[0], p.a[1], p.a[2], p.a[3]);
  }
  display_service();
}

namespace {

uint64_t next_deadline() {
  uint64_t dl = UINT64_MAX;
  for (auto& kv : g_threads) {
    Thread* t = kv.second.get();
    if (t->state == TState::Waiting && t->hasTimeout) dl = std::min(dl, t->wakeAt);
  }
  for (auto& kv : g_alarms) dl = std::min(dl, kv.second.at);
  dl = std::min(dl, display_next_event_us());
  if (g_quitDeadline) dl = std::min(dl, g_quitDeadline);
  if (plat::quit_requested()) dl = std::min(dl, now_us() + 20000);
  if (!g_pending.empty()) dl = 0;
  return dl;
}

void report_stuck() {
  std::fprintf(stderr, "all guest threads are blocked; nothing can wake them\n");
  for (auto& kv : g_threads) {
    Thread* t = kv.second.get();
    if (t->state == TState::Dormant) continue;
    std::fprintf(stderr, "  thread '%s' (prio %u) waiting type %d on %X\n", t->name.c_str(), t->prio, t->waitType,
                 t->waitId);
  }
}

// Gives the CPU to the best ready thread. self must already be queued/blocked as appropriate.
// With finished set, self is done for good and the call returns after the hand-off.
void reschedule_from(Thread* self, bool finished = false) {
  Thread* next = nullptr;
  for (;;) {
    service_events();
    next = pick_ready();
    if (next) break;
    uint64_t dl = next_deadline();
    if (dl == UINT64_MAX) {
      report_stuck();
      g_exitCode = 3;
      if (finished) {
        signal_done();
        return;
      }
      if (self->isMain) throw GameExitSignal{};
      signal_done();
      block_forever();
    }
    uint64_t n = now_us();
    if (dl > n) std::this_thread::sleep_for(std::chrono::microseconds(std::min<uint64_t>(dl - n, 5000)));
  }
  g_ready.remove(next);
  next->state = TState::Running;
  if (next == self) return;
  std::shared_ptr<Run> run = self->run;
  std::unique_lock<std::mutex> lk(g_m);
  g_cur = next;
  next->run->cv.notify_all();
  if (finished) return;
  run->cv.wait(lk, [&] { return (g_cur == self && self->run == run) || run->killed; });
  if (run->killed) throw ThreadKilled{};
}

uint32_t do_wait(Cpu& c, Thread* t, bool cb) {
  t->waitDone = false;
  t->cbWait = cb;
  t->cbWake = false;
  t->state = TState::Waiting;
  for (;;) {
    reschedule_from(t);
    if (t->waitDone) break;
    if (t->cbWake) {
      t->cbWake = false;
      run_callbacks(c);
      if (t->waitDone) break;
    }
    t->state = TState::Waiting;
  }
  t->cbWait = false;
  t->waitId = 0;
  return t->waitRet;
}

void setup_wait(Thread* t, int type, int32_t id, uint32_t timeoutAddr, bool useTimeout, uint32_t timeoutRet) {
  t->waitType = type;
  t->waitId = id;
  t->timeoutAddr = timeoutAddr;
  t->timeoutRet = timeoutRet;
  t->hasTimeout = useTimeout;
  if (useTimeout) t->wakeAt = now_us() + (timeoutAddr ? lw(timeoutAddr) : 0);
}

}  // namespace

void preempt_check() {
  Thread* t = g_cur;
  if (!t || t->intrOff || g_inIntr) return;
  Thread* b = pick_ready();
  if (b && b->prio < t->prio) {
    t->state = TState::Ready;
    g_ready.push_front(t);
    reschedule_from(t);
  }
}

void sleep_until(Cpu& c, uint64_t wakeUs, bool cb) {
  Thread* t = g_cur;
  setup_wait(t, WT_Delay, 0, 0, true, 0);
  t->wakeAt = wakeUs;
  do_wait(c, t, cb);
}

uint32_t call_guest(Cpu& c, uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
  Cpu t = c;
  t.r[4] = a0;
  t.r[5] = a1;
  t.r[6] = a2;
  t.r[7] = a3;
  t.r[29] -= 0x80;
  t.r[31] = 0;
  call(t, addr);
  return t.r[2];
}

void raise_guest_call(uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
  g_pending.push_back({addr, {a0, a1, a2, a3}});
}

bool intr_handlers_registered(int intno) {
  for (auto& h : g_intr)
    if (h.intno == intno && h.enabled) return true;
  return false;
}

void dispatch_intr(int intno) {
  if (g_inIntr) return;
  std::vector<IntrHandler> hs;
  for (auto& h : g_intr)
    if (h.intno == intno && h.enabled) hs.push_back(h);
  for (auto& h : hs) run_intr_call(h.func, h.sub, h.arg, 0, 0);
}

bool run_callbacks(Cpu& c) {
  bool any = false;
  std::vector<int32_t> ids;
  for (auto& kv : g_callbacks)
    if (kv.second->thread == g_cur->uid && kv.second->notifyCount) ids.push_back(kv.first);
  for (int32_t id : ids) {
    Callback* cb = find(g_callbacks, id);
    if (!cb || !cb->notifyCount) continue;
    uint32_t count = cb->notifyCount, narg = cb->notifyArg, func = cb->func, arg = cb->arg;
    cb->notifyCount = 0;
    uint32_t r = call_guest(c, func, count, narg, arg);
    any = true;
    if (r != 0) g_callbacks.erase(id);
  }
  return any;
}

void notify_callback(int32_t cbid, uint32_t arg) {
  Callback* cb = find(g_callbacks, cbid);
  if (!cb) return;
  ++cb->notifyCount;
  cb->notifyArg = arg;
  Thread* t = find(g_threads, cb->thread);
  if (t && t->state == TState::Waiting && t->cbWait) {
    t->cbWake = true;
    t->state = TState::Ready;
    g_ready.push_back(t);
  }
}

void set_exit_callback(int32_t cbid) { g_exitCb = cbid; }

void request_exit(int code) { g_exitCode = code; }

void game_exit() { throw GameExitSignal{}; }

namespace {

void poll_hook(Cpu&) {
  service_events();
  preempt_check();
}

// ---- thread lifecycle ----

void finish_thread(Thread* t, uint32_t status) {
  t->exitStatus = status;
  t->state = TState::Dormant;
  t->waitType = 0;
  std::list<Thread*> ew;
  ew.swap(t->endWaiters);
  for (Thread* w : ew) {
    w->waitList = nullptr;
    release_wait(w, status);
  }
  if (t->deleteOnExit) {
    if (t->stackAddr) sysmem_free(t->stackAddr);
    auto it = g_threads.find(t->uid);
    if (it != g_threads.end()) {
      g_graveyard.push_back(std::move(it->second));
      g_threads.erase(it);
    }
  }
  reschedule_from(t, true);
}

void fatal(Thread* t, const char* what) {
  std::fprintf(stderr, "stopped in thread '%s': %s\n", t->name.c_str(), what);
  std::fflush(stderr);
  g_exitCode = 1;
}

void thread_entry(Thread* t, std::shared_ptr<Run> run) {
  {
    std::unique_lock<std::mutex> lk(g_m);
    run->cv.wait(lk, [&] { return (g_cur == t && t->run == run) || run->killed; });
  }
  if (run->killed) return;
  Cpu cpu;
  cpu.r[28] = t->gp;
  cpu.r[26] = thread_local_block();
  cpu.r[29] = t->stackAddr + t->stackSize - ((t->argSize + 15) & ~15u) - 0x40;
  cpu.r[4] = t->argSize;
  cpu.r[5] = t->argAddr;
  uint32_t status = 0;
  try {
    try {
      call(cpu, t->entry);
      status = cpu.r[2];
    } catch (ThreadExit& e) {
      status = e.status;
    }
    finish_thread(t, status);
  } catch (ThreadKilled&) {
  } catch (GameExitSignal&) {
    signal_done();
    block_forever();
  } catch (std::exception& e) {
    fatal(t, e.what());
    signal_done();
    block_forever();
  }
}

void h_create_thread(Cpu& c) {
  std::string name = cstr(arg(c, 0), 31);
  uint32_t entry = arg(c, 1), prio = arg(c, 2), stack = arg(c, 3), attr = arg(c, 4);
  if (prio < 1 || prio > 0x7F) return ret(c, E_ILLEGAL_PRIORITY);
  if (!entry) return ret(c, E_ILLEGAL_ENTRY);
  if (stack < 0x200) stack = 0x200;
  stack = (stack + 0xFF) & ~0xFFu;
  uint32_t mem = sysmem_alloc(stack, 0x100, true, name.c_str());
  if (!mem) return ret(c, E_NO_MEMORY);
  auto t = std::make_unique<Thread>();
  t->uid = alloc_uid();
  t->name = name;
  t->entry = entry;
  t->attr = attr;
  t->stackSize = stack;
  t->stackAddr = mem;
  t->gp = g_gp;
  t->initPrio = t->prio = prio;
  int32_t uid = t->uid;
  g_threads[uid] = std::move(t);
  ret(c, static_cast<uint32_t>(uid));
}

void h_start_thread(Cpu& c) {
  Thread* t = find(g_threads, arg(c, 0));
  if (!t) return ret(c, E_UNKNOWN_UID);
  if (t->state != TState::Dormant) return ret(c, E_NOT_DORMANT);
  uint32_t argSize = arg(c, 1), argp = arg(c, 2);
  t->argSize = argSize;
  t->argAddr = 0;
  t->prio = t->initPrio;
  t->suspended = false;
  t->wakeupCount = 0;
  if (argSize && argp) {
    uint32_t dst = t->stackAddr + t->stackSize - ((argSize + 15) & ~15u);
    for (uint32_t i = 0; i < argSize; ++i) sb(dst + i, lbu(argp + i));
    t->argAddr = dst;
  }
  t->run = std::make_shared<Run>();
  make_ready(t);
  std::thread(thread_entry, t, t->run).detach();
  ret(c, 0);
  preempt_check();
}

void h_exit_thread(Cpu& c) { throw ThreadExit{arg(c, 0)}; }

void h_exit_delete_thread(Cpu& c) {
  g_cur->deleteOnExit = true;
  throw ThreadExit{arg(c, 0)};
}

void terminate(Thread* t, uint32_t status) {
  g_ready.remove(t);
  if (t->waitList) {
    t->waitList->remove(t);
    t->waitList = nullptr;
  }
  t->hasTimeout = false;
  t->state = TState::Dormant;
  t->exitStatus = status;
  {
    std::lock_guard<std::mutex> lk(g_m);
    if (t->run) {
      t->run->killed = true;
      t->run->cv.notify_all();
    }
  }
  std::list<Thread*> ew;
  ew.swap(t->endWaiters);
  for (Thread* w : ew) {
    w->waitList = nullptr;
    release_wait(w, status);
  }
}

void h_terminate_thread(Cpu& c) {
  Thread* t = find(g_threads, arg(c, 0));
  if (!t) return ret(c, E_UNKNOWN_UID);
  if (t == g_cur) return ret(c, E_ILLEGAL_THREAD);
  if (t->state == TState::Dormant) return ret(c, E_DORMANT);
  terminate(t, E_THREAD_TERMINATED);
  ret(c, 0);
}

void h_delete_thread(Cpu& c) {
  Thread* t = find(g_threads, arg(c, 0));
  if (!t) return ret(c, E_UNKNOWN_UID);
  if (t->state != TState::Dormant) return ret(c, E_NOT_DORMANT);
  if (t->stackAddr) sysmem_free(t->stackAddr);
  auto it = g_threads.find(t->uid);
  g_graveyard.push_back(std::move(it->second));
  g_threads.erase(it);
  ret(c, 0);
}

void h_terminate_delete_thread(Cpu& c) {
  Thread* t = find(g_threads, arg(c, 0));
  if (!t) return ret(c, E_UNKNOWN_UID);
  if (t == g_cur) return ret(c, E_ILLEGAL_THREAD);
  if (t->state != TState::Dormant) terminate(t, E_THREAD_TERMINATED);
  h_delete_thread(c);
}

void h_get_thread_id(Cpu& c) { ret(c, static_cast<uint32_t>(g_cur->uid)); }

void h_get_thread_prio(Cpu& c) { ret(c, g_cur->prio); }

void h_change_thread_prio(Cpu& c) {
  Thread* t = thread_arg(arg(c, 0));
  uint32_t p = arg(c, 1);
  if (!t) return ret(c, E_UNKNOWN_UID);
  if (t->state == TState::Dormant) return ret(c, E_DORMANT);
  if (p == 0) p = g_cur->prio;
  if (p < 1 || p > 0x7F) return ret(c, E_ILLEGAL_PRIORITY);
  t->prio = p;
  ret(c, 0);
  preempt_check();
}

void h_rotate_ready_queue(Cpu& c) {
  uint32_t p = arg(c, 0);
  if (p == 0) p = g_cur->prio;
  ret(c, 0);
  if (p == g_cur->prio) {
    g_cur->state = TState::Ready;
    g_ready.push_back(g_cur);
    reschedule_from(g_cur);
    return;
  }
  for (Thread* t : g_ready) {
    if (t->prio == p) {
      g_ready.remove(t);
      g_ready.push_back(t);
      break;
    }
  }
}

void h_suspend_thread(Cpu& c) {
  Thread* t = find(g_threads, arg(c, 0));
  if (!t) return ret(c, E_UNKNOWN_UID);
  if (t->state == TState::Dormant) return ret(c, E_DORMANT);
  t->suspended = true;
  ret(c, 0);
  if (t == g_cur) {
    t->state = TState::Ready;
    g_ready.push_back(t);
    reschedule_from(t);
  }
}

void h_resume_thread(Cpu& c) {
  Thread* t = find(g_threads, arg(c, 0));
  if (!t) return ret(c, E_UNKNOWN_UID);
  if (!t->suspended) return ret(c, E_NOT_SUSPEND);
  t->suspended = false;
  ret(c, 0);
  preempt_check();
}

void h_sleep_thread_impl(Cpu& c, bool cb) {
  Thread* t = g_cur;
  if (t->wakeupCount > 0) {
    --t->wakeupCount;
    return ret(c, 0);
  }
  setup_wait(t, WT_Sleep, 0, 0, false, 0);
  ret(c, do_wait(c, t, cb));
}
void h_sleep_thread(Cpu& c) { h_sleep_thread_impl(c, false); }
void h_sleep_thread_cb(Cpu& c) { h_sleep_thread_impl(c, true); }

void h_wakeup_thread(Cpu& c) {
  Thread* t = find(g_threads, arg(c, 0));
  if (!t) return ret(c, E_UNKNOWN_UID);
  if (t == g_cur || t->state == TState::Dormant) return ret(c, t == g_cur ? E_ILLEGAL_THREAD : E_DORMANT);
  if (t->state == TState::Waiting && t->waitType == WT_Sleep) release_wait(t, 0);
  else ++t->wakeupCount;
  ret(c, 0);
  preempt_check();
}

void h_delay_impl(Cpu& c, bool cb) {
  uint32_t us = arg(c, 0);
  Thread* t = g_cur;
  ret(c, 0);
  if (us == 0) {
    t->state = TState::Ready;
    g_ready.push_back(t);
    reschedule_from(t);
    return;
  }
  setup_wait(t, WT_Delay, 0, 0, true, 0);
  t->wakeAt = now_us() + us;
  do_wait(c, t, cb);
}
void h_delay(Cpu& c) { h_delay_impl(c, false); }
void h_delay_cb(Cpu& c) { h_delay_impl(c, true); }

void h_wait_end_impl(Cpu& c, bool cb) {
  Thread* t = find(g_threads, arg(c, 0));
  if (!t) return ret(c, E_UNKNOWN_UID);
  if (t == g_cur) return ret(c, E_ILLEGAL_THREAD);
  if (t->state == TState::Dormant) return ret(c, t->exitStatus);
  Thread* me = g_cur;
  uint32_t ta = arg(c, 1);
  enqueue_waiter(t->endWaiters, me, false);
  setup_wait(me, WT_ThreadEnd, t->uid, ta, ta != 0, E_WAIT_TIMEOUT);
  ret(c, do_wait(c, me, cb));
}
void h_wait_end(Cpu& c) { h_wait_end_impl(c, false); }
void h_wait_end_cb(Cpu& c) { h_wait_end_impl(c, true); }

void h_thread_exit_status(Cpu& c) {
  Thread* t = find(g_threads, arg(c, 0));
  if (!t) return ret(c, E_UNKNOWN_UID);
  ret(c, t->state == TState::Dormant ? t->exitStatus : E_NOT_DORMANT);
}

void h_stack_free(Cpu& c) {
  Thread* t = thread_arg(arg(c, 0));
  ret(c, t ? t->stackSize - 0x200 : E_UNKNOWN_UID);
}

uint32_t status_bits(Thread* t) {
  switch (t->state) {
    case TState::Running: return 1;
    case TState::Ready: return t->suspended ? 8 : 2;
    case TState::Waiting: return t->suspended ? 12 : 4;
    case TState::Dormant: return 0x10;
  }
  return 0;
}

void h_refer_thread_status(Cpu& c) {
  Thread* t = find(g_threads, arg(c, 0));
  uint32_t p = arg(c, 1);
  if (!t) return ret(c, E_UNKNOWN_UID);
  if (!p) return ret(c, E_ERROR);
  std::memset(ptr(p), 0, 104);
  sw(p, 104);
  write_cstr(p + 4, t->name, 32);
  sw(p + 36, t->attr);
  sw(p + 40, status_bits(t));
  sw(p + 44, t->entry);
  sw(p + 48, t->stackAddr);
  sw(p + 52, t->stackSize);
  sw(p + 56, t->gp);
  sw(p + 60, t->initPrio);
  sw(p + 64, t->prio);
  sw(p + 68, static_cast<uint32_t>(t->waitType));
  sw(p + 72, static_cast<uint32_t>(t->waitId));
  sw(p + 76, static_cast<uint32_t>(t->wakeupCount));
  sw(p + 80, t->exitStatus);
  ret(c, 0);
}

void h_refer_thread_run_status(Cpu& c) {
  Thread* t = thread_arg(arg(c, 0));
  uint32_t p = arg(c, 1);
  if (!t) return ret(c, E_UNKNOWN_UID);
  if (!p) return ret(c, E_ERROR);
  std::memset(ptr(p), 0, 40);
  sw(p, 40);
  sw(p + 4, status_bits(t));
  sw(p + 8, t->prio);
  sw(p + 12, static_cast<uint32_t>(t->waitType));
  sw(p + 16, static_cast<uint32_t>(t->waitId));
  ret(c, 0);
}

void h_release_wait(Cpu& c) {
  Thread* t = find(g_threads, arg(c, 0));
  if (!t) return ret(c, E_UNKNOWN_UID);
  if (t->state != TState::Waiting) return ret(c, E_ERROR);
  release_wait(t, E_WAIT_CANCEL);
  ret(c, 0);
  preempt_check();
}

// ---- semaphores ----

void h_create_sema(Cpu& c) {
  auto s = std::make_unique<Sema>();
  s->uid = alloc_uid();
  s->name = cstr(arg(c, 0), 31);
  s->attr = arg(c, 1);
  s->init = arg(c, 2);
  s->count = static_cast<int32_t>(arg(c, 2));
  s->max = static_cast<int32_t>(arg(c, 3));
  int32_t uid = s->uid;
  g_semas[uid] = std::move(s);
  ret(c, static_cast<uint32_t>(uid));
}

void h_delete_sema(Cpu& c) {
  Sema* s = find(g_semas, arg(c, 0));
  if (!s) return ret(c, E_UNKNOWN_UID);
  std::list<Thread*> w;
  w.swap(s->waiters);
  for (Thread* t : w) {
    t->waitList = nullptr;
    release_wait(t, E_WAIT_DELETE);
  }
  g_semas.erase(s->uid);
  ret(c, 0);
  preempt_check();
}

void sema_dispatch(Sema* s) {
  for (auto it = s->waiters.begin(); it != s->waiters.end();) {
    Thread* t = *it;
    ++it;
    if (static_cast<int32_t>(t->need) <= s->count) {
      s->count -= static_cast<int32_t>(t->need);
      t->waitList = nullptr;
      s->waiters.remove(t);
      release_wait(t, 0);
    }
  }
}

void h_signal_sema(Cpu& c) {
  Sema* s = find(g_semas, arg(c, 0));
  int32_t n = static_cast<int32_t>(arg(c, 1));
  if (!s) return ret(c, E_UNKNOWN_UID);
  if (n < 0) return ret(c, E_ILLEGAL_COUNT);
  if (s->count + n > s->max) return ret(c, E_SEMA_OVF);
  s->count += n;
  sema_dispatch(s);
  ret(c, 0);
  preempt_check();
}

void sema_wait(Cpu& c, bool cb) {
  Sema* s = find(g_semas, arg(c, 0));
  int32_t n = static_cast<int32_t>(arg(c, 1));
  uint32_t ta = arg(c, 2);
  if (!s) return ret(c, E_UNKNOWN_UID);
  if (n <= 0) return ret(c, E_ILLEGAL_COUNT);
  if (cb) run_callbacks(c);
  if (s->count >= n && s->waiters.empty()) {
    s->count -= n;
    return ret(c, 0);
  }
  if (ta && lw(ta) == 0) return ret(c, E_WAIT_TIMEOUT);
  Thread* t = g_cur;
  t->need = static_cast<uint32_t>(n);
  enqueue_waiter(s->waiters, t, (s->attr & 0x100) != 0);
  setup_wait(t, WT_Sema, s->uid, ta, ta != 0, E_WAIT_TIMEOUT);
  ret(c, do_wait(c, t, cb));
}
void h_wait_sema(Cpu& c) { sema_wait(c, false); }
void h_wait_sema_cb(Cpu& c) { sema_wait(c, true); }

void h_poll_sema(Cpu& c) {
  Sema* s = find(g_semas, arg(c, 0));
  int32_t n = static_cast<int32_t>(arg(c, 1));
  if (!s) return ret(c, E_UNKNOWN_UID);
  if (n <= 0) return ret(c, E_ILLEGAL_COUNT);
  if (s->count < n || !s->waiters.empty()) return ret(c, E_SEMA_ZERO);
  s->count -= n;
  ret(c, 0);
}

void h_refer_sema(Cpu& c) {
  Sema* s = find(g_semas, arg(c, 0));
  uint32_t p = arg(c, 1);
  if (!s) return ret(c, E_UNKNOWN_UID);
  sw(p, 56);
  write_cstr(p + 4, s->name, 32);
  sw(p + 36, s->attr);
  sw(p + 40, s->init);
  sw(p + 44, static_cast<uint32_t>(s->count));
  sw(p + 48, static_cast<uint32_t>(s->max));
  sw(p + 52, static_cast<uint32_t>(s->waiters.size()));
  ret(c, 0);
}

// ---- event flags ----

bool ef_match(uint32_t pattern, uint32_t bits, uint32_t mode) {
  return (mode & 1) ? (pattern & bits) != 0 : (pattern & bits) == bits;
}

void ef_apply(EventFlag* e, uint32_t bits, uint32_t mode, uint32_t out) {
  put32(out, e->pattern);
  if (mode & 0x10) e->pattern = 0;
  if (mode & 0x20) e->pattern &= ~bits;
}

void h_create_evf(Cpu& c) {
  auto e = std::make_unique<EventFlag>();
  e->uid = alloc_uid();
  e->name = cstr(arg(c, 0), 31);
  e->attr = arg(c, 1);
  e->init = e->pattern = arg(c, 2);
  int32_t uid = e->uid;
  g_flags[uid] = std::move(e);
  ret(c, static_cast<uint32_t>(uid));
}

void h_delete_evf(Cpu& c) {
  EventFlag* e = find(g_flags, arg(c, 0));
  if (!e) return ret(c, E_UNKNOWN_UID);
  std::list<Thread*> w;
  w.swap(e->waiters);
  for (Thread* t : w) {
    t->waitList = nullptr;
    release_wait(t, E_WAIT_DELETE);
  }
  g_flags.erase(e->uid);
  ret(c, 0);
  preempt_check();
}

void h_set_evf(Cpu& c) {
  EventFlag* e = find(g_flags, arg(c, 0));
  if (!e) return ret(c, E_UNKNOWN_UID);
  e->pattern |= arg(c, 1);
  for (auto it = e->waiters.begin(); it != e->waiters.end();) {
    Thread* t = *it;
    ++it;
    if (ef_match(e->pattern, t->efBits, t->efMode)) {
      ef_apply(e, t->efBits, t->efMode, t->efOut);
      e->waiters.remove(t);
      t->waitList = nullptr;
      release_wait(t, 0);
    }
  }
  ret(c, 0);
  preempt_check();
}

void h_clear_evf(Cpu& c) {
  EventFlag* e = find(g_flags, arg(c, 0));
  if (!e) return ret(c, E_UNKNOWN_UID);
  e->pattern &= arg(c, 1);
  ret(c, 0);
}

void evf_wait(Cpu& c, bool cb, bool poll) {
  EventFlag* e = find(g_flags, arg(c, 0));
  uint32_t bits = arg(c, 1), mode = arg(c, 2), out = arg(c, 3), ta = poll ? 0 : arg(c, 4);
  if (!e) return ret(c, E_UNKNOWN_UID);
  if (bits == 0) return ret(c, E_ILLEGAL_MASK);
  if (mode & ~0x31u) return ret(c, E_ILLEGAL_MODE);
  if (cb) run_callbacks(c);
  if (ef_match(e->pattern, bits, mode)) {
    ef_apply(e, bits, mode, out);
    return ret(c, 0);
  }
  if (poll) return ret(c, E_EVF_COND);
  if (!(e->attr & 0x200) && !e->waiters.empty()) return ret(c, E_EVF_MULTI);
  put32(out, e->pattern);
  if (ta && lw(ta) == 0) return ret(c, E_WAIT_TIMEOUT);
  Thread* t = g_cur;
  t->efBits = bits;
  t->efMode = mode;
  t->efOut = out;
  enqueue_waiter(e->waiters, t, false);
  setup_wait(t, WT_EventFlag, e->uid, ta, ta != 0, E_WAIT_TIMEOUT);
  ret(c, do_wait(c, t, cb));
}
void h_wait_evf(Cpu& c) { evf_wait(c, false, false); }
void h_wait_evf_cb(Cpu& c) { evf_wait(c, true, false); }
void h_poll_evf(Cpu& c) { evf_wait(c, false, true); }

void h_refer_evf(Cpu& c) {
  EventFlag* e = find(g_flags, arg(c, 0));
  uint32_t p = arg(c, 1);
  if (!e) return ret(c, E_UNKNOWN_UID);
  sw(p, 52);
  write_cstr(p + 4, e->name, 32);
  sw(p + 36, e->attr);
  sw(p + 40, e->init);
  sw(p + 44, e->pattern);
  sw(p + 48, static_cast<uint32_t>(e->waiters.size()));
  ret(c, 0);
}

// ---- mutexes ----

void h_create_mutex(Cpu& c) {
  auto m = std::make_unique<Mutex>();
  m->uid = alloc_uid();
  m->name = cstr(arg(c, 0), 31);
  m->attr = arg(c, 1);
  int32_t uid = m->uid;
  if (arg(c, 2) > 0) {
    m->owner = g_cur;
    m->count = static_cast<int32_t>(arg(c, 2));
  }
  g_mutexes[uid] = std::move(m);
  ret(c, static_cast<uint32_t>(uid));
}

void h_delete_mutex(Cpu& c) {
  Mutex* m = find(g_mutexes, arg(c, 0));
  if (!m) return ret(c, E_UNKNOWN_UID);
  std::list<Thread*> w;
  w.swap(m->waiters);
  for (Thread* t : w) {
    t->waitList = nullptr;
    release_wait(t, E_WAIT_DELETE);
  }
  g_mutexes.erase(m->uid);
  ret(c, 0);
  preempt_check();
}

void mutex_lock(Cpu& c, bool cb, bool poll) {
  Mutex* m = find(g_mutexes, arg(c, 0));
  int32_t n = static_cast<int32_t>(arg(c, 1));
  uint32_t ta = poll ? 0 : arg(c, 2);
  if (!m) return ret(c, E_UNKNOWN_UID);
  if (n <= 0) return ret(c, E_ILLEGAL_COUNT);
  Thread* me = g_cur;
  if (cb) run_callbacks(c);
  if (!m->owner) {
    m->owner = me;
    m->count = n;
    return ret(c, 0);
  }
  if (m->owner == me) {
    if (!(m->attr & 0x200)) return ret(c, E_MUTEX_LOCKED);
    m->count += n;
    return ret(c, 0);
  }
  if (poll) return ret(c, E_MUTEX_LOCKED);
  if (ta && lw(ta) == 0) return ret(c, E_WAIT_TIMEOUT);
  me->need = static_cast<uint32_t>(n);
  enqueue_waiter(m->waiters, me, (m->attr & 0x100) != 0);
  setup_wait(me, WT_Mutex, m->uid, ta, ta != 0, E_WAIT_TIMEOUT);
  ret(c, do_wait(c, me, cb));
}
void h_lock_mutex(Cpu& c) { mutex_lock(c, false, false); }
void h_lock_mutex_cb(Cpu& c) { mutex_lock(c, true, false); }
void h_trylock_mutex(Cpu& c) { mutex_lock(c, false, true); }

void h_unlock_mutex(Cpu& c) {
  Mutex* m = find(g_mutexes, arg(c, 0));
  int32_t n = static_cast<int32_t>(arg(c, 1));
  if (!m) return ret(c, E_UNKNOWN_UID);
  if (m->owner != g_cur) return ret(c, E_MUTEX_UNLOCKED);
  m->count -= n;
  if (m->count < 0) m->count = 0;
  if (m->count == 0) {
    m->owner = nullptr;
    if (!m->waiters.empty()) {
      Thread* t = m->waiters.front();
      m->waiters.pop_front();
      t->waitList = nullptr;
      m->owner = t;
      m->count = static_cast<int32_t>(t->need);
      release_wait(t, 0);
    }
  }
  ret(c, 0);
  preempt_check();
}

// ---- fixed pools ----

void h_create_fpl(Cpu& c) {
  uint32_t blockSize = arg(c, 3), num = arg(c, 4);
  if (!blockSize || !num) return ret(c, E_ERROR);
  uint32_t bs = (blockSize + 3) & ~3u;
  uint32_t base = sysmem_alloc(bs * num, 0x40, false, "fpl");
  if (!base) return ret(c, E_NO_MEMORY);
  auto f = std::make_unique<Fpl>();
  f->uid = alloc_uid();
  f->name = cstr(arg(c, 0), 31);
  f->attr = arg(c, 2);
  f->blockSize = bs;
  f->numBlocks = num;
  f->base = base;
  for (uint32_t i = num; i-- > 0;) f->freeBlocks.push_back(base + i * bs);
  int32_t uid = f->uid;
  g_fpls[uid] = std::move(f);
  ret(c, static_cast<uint32_t>(uid));
}

void h_delete_fpl(Cpu& c) {
  Fpl* f = find(g_fpls, arg(c, 0));
  if (!f) return ret(c, E_UNKNOWN_UID);
  std::list<Thread*> w;
  w.swap(f->waiters);
  for (Thread* t : w) {
    t->waitList = nullptr;
    release_wait(t, E_WAIT_DELETE);
  }
  sysmem_free(f->base);
  g_fpls.erase(f->uid);
  ret(c, 0);
  preempt_check();
}

void fpl_alloc(Cpu& c, bool cb, bool poll) {
  Fpl* f = find(g_fpls, arg(c, 0));
  uint32_t out = arg(c, 1), ta = poll ? 0 : arg(c, 2);
  if (!f) return ret(c, E_UNKNOWN_UID);
  if (cb) run_callbacks(c);
  if (!f->freeBlocks.empty() && f->waiters.empty()) {
    put32(out, f->freeBlocks.back());
    f->freeBlocks.pop_back();
    return ret(c, 0);
  }
  if (poll) return ret(c, E_FPL_NOMEM);
  if (ta && lw(ta) == 0) return ret(c, E_WAIT_TIMEOUT);
  Thread* t = g_cur;
  t->ptrOut = out;
  enqueue_waiter(f->waiters, t, (f->attr & 0x100) != 0);
  setup_wait(t, WT_Fpl, f->uid, ta, ta != 0, E_WAIT_TIMEOUT);
  ret(c, do_wait(c, t, cb));
}
void h_alloc_fpl(Cpu& c) { fpl_alloc(c, false, false); }
void h_alloc_fpl_cb(Cpu& c) { fpl_alloc(c, true, false); }
void h_try_alloc_fpl(Cpu& c) { fpl_alloc(c, false, true); }

void h_free_fpl(Cpu& c) {
  Fpl* f = find(g_fpls, arg(c, 0));
  uint32_t blk = arg(c, 1);
  if (!f) return ret(c, E_UNKNOWN_UID);
  if (blk < f->base || blk >= f->base + f->blockSize * f->numBlocks || (blk - f->base) % f->blockSize)
    return ret(c, E_ERROR);
  if (!f->waiters.empty()) {
    Thread* t = f->waiters.front();
    f->waiters.pop_front();
    t->waitList = nullptr;
    put32(t->ptrOut, blk);
    release_wait(t, 0);
  } else {
    f->freeBlocks.push_back(blk);
  }
  ret(c, 0);
  preempt_check();
}

// ---- callbacks ----

void h_create_callback(Cpu& c) {
  auto cb = std::make_unique<Callback>();
  cb->uid = alloc_uid();
  cb->name = cstr(arg(c, 0), 31);
  cb->thread = g_cur->uid;
  cb->func = arg(c, 1);
  cb->arg = arg(c, 2);
  int32_t uid = cb->uid;
  g_callbacks[uid] = std::move(cb);
  ret(c, static_cast<uint32_t>(uid));
}

void h_delete_callback(Cpu& c) {
  if (!g_callbacks.erase(static_cast<int32_t>(arg(c, 0)))) return ret(c, E_UNKNOWN_UID);
  ret(c, 0);
}

void h_notify_callback(Cpu& c) {
  if (!find(g_callbacks, arg(c, 0))) return ret(c, E_UNKNOWN_UID);
  notify_callback(static_cast<int32_t>(arg(c, 0)), arg(c, 1));
  ret(c, 0);
}

void h_cancel_callback(Cpu& c) {
  Callback* cb = find(g_callbacks, arg(c, 0));
  if (!cb) return ret(c, E_UNKNOWN_UID);
  cb->notifyCount = 0;
  ret(c, 0);
}

void h_callback_count(Cpu& c) {
  Callback* cb = find(g_callbacks, arg(c, 0));
  ret(c, cb ? cb->notifyCount : E_UNKNOWN_UID);
}

void h_check_callback(Cpu& c) { ret(c, run_callbacks(c) ? 1 : 0); }

// ---- time ----

void h_system_time_wide(Cpu& c) { ret64(c, now_us()); }
void h_system_time_low(Cpu& c) { ret(c, static_cast<uint32_t>(now_us())); }
void h_system_time(Cpu& c) {
  put64(arg(c, 0), now_us());
  ret(c, 0);
}
void h_usec_to_clock(Cpu& c) {
  put64(arg(c, 1), arg(c, 0));
  ret(c, 0);
}
void h_usec_to_clock_wide(Cpu& c) { ret64(c, arg(c, 0)); }
void h_clock_to_usec(Cpu& c) {
  uint32_t p = arg(c, 0);
  uint64_t v = p ? (static_cast<uint64_t>(lw(p + 4)) << 32) | lw(p) : 0;
  put32(arg(c, 1), static_cast<uint32_t>(v % 1000000));
  put32(arg(c, 2), static_cast<uint32_t>(v / 1000000));
  ret(c, 0);
}
void h_clock_to_usec_wide(Cpu& c) { ret64(c, arg64(c, 0)); }

// ---- alarms ----

void set_alarm(Cpu& c, uint64_t us, uint32_t handler, uint32_t common) {
  if (!handler) return ret(c, E_ILLEGAL_ENTRY);
  Alarm a{alloc_uid(), now_us() + us, handler, common};
  g_alarms[a.uid] = a;
  ret(c, static_cast<uint32_t>(a.uid));
}
void h_set_alarm(Cpu& c) { set_alarm(c, arg(c, 0), arg(c, 1), arg(c, 2)); }
void h_set_sysclock_alarm(Cpu& c) {
  uint32_t p = arg(c, 0);
  uint64_t v = p ? (static_cast<uint64_t>(lw(p + 4)) << 32) | lw(p) : 0;
  set_alarm(c, v, arg(c, 1), arg(c, 2));
}
void h_cancel_alarm(Cpu& c) {
  if (!g_alarms.erase(static_cast<int32_t>(arg(c, 0)))) return ret(c, E_NOT_FOUND_ALARM);
  ret(c, 0);
}

// ---- interrupts ----

void h_register_subintr(Cpu& c) {
  int intno = static_cast<int>(arg(c, 0)), sub = static_cast<int>(arg(c, 1));
  for (auto& h : g_intr) {
    if (h.intno == intno && h.sub == sub) {
      h.func = arg(c, 2);
      h.arg = arg(c, 3);
      return ret(c, 0);
    }
  }
  g_intr.push_back({intno, sub, arg(c, 2), arg(c, 3), false});
  ret(c, 0);
}

void h_release_subintr(Cpu& c) {
  int intno = static_cast<int>(arg(c, 0)), sub = static_cast<int>(arg(c, 1));
  g_intr.erase(std::remove_if(g_intr.begin(), g_intr.end(),
                              [&](const IntrHandler& h) { return h.intno == intno && h.sub == sub; }),
               g_intr.end());
  ret(c, 0);
}

void subintr_enable(Cpu& c, bool on) {
  int intno = static_cast<int>(arg(c, 0)), sub = static_cast<int>(arg(c, 1));
  for (auto& h : g_intr)
    if (h.intno == intno && h.sub == sub) h.enabled = on;
  ret(c, 0);
}
void h_enable_subintr(Cpu& c) { subintr_enable(c, true); }
void h_disable_subintr(Cpu& c) { subintr_enable(c, false); }

void h_cpu_suspend_intr(Cpu& c) {
  bool was = !g_cur->intrOff;
  g_cur->intrOff = true;
  ret(c, was ? 1 : 0);
}

void h_cpu_resume_intr(Cpu& c) {
  g_cur->intrOff = arg(c, 0) == 0;
  ret(c, 0);
  if (!g_cur->intrOff) preempt_check();
}

void h_is_intr_enabled(Cpu& c) { ret(c, g_cur->intrOff ? 0 : 1); }
void h_zero(Cpu& c) { ret(c, 0); }

}  // namespace

void sched_init(uint32_t gp) {
  g_gp = gp;
  g_threads.clear();
  g_graveyard.clear();
  g_semas.clear();
  g_flags.clear();
  g_mutexes.clear();
  g_fpls.clear();
  g_callbacks.clear();
  g_alarms.clear();
  g_intr.clear();
  g_pending.clear();
  g_ready.clear();
  g_cur = nullptr;
  g_done = false;
  g_exitCode = 0;
  g_exitCb = 0;
  g_exitNotified = false;
  g_quitDeadline = 0;
  uint32_t stack = sysmem_alloc(0x4000, 0x100, true, "intr stack");
  g_intrSp = stack + 0x4000 - 0x40;
  g_intrK0 = thread_local_block();
  set_hle_poll(poll_hook);
}

void register_threadman() {
  const char* m = "ThreadManForUser";
  register_hle(m, 0x446D8DE6, "sceKernelCreateThread", h_create_thread);
  register_hle(m, 0xF475845D, "sceKernelStartThread", h_start_thread);
  register_hle(m, 0xAA73C935, "sceKernelExitThread", h_exit_thread);
  register_hle(m, 0x809CE29B, "sceKernelExitDeleteThread", h_exit_delete_thread);
  register_hle(m, 0x616403BA, "sceKernelTerminateThread", h_terminate_thread);
  register_hle(m, 0x383F7BCC, "sceKernelTerminateDeleteThread", h_terminate_delete_thread);
  register_hle(m, 0x9FA03CD3, "sceKernelDeleteThread", h_delete_thread);
  register_hle(m, 0x293B45B8, "sceKernelGetThreadId", h_get_thread_id);
  register_hle(m, 0x94AA61EE, "sceKernelGetThreadCurrentPriority", h_get_thread_prio);
  register_hle(m, 0x71BC9871, "sceKernelChangeThreadPriority", h_change_thread_prio);
  register_hle(m, 0xEA748E31, "sceKernelChangeCurrentThreadAttr", h_zero);
  register_hle(m, 0x912354A7, "sceKernelRotateThreadReadyQueue", h_rotate_ready_queue);
  register_hle(m, 0x9944F31F, "sceKernelSuspendThread", h_suspend_thread);
  register_hle(m, 0x75156E8F, "sceKernelResumeThread", h_resume_thread);
  register_hle(m, 0x9ACE131E, "sceKernelSleepThread", h_sleep_thread);
  register_hle(m, 0x82826F70, "sceKernelSleepThreadCB", h_sleep_thread_cb);
  register_hle(m, 0xD59EAD2F, "sceKernelWakeupThread", h_wakeup_thread);
  register_hle(m, 0xCEADEB47, "sceKernelDelayThread", h_delay);
  register_hle(m, 0x68DA9E36, "sceKernelDelayThreadCB", h_delay_cb);
  register_hle(m, 0x278C0DF5, "sceKernelWaitThreadEnd", h_wait_end);
  register_hle(m, 0x840E8133, "sceKernelWaitThreadEndCB", h_wait_end_cb);
  register_hle(m, 0x3B183E26, "sceKernelGetThreadExitStatus", h_thread_exit_status);
  register_hle(m, 0x52089CA1, "sceKernelGetThreadStackFreeSize", h_stack_free);
  register_hle(m, 0x17C1684E, "sceKernelReferThreadStatus", h_refer_thread_status);
  register_hle(m, 0xFFC36A14, "sceKernelReferThreadRunStatus", h_refer_thread_run_status);
  register_hle(m, 0x2C34E053, "sceKernelReleaseWaitThread", h_release_wait);

  register_hle(m, 0xD6DA4BA1, "sceKernelCreateSema", h_create_sema);
  register_hle(m, 0x28B6489C, "sceKernelDeleteSema", h_delete_sema);
  register_hle(m, 0x3F53E640, "sceKernelSignalSema", h_signal_sema);
  register_hle(m, 0x4E3A1105, "sceKernelWaitSema", h_wait_sema);
  register_hle(m, 0x6D212BAC, "sceKernelWaitSemaCB", h_wait_sema_cb);
  register_hle(m, 0x58B1F937, "sceKernelPollSema", h_poll_sema);
  register_hle(m, 0xBC6FEBC5, "sceKernelReferSemaStatus", h_refer_sema);

  register_hle(m, 0x55C20A00, "sceKernelCreateEventFlag", h_create_evf);
  register_hle(m, 0xEF9E4C70, "sceKernelDeleteEventFlag", h_delete_evf);
  register_hle(m, 0x1FB15A32, "sceKernelSetEventFlag", h_set_evf);
  register_hle(m, 0x812346E4, "sceKernelClearEventFlag", h_clear_evf);
  register_hle(m, 0x402FCF22, "sceKernelWaitEventFlag", h_wait_evf);
  register_hle(m, 0x328C546A, "sceKernelWaitEventFlagCB", h_wait_evf_cb);
  register_hle(m, 0x30FD48F0, "sceKernelPollEventFlag", h_poll_evf);
  register_hle(m, 0xA66B0120, "sceKernelReferEventFlagStatus", h_refer_evf);

  register_hle(m, 0xB7D098C6, "sceKernelCreateMutex", h_create_mutex);
  register_hle(m, 0xF8170FBE, "sceKernelDeleteMutex", h_delete_mutex);
  register_hle(m, 0xB011B11F, "sceKernelLockMutex", h_lock_mutex);
  register_hle(m, 0x5BF4DD27, "sceKernelLockMutexCB", h_lock_mutex_cb);
  register_hle(m, 0x0DDCD2C9, "sceKernelTryLockMutex", h_trylock_mutex);
  register_hle(m, 0x6B30100F, "sceKernelUnlockMutex", h_unlock_mutex);

  register_hle(m, 0xC07BB470, "sceKernelCreateFpl", h_create_fpl);
  register_hle(m, 0xED1410E0, "sceKernelDeleteFpl", h_delete_fpl);
  register_hle(m, 0xD979E9BF, "sceKernelAllocateFpl", h_alloc_fpl);
  register_hle(m, 0xE7282CB6, "sceKernelAllocateFplCB", h_alloc_fpl_cb);
  register_hle(m, 0x623AE665, "sceKernelTryAllocateFpl", h_try_alloc_fpl);
  register_hle(m, 0xF6414A71, "sceKernelFreeFpl", h_free_fpl);

  register_hle(m, 0xE81CAF8F, "sceKernelCreateCallback", h_create_callback);
  register_hle(m, 0xEDBA5844, "sceKernelDeleteCallback", h_delete_callback);
  register_hle(m, 0xC11BA8C4, "sceKernelNotifyCallback", h_notify_callback);
  register_hle(m, 0xBA4051D6, "sceKernelCancelCallback", h_cancel_callback);
  register_hle(m, 0x2A3D44FF, "sceKernelGetCallbackCount", h_callback_count);
  register_hle(m, 0x349D6D6C, "sceKernelCheckCallback", h_check_callback);

  register_hle(m, 0x82BC5777, "sceKernelGetSystemTimeWide", h_system_time_wide);
  register_hle(m, 0x369ED59D, "sceKernelGetSystemTimeLow", h_system_time_low);
  register_hle(m, 0xDB738F35, "sceKernelGetSystemTime", h_system_time);
  register_hle(m, 0x110DEC9A, "sceKernelUSec2SysClock", h_usec_to_clock);
  register_hle(m, 0xC8CD158C, "sceKernelUSec2SysClockWide", h_usec_to_clock_wide);
  register_hle(m, 0xBA6B92E2, "sceKernelSysClock2USec", h_clock_to_usec);
  register_hle(m, 0xE1619D7C, "sceKernelSysClock2USecWide", h_clock_to_usec_wide);
  register_hle(m, 0x6652B8CA, "sceKernelSetAlarm", h_set_alarm);
  register_hle(m, 0xB2C25152, "sceKernelSetSysClockAlarm", h_set_sysclock_alarm);
  register_hle(m, 0x7E65B999, "sceKernelCancelAlarm", h_cancel_alarm);

  const char* im = "InterruptManager";
  register_hle(im, 0xCA04A2B9, "sceKernelRegisterSubIntrHandler", h_register_subintr);
  register_hle(im, 0xD61E6961, "sceKernelReleaseSubIntrHandler", h_release_subintr);
  register_hle(im, 0xFB8E22EC, "sceKernelEnableSubIntr", h_enable_subintr);
  register_hle(im, 0x8A389411, "sceKernelDisableSubIntr", h_disable_subintr);

  const char* kl = "Kernel_Library";
  register_hle(kl, 0x092968F4, "sceKernelCpuSuspendIntr", h_cpu_suspend_intr);
  register_hle(kl, 0x5F10D406, "sceKernelCpuResumeIntr", h_cpu_resume_intr);
  register_hle(kl, 0xB55249D2, "sceKernelIsCpuIntrEnable", h_is_intr_enabled);
}

}  // namespace k

int kernel_run_main(uint32_t entry, uint32_t argSize, const void* args) {
  using namespace k;
  auto t = std::make_unique<Thread>();
  Thread* main = t.get();
  main->uid = alloc_uid();
  main->name = "user_main";
  main->entry = entry;
  main->gp = g_gp;
  main->initPrio = main->prio = 0x20;
  main->stackSize = 0x40000;
  main->stackAddr = sysmem_alloc(main->stackSize, 0x100, true, "user_main stack");
  main->isMain = true;
  main->run = std::make_shared<Run>();
  main->state = TState::Running;
  if (!main->stackAddr) {
    std::fprintf(stderr, "no memory for the main thread stack\n");
    return 1;
  }
  g_threads[main->uid] = std::move(t);
  g_cur = main;

  Cpu cpu;
  cpu.r[28] = g_gp;
  cpu.r[26] = thread_local_block();
  uint32_t top = main->stackAddr + main->stackSize;
  if (argSize && args) {
    top -= (argSize + 15) & ~15u;
    mem_write(top, args, argSize);
    cpu.r[4] = argSize;
    cpu.r[5] = top;
  }
  cpu.r[29] = top - 0x40;
  cpu.r[31] = 0;

  bool stopped = false;
  try {
    try {
      call(cpu, entry);
      finish_thread(main, cpu.r[2]);
    } catch (ThreadExit& e) {
      finish_thread(main, e.status);
    }
  } catch (ThreadKilled&) {
  } catch (GameExitSignal&) {
    stopped = true;
  } catch (std::exception& e) {
    fatal(main, e.what());
    stopped = true;
  }
  if (!stopped) {
    std::unique_lock<std::mutex> lk(g_m);
    g_doneCv.wait(lk, [] { return g_done; });
  }
  return g_exitCode;
}

}  // namespace psp
