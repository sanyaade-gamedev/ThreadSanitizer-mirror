/*
  This file is part of ThreadSanitizer, a dynamic data race detector.

  Copyright (C) 2008-2009 Google Inc
     opensource@google.com

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
  02111-1307, USA.

  The GNU General Public License is contained in the file COPYING.
*/

// Author: Konstantin Serebryany.
// Author: Timur Iskhodzhanov.
//
// ******* WARNING ********
// This code is experimental. Do not expect anything here to work.
// ***** END WARNING ******

#include "pin.H"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <assert.h>

#include "thread_sanitizer.h"
#include "ts_lock.h"

#if defined(__GNUC__)
# include <cxxabi.h>  // __cxa_demangle
# define YIELD() usleep(0)
# define ATOMIC_READ(a) __sync_add_and_fetch(a, 0)

#elif defined(_MSC_VER)
namespace WINDOWS
{
// This is the way of including winows.h recommended by PIN docs.
#include<Windows.h>
}

#include <intrin.h>
# define YIELD() // __yield()
# define popen(x,y) (NULL)
# define ATOMIC_READ(a)         _InterlockedCompareExchange(a, 0, 0)
#endif


static void DumpEvent(EventType type, int32_t tid, uintptr_t pc,
                      uintptr_t a, uintptr_t info);
// TODO(kcc): do we need to handle these as a part of some TRACE?
#define REPORT_READ_RANGE(x, size) do { \
  if (size) DumpEvent(READ, tid, pc, (uintptr_t)(x), (size)); } while(0)

#define REPORT_WRITE_RANGE(x, size) do { \
  if (size) DumpEvent(WRITE, tid, pc, (uintptr_t)(x), (size)); } while(0)

#define EXTRA_REPLACE_PARAMS THREADID tid, uintptr_t pc,
#include "ts_replace.h"

#ifdef NDEBUG
# error "Please don't define NDEBUG"
#endif

//------ Global PIN lock ------- {{{1
class ScopedReentrantClientLock {
 public:
  ScopedReentrantClientLock(int line)
    : line_(line) {
    // if (line && G_flags->debug_level >= 5)  Printf("??Try  at line %d\n", line);
    PIN_LockClient();
    if (line && G_flags->debug_level >= 5)  Printf("++Lock at line %d\n", line);
  }
  ~ScopedReentrantClientLock() {
    if (line_ && G_flags->debug_level >= 5) Printf("--Unlock at line %d\n", line_);
    PIN_UnlockClient();
  }
 private:
  int line_;
};

//--------------- Globals ----------------- {{{1
extern FILE *G_out;


static bool main_entered, main_exited;

// Number of threads created by pthread_create (i.e. not counting main thread).
static int n_created_threads = 0;
// Number of started threads, i.e. the number of CallbackForThreadStart calls.
static int n_started_threads = 0;

const uint32_t kMaxThreads = PIN_MAX_THREADS;

static TSLock g_main_ts_lock;
static TSLock g_thread_create_lock;
// Under g_thread_create_lock.
static THREADID g_tid_of_thread_which_called_create_thread = -1;

static uintptr_t g_current_pc;

#ifdef _MSC_VER
// On Windows, we need to create a h-b arc between
// RtlQueueWorkItem(callback, x, y) and the call to callback.
// Same for RegisterWaitForSingleObject.
static hash_set<uintptr_t> *g_windows_thread_pool_calback_set;
// Similarly, we need h-b arcs between the returns from callbacks and
// thre related UnregisterWaitEx. Damn, what a stupid interface!
static hash_map<uintptr_t, uintptr_t> *g_windows_thread_pool_wait_object_map;
#endif

//--------------- StackFrame ----------------- {{{1
struct StackFrame {
  uintptr_t pc;
  uintptr_t sp;
  StackFrame(uintptr_t p, uintptr_t s) : pc(p), sp(s) { }
};
//--------------- TraceInfo ----------------- {{{1
// Information about one Memory Operation.
struct MopInfo {
  uintptr_t pc;
  uintptr_t size;
  bool      is_write;
};

// An instance of this class is created for each TRACE (SEME region)
// during instrumentation.
class TraceInfo {
 public:
  static TraceInfo *NewTraceInfo(size_t n_mops, uintptr_t pc);
  void DeleteTraceInfo(TraceInfo *trace_info) {
    delete [] (uintptr_t*)trace_info;
  }
  MopInfo *GetMop(size_t i) {
    DCHECK(i < n_mops_);
    return &mops_[i];
  }

  size_t n_mops() const { return n_mops_; }
  size_t pc()     const { return pc_; }
  size_t id()     const { return id_; }
  size_t &counter()     { return counter_; }

  static void PrintTraceProfile();

 private:
  TraceInfo() { }

  size_t n_mops_;
  size_t pc_;
  size_t id_;
  size_t counter_;
  MopInfo mops_[1];

  static size_t id_counter_;
  static vector<TraceInfo*> *g_all_traces;
};

size_t TraceInfo::id_counter_;
vector<TraceInfo*> *TraceInfo::g_all_traces;

TraceInfo *TraceInfo::NewTraceInfo(size_t n_mops, uintptr_t pc) {
  size_t mem_size = (sizeof(TraceInfo) + (n_mops - 1) * sizeof(MopInfo));
  uint8_t *mem = new uint8_t[mem_size];
  TraceInfo *res = new (mem) TraceInfo;
  res->n_mops_ = n_mops;
  res->pc_ = pc;
  res->id_ = id_counter_++;
  res->counter_ = 0;
  if (g_all_traces == NULL) {
    g_all_traces = new vector<TraceInfo*>;
    CHECK(id_counter_ == 1);
  }
  g_all_traces->push_back(res);
  return res;
}

void TraceInfo::PrintTraceProfile() {
  int64_t total_counter = 0;
  multimap<size_t, TraceInfo*> traces;
  for (size_t i = 0; i < g_all_traces->size(); i++) {
    TraceInfo *trace = (*g_all_traces)[i];
    traces.insert(make_pair(trace->counter(), trace));
    total_counter += trace->counter();
  }
  Printf("TraceProfile: %ld traces, %lld hits\n",
         g_all_traces->size(), total_counter);
  int i = 0;
  for (multimap<size_t, TraceInfo*>::reverse_iterator it = traces.rbegin();
       it != traces.rend(); ++it, i++) {
    TraceInfo *trace = it->second;
    int64_t c = it->first;
    int64_t permile = (c * 1000) / total_counter;
    if (permile == 0 || i >= 20) break;
    Printf("TR%ld c=%lld (%lld/1000) n_mops=%ld\n", trace->id(), c,
           permile, trace->n_mops());
  }
}

//--------------- PinThread ----------------- {{{1
const size_t kThreadLocksEventBufferSize = 2048 - 2;
// The number of mops should be at least 2 less than the size of TLEB
// so that we have space to put SBLOCK_ENTER token and the trace_info ptr.
const size_t kMaxMopsPerTrace = kThreadLocksEventBufferSize - 2;

REG tls_reg;

struct PinThread;

struct ThreadLocalEventBuffer {
  PinThread *t;
  size_t size;
  uintptr_t events[kThreadLocksEventBufferSize];
};

struct PinThread {
  ThreadLocalEventBuffer tleb;
  int          uniq_tid;
  volatile long last_child_tid;
  THREADID     tid;
  THREADID     parent_tid;
  pthread_t    my_ptid;
  size_t       thread_stack_size_if_known;
  size_t       last_child_stack_size_if_known;
  vector<StackFrame> shadow_stack;
  TraceInfo    *trace_info;
  int ignore_all_mops;  // if >0, ignore all mops.
  int ignore_lock_events;  // if > 0, ignore all lock/unlock events.
  int spin_lock_recursion_depth;
  bool         thread_finished;
  bool         thread_done;
};

// Array of pin threads, indexed by pin's THREADID.
static PinThread *g_pin_threads;

// Used only if separate_analysis_thread==true.
static vector<ThreadLocalEventBuffer *> *g_tleb_queue;

#ifdef _MSC_VER
static hash_set<pthread_t> *g_win_handles_which_are_threads;
#endif

//------------- ThreadSanitizer exports ------------ {{{1
string Demangle(const char *str) {
#if defined(__GNUC__)
  int status;
  char *demangled = __cxxabiv1::__cxa_demangle(str, 0, 0, &status);
  if (demangled) {
    string res = demangled;
    free(demangled);
    return res;
  }
#endif
  return str;
}

void PcToStrings(uintptr_t pc, bool demangle,
                string *img_name, string *rtn_name,
                string *file_name, int *line_no) {
  if (G_flags->symbolize) {
    RTN rtn;
    G_stats->lock_sites[5]++;
    ScopedReentrantClientLock lock(__LINE__);
    // ClientLock must be held.
    PIN_GetSourceLocation(pc, NULL, line_no, file_name);
    *file_name = ConvertToPlatformIndependentPath(*file_name);
    rtn = RTN_FindByAddress(pc);
    string name;
    if (RTN_Valid(rtn)) {
      *rtn_name = demangle
          ? Demangle(RTN_Name(rtn).c_str())
          : RTN_Name(rtn);
      *img_name = IMG_Name(SEC_Img(RTN_Sec(rtn)));
    }
  }
}

string PcToRtnName(uintptr_t pc, bool demangle) {
  string res;
  if (G_flags->symbolize) {
    {
      G_stats->lock_sites[6]++;
      ScopedReentrantClientLock lock(__LINE__);
      RTN rtn = RTN_FindByAddress(pc);
      if (RTN_Valid(rtn)) {
        res = demangle
            ? Demangle(RTN_Name(rtn).c_str())
            : RTN_Name(rtn);
      }
    }
  }
  return res;
}

uintptr_t GetPcOfCurrentThread() {
  return g_current_pc;
}

//--------------- ThreadLocalEventBuffer ----------------- {{{1
// thread local event buffer is an array of uintptr_t.
// The events are encoded like this:
// { RTN_CALL, call_pc, target_pc }
// { RTN_EXIT }
// { SBLOCK_ENTER, trace_info_of_size_n, addr1, addr2, ... addr_n}

enum TLEBSpecificEvents {
  TLEB_IGNORE_ALL_BEGIN = LAST_EVENT + 1,
  TLEB_IGNORE_ALL_END,
  TLEB_IGNORE_SYNC_BEGIN,
  TLEB_IGNORE_SYNC_END,
};

static bool DumpEventPlainText(EventType type, int32_t tid, uintptr_t pc,
                        uintptr_t a, uintptr_t info) {
#if DEBUG == 0 || defined(_MSC_VER)
  return false;
#else 
  if (G_flags->dump_events.empty()) return false;

  static hash_set<uintptr_t> *pc_set;
  if (pc_set == NULL) {
    pc_set = new hash_set<uintptr_t>;
  }
  static FILE *log_file = NULL;
  if (log_file == NULL) {
    log_file = popen(("gzip > " + G_flags->dump_events).c_str(), "w");
  }
  if (G_flags->symbolize && pc_set->insert(pc).second) {
    string img_name, rtn_name, file_name;
    int line = 0;
    PcToStrings(pc, false, &img_name, &rtn_name, &file_name, &line);
    if (file_name.empty()) file_name = "unknown";
    if (img_name.empty()) img_name = "unknown";
    if (rtn_name.empty()) rtn_name = "unknown";
    if (line == 0) line = 1;
    fprintf(log_file, "#PC %lx %s %s %s %d\n",
            (long)pc, img_name.c_str(), rtn_name.c_str(),
            file_name.c_str(), line);
  }
  fprintf(log_file, "%s %x %lx %lx %lx\n", kEventNames[type], tid,
          (long)pc, (long)a, (long)info);
  return true;
#endif
}

static void DumpEventInternal(EventType type, int32_t uniq_tid, uintptr_t pc,
                              uintptr_t a, uintptr_t info) {
  if (DumpEventPlainText(type, uniq_tid, pc, a, info)) return;
  // PIN wraps the tid (after 2048), but we need a uniq tid.
  Event event(type, uniq_tid, pc, a, info);
  ThreadSanitizerHandleOneEvent(&event);
}

static void TLEBFlushUnlocked(ThreadLocalEventBuffer &tleb) {
  PinThread &t = *tleb.t;
  DCHECK(tleb.size <= kThreadLocksEventBufferSize);
  if (DEBUG_MODE && t.thread_done) {
    Printf("ACHTUNG!!! an event from a dead thread T%d\n", t.tid);
  }
  DCHECK(!t.thread_done);
  if (tleb.size == 0) return;

  if (1 || DEBUG_MODE) {
    size_t max_idx = TS_ARRAY_SIZE(G_stats->tleb_flush);
    size_t idx = (tleb.size * max_idx - 1) / kThreadLocksEventBufferSize;
    CHECK(idx < max_idx);
    G_stats->tleb_flush[idx]++;
  }

  size_t i;
  for (i = 0; i < tleb.size; ) {
    uintptr_t event = tleb.events[i++];
    if (event == RTN_EXIT) {
      if (DumpEventPlainText(RTN_EXIT, t.uniq_tid, 0, 0, 0)) continue;
      ThreadSanitizerHandleRtnExit(t.uniq_tid);
    } else if (event == RTN_CALL) {
      uintptr_t call_pc = tleb.events[i++];
      uintptr_t target_pc = tleb.events[i++];
      IGNORE_BELOW_RTN ignore_below = (IGNORE_BELOW_RTN)tleb.events[i++];
      if (DumpEventPlainText(RTN_CALL, t.uniq_tid, call_pc,
                             target_pc, ignore_below)) continue;
      ThreadSanitizerHandleRtnCall(t.uniq_tid, call_pc, target_pc,
                                   ignore_below);
    } else if (event == SBLOCK_ENTER){
      bool do_this_trace = ((G_flags->literace_sampling == 0 ||
                             !LiteRaceSkipTrace(t.uniq_tid, t.trace_info->id(),
                                                G_flags->literace_sampling)));
      if (t.ignore_all_mops)
        do_this_trace = false;

      TraceInfo *trace_info = (TraceInfo*) tleb.events[i++];
      DCHECK(trace_info);
      size_t n = trace_info->n_mops();
      if (do_this_trace) {
        if (DumpEventPlainText(SBLOCK_ENTER, t.uniq_tid, 
                               trace_info->pc(), 0, 0) == false) {;
          ThreadSanitizerEnterSblock(t.uniq_tid, trace_info->pc());
        }
        for (size_t j = 0; j < n; j++) {
          MopInfo *mop = trace_info->GetMop(j);
          DCHECK(mop->size);
          DCHECK(mop);
          uintptr_t addr = tleb.events[i + j];
          if (addr) {
            g_current_pc = mop->pc;
            if (DumpEventPlainText(mop->is_write ? WRITE : READ, t.uniq_tid,
                                   mop->pc, addr, mop->size)) continue;
            ThreadSanitizerHandleMemoryAccess(t.uniq_tid, addr,
                                              mop->size, mop->is_write);
          }
        }
      }
      i += n;
    } else if (event == THR_START) {
      uintptr_t parent = -1;
      if (t.parent_tid != (THREADID)-1) {
        parent = g_pin_threads[t.parent_tid].uniq_tid;
      }
      DumpEventInternal(THR_START, t.uniq_tid, 0, 0, parent);
    } else if (event == THR_END) {
      DumpEventInternal(THR_END, t.uniq_tid, 0, 0, 0);
      DCHECK(t.thread_finished == true);
      DCHECK(t.thread_done == false);
      t.thread_done = true;
      i += 3;  // consume the unneeded data.
      DCHECK(i == tleb.size);  // should be last event in this tleb.
    } else if (event == TLEB_IGNORE_ALL_BEGIN){
      t.ignore_all_mops++;
    } else if (event == TLEB_IGNORE_ALL_END){
      t.ignore_all_mops--;
      CHECK(t.ignore_all_mops >= 0);
    } else if (event == TLEB_IGNORE_SYNC_BEGIN){
      t.ignore_lock_events++;
    } else if (event == TLEB_IGNORE_SYNC_END){
      t.ignore_lock_events--;
      CHECK(t.ignore_lock_events >= 0);
    } else {
      // all other events.
      CHECK(event > NOOP && event < LAST_EVENT);
      uintptr_t pc    = tleb.events[i++];
      uintptr_t a     = tleb.events[i++];
      uintptr_t info  = tleb.events[i++];
      if (t.ignore_lock_events &&
          (event == WRITER_LOCK || event == READER_LOCK || event == UNLOCK)) {
        // do nothing, we are ignoring locks.
      } else if (t.ignore_all_mops && (event == READ || event == WRITE)) {
        // do nothing, we are ignoring mops.
      } else {
        DumpEventInternal((EventType)event, t.uniq_tid, pc, a, info);
      }
    }
  }
  DCHECK(i == tleb.size);
  tleb.size = 0;
  if (DEBUG_MODE) { // for sanity checking.
    memset(tleb.events, 0xf0, sizeof(tleb.events));
  }
}

static void TLEBFlushLocked(PinThread &t) {
  if (G_flags->dry_run) {
    t.tleb.size = 0;
    return;
  }
  CHECK(t.tleb.size <= kThreadLocksEventBufferSize);
  G_stats->lock_sites[1]++;
  if (G_flags->separate_analysis_thread) {
    ThreadLocalEventBuffer *tleb_copy = new ThreadLocalEventBuffer;
    memcpy(tleb_copy, &t.tleb, sizeof(uintptr_t) * (t.tleb.size + 2));
    tleb_copy->t = &t;
    CHECK(g_tleb_queue);
    {
      ScopedLock lock(&g_main_ts_lock);
      g_tleb_queue->push_back(tleb_copy);
      // Printf("Sent     %p t=%d size=%ld\n", tleb_copy,
      // (int)tleb_copy->t->tid, tleb_copy->size);
    }
    t.tleb.size = 0;
  } else {
    ScopedLock lock(&g_main_ts_lock);
    TLEBFlushUnlocked(t.tleb);
  }
}

static void TLEBAddRtnCall(PinThread &t, uintptr_t call_pc,
                           uintptr_t target_pc, IGNORE_BELOW_RTN ignore_below) {
  DCHECK(t.tleb.size <= kThreadLocksEventBufferSize);
  if (t.tleb.size + 4 > kThreadLocksEventBufferSize) {
    TLEBFlushLocked(t);
    DCHECK(t.tleb.size == 0);
  }
  t.tleb.events[t.tleb.size++] = RTN_CALL;
  t.tleb.events[t.tleb.size++] = call_pc;
  t.tleb.events[t.tleb.size++] = target_pc;
  t.tleb.events[t.tleb.size++] = ignore_below;
  DCHECK(t.tleb.size <= kThreadLocksEventBufferSize);
}

static void TLEBAddRtnExit(PinThread &t) {
  if (t.tleb.size + 1 > kThreadLocksEventBufferSize) {
    TLEBFlushLocked(t);
  }
  t.tleb.events[t.tleb.size++] = RTN_EXIT;
  DCHECK(t.tleb.size <= kThreadLocksEventBufferSize);
}

static uintptr_t *TLEBAddTrace(PinThread &t) {
  size_t n = t.trace_info->n_mops();
  DCHECK(n > 0);
  if (t.tleb.size + 2 + n > kThreadLocksEventBufferSize) {
    TLEBFlushLocked(t);
  }
  t.tleb.events[t.tleb.size++] = SBLOCK_ENTER;
  t.tleb.events[t.tleb.size++] = (uintptr_t)t.trace_info;
  // not every address will be written to. so they will stay 0.
  for (size_t i = 0; i < n; i++) {
    t.tleb.events[t.tleb.size + i] = 0;
  }
  uintptr_t *mop_addresses = &t.tleb.events[t.tleb.size];
  t.tleb.size += n;
  DCHECK(t.tleb.size <= kThreadLocksEventBufferSize);
  return mop_addresses;
}

static void TLEBStartThread(PinThread &t) {
  CHECK(t.tleb.size == 0);
  t.tleb.events[t.tleb.size++] = THR_START;
}

static void TLEBSimpleEvent(PinThread &t, uintptr_t event) {
  if (t.tleb.size + 1 > kThreadLocksEventBufferSize) {
    TLEBFlushLocked(t);
  }
  t.tleb.events[t.tleb.size++] = event;
  DCHECK(t.tleb.size <= kThreadLocksEventBufferSize);
}

static void TLEBAddGenericEventAndFlush(PinThread &t,
                                        EventType type, uintptr_t pc,
                                        uintptr_t a, uintptr_t info) {
  if (t.tleb.size + 4 > kThreadLocksEventBufferSize) {
    TLEBFlushLocked(t);
  }
  DCHECK(type > NOOP && type < LAST_EVENT);
  t.tleb.events[t.tleb.size++] = type;
  t.tleb.events[t.tleb.size++] = pc;
  t.tleb.events[t.tleb.size++] = a;
  t.tleb.events[t.tleb.size++] = info;
  TLEBFlushLocked(t);
  DCHECK(t.tleb.size <= kThreadLocksEventBufferSize);
}


// Must be called from its thread (except for THR_END case)!
static void DumpEvent(EventType type, int32_t tid, uintptr_t pc,
                      uintptr_t a, uintptr_t info) {
  PinThread &t = g_pin_threads[tid];
  TLEBAddGenericEventAndFlush(t, type, pc, a, info);
}

//--------- Wraping and relacing --------------- {{{1
static set<string> g_wrapped_functions;
static void InformAboutFunctionWrap(RTN rtn, string name) {
  g_wrapped_functions.insert(name);
  if (!debug_wrap) return;
  Printf("Function wrapped: %s (%s %s)\n", name.c_str(),
         RTN_Name(rtn).c_str(), IMG_Name(SEC_Img(RTN_Sec(rtn))).c_str());
}

static bool RtnMatchesName(const string &rtn_name, const string &name) {
  CHECK(name.size() > 0);
  size_t pos = rtn_name.find(name);
  if (pos == string::npos) {
    return false;
  }
  if (pos == 0 && name.size() == rtn_name.size()) {
  //  Printf("Full match: %s %s\n", rtn_name.c_str(), name.c_str());
    return true;
  }
  // match MyFuncName@123
  if (pos == 0 && name.size() < rtn_name.size()
      && rtn_name[name.size()] == '@') {
  //  Printf("Versioned match: %s %s\n", rtn_name.c_str(), name.c_str());
    return true;
  }
  // match _MyFuncName@123
  if (pos == 1 && rtn_name[0] == '_' && name.size() < rtn_name.size()
      && rtn_name[name.size() + 1] == '@') {
    // Printf("Versioned match: %s %s\n", rtn_name.c_str(), name.c_str());
    return true;
  }

  return false;
}

#define WRAP_NAME(name) Wrap_##name
#define WRAP4(name) WrapFunc4(img, rtn, #name, (AFUNPTR)Wrap_##name)
#define WRAPSTD1(name) WrapStdCallFunc1(rtn, #name, (AFUNPTR)Wrap_##name)
#define WRAPSTD2(name) WrapStdCallFunc2(rtn, #name, (AFUNPTR)Wrap_##name)
#define WRAPSTD3(name) WrapStdCallFunc3(rtn, #name, (AFUNPTR)Wrap_##name)
#define WRAPSTD4(name) WrapStdCallFunc4(rtn, #name, (AFUNPTR)Wrap_##name)
#define WRAPSTD6(name) WrapStdCallFunc6(rtn, #name, (AFUNPTR)Wrap_##name)
#define WRAP_PARAM4  THREADID tid, ADDRINT pc, CONTEXT *ctx, \
                                AFUNPTR f,\
                                uintptr_t arg0, uintptr_t arg1, \
                                uintptr_t arg2, uintptr_t arg3

#define WRAP_PARAM6 WRAP_PARAM4, uintptr_t arg4, uintptr_t arg5

static uintptr_t CallFun4(CONTEXT *ctx, THREADID tid,
                         AFUNPTR f, uintptr_t arg0, uintptr_t arg1,
                         uintptr_t arg2, uintptr_t arg3) {
  uintptr_t ret = 0xdeadbee1;
  PIN_CallApplicationFunction(ctx, tid,
                              CALLINGSTD_DEFAULT, (AFUNPTR)(f),
                              PIN_PARG(uintptr_t), &ret,
                              PIN_PARG(uintptr_t), arg0,
                              PIN_PARG(uintptr_t), arg1,
                              PIN_PARG(uintptr_t), arg2,
                              PIN_PARG(uintptr_t), arg3,
                              PIN_PARG_END());
  return ret;
}

static uintptr_t CallFun6(CONTEXT *ctx, THREADID tid,
                         AFUNPTR f, uintptr_t arg0, uintptr_t arg1,
                         uintptr_t arg2, uintptr_t arg3,
                         uintptr_t arg4, uintptr_t arg5) {
  uintptr_t ret = 0xdeadbee1;
  PIN_CallApplicationFunction(ctx, tid,
                              CALLINGSTD_DEFAULT, (AFUNPTR)(f),
                              PIN_PARG(uintptr_t), &ret,
                              PIN_PARG(uintptr_t), arg0,
                              PIN_PARG(uintptr_t), arg1,
                              PIN_PARG(uintptr_t), arg2,
                              PIN_PARG(uintptr_t), arg3,
                              PIN_PARG(uintptr_t), arg4,
                              PIN_PARG(uintptr_t), arg5,
                              PIN_PARG_END());
  return ret;
}


#define CALL_ME_INSIDE_WRAPPER_4() CallFun4(ctx, tid, f, arg0, arg1, arg2, arg3)
#define CALL_ME_INSIDE_WRAPPER_6() CallFun6(ctx, tid, f, arg0, arg1, arg2, arg3, arg4, arg5)

// Completely replace (i.e. not wrap) a function with 3 (or less) parameters.
// The original function will not be called.
void ReplaceFunc3(IMG img, RTN rtn, const char *name, AFUNPTR replacement_func) {
  if (RTN_Valid(rtn) && RtnMatchesName(RTN_Name(rtn), name)) {
    InformAboutFunctionWrap(rtn, name);
    PROTO proto = PROTO_Allocate(PIN_PARG(uintptr_t),
                                 CALLINGSTD_DEFAULT,
                                 "proto",
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG_END());
    RTN_ReplaceSignature(rtn,
                         AFUNPTR(replacement_func),
                         IARG_PROTOTYPE, proto,
                         IARG_THREAD_ID,
                         IARG_INST_PTR,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
                         IARG_END);
    PROTO_Free(proto);
  }
}

// Wrap a function with up to 4 parameters.
void WrapFunc4(IMG img, RTN rtn, const char *name, AFUNPTR replacement_func) {
  if (RTN_Valid(rtn) && RtnMatchesName(RTN_Name(rtn), name)) {
    InformAboutFunctionWrap(rtn, name);
    PROTO proto = PROTO_Allocate(PIN_PARG(uintptr_t),
                                 CALLINGSTD_DEFAULT,
                                 "proto",
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG_END());
    RTN_ReplaceSignature(rtn,
                         AFUNPTR(replacement_func),
                         IARG_PROTOTYPE, proto,
                         IARG_THREAD_ID,
                         IARG_INST_PTR,
                         IARG_CONTEXT,
                         IARG_ORIG_FUNCPTR,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 3,
                         IARG_END);
    PROTO_Free(proto);
  }
}

// Wrap a function with up to 6 parameters.
void WrapFunc6(IMG img, RTN rtn, const char *name, AFUNPTR replacement_func) {
  if (RTN_Valid(rtn) && RtnMatchesName(RTN_Name(rtn), name)) {
    InformAboutFunctionWrap(rtn, name);
    PROTO proto = PROTO_Allocate(PIN_PARG(uintptr_t),
                                 CALLINGSTD_DEFAULT,
                                 "proto",
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG_END());
    RTN_ReplaceSignature(rtn,
                         AFUNPTR(replacement_func),
                         IARG_PROTOTYPE, proto,
                         IARG_THREAD_ID,
                         IARG_INST_PTR,
                         IARG_CONTEXT,
                         IARG_ORIG_FUNCPTR,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 3,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 4,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 5,
                         IARG_END);
    PROTO_Free(proto);
  }
}


//--------- Instrumentation callbacks --------------- {{{1
//---------- Debug -----------------------------------{{{2
#define DEB_PR (0)

static void ShowPcAndSp(const char *where, THREADID tid,
                        ADDRINT pc, ADDRINT sp) {
    Printf("%s T%d sp=%ld pc=%p %s\n", where, tid, sp, pc,
           PcToRtnName(pc, true).c_str());
}

static void PrintShadowStack(PinThread &t) {
  Printf("T%d Shadow stack (%d)\n", t.tid, (int)t.shadow_stack.size());
  for (int i = t.shadow_stack.size() - 1; i >= 0; i--) {
    uintptr_t pc = t.shadow_stack[i].pc;
    uintptr_t sp = t.shadow_stack[i].sp;
    Printf("  sp=%ld pc=%lx %s\n", sp, pc, PcToRtnName(pc, true).c_str());
  }
}

static void DebugOnlyShowPcAndSp(const char *where, THREADID tid,
                                 ADDRINT pc, ADDRINT sp) {
  if (DEB_PR) {
    ShowPcAndSp(where, tid, pc, sp);
  }
}

static uintptr_t WRAP_NAME(ThreadSanitizerQuery)(WRAP_PARAM4) {
  const char *query = (const char*)arg0;
  return (uintptr_t)ThreadSanitizerQuery(query);
}

//--------- Ignores -------------------------------- {{{2
static void IgnoreMopsBegin(THREADID tid, ADDRINT pc) {
//  if (tid == 0) Printf("Ignore++ %d\n", z++); 
  TLEBSimpleEvent(g_pin_threads[tid], TLEB_IGNORE_ALL_BEGIN);
}
static void IgnoreMopsEnd(THREADID tid, ADDRINT pc) {
//  if (tid == 0) Printf("Ignore-- %d\n", z--);
  TLEBSimpleEvent(g_pin_threads[tid], TLEB_IGNORE_ALL_END);
}

static void IgnoreSyncAndMopsBegin(THREADID tid, ADDRINT pc) {
  IgnoreMopsBegin(tid, pc);
  TLEBSimpleEvent(g_pin_threads[tid], TLEB_IGNORE_SYNC_BEGIN);
}
static void IgnoreSyncAndMopsEnd(THREADID tid, ADDRINT pc) {
  IgnoreMopsEnd(tid, pc);
  TLEBSimpleEvent(g_pin_threads[tid], TLEB_IGNORE_SYNC_END);
}

//--------- __cxa_guard_* -------------------------- {{{2
// From gcc/cp/decl.c:
// --------------------------------------------------------------
//      Emit code to perform this initialization but once.  This code
//      looks like:
//
//      static <type> guard;
//      if (!guard.first_byte) {
//        if (__cxa_guard_acquire (&guard)) {
//          bool flag = false;
//          try {
//            // Do initialization.
//            flag = true; __cxa_guard_release (&guard);
//            // Register variable for destruction at end of program.
//           } catch {
//          if (!flag) __cxa_guard_abort (&guard);
//         }
//      }
// --------------------------------------------------------------
// So, when __cxa_guard_acquire returns true, we start ignoring all accesses
// and in __cxa_guard_release we stop ignoring them.
// We also need to ignore all accesses inside these two functions.

static void Before_cxa_guard_acquire(THREADID tid, ADDRINT pc, ADDRINT guard) {
  IgnoreMopsBegin(tid, pc);
}

static void After_cxa_guard_acquire(THREADID tid, ADDRINT pc, ADDRINT ret) {
  if (ret) {
    // Continue ignoring, it will end in __cxa_guard_release.
  } else {
    // Stop ignoring, there will be no matching call to __cxa_guard_release.
    IgnoreMopsEnd(tid, pc);
  }
}

static void After_cxa_guard_release(THREADID tid, ADDRINT pc) {
  IgnoreMopsEnd(tid, pc);
}

static uintptr_t WRAP_NAME(pthread_once)(WRAP_PARAM4) {
  uintptr_t ret;
  IgnoreMopsBegin(tid, pc);
  ret = CALL_ME_INSIDE_WRAPPER_4();
  IgnoreMopsEnd(tid, pc);
  return ret;
}

void TmpCallback1(THREADID tid, ADDRINT pc) {
  Printf("%s T%d %lx\n", __FUNCTION__, tid, pc);
}
void TmpCallback2(THREADID tid, ADDRINT pc) {
  Printf("%s T%d %lx\n", __FUNCTION__, tid, pc);
}

//--------- Threads --------------------------------- {{{2
static void HandleThreadCreateBefore(THREADID tid, ADDRINT pc) {
  G_stats->lock_sites[8]++;
  DumpEvent(THR_CREATE_BEFORE, tid, pc, 0, 0);
  g_thread_create_lock.Lock();
  CHECK(g_tid_of_thread_which_called_create_thread == (THREADID)-1);
  g_tid_of_thread_which_called_create_thread = tid;
  n_created_threads++;
}

static THREADID HandleThreadCreateAfter(THREADID tid, pthread_t child_ptid) {
  // Spin, waiting for last_child_tid to appear (i.e. wait for the thread to
  // actually start) so that we know the child's tid. No locks.
  while (!ATOMIC_READ(&g_pin_threads[tid].last_child_tid)) {
    YIELD();
  }

  CHECK(g_tid_of_thread_which_called_create_thread != (THREADID)-1);
  g_tid_of_thread_which_called_create_thread = -1;

  THREADID last_child_tid = g_pin_threads[tid].last_child_tid;
  CHECK(last_child_tid);

  g_pin_threads[last_child_tid].my_ptid = child_ptid;
  g_pin_threads[tid].last_child_tid = 0;

  g_thread_create_lock.Unlock();

  DumpEvent(THR_CREATE_AFTER, tid, 0, 0, last_child_tid);

  return last_child_tid;
}

static uintptr_t WRAP_NAME(pthread_create)(WRAP_PARAM4) {
  HandleThreadCreateBefore(tid, pc);

  IgnoreMopsBegin(tid, pc);
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  IgnoreMopsEnd(tid, pc);

  pthread_t child_ptid = *(pthread_t*)arg0;
  HandleThreadCreateAfter(tid, child_ptid);

  return ret;
}

void CallbackForThreadStart(THREADID tid, CONTEXT *ctxt,
                            INT32 flags, void *v) {
  // We can not rely on PIN_GetParentTid() since it is broken on Windows.

  if (g_pin_threads == NULL) {
    g_pin_threads = new PinThread[kMaxThreads];
  }
  PinThread &t = g_pin_threads[tid];
  PIN_SetContextReg(ctxt, tls_reg, (ADDRINT)0xfafafa);

  bool has_parent = true;
  if (tid == 0) {
    // Main thread or we have attached to a running process.
    has_parent = false;
  } else {
    CHECK(tid > 0);
  }

  CHECK(tid < kMaxThreads);
  memset(&t, 0, sizeof(PinThread));
  t.uniq_tid = n_started_threads++;
  t.tid = tid;
  t.tleb.t = &t;

#if 0
  if (n_started_threads == 2) {
    // we are creating the first non-main thread. Flush the code cache and start
    // doing real work.
    CODECACHE_FlushCache();
  }
#endif  // _MSC_VER

  t.parent_tid = -1;
  if (has_parent) {
    t.parent_tid = g_tid_of_thread_which_called_create_thread;
#if !defined(_MSC_VER)  // On Windows, threads may appear out of thin air.
    CHECK(t.parent_tid != (THREADID)-1);
#endif  // _MSC_VER
  }

  if (debug_thread) {
    Printf("T%d ThreadStart parent=%d child=%d\n", tid, t.parent_tid, tid);
  }

  if (has_parent && t.parent_tid != (THREADID)-1) {
    g_pin_threads[t.parent_tid].last_child_tid = tid;
    t.thread_stack_size_if_known =
        g_pin_threads[t.parent_tid].last_child_stack_size_if_known;
  }

  // This is a lock-free (thread local) operation.
  TLEBStartThread(t);
}

static void Before_start_thread(THREADID tid, ADDRINT pc, ADDRINT sp) {
  PinThread &t = g_pin_threads[tid];
  DumpEvent(THR_STACK_TOP, tid, pc, sp, t.thread_stack_size_if_known);
}

#ifdef _MSC_VER
static uintptr_t WRAP_NAME(CreateThread)(WRAP_PARAM6) {
  PinThread &t = g_pin_threads[tid];
  t.last_child_stack_size_if_known = arg1 ? arg1 : 1024 * 1024;

  HandleThreadCreateBefore(tid, pc);
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_6();
  pthread_t child_ptid = ret;
  THREADID child_tid = HandleThreadCreateAfter(tid, child_ptid);
  {
    ScopedReentrantClientLock lock(__LINE__);
    if (g_win_handles_which_are_threads == NULL) {
      g_win_handles_which_are_threads = new hash_set<pthread_t>;
    }
    g_win_handles_which_are_threads->insert(child_ptid);
  }
  return ret;
}

static void Before_BaseThreadInitThunk(THREADID tid, ADDRINT pc, ADDRINT sp) {
  PinThread &t = g_pin_threads[tid];
  size_t stack_size = t.thread_stack_size_if_known;
  // Printf("T%d %s %p %p\n", tid, __FUNCTION__, sp, stack_size);
  DumpEvent(THR_STACK_TOP, tid, pc, sp, stack_size);
}

static void Before_RtlExitUserThread(THREADID tid, ADDRINT pc) {
  PinThread &t = g_pin_threads[tid];
  if (t.tid != 0) {
    // Once we started exiting the thread, ignore the locking events.
    // This way we will avoid h-b arcs between unrelated threads.
    // We also start ignoring all mops, otherwise we will get tons of race
    // reports from the windows guts.
    IgnoreSyncAndMopsBegin(tid, pc);
  }
}
#endif  // _MSC_VER

void CallbackForThreadFini(THREADID tid, const CONTEXT *ctxt,
                          INT32 code, void *v) {
  PinThread &t = g_pin_threads[tid];
  t.thread_finished = true;
  // We can not DumpEvent here,
  // due to possible deadlock with PIN's internal lock.
  if (debug_thread) {
    Printf("T%d Thread finished\n", tid);
  }
}

static void HandleThreadJoinAfter(THREADID tid, pthread_t joined_ptid) {
  THREADID joined_tid = 0;
  for (joined_tid = 1; joined_tid < kMaxThreads; joined_tid++) {
    if (g_pin_threads[joined_tid].thread_finished == false) continue;
    if (g_pin_threads[joined_tid].my_ptid == joined_ptid)
      break;
  }
  if (joined_tid == kMaxThreads) {
    if (DEBUG_MODE) {
      Printf("WARNING: Can not join thread, can't find the child's tid. "
             "T=%d ptid=%d\n", tid, (int)joined_ptid);
      ThreadSanitizerDumpAllStacks();
    }
    return;
  }
  CHECK(joined_tid < kMaxThreads);
  CHECK(joined_tid > 0);
  g_pin_threads[joined_tid].my_ptid = 0;
  int joined_uniq_tid = g_pin_threads[joined_tid].uniq_tid;

  if (G_flags->debug_level >= 2) {
    Printf("T%d JoinAfter   parent=%d child=%d (uniq=%d)\n", tid, tid,
           joined_tid, joined_uniq_tid);
  }
  DumpEvent(THR_END, joined_tid, 0, 0, 0);
  DumpEvent(THR_JOIN_AFTER, tid, 0, joined_uniq_tid, 0);
}

static uintptr_t WRAP_NAME(pthread_join)(WRAP_PARAM4) {
  if (G_flags->debug_level >= 2)
    Printf("T%d in  pthread_join %p\n", tid, arg0);
  pthread_t joined_ptid = (pthread_t)arg0;
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  HandleThreadJoinAfter(tid, joined_ptid);
  if (G_flags->debug_level >= 2)
    Printf("T%d out pthread_join %p\n", tid, arg0);
  return ret;
}

#ifdef _MSC_VER


uintptr_t CallStdCallFun1(CONTEXT *ctx, THREADID tid,
                         AFUNPTR f, uintptr_t arg0) {
  uintptr_t ret = 0xdeadbee1;
  PIN_CallApplicationFunction(ctx, tid,
                              CALLINGSTD_STDCALL, (AFUNPTR)(f),
                              PIN_PARG(uintptr_t), &ret,
                              PIN_PARG(uintptr_t), arg0,
                              PIN_PARG_END());
  return ret;
}

uintptr_t CallStdCallFun2(CONTEXT *ctx, THREADID tid,
                         AFUNPTR f, uintptr_t arg0, uintptr_t arg1) {
  uintptr_t ret = 0xdeadbee2;
  PIN_CallApplicationFunction(ctx, tid,
                              CALLINGSTD_STDCALL, (AFUNPTR)(f),
                              PIN_PARG(uintptr_t), &ret,
                              PIN_PARG(uintptr_t), arg0,
                              PIN_PARG(uintptr_t), arg1,
                              PIN_PARG_END());
  return ret;
}

uintptr_t CallStdCallFun3(CONTEXT *ctx, THREADID tid,
                         AFUNPTR f, uintptr_t arg0, uintptr_t arg1,
                         uintptr_t arg2) {
  uintptr_t ret = 0xdeadbee3;
  PIN_CallApplicationFunction(ctx, tid,
                              CALLINGSTD_STDCALL, (AFUNPTR)(f),
                              PIN_PARG(uintptr_t), &ret,
                              PIN_PARG(uintptr_t), arg0,
                              PIN_PARG(uintptr_t), arg1,
                              PIN_PARG(uintptr_t), arg2,
                              PIN_PARG_END());
  return ret;
}

uintptr_t CallStdCallFun4(CONTEXT *ctx, THREADID tid,
                         AFUNPTR f, uintptr_t arg0, uintptr_t arg1,
                         uintptr_t arg2, uintptr_t arg3) {
  uintptr_t ret = 0xdeadbee4;
  PIN_CallApplicationFunction(ctx, tid,
                              CALLINGSTD_STDCALL, (AFUNPTR)(f),
                              PIN_PARG(uintptr_t), &ret,
                              PIN_PARG(uintptr_t), arg0,
                              PIN_PARG(uintptr_t), arg1,
                              PIN_PARG(uintptr_t), arg2,
                              PIN_PARG(uintptr_t), arg3,
                              PIN_PARG_END());
  return ret;
}

uintptr_t CallStdCallFun6(CONTEXT *ctx, THREADID tid,
                         AFUNPTR f, uintptr_t arg0, uintptr_t arg1,
                         uintptr_t arg2, uintptr_t arg3,
                         uintptr_t arg4, uintptr_t arg5) {
  uintptr_t ret = 0xdeadbee6;
  PIN_CallApplicationFunction(ctx, tid,
                              CALLINGSTD_STDCALL, (AFUNPTR)(f),
                              PIN_PARG(uintptr_t), &ret,
                              PIN_PARG(uintptr_t), arg0,
                              PIN_PARG(uintptr_t), arg1,
                              PIN_PARG(uintptr_t), arg2,
                              PIN_PARG(uintptr_t), arg3,
                              PIN_PARG(uintptr_t), arg4,
                              PIN_PARG(uintptr_t), arg5,
                              PIN_PARG_END());
  return ret;
}

uintptr_t WRAP_NAME(RtlInitializeCriticalSection)(WRAP_PARAM4) {
//  Printf("T%d pc=%p %s: %p\n", tid, pc, __FUNCTION__+8, arg0);
  DumpEvent(LOCK_CREATE, tid, pc, arg0, 0);
  return CallStdCallFun1(ctx, tid, f, arg0);
}
uintptr_t WRAP_NAME(RtlDeleteCriticalSection)(WRAP_PARAM4) {
//  Printf("T%d pc=%p %s: %p\n", tid, pc, __FUNCTION__+8, arg0);
  DumpEvent(LOCK_DESTROY, tid, pc, arg0, 0);
  return CallStdCallFun1(ctx, tid, f, arg0);
}
uintptr_t WRAP_NAME(RtlEnterCriticalSection)(WRAP_PARAM4) {
//  Printf("T%d pc=%p %s: %p\n", tid, pc, __FUNCTION__+8, arg0);
  uintptr_t ret = CallStdCallFun1(ctx, tid, f, arg0);
  DumpEvent(WRITER_LOCK, tid, pc, arg0, 0);
  return ret;
}
uintptr_t WRAP_NAME(RtlTryEnterCriticalSection)(WRAP_PARAM4) {
  // Printf("T%d pc=%p %s: %p\n", tid, pc, __FUNCTION__+5, arg0);
  uintptr_t ret = CallStdCallFun1(ctx, tid, f, arg0);
  if (ret) {
    DumpEvent(WRITER_LOCK, tid, pc, arg0, 0);
  }
  return ret;
}
uintptr_t WRAP_NAME(RtlLeaveCriticalSection)(WRAP_PARAM4) {
//  Printf("T%d pc=%p %s: %p\n", tid, pc, __FUNCTION__+8, arg0);
  DumpEvent(UNLOCK, tid, pc, arg0, 0);
  return CallStdCallFun1(ctx, tid, f, arg0);
}

uintptr_t WRAP_NAME(SetEvent)(WRAP_PARAM4) {
  //Printf("T%d before pc=%p %s: %p\n", tid, pc, __FUNCTION__+8, arg0);
  DumpEvent(SIGNAL, tid, pc, arg0, 0);
  uintptr_t ret = CallStdCallFun1(ctx, tid, f, arg0);
  //Printf("T%d after pc=%p %s: %p\n", tid, pc, __FUNCTION__+8, arg0);
  return ret;
}

uintptr_t WRAP_NAME(RtlInterlockedPushEntrySList)(WRAP_PARAM4) {
  DumpEvent(SIGNAL, tid, pc, arg1, 0);
  uintptr_t ret = CallStdCallFun2(ctx, tid, f, arg0, arg1);
  // Printf("T%d %s list=%p item=%p\n", tid, __FUNCTION__, arg0, arg1);
  return ret;
}

uintptr_t WRAP_NAME(RtlInterlockedPopEntrySList)(WRAP_PARAM4) {
  uintptr_t ret = CallStdCallFun1(ctx, tid, f, arg0);
  // Printf("T%d %s list=%p item=%p\n", tid, __FUNCTION__, arg0, ret);
  if (ret) {
    DumpEvent(WAIT_BEFORE, tid, pc, ret, 0);
    DumpEvent(WAIT_AFTER, tid, pc, 0, 0);
  }
  return ret;
}

uintptr_t WRAP_NAME(RtlAcquireSRWLockExclusive)(WRAP_PARAM4) {
  uintptr_t ret = CallStdCallFun1(ctx, tid, f, arg0);
  DumpEvent(WRITER_LOCK, tid, pc, arg0, 0);
  return ret;
}
uintptr_t WRAP_NAME(RtlAcquireSRWLockShared)(WRAP_PARAM4) {
  uintptr_t ret = CallStdCallFun1(ctx, tid, f, arg0);
  DumpEvent(READER_LOCK, tid, pc, arg0, 0);
  return ret;
}
uintptr_t WRAP_NAME(RtlTryAcquireSRWLockExclusive)(WRAP_PARAM4) {
  // Printf("T%d %s %p\n", tid, __FUNCTION__, arg0);
  uintptr_t ret = CallStdCallFun1(ctx, tid, f, arg0);
  if (ret) {
    DumpEvent(WRITER_LOCK, tid, pc, arg0, 0);
  }
  return ret;
}
uintptr_t WRAP_NAME(RtlTryAcquireSRWLockShared)(WRAP_PARAM4) {
  // Printf("T%d %s %p\n", tid, __FUNCTION__, arg0);
  uintptr_t ret = CallStdCallFun1(ctx, tid, f, arg0);
  if (ret) {
    DumpEvent(READER_LOCK, tid, pc, arg0, 0);
  }
  return ret;
}
uintptr_t WRAP_NAME(RtlReleaseSRWLockExclusive)(WRAP_PARAM4) {
  // Printf("T%d %s %p\n", tid, __FUNCTION__, arg0);
  DumpEvent(UNLOCK, tid, pc, arg0, 0);
  uintptr_t ret = CallStdCallFun1(ctx, tid, f, arg0);
  return ret;
}
uintptr_t WRAP_NAME(RtlReleaseSRWLockShared)(WRAP_PARAM4) {
  // Printf("T%d %s %p\n", tid, __FUNCTION__, arg0);
  DumpEvent(UNLOCK, tid, pc, arg0, 0);
  uintptr_t ret = CallStdCallFun1(ctx, tid, f, arg0);
  return ret;
}
uintptr_t WRAP_NAME(RtlInitializeSRWLock)(WRAP_PARAM4) {
  DumpEvent(LOCK_CREATE, tid, pc, arg0, 0);
  uintptr_t ret = CallStdCallFun1(ctx, tid, f, arg0);
  return ret;
}

uintptr_t WRAP_NAME(RtlWakeConditionVariable)(WRAP_PARAM4) {
  // Printf("T%d %s arg0=%p\n", tid, __FUNCTION__, arg0);
  DumpEvent(SIGNAL, tid, pc, arg0, 0);
  uintptr_t ret = CallStdCallFun1(ctx, tid, f, arg0);
  return ret;
}
uintptr_t WRAP_NAME(RtlAllWakeConditionVariable)(WRAP_PARAM4) {
  // Printf("T%d %s arg0=%p\n", tid, __FUNCTION__, arg0);
  DumpEvent(SIGNAL, tid, pc, arg0, 0);
  uintptr_t ret = CallStdCallFun1(ctx, tid, f, arg0);
  return ret;
}
uintptr_t WRAP_NAME(RtlSleepConditionVariableSRW)(WRAP_PARAM4) {
  uintptr_t ret = CallStdCallFun4(ctx, tid, f, arg0, arg1, arg2, arg3);
  DumpEvent(WAIT, tid, pc, arg0, 0);
  // Printf("T%d %s arg0=%p arg1=%p; ret=%d\n", tid, __FUNCTION__, arg0, arg1, ret);
  return ret;
}
uintptr_t WRAP_NAME(RtlSleepConditionVariableCS)(WRAP_PARAM4) {
  uintptr_t ret = CallStdCallFun3(ctx, tid, f, arg0, arg1, arg2);
  DumpEvent(WAIT, tid, pc, arg0, 0);
  // Printf("T%d %s arg0=%p arg1=%p; ret=%d\n", tid, __FUNCTION__, arg0, arg1, ret);
  return ret;
}

uintptr_t WRAP_NAME(RtlQueueWorkItem)(WRAP_PARAM4) {
  // Printf("T%d %s arg0=%p arg1=%p; arg2=%d\n", tid, __FUNCTION__, arg0, arg1, arg2);
  g_windows_thread_pool_calback_set->insert(arg0);
  DumpEvent(SIGNAL, tid, pc, arg0, 0);
  uintptr_t ret = CallStdCallFun3(ctx, tid, f, arg0, arg1, arg2);
  return ret;
}

uintptr_t WRAP_NAME(RegisterWaitForSingleObject)(WRAP_PARAM6) {
  // Printf("T%d %s arg0=%p arg2=%p\n", tid, __FUNCTION__, arg0, arg2);
  g_windows_thread_pool_calback_set->insert(arg2);
  DumpEvent(SIGNAL, tid, pc, arg2, 0);
  uintptr_t ret = CallStdCallFun6(ctx, tid, f, arg0, arg1, arg2, arg3, arg4, arg5);
  if (ret) {
    uintptr_t wait_object = *(uintptr_t*)arg0;
    (*g_windows_thread_pool_wait_object_map)[wait_object] = arg2;
    // Printf("T%d %s *arg0=%p\n", tid, __FUNCTION__, wait_object);
  }
  return ret;
}

uintptr_t WRAP_NAME(UnregisterWaitEx)(WRAP_PARAM4) {
  CHECK(g_windows_thread_pool_wait_object_map);
  uintptr_t obj = (*g_windows_thread_pool_wait_object_map)[arg0];
  // Printf("T%d %s arg0=%p obj=%p\n", tid, __FUNCTION__, arg0, obj);
  uintptr_t ret = CallStdCallFun2(ctx, tid, f, arg0, arg1);
  if (ret) {
    DumpEvent(WAIT, tid, pc, obj, 0);
  }
  return ret;
}

uintptr_t WRAP_NAME(VirtualAlloc)(WRAP_PARAM4) {
  Printf("T%d VirtualAlloc: %p %p %p %p\n", tid, arg0, arg1, arg2, arg3);
  uintptr_t ret = CallStdCallFun4(ctx, tid, f, arg0, arg1, arg2, arg3);
  return ret;
}

uintptr_t WRAP_NAME(GlobalAlloc)(WRAP_PARAM4) {
  uintptr_t ret = CallStdCallFun2(ctx, tid, f, arg0, arg1);
  Printf("T%d %s(%p %p)=%p\n", tid, __FUNCTION__, arg0, arg1, ret);
  if (ret != 0) {
    DumpEvent(MALLOC, tid, pc, ret, arg1);
  }
  return ret;
}

uintptr_t WRAP_NAME(ZwAllocateVirtualMemory)(WRAP_PARAM6) {
  // Printf("T%d >>%s(%p %p %p %p %p %p)\n", tid, __FUNCTION__, arg0, arg1, arg2, arg3, arg4, arg5);
  uintptr_t ret = CallStdCallFun6(ctx, tid, f, arg0, arg1, arg2, arg3, arg4, arg5);
  // Printf("T%d <<%s(%p %p) = %p\n", tid, __FUNCTION__, *(void**)arg1, *(void**)arg3, ret);
  if (ret == 0) {
    DumpEvent(MALLOC, tid, pc, *(uintptr_t*)arg1, *(uintptr_t*)arg3);
  }
  return ret;
}

uintptr_t WRAP_NAME(AllocateHeap)(WRAP_PARAM4) {
  uintptr_t ret = CallStdCallFun3(ctx, tid, f, arg0, arg1, arg2);
  // Printf("T%d RtlAllocateHeap(%p %p %p)=%p\n", tid, arg0, arg1, arg2, ret);
  if (ret != 0) {
    DumpEvent(MALLOC, tid, pc, ret, arg3);
  }
  return ret;
}

uintptr_t WRAP_NAME(HeapCreate)(WRAP_PARAM4) {
  uintptr_t ret = CallStdCallFun3(ctx, tid, f, arg0, arg1, arg2);
  Printf("T%d %s(%p %p %p)=%p\n", tid, __FUNCTION__, arg0, arg1, arg2, ret);
  return ret;
}


uintptr_t WRAP_NAME(WaitForSingleObjectEx)(WRAP_PARAM4) {
  if (G_flags->verbosity >= 1) {
    ShowPcAndSp(__FUNCTION__, tid, pc, 0);
    Printf("arg0=%lx arg1=%lx\n", arg0, arg1);
  }
  bool is_thread_handle = false;

  {
    ScopedReentrantClientLock lock(__LINE__);
    if (g_win_handles_which_are_threads) {
      is_thread_handle = g_win_handles_which_are_threads->count(arg0) > 0;
      g_win_handles_which_are_threads->erase(arg0);
    }
  }

  //Printf("T%d before pc=%p %s: %p\n", tid, pc, __FUNCTION__+8, arg0, arg1);
  uintptr_t ret = CallStdCallFun3(ctx, tid, f, arg0, arg1, arg2);
  //Printf("T%d after pc=%p %s: %p\n", tid, pc, __FUNCTION__+8, arg0, arg1);

  if (ret == 0) {
    if (is_thread_handle) {
      HandleThreadJoinAfter(tid, arg0);
    } else {
      DumpEvent(WAIT_BEFORE, tid, pc, arg0, 0);
      DumpEvent(WAIT_AFTER, tid, pc, 0, 0);
    }
  }

  return ret;
}

#endif  // _MSC_VER

//--------- main() --------------------------------- {{{2
void Before_main(THREADID tid, ADDRINT pc, ADDRINT argc, ADDRINT argv) {
  CHECK(tid == 0);
  main_entered = true;
}

void After_main(THREADID tid, ADDRINT pc) {
  CHECK(tid == 0);
  main_exited = true;
}

//--------- memory allocation ---------------------- {{{2
uintptr_t WRAP_NAME(mmap)(WRAP_PARAM6) {
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_6();

  if (ret != (ADDRINT)-1L) {
    DumpEvent(MALLOC, tid, pc, ret, arg1);
  }

  return ret;
}

uintptr_t WRAP_NAME(munmap)(WRAP_PARAM6) {
  PinThread &t = g_pin_threads[tid];
  TLEBFlushLocked(t);
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_6();
  // We don't actually send eny event here, no need.
  return ret;
}

uintptr_t WRAP_NAME(malloc)(WRAP_PARAM4) {
  IgnoreSyncAndMopsBegin(tid, pc);
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  IgnoreSyncAndMopsEnd(tid, pc);

  DumpEvent(MALLOC, tid, pc, ret, arg0);
  return ret;
}

uintptr_t WRAP_NAME(realloc)(WRAP_PARAM4) {
  PinThread &t = g_pin_threads[tid];
  TLEBFlushLocked(t);
  IgnoreSyncAndMopsBegin(tid, pc);
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  IgnoreSyncAndMopsEnd(tid, pc);

  // TODO: handle FREE? We don't do it in Valgrind right now.
  DumpEvent(MALLOC, tid, pc, ret, arg1);
  return ret;
}

uintptr_t WRAP_NAME(calloc)(WRAP_PARAM4) {
  IgnoreSyncAndMopsBegin(tid, pc);
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  IgnoreSyncAndMopsEnd(tid, pc);

  DumpEvent(MALLOC, tid, pc, ret, arg0*arg1);
  return ret;
}

uintptr_t WRAP_NAME(free)(WRAP_PARAM4) {
  DumpEvent(FREE, tid, pc, arg0, 0);

  IgnoreSyncAndMopsBegin(tid, pc);
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  IgnoreSyncAndMopsEnd(tid, pc);
  return ret;
}


//-------- Routines and stack ---------------------- {{{2
static void UpdateCallStack(PinThread &t, ADDRINT sp) {
  while (t.shadow_stack.size() > 0 && sp >= t.shadow_stack.back().sp) {
    TLEBAddRtnExit(t);
    size_t size = t.shadow_stack.size();
    CHECK(size < 1000000);  // stay sane.
    uintptr_t popped_pc = t.shadow_stack.back().pc;
#ifdef _MSC_VER
    // h-b edge from here to UnregisterWaitEx.
    CHECK(g_windows_thread_pool_calback_set);
    if (g_windows_thread_pool_calback_set->count(popped_pc)) {
      DumpEvent(SIGNAL, t.tid, 0, popped_pc, 0);
      // Printf("T%d ret %p\n", t.tid, popped_pc);
    }
#endif

    if (debug_rtn) {
      ShowPcAndSp("RET : ", t.tid, popped_pc, sp);
    }
    t.shadow_stack.pop_back();
    CHECK(size - 1 == t.shadow_stack.size());
    if (DEB_PR) {
      Printf("POP SHADOW STACK\n");
      PrintShadowStack(t);
    }
  }
}

void InsertBeforeEvent_SysCall(THREADID tid, ADDRINT sp) {
  PinThread &t = g_pin_threads[tid];
  UpdateCallStack(t, sp);
  TLEBFlushLocked(t);
}

void InsertBeforeEvent_Call(THREADID tid, ADDRINT pc, ADDRINT target,
                            ADDRINT sp, IGNORE_BELOW_RTN ignore_below) {
  PinThread &t = g_pin_threads[tid];
  DebugOnlyShowPcAndSp(__FUNCTION__, t.tid, pc, sp);
  UpdateCallStack(t, sp);
  TLEBAddRtnCall(t, pc, target, ignore_below);
  t.shadow_stack.push_back(StackFrame(target, sp));
  if (DEB_PR) {
    PrintShadowStack(t);
  }
  if (DEBUG_MODE && debug_rtn) {
    ShowPcAndSp("CALL: ", t.tid, target, sp);
  }

#ifdef _MSC_VER
  // h-b edge from RtlQueueWorkItem to here.
  CHECK(g_windows_thread_pool_calback_set);
  if (g_windows_thread_pool_calback_set->count(target)) {
    DumpEvent(WAIT_BEFORE, tid, pc, target, 0);
    DumpEvent(WAIT_AFTER, tid, pc, 0, 0);
  }
#endif
}

static void OnTraceNoMops(THREADID tid, ADDRINT sp) {
  PinThread &t = g_pin_threads[tid];
  UpdateCallStack(t, sp);
  G_stats->mops_per_trace[0]++;
}

static void OnTrace(THREADID tid, ADDRINT sp,
                    TraceInfo *trace_info,
                    uintptr_t **tls_reg_p) {
  PinThread &t = g_pin_threads[tid];
  DCHECK(trace_info);
  uintptr_t pc = trace_info->pc();
  DebugOnlyShowPcAndSp(__FUNCTION__, t.tid, pc, sp);

  UpdateCallStack(t, sp);

  size_t n = trace_info->n_mops();
  DCHECK(n > 0);

  t.trace_info = trace_info;
  trace_info->counter()++;
  *tls_reg_p = TLEBAddTrace(t);

  // stats
  const size_t mop_stat_size = TS_ARRAY_SIZE(G_stats->mops_per_trace);
  G_stats->mops_per_trace[n < mop_stat_size ? n : mop_stat_size - 1]++;
}

//---------- Memory accesses -------------------------- {{{2
// 'addr' is the section of t.tleb.events which is set in OnTrace.
// 'idx' is the number of this mop in its trace.
// 'a' is the actuall address.
// 'tid' is thread ID, used only in debug mode.
//
// In opt mode this is just one instruction! Something like this:
// mov %rcx,(%rdi,%rdx,8)
static void OnMop(uintptr_t *addr, THREADID tid, ADDRINT idx, ADDRINT a) {
  if (DEBUG_MODE) {
    PinThread &t= g_pin_threads[tid];
    CHECK(idx < kMaxMopsPerTrace);
    CHECK(idx < t.trace_info->n_mops());
    CHECK(addr >= t.tleb.events);
    CHECK(addr < t.tleb.events + kThreadLocksEventBufferSize);
    if (t.tleb.size > 0) {
      CHECK(addr + idx < t.tleb.events + t.tleb.size);
    } else {
      // t.tleb.size is zero. We just flushed but we are still
      // getting mop events from the old trace.
      // This way we may loose some races, but probably we won't
      // because such situation happens only (?) inside our interceptors.
    }
    if (a == G_flags->trace_addr) {
      Printf("T%d %s %lx\n", t.tid, __FUNCTION__, a);
    }
  }
  addr[idx] = a;
}

static void On_PredicatedMop(BOOL is_running, uintptr_t *addr,
                             THREADID tid, ADDRINT idx, ADDRINT a) {
  if (is_running) {
    OnMop(addr, tid, idx, a);
  }
}

//---------- I/O; exit------------------------------- {{{2
static const uintptr_t kIOMagic = 0x1234c678;

static void Before_SignallingIOCall(THREADID tid, ADDRINT pc) {
  DumpEvent(SIGNAL, tid, pc, kIOMagic, 0);
}

static void After_WaitingIOCall(THREADID tid, ADDRINT pc) {
  DumpEvent(WAIT_BEFORE, tid, pc, kIOMagic, 0);
  DumpEvent(WAIT_AFTER, tid, pc, 0, 0);
}

static const uintptr_t kAtexitMagic = 0x9876f432;

static void On_atexit(THREADID tid, ADDRINT pc) {
  DumpEvent(SIGNAL, tid, pc, kAtexitMagic, 0);
}

static void On_exit(THREADID tid, ADDRINT pc) {
  DumpEvent(WAIT_BEFORE, tid, pc, kAtexitMagic, 0);
  DumpEvent(WAIT_AFTER, tid, pc, 0, 0);
}

//---------- Synchronization -------------------------- {{{2
// locks
static void Before_pthread_unlock(THREADID tid, ADDRINT pc, ADDRINT mu) {
  DumpEvent(UNLOCK, tid, pc, mu, 0);
}

static uintptr_t WRAP_NAME(pthread_mutex_lock)(WRAP_PARAM4) {
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  DumpEvent(WRITER_LOCK, tid, pc, arg0, 0);
  return ret;
}

// In some versions of libpthread, pthread_spin_lock is effectively
// a recursive function. It jumps to its first insn:
//    beb0:       f0 ff 0f                lock decl (%rdi)
//    beb3:       75 0b                   jne    bec0 <pthread_spin_lock+0x10>
//    beb5:       31 c0                   xor    %eax,%eax
//    beb7:       c3                      retq
//    beb8:       0f 1f 84 00 00 00 00    nopl   0x0(%rax,%rax,1)
//    bebf:       00
//    bec0:       f3 90                   pause
//    bec2:       83 3f 00                cmpl   $0x0,(%rdi)
//    bec5:       7f e9  >>>>>>>>>>>>>    jg     beb0 <pthread_spin_lock> 
//    bec7:       eb f7                   jmp    bec0 <pthread_spin_lock+0x10>
//
// So, we need to act only when we return from the last (depth=0) invocation.
static uintptr_t WRAP_NAME(pthread_spin_lock)(WRAP_PARAM4) {
  PinThread &t= g_pin_threads[tid];
  t.spin_lock_recursion_depth++;
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  t.spin_lock_recursion_depth--;
  if (t.spin_lock_recursion_depth == 0) {
    DumpEvent(WRITER_LOCK, tid, pc, arg0, 0);
  }
  return ret;
}

static uintptr_t WRAP_NAME(pthread_rwlock_wrlock)(WRAP_PARAM4) {
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  DumpEvent(WRITER_LOCK, tid, pc, arg0, 0);
  return ret;
}

static uintptr_t WRAP_NAME(pthread_rwlock_rdlock)(WRAP_PARAM4) {
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  DumpEvent(READER_LOCK, tid, pc, arg0, 0);
  return ret;
}

static uintptr_t WRAP_NAME(pthread_mutex_trylock)(WRAP_PARAM4) {
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  if (ret == 0)
    DumpEvent(WRITER_LOCK, tid, pc, arg0, 0);
  return ret;
}

static uintptr_t WRAP_NAME(pthread_spin_trylock)(WRAP_PARAM4) {
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  if (ret == 0)
    DumpEvent(WRITER_LOCK, tid, pc, arg0, 0);
  return ret;
}

static uintptr_t WRAP_NAME(pthread_spin_init)(WRAP_PARAM4) {
  DumpEvent(UNLOCK_OR_INIT, tid, pc, arg0, 0);
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  return ret;
}
static uintptr_t WRAP_NAME(pthread_spin_destroy)(WRAP_PARAM4) {
  DumpEvent(LOCK_DESTROY, tid, pc, arg0, 0);
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  return ret;
}
static uintptr_t WRAP_NAME(pthread_spin_unlock)(WRAP_PARAM4) {
  DumpEvent(UNLOCK_OR_INIT, tid, pc, arg0, 0);
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  return ret;
}

static uintptr_t WRAP_NAME(pthread_rwlock_trywrlock)(WRAP_PARAM4) {
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  if (ret == 0)
    DumpEvent(WRITER_LOCK, tid, pc, arg0, 0);
  return ret;
}

static uintptr_t WRAP_NAME(pthread_rwlock_tryrdlock)(WRAP_PARAM4) {
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  if (ret == 0)
    DumpEvent(READER_LOCK, tid, pc, arg0, 0);
  return ret;
}


static void Before_pthread_mutex_init(THREADID tid, ADDRINT pc, ADDRINT mu) {
  DumpEvent(LOCK_CREATE, tid, pc, mu, 0);
}
static void Before_pthread_rwlock_init(THREADID tid, ADDRINT pc, ADDRINT mu) {
  DumpEvent(LOCK_CREATE, tid, pc, mu, 0);
}

static void Before_pthread_mutex_destroy(THREADID tid, ADDRINT pc, ADDRINT mu) {
  DumpEvent(LOCK_DESTROY, tid, pc, mu, 0);
}
static void Before_pthread_rwlock_destroy(THREADID tid, ADDRINT pc, ADDRINT mu) {
  DumpEvent(LOCK_DESTROY, tid, pc, mu, 0);
}

// barrier
static uintptr_t WRAP_NAME(pthread_barrier_init)(WRAP_PARAM4) {
  DumpEvent(CYCLIC_BARRIER_INIT, tid, pc, arg0, arg2);
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  return ret;
}
static uintptr_t WRAP_NAME(pthread_barrier_wait)(WRAP_PARAM4) {
  DumpEvent(CYCLIC_BARRIER_WAIT_BEFORE, tid, pc, arg0, 0);
  uintptr_t ret = CALL_ME_INSIDE_WRAPPER_4();
  DumpEvent(CYCLIC_BARRIER_WAIT_AFTER, tid, pc, arg0, 0);
  return ret;
}


// condvar
static void Before_pthread_cond_signal(THREADID tid, ADDRINT pc, ADDRINT cv) {
  DumpEvent(SIGNAL, tid, pc, cv, 0);
}
static void Before_pthread_cond_wait(THREADID tid, ADDRINT pc,
                                     ADDRINT cv, ADDRINT mu) {
  DumpEvent(WAIT_BEFORE, tid, pc, cv, mu);
}
static void After_pthread_cond_wait(THREADID tid, ADDRINT pc) {
  DumpEvent(WAIT_AFTER, tid, pc, 0, 0);
}
static void After_pthread_cond_timedwait(THREADID tid, ADDRINT pc,
                                         ADDRINT ret) {
  if (ret == 0) {
    DumpEvent(WAIT_AFTER, tid, pc, 0, 0);
  } else {
    DumpEvent(TWAIT_AFTER, tid, pc, 0, 0);
  }
}

// sem
static void After_sem_open(THREADID tid, ADDRINT pc, ADDRINT ret) {
  // TODO(kcc): need to handle it more precise?
  DumpEvent(SIGNAL, tid, pc, ret, 0);
}
static void Before_sem_post(THREADID tid, ADDRINT pc, ADDRINT sem) {
  DumpEvent(SIGNAL, tid, pc, sem, 0);
}
static void Before_sem_wait(THREADID tid, ADDRINT pc, ADDRINT sem) {
  DumpEvent(WAIT_BEFORE, tid, pc, sem, 0);
}
static void After_sem_wait(THREADID tid, ADDRINT pc) {
  DumpEvent(WAIT_AFTER, tid, pc, 0, 0);
}
static void After_sem_trywait(THREADID tid, ADDRINT pc, ADDRINT ret) {
  if (ret == 0) {
    DumpEvent(WAIT_AFTER, tid, pc, 0, 0);
  } else {
    DumpEvent(TWAIT_AFTER, tid, pc, 0, 0);
  }
}

//--------- Annotations -------------------------- {{{2
static void On_AnnotateBenignRace(THREADID tid, ADDRINT pc,
                                  ADDRINT file, ADDRINT line,
                                  ADDRINT a, ADDRINT descr) {
  DumpEvent(BENIGN_RACE, tid, descr, a, 1);
}

static void On_AnnotateBenignRaceSized(THREADID tid, ADDRINT pc,
                                       ADDRINT file, ADDRINT line,
                                       ADDRINT a, ADDRINT size, ADDRINT descr) {
  DumpEvent(BENIGN_RACE, tid, descr, a, size);
}

static void On_AnnotateExpectRace(THREADID tid, ADDRINT pc,
                                  ADDRINT file, ADDRINT line,
                                  ADDRINT a, ADDRINT descr) {
  DumpEvent(EXPECT_RACE, tid, descr, a, 1);
}

static void On_AnnotateTraceMemory(THREADID tid, ADDRINT pc,
                                   ADDRINT file, ADDRINT line,
                                   ADDRINT a) {
  DumpEvent(TRACE_MEM, tid, pc, a, 0);
}

static void On_AnnotateNewMemory(THREADID tid, ADDRINT pc,
                                   ADDRINT file, ADDRINT line,
                                   ADDRINT a, ADDRINT size) {
  DumpEvent(MALLOC, tid, pc, a, size);
}

static void On_AnnotateNoOp(THREADID tid, ADDRINT pc,
                            ADDRINT file, ADDRINT line, ADDRINT a) {
  Printf("%s T%d: %s:%d %p\n", __FUNCTION__, tid, (char*)file, (int)line, a);
  //DumpEvent(STACK_TRACE, tid, pc, 0, 0);
//  PrintShadowStack(tid);
}

static void On_AnnotateFlushState(THREADID tid, ADDRINT pc,
                                  ADDRINT file, ADDRINT line) {
  DumpEvent(FLUSH_STATE, tid, pc, 0, 0);
}

static void On_AnnotateCondVarSignal(THREADID tid, ADDRINT pc,
                                     ADDRINT file, ADDRINT line, ADDRINT obj) {
  DumpEvent(SIGNAL, tid, pc, obj, 0);
}

static void On_AnnotateCondVarWait(THREADID tid, ADDRINT pc,
                                   ADDRINT file, ADDRINT line, ADDRINT obj) {
  DumpEvent(WAIT_BEFORE, tid, pc, obj, 0);
  DumpEvent(WAIT_AFTER, tid, pc, obj, 0);
}


static void On_AnnotateIgnoreReadsBegin(THREADID tid, ADDRINT pc,
                                        ADDRINT file, ADDRINT line) {
  DumpEvent(IGNORE_READS_BEG, tid, pc, 0, 0);
}
static void On_AnnotateIgnoreReadsEnd(THREADID tid, ADDRINT pc,
                                      ADDRINT file, ADDRINT line) {
  DumpEvent(IGNORE_READS_END, tid, pc, 0, 0);
}
static void On_AnnotateIgnoreWritesBegin(THREADID tid, ADDRINT pc,
                                         ADDRINT file, ADDRINT line) {
  DumpEvent(IGNORE_WRITES_BEG, tid, pc, 0, 0);
}
static void On_AnnotateIgnoreWritesEnd(THREADID tid, ADDRINT pc,
                                       ADDRINT file, ADDRINT line) {
  DumpEvent(IGNORE_WRITES_END, tid, pc, 0, 0);
}

static void On_AnnotatePublishMemoryRange(THREADID tid, ADDRINT pc,
                                          ADDRINT file, ADDRINT line,
                                          ADDRINT a, ADDRINT size) {
  DumpEvent(PUBLISH_RANGE, tid, pc, a, size);
}

static void On_AnnotateUnpublishMemoryRange(THREADID tid, ADDRINT pc,
                                          ADDRINT file, ADDRINT line,
                                          ADDRINT a, ADDRINT size) {
//  Printf("T%d %s %lx %lx\n", tid, __FUNCTION__, a, size);
  DumpEvent(UNPUBLISH_RANGE, tid, pc, a, size);
}


static void On_AnnotateMutexIsUsedAsCondVar(THREADID tid, ADDRINT pc,
                                            ADDRINT file, ADDRINT line,
                                            ADDRINT mu) {
  DumpEvent(HB_LOCK, tid, pc, mu, 0);
}

static void On_AnnotatePCQCreate(THREADID tid, ADDRINT pc,
                                 ADDRINT file, ADDRINT line,
                                 ADDRINT pcq) {
  DumpEvent(PCQ_CREATE, tid, pc, pcq, 0);
}

static void On_AnnotatePCQDestroy(THREADID tid, ADDRINT pc,
                                  ADDRINT file, ADDRINT line,
                                  ADDRINT pcq) {
  DumpEvent(PCQ_DESTROY, tid, pc, pcq, 0);
}

static void On_AnnotatePCQPut(THREADID tid, ADDRINT pc,
                              ADDRINT file, ADDRINT line,
                              ADDRINT pcq) {
  DumpEvent(PCQ_PUT, tid, pc, pcq, 0);
}

static void On_AnnotatePCQGet(THREADID tid, ADDRINT pc,
                              ADDRINT file, ADDRINT line,
                              ADDRINT pcq) {
  DumpEvent(PCQ_GET, tid, pc, pcq, 0);
}

//--------- Instrumentation ----------------------- {{{1
static bool IgnoreImage(IMG img) {
  string name = IMG_Name(img);
  if (name.find("/ld-") != string::npos)
    return true;
  return false;
}

static bool IgnoreRtn(RTN rtn) {
  CHECK(rtn != RTN_Invalid());
  ADDRINT rtn_address = RTN_Address(rtn);
  if (ThreadSanitizerWantToInstrumentSblock(rtn_address) == false)
    return true;
  return false;
}

static bool InstrumentCall(INS ins) {
  // Call.
  if (INS_IsProcedureCall(ins) && !INS_IsSyscall(ins)) {
    IGNORE_BELOW_RTN ignore_below = IGNORE_BELOW_RTN_UNKNOWN;
    if (INS_IsDirectBranchOrCall(ins)) {
      ADDRINT target = INS_DirectBranchOrCallTargetAddress(ins);
      bool ignore = ThreadSanitizerIgnoreAccessesBelowFunction(target);
      ignore_below = ignore ? IGNORE_BELOW_RTN_YES : IGNORE_BELOW_RTN_NO;
    }
    INS_InsertCall(ins, IPOINT_BEFORE,
                   (AFUNPTR)InsertBeforeEvent_Call,
                   IARG_THREAD_ID,
                   IARG_INST_PTR,
                   IARG_BRANCH_TARGET_ADDR,
                   IARG_REG_VALUE, REG_STACK_PTR,
                   IARG_ADDRINT, ignore_below,
                   IARG_END);
    return true;
  }
  if (INS_IsSyscall(ins)) {
    INS_InsertCall(ins, IPOINT_BEFORE,
                   (AFUNPTR)InsertBeforeEvent_SysCall,
                   IARG_THREAD_ID,
                   IARG_REG_VALUE, REG_STACK_PTR,
                   IARG_END);
  }
  return false;
}


// return the number of inserted instrumentations.
static void InstrumentMopsInBBl(BBL bbl, RTN rtn, TraceInfo *trace_info, size_t *mop_idx) {
  INS tail = BBL_InsTail(bbl);
  // All memory reads/writes
  for( INS ins = BBL_InsHead(bbl);
       INS_Valid(ins);
       ins = INS_Next(ins) ) {
    if (ins != tail) {
      CHECK(!INS_IsRet(ins));
      CHECK(!INS_IsProcedureCall(ins));
    }
    // bool is_stack = INS_IsStackRead(ins) || INS_IsStackWrite(ins);
    if (INS_IsAtomicUpdate(ins)) continue;

    int n_mops = INS_MemoryOperandCount(ins);
    if (n_mops == 0) continue;

    string opcode_str = OPCODE_StringShort(INS_Opcode(ins));
    if (trace_info && debug_ins) {
      Printf("  INS: opcode=%s n_mops=%d dis=\"%s\"\n",
             opcode_str.c_str(),  n_mops,
             INS_Disassemble(ins).c_str());
    }

    bool ins_ignore_writes = false;
    bool ins_ignore_reads = false;

    // CALL writes to stack and (if the call is indirect) reads the target
    // address. We don't want to handle the stack write.
    if (INS_IsCall(ins)) {
      CHECK(n_mops == 1 || n_mops == 2);
      ins_ignore_writes = true;
    }

    // PUSH: we ignore the write to stack but we don't ignore the read (if any).
    if (opcode_str == "PUSH") {
      CHECK(n_mops == 1 || n_mops == 2);
      ins_ignore_writes = true;
    }

    // POP: we are reading from stack, Ignore it.
    if (opcode_str == "POP") {
      CHECK(n_mops == 1 || n_mops == 2);
      ins_ignore_reads = true;
      continue;
    }

    // RET/LEAVE -- ignore it, it just reads the return address and stack.
    if (INS_IsRet(ins) || opcode_str == "LEAVE") {
      CHECK(n_mops == 1);
      continue;
    }

    bool is_predicated = INS_IsPredicated(ins);
    for (int i = 0; i < n_mops; i++) {
      if (*mop_idx >= kMaxMopsPerTrace) {
        Report("INFO: too many mops in trace: %d %s\n",
            INS_Address(ins), PcToRtnName(INS_Address(ins), true).c_str());
        return;
      }
      size_t size = INS_MemoryOperandSize(ins, i);
      CHECK(size);
      bool is_write = INS_MemoryOperandIsWritten(ins, i);

      if (ins_ignore_writes && is_write) continue;
      if (ins_ignore_reads && !is_write) continue;

      if (trace_info) {
        if (debug_ins) {
          Printf("    size=%ld is_w=%d\n", size, (int)is_write);
        }
        MopInfo *mop = trace_info->GetMop(*mop_idx);
        mop->pc = INS_Address(ins);
        mop->size = size;
        mop->is_write = is_write;
        if (is_predicated) {
          INS_InsertPredicatedCall(ins, IPOINT_BEFORE,
            (AFUNPTR)On_PredicatedMop,
            IARG_EXECUTING,
            IARG_REG_VALUE, tls_reg,
            IARG_THREAD_ID,
            IARG_ADDRINT, *mop_idx,
            IARG_MEMORYOP_EA, i,
            IARG_END);
        } else {
          INS_InsertCall(ins, IPOINT_BEFORE,
            (AFUNPTR)OnMop,
            IARG_REG_VALUE, tls_reg,
            IARG_THREAD_ID,
            IARG_ADDRINT, *mop_idx,
            IARG_MEMORYOP_EA, i,
            IARG_END);
        }
      }
      (*mop_idx)++;
    }
  }
}

void CallbackForTRACE(TRACE trace, void *v) {
  CHECK(n_started_threads > 0);
#if 0
  if (n_started_threads == 1) {
    // There are no threads running other than the main thread.
    // Do not instrument anything. When another thread starts,
    // we will flush the code cache.
    return;
  }
#endif  // _MSC_VER

  RTN rtn = TRACE_Rtn(trace);
  bool ignore_memory = false;
  string img_name = "<>";
  string rtn_name = "<>";
  if (RTN_Valid(rtn)) {
    SEC sec = RTN_Sec(rtn);
    IMG img = SEC_Img(sec);
    rtn_name = RTN_Name(rtn);
    img_name = IMG_Name(img);

    if (IgnoreImage(img)) {
      // Printf("Ignoring memory accesses in %s\n", IMG_Name(img).c_str());
      ignore_memory = true;
    } else if (IgnoreRtn(rtn)) {
      ignore_memory = true;
    }
  }

  size_t n_mops = 0;
  // count the mops.
  for(BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
    if (!ignore_memory) {
      InstrumentMopsInBBl(bbl, rtn, NULL, &n_mops);
    }
  }

  // Handle the head of the trace
  INS head = BBL_InsHead(TRACE_BblHead(trace));
  CHECK(n_mops <= kMaxMopsPerTrace);

  TraceInfo *trace_info = NULL;
  if (n_mops) {
    trace_info = TraceInfo::NewTraceInfo(n_mops, INS_Address(head));
    INS_InsertCall(head, IPOINT_BEFORE,
                   (AFUNPTR)OnTrace,
                   IARG_THREAD_ID,
                   IARG_REG_VALUE, REG_STACK_PTR,
                   IARG_PTR, trace_info,
                   IARG_REG_REFERENCE, tls_reg,
                   IARG_END);
  } else {
    INS_InsertCall(head, IPOINT_BEFORE,
                   (AFUNPTR)OnTraceNoMops,
                   IARG_THREAD_ID,
                   IARG_REG_VALUE, REG_STACK_PTR,
                   IARG_END);
  }

  // instrument the mops. We want to do it after we instrumented the head
  // to maintain the right order of instrumentation callbacks (head first, then
  // mops).
  size_t i = 0;
  if (n_mops) {
    if (debug_ins) {
      Printf("TRACE %ld (%p); n_mops=%ld %s\n", trace_info->id(),
             TRACE_Address(trace),
             trace_info->n_mops(),
             PcToRtnName(trace_info->pc(), false).c_str());
    }
    for(BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
      InstrumentMopsInBBl(bbl, rtn, trace_info, &i);
    }
  }

  // instrument the calls, do it after all other instrumentation.
  for(BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
   InstrumentCall(BBL_InsTail(bbl));
  }
  CHECK(n_mops == i);
}


#define INSERT_FN_HELPER(point, name, rtn, to_insert, ...) \
    RTN_Open(rtn); \
    if (G_flags->verbosity >= 2) Printf("RTN: Inserting %-50s (%s) %s (%s) img: %s\n", \
    #to_insert, #point, RTN_Name(rtn).c_str(), name, IMG_Name(img).c_str());\
    RTN_InsertCall(rtn, point, (AFUNPTR)to_insert, IARG_THREAD_ID, \
                   IARG_INST_PTR, __VA_ARGS__, IARG_END);\
    RTN_Close(rtn); \

#define INSERT_FN(point, name, to_insert, ...) \
  while (RtnMatchesName(rtn_name, name)) {\
    INSERT_FN_HELPER(point, name, rtn, to_insert, __VA_ARGS__); \
    break;\
  }\


#define INSERT_BEFORE_FN(name, to_insert, ...) \
    INSERT_FN(IPOINT_BEFORE, name, to_insert, __VA_ARGS__)

#define INSERT_BEFORE_0(name, to_insert) \
    INSERT_BEFORE_FN(name, to_insert, IARG_END);

#define INSERT_BEFORE_1(name, to_insert) \
    INSERT_BEFORE_FN(name, to_insert, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 0)

#define INSERT_BEFORE_2(name, to_insert) \
    INSERT_BEFORE_FN(name, to_insert, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 0, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 1)

#define INSERT_BEFORE_3(name, to_insert) \
    INSERT_BEFORE_FN(name, to_insert, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 0, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 1, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 2)

#define INSERT_BEFORE_4(name, to_insert) \
    INSERT_BEFORE_FN(name, to_insert, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 0, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 1, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 2, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 3)

#define INSERT_BEFORE_5(name, to_insert) \
    INSERT_BEFORE_FN(name, to_insert, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 0, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 1, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 2, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 3, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 4)

#define INSERT_BEFORE_6(name, to_insert) \
    INSERT_BEFORE_FN(name, to_insert, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 0, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 1, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 2, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 3, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 4, \
                     IARG_FUNCARG_ENTRYPOINT_VALUE, 5)

#define INSERT_AFTER_FN(name, to_insert, ...) \
    INSERT_FN(IPOINT_AFTER, name, to_insert, __VA_ARGS__)

#define INSERT_AFTER_0(name, to_insert) \
    INSERT_AFTER_FN(name, to_insert, IARG_END)

#define INSERT_AFTER_1(name, to_insert) \
    INSERT_AFTER_FN(name, to_insert, IARG_FUNCRET_EXITPOINT_VALUE)


#ifdef _MSC_VER
void WrapStdCallFunc1(RTN rtn, char *name, AFUNPTR replacement_func) {
  if (RTN_Valid(rtn) && RtnMatchesName(RTN_Name(rtn), name)) {
    InformAboutFunctionWrap(rtn, name);
    PROTO proto = PROTO_Allocate(PIN_PARG(uintptr_t),
                                 CALLINGSTD_STDCALL,
                                 "proto",
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG_END());
    RTN_ReplaceSignature(rtn,
                         AFUNPTR(replacement_func),
                         IARG_PROTOTYPE, proto,
                         IARG_THREAD_ID,
                         IARG_INST_PTR,
                         IARG_CONTEXT,
                         IARG_ORIG_FUNCPTR,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                         IARG_END);
    PROTO_Free(proto);
  }
}

void WrapStdCallFunc2(RTN rtn, char *name, AFUNPTR replacement_func) {
  if (RTN_Valid(rtn) && RtnMatchesName(RTN_Name(rtn), name)) {
    InformAboutFunctionWrap(rtn, name);
    PROTO proto = PROTO_Allocate(PIN_PARG(uintptr_t),
                                 CALLINGSTD_STDCALL,
                                 "proto",
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG_END());
    RTN_ReplaceSignature(rtn,
                         AFUNPTR(replacement_func),
                         IARG_PROTOTYPE, proto,
                         IARG_THREAD_ID,
                         IARG_INST_PTR,
                         IARG_CONTEXT,
                         IARG_ORIG_FUNCPTR,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
                         IARG_END);
    PROTO_Free(proto);
  }
}

void WrapStdCallFunc3(RTN rtn, char *name, AFUNPTR replacement_func) {
  if (RTN_Valid(rtn) && RtnMatchesName(RTN_Name(rtn), name)) {
    InformAboutFunctionWrap(rtn, name);
    PROTO proto = PROTO_Allocate(PIN_PARG(uintptr_t),
                                 CALLINGSTD_STDCALL,
                                 "proto",
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG_END());
    RTN_ReplaceSignature(rtn,
                         AFUNPTR(replacement_func),
                         IARG_PROTOTYPE, proto,
                         IARG_THREAD_ID,
                         IARG_INST_PTR,
                         IARG_CONTEXT,
                         IARG_ORIG_FUNCPTR,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
                         IARG_END);
    PROTO_Free(proto);
  }
}

void WrapStdCallFunc4(RTN rtn, char *name, AFUNPTR replacement_func) {
  if (RTN_Valid(rtn) && RtnMatchesName(RTN_Name(rtn), name)) {
    InformAboutFunctionWrap(rtn, name);
    PROTO proto = PROTO_Allocate(PIN_PARG(uintptr_t),
                                 CALLINGSTD_STDCALL,
                                 "proto",
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG_END());
    RTN_ReplaceSignature(rtn,
                         AFUNPTR(replacement_func),
                         IARG_PROTOTYPE, proto,
                         IARG_THREAD_ID,
                         IARG_INST_PTR,
                         IARG_CONTEXT,
                         IARG_ORIG_FUNCPTR,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 3,
                         IARG_END);
    PROTO_Free(proto);
  }
}

void WrapStdCallFunc6(RTN rtn, char *name, AFUNPTR replacement_func) {
  if (RTN_Valid(rtn) && RtnMatchesName(RTN_Name(rtn), name)) {
    InformAboutFunctionWrap(rtn, name);
    PROTO proto = PROTO_Allocate(PIN_PARG(uintptr_t),
                                 CALLINGSTD_STDCALL,
                                 "proto",
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG(uintptr_t),
                                 PIN_PARG_END());
    RTN_ReplaceSignature(rtn,
                         AFUNPTR(replacement_func),
                         IARG_PROTOTYPE, proto,
                         IARG_THREAD_ID,
                         IARG_INST_PTR,
                         IARG_CONTEXT,
                         IARG_ORIG_FUNCPTR,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 3,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 4,
                         IARG_FUNCARG_ENTRYPOINT_VALUE, 5,
                         IARG_END);
    PROTO_Free(proto);
  }
}


#endif

static void MaybeInstrumentOneRoutine(IMG img, RTN rtn) {
  if (IgnoreImage(img)) {
    return;
  }
  string rtn_name = RTN_Name(rtn);
  string img_name = IMG_Name(img);
  if (debug_wrap) {
    Printf("%s: %s %s pc=%p\n", __FUNCTION__, rtn_name.c_str(),
           img_name.c_str(), RTN_Address(rtn));
  }

  // main()
  INSERT_BEFORE_2("main", Before_main);
  INSERT_AFTER_0("main", After_main);

  // malloc/free/etc
  WrapFunc4(img, rtn, "malloc", (AFUNPTR)WRAP_NAME(malloc));
  WrapFunc4(img, rtn, "realloc", (AFUNPTR)WRAP_NAME(realloc));
  WrapFunc4(img, rtn, "calloc", (AFUNPTR)WRAP_NAME(calloc));
  WrapFunc4(img, rtn, "free", (AFUNPTR)WRAP_NAME(free));

  // Linux: operator new/delete
  WrapFunc4(img, rtn, "_Znwm", (AFUNPTR)WRAP_NAME(malloc));
  WrapFunc4(img, rtn, "_Znam", (AFUNPTR)WRAP_NAME(malloc));
  WrapFunc4(img, rtn, "_Znwj", (AFUNPTR)WRAP_NAME(malloc));
  WrapFunc4(img, rtn, "_Znaj", (AFUNPTR)WRAP_NAME(malloc));
  WrapFunc4(img, rtn, "_ZnwmRKSt9nothrow_t", (AFUNPTR)WRAP_NAME(malloc));
  WrapFunc4(img, rtn, "_ZnamRKSt9nothrow_t", (AFUNPTR)WRAP_NAME(malloc));
  WrapFunc4(img, rtn, "_ZnwjRKSt9nothrow_t", (AFUNPTR)WRAP_NAME(malloc));
  WrapFunc4(img, rtn, "_ZnajRKSt9nothrow_t", (AFUNPTR)WRAP_NAME(malloc));
  WrapFunc4(img, rtn, "_ZdaPv", (AFUNPTR)WRAP_NAME(free));
  WrapFunc4(img, rtn, "_ZdlPv", (AFUNPTR)WRAP_NAME(free));
  WrapFunc4(img, rtn, "_ZdlPvRKSt9nothrow_t", (AFUNPTR)WRAP_NAME(free));
  WrapFunc4(img, rtn, "_ZdaPvRKSt9nothrow_t", (AFUNPTR)WRAP_NAME(free));

  // Windows: operator new/delete
  WrapFunc4(img, rtn, "operator new", (AFUNPTR)WRAP_NAME(malloc));
  WrapFunc4(img, rtn, "operator new[]", (AFUNPTR)WRAP_NAME(malloc));
  WrapFunc4(img, rtn, "operator delete", (AFUNPTR)WRAP_NAME(free));
  WrapFunc4(img, rtn, "operator delete[]", (AFUNPTR)WRAP_NAME(free));

  WrapFunc6(img, rtn, "mmap", (AFUNPTR)WRAP_NAME(mmap));
  WrapFunc6(img, rtn, "munmap", (AFUNPTR)WRAP_NAME(munmap));

  // ThreadSanitizerQuery
  WrapFunc4(img, rtn, "ThreadSanitizerQuery",
            (AFUNPTR)WRAP_NAME(ThreadSanitizerQuery));

  // pthread create/join
  WrapFunc4(img, rtn, "pthread_create", (AFUNPTR)WRAP_NAME(pthread_create));
  WrapFunc4(img, rtn, "pthread_join", (AFUNPTR)WRAP_NAME(pthread_join));

  INSERT_FN(IPOINT_BEFORE, "start_thread",
            Before_start_thread,
            IARG_REG_VALUE, REG_STACK_PTR, IARG_END);

   // pthread_cond_*
  INSERT_BEFORE_1("pthread_cond_signal", Before_pthread_cond_signal);
  INSERT_BEFORE_2("pthread_cond_wait", Before_pthread_cond_wait);
  INSERT_AFTER_0("pthread_cond_wait", After_pthread_cond_wait);

  INSERT_BEFORE_2("pthread_cond_timedwait", Before_pthread_cond_wait);
  INSERT_AFTER_1("pthread_cond_timedwait", After_pthread_cond_timedwait);

  // pthread_mutex_*
  INSERT_BEFORE_1("pthread_mutex_init", Before_pthread_mutex_init);
  INSERT_BEFORE_1("pthread_mutex_destroy", Before_pthread_mutex_destroy);
  INSERT_BEFORE_1("pthread_mutex_unlock", Before_pthread_unlock);


  WRAP4(pthread_mutex_lock);
  WRAP4(pthread_mutex_trylock);
  WRAP4(pthread_spin_lock);
  WRAP4(pthread_spin_trylock);
  WRAP4(pthread_spin_init);
  WRAP4(pthread_spin_destroy);
  WRAP4(pthread_spin_unlock);
  WRAP4(pthread_rwlock_wrlock);
  WRAP4(pthread_rwlock_rdlock);
  WRAP4(pthread_rwlock_trywrlock);
  WRAP4(pthread_rwlock_tryrdlock);

  // pthread_rwlock_*
  INSERT_BEFORE_1("pthread_rwlock_init", Before_pthread_rwlock_init);
  INSERT_BEFORE_1("pthread_rwlock_destroy", Before_pthread_rwlock_destroy);
  INSERT_BEFORE_1("pthread_rwlock_unlock", Before_pthread_unlock);

  // pthread_barrier_*
  WrapFunc4(img, rtn, "pthread_barrier_init",
            (AFUNPTR)WRAP_NAME(pthread_barrier_init));
  WrapFunc4(img, rtn, "pthread_barrier_wait",
            (AFUNPTR)WRAP_NAME(pthread_barrier_wait));

  // sem_*
  INSERT_AFTER_1("sem_open", After_sem_open);
  INSERT_BEFORE_1("sem_post", Before_sem_post);
  INSERT_BEFORE_1("sem_wait", Before_sem_wait);
  INSERT_AFTER_0("sem_wait", After_sem_wait);
  INSERT_BEFORE_1("sem_trywait", Before_sem_wait);
  INSERT_AFTER_1("sem_trywait", After_sem_trywait);


#ifdef _MSC_VER
  WrapStdCallFunc6(rtn, "CreateThread", (AFUNPTR)WRAP_NAME(CreateThread));

  INSERT_FN(IPOINT_BEFORE, "BaseThreadInitThunk",
            Before_BaseThreadInitThunk,
            IARG_REG_VALUE, REG_STACK_PTR, IARG_END);

  INSERT_BEFORE_0("RtlExitUserThread", Before_RtlExitUserThread);
  INSERT_BEFORE_0("ExitThread", Before_RtlExitUserThread);

  WRAPSTD1(RtlInitializeCriticalSection);
  WRAPSTD1(RtlDeleteCriticalSection);
  WRAPSTD1(RtlEnterCriticalSection);
  WRAPSTD1(RtlTryEnterCriticalSection);
  WRAPSTD1(RtlLeaveCriticalSection);
  WRAPSTD1(SetEvent);

  WRAPSTD1(RtlInterlockedPopEntrySList);
  WRAPSTD2(RtlInterlockedPushEntrySList);

#if 1
  WRAPSTD1(RtlAcquireSRWLockExclusive);
  WRAPSTD1(RtlAcquireSRWLockShared);
  WRAPSTD1(RtlTryAcquireSRWLockExclusive);
  WRAPSTD1(RtlTryAcquireSRWLockShared);
  WRAPSTD1(RtlReleaseSRWLockExclusive);
  WRAPSTD1(RtlReleaseSRWLockShared);
  WRAPSTD1(RtlInitializeSRWLock);

  WRAPSTD1(RtlWakeConditionVariable);
  WRAPSTD1(RtlAllWakeConditionVariable);
  WRAPSTD4(RtlSleepConditionVariableSRW);
  WRAPSTD3(RtlSleepConditionVariableCS);
#endif

  WRAPSTD3(RtlQueueWorkItem);
  WRAPSTD6(RegisterWaitForSingleObject);
  WRAPSTD2(UnregisterWaitEx);

  WRAPSTD3(WaitForSingleObjectEx);

  WrapStdCallFunc4(rtn, "VirtualAlloc", (AFUNPTR)(WRAP_NAME(VirtualAlloc)));
  WrapStdCallFunc6(rtn, "ZwAllocateVirtualMemory", (AFUNPTR)(WRAP_NAME(ZwAllocateVirtualMemory)));
  WrapStdCallFunc2(rtn, "GlobalAlloc", (AFUNPTR)WRAP_NAME(GlobalAlloc));
//  WrapStdCallFunc3(rtn, "RtlAllocateHeap", (AFUNPTR) WRAP_NAME(AllocateHeap));
//  WrapStdCallFunc3(rtn, "HeapCreate", (AFUNPTR) WRAP_NAME(HeapCreate));
#endif

  // Annotations.
  INSERT_BEFORE_4("AnnotateBenignRace", On_AnnotateBenignRace);
  INSERT_BEFORE_5("AnnotateBenignRaceSized", On_AnnotateBenignRaceSized);
  INSERT_BEFORE_4("AnnotateExpectRace", On_AnnotateExpectRace);
  INSERT_BEFORE_3("AnnotateTraceMemory", On_AnnotateTraceMemory);
  INSERT_BEFORE_4("AnnotateNewMemory", On_AnnotateNewMemory);
  INSERT_BEFORE_3("AnnotateNoOp", On_AnnotateNoOp);
  INSERT_BEFORE_2("AnnotateFlushState", On_AnnotateFlushState);

  INSERT_BEFORE_3("AnnotateCondVarWait", On_AnnotateCondVarWait);
  INSERT_BEFORE_3("AnnotateCondVarSignal", On_AnnotateCondVarSignal);
  INSERT_BEFORE_3("AnnotateCondVarSignalAll", On_AnnotateCondVarSignal);

  INSERT_BEFORE_0("AnnotateIgnoreReadsBegin", On_AnnotateIgnoreReadsBegin);
  INSERT_BEFORE_0("AnnotateIgnoreReadsEnd", On_AnnotateIgnoreReadsEnd);
  INSERT_BEFORE_0("AnnotateIgnoreWritesBegin", On_AnnotateIgnoreWritesBegin);
  INSERT_BEFORE_0("AnnotateIgnoreWritesEnd", On_AnnotateIgnoreWritesEnd);
  INSERT_BEFORE_4("AnnotatePublishMemoryRange", On_AnnotatePublishMemoryRange);
  INSERT_BEFORE_4("AnnotateUnpublishMemoryRange", On_AnnotateUnpublishMemoryRange);
  INSERT_BEFORE_3("AnnotateMutexIsUsedAsCondVar", On_AnnotateMutexIsUsedAsCondVar);

  INSERT_BEFORE_3("AnnotatePCQCreate", On_AnnotatePCQCreate);
  INSERT_BEFORE_3("AnnotatePCQDestroy", On_AnnotatePCQDestroy);
  INSERT_BEFORE_3("AnnotatePCQPut", On_AnnotatePCQPut);
  INSERT_BEFORE_3("AnnotatePCQGet", On_AnnotatePCQGet);

  // I/O
  INSERT_BEFORE_0("write", Before_SignallingIOCall);
  INSERT_BEFORE_0("unlink", Before_SignallingIOCall);
  INSERT_BEFORE_0("rmdir", Before_SignallingIOCall);
//  INSERT_BEFORE_0("send", Before_SignallingIOCall);
  INSERT_AFTER_0("__read_nocancel", After_WaitingIOCall);
  INSERT_AFTER_0("fopen", After_WaitingIOCall);
  INSERT_AFTER_0("__fopen_internal", After_WaitingIOCall);
  INSERT_AFTER_0("open", After_WaitingIOCall);
  INSERT_AFTER_0("opendir", After_WaitingIOCall);
//  INSERT_AFTER_0("recv", After_WaitingIOCall);

  // strlen and friends.
  ReplaceFunc3(img, rtn, "memchr", (AFUNPTR)Replace_memchr);
  ReplaceFunc3(img, rtn, "strchr", (AFUNPTR)Replace_strchr);
  ReplaceFunc3(img, rtn, "index", (AFUNPTR)Replace_strchr);
  ReplaceFunc3(img, rtn, "strrchr", (AFUNPTR)Replace_strrchr);
  ReplaceFunc3(img, rtn, "rindex", (AFUNPTR)Replace_strrchr);
  ReplaceFunc3(img, rtn, "strlen", (AFUNPTR)Replace_strlen);
  ReplaceFunc3(img, rtn, "strcmp", (AFUNPTR)Replace_strcmp);
  ReplaceFunc3(img, rtn, "memcpy", (AFUNPTR)Replace_memcpy);
  ReplaceFunc3(img, rtn, "strcpy", (AFUNPTR)Replace_strcpy);

  // pthread_once
  WrapFunc4(img, rtn, "pthread_once", (AFUNPTR)WRAP_NAME(pthread_once));

  // __cxa_guard_acquire / __cxa_guard_release
  INSERT_BEFORE_1("__cxa_guard_acquire", Before_cxa_guard_acquire);
  INSERT_AFTER_1("__cxa_guard_acquire", After_cxa_guard_acquire);
  INSERT_AFTER_0("__cxa_guard_release", After_cxa_guard_release);

  INSERT_BEFORE_0("atexit", On_atexit);
  INSERT_BEFORE_0("exit", On_exit);
}

// Pin calls this function every time a new img is loaded.
static void CallbackForIMG(IMG img, void *v) {
  if (debug_wrap) {
    Printf("Started CallbackForIMG %s\n", IMG_Name(img).c_str());
  }

  string img_name = IMG_Name(img);
  for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec)) {
    for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn)) {
      MaybeInstrumentOneRoutine(img, rtn);
    }
  }
  // In DEBUG_MODE check that we have the debug symbols in the Windows guts.
  // We should work w/o them too.
  if (DEBUG_MODE && img_name.find("ntdll.dll") != string::npos) {
    if (g_wrapped_functions.count("RtlTryAcquireSRWLockExclusive") == 0) {
      Printf("WARNING: Debug symbols for ntdll.dll not found.\n");
    }
  }
}

// Returns:
// TRUE
// If user is interested to inject Pin (and tool) into child/exec-ed process
// FALSE
// If user is not interested to inject Pin (and tool) into child/exec-ed process
static BOOL CallbackForExec(CHILD_PROCESS childProcess, VOID *val) {
  int argc = 0;
  const CHAR *const * argv = NULL;
  CHILD_PROCESS_GetCommandLine(childProcess, &argc, &argv);
  CHECK(argc > 0);
  CHECK(argv);
  bool follow = G_flags->trace_children;
  if (DEBUG_MODE) {
    Printf("CallbackForExec: follow=%d: ", follow);
    for (int i = 0; i < argc; i++) {
      Printf("%s ", argv[i]);
    }
  }
  Printf("\n");
  return follow;
}

//--------- ThreadSanitizerThread --------- {{{1
static void ConsumeTLEBQueue(const vector<ThreadLocalEventBuffer *> &vec) {
  size_t n = vec.size();
  for (size_t i = 0; i < n; i++) {
    ThreadLocalEventBuffer *tleb = vec[i];
    TLEBFlushUnlocked(*tleb);
    delete tleb;
  }
}

static void ThreadSanitizerThread(void *) {
  while(true) {
    size_t n;
    vector<ThreadLocalEventBuffer *> vec;
    {
      G_stats->lock_sites[1]++;
      ScopedLock lock(&g_main_ts_lock);
      n = g_tleb_queue->size();
      if (n < 100) {
        // We have just few TLEBs. Take them and release the lock.
        vec.swap(*g_tleb_queue);
      } else {
        // We have too many TLEBs.
        // Consume them while holding the lock to avoid overflow in the queue.
        ConsumeTLEBQueue(*g_tleb_queue);
        g_tleb_queue->resize(0);
        continue;
      }
    }
    if (n) {
      ConsumeTLEBQueue(vec);
    } else if (PIN_IsProcessExiting()) {
      return;
    }
  }
}
static void StartThreadSanitizerThread() {
  if (G_flags->separate_analysis_thread == false) return;
  g_tleb_queue = new vector<ThreadLocalEventBuffer*>;
  PIN_THREAD_UID uid;
  PIN_SpawnInternalThread(&ThreadSanitizerThread, NULL, 0, &uid);
}

//--------- Fini ---------- {{{1
static void CallbackForFini(INT32 code, void *v) {
  if (!G_flags->separate_analysis_thread) {
    DumpEvent(THR_END, 0, 0, 0, 0);
  }
  ThreadSanitizerFini();
  if (G_flags->show_stats) {
    TraceInfo::PrintTraceProfile();
  }
  if (G_flags->error_exitcode && GetNumberOfFoundErrors() > 0) {
    exit(G_flags->error_exitcode);
  }
}

//--------- Main -------------------------- {{{1
int main(INT32 argc, CHAR **argv) {
  PIN_Init(argc, argv);
  PIN_InitSymbols();
  G_out = stderr;

  // Init ThreadSanitizer.
  int first_param = 1;
  // skip until '-t something.so'.
  for (; first_param < argc && argv[first_param] != string("-t");
       first_param++) {
  }
  first_param += 2;
  vector<string> args;
  for (; first_param < argc; first_param++) {
    string param = argv[first_param];
    if (param == "--") break;
    if (param == "-short_name") continue;
    if (param == "-slow_asserts") continue;
    if (param == "1") continue;
    args.push_back(param);
  }

  G_flags = new FLAGS;
  ThreadSanitizerParseFlags(&args);

  if (G_flags->dry_run >= 2) {
    PIN_StartProgram();
    return 0;
  }

  if (!G_flags->log_file.empty()) {
    // Replace %p with tool PID
    string fname = G_flags->log_file;
    char pid_str[100] = "";
    sprintf(pid_str, "%u", getpid());
    while (fname.find("%p") != fname.npos)
      fname.replace(fname.find("%p"), 2, pid_str);

    G_out = fopen(fname.c_str(), "w");
    CHECK(G_out);
  }

  ThreadSanitizerInit();

  tls_reg = PIN_ClaimToolRegister();
  CHECK(REG_valid(tls_reg));
#if _MSC_VER
  g_windows_thread_pool_calback_set = new hash_set<uintptr_t>;
  g_windows_thread_pool_wait_object_map = new hash_map<uintptr_t, uintptr_t>;
#endif

  // Set up PIN callbacks.
  PIN_AddThreadStartFunction(CallbackForThreadStart, 0);
  PIN_AddThreadFiniFunction(CallbackForThreadFini, 0);
  PIN_AddFiniFunction(CallbackForFini, 0);
  IMG_AddInstrumentFunction(CallbackForIMG, 0);
  TRACE_AddInstrumentFunction(CallbackForTRACE, 0);
  PIN_AddFollowChildProcessFunction(CallbackForExec, NULL);

  Report("ThreadSanitizerPin r%s: "
         "pure-happens-before=%s fast-mode=%s ignore-in-dtor=%s\n",
         TS_VERSION,
         G_flags->pure_happens_before ? "yes" : "no",
         G_flags->fast_mode ? "yes" : "no",
         G_flags->ignore_in_dtor ? "yes" : "no");
  if (DEBUG_MODE) {
    Report("INFO: Debug build\n");
  }
  StartThreadSanitizerThread();
  // Fire!
  PIN_StartProgram();
  return 0;
}

//--------- Include thread_sanitizer.cc --------- {{{1
// ... for performance reasons...
#ifdef INCLUDE_THREAD_SANITIZER_CC
# undef INCLUDE_THREAD_SANITIZER_CC
# include "thread_sanitizer.cc"
#endif

//--------- Questions about PIN -------------------------- {{{1
/* Questions about PIN:

  - Names (e.g. pthread_create@... __pthread_mutex_unlock)
  - How to get name of a global var by it's address?
  - How to get stack pointer at thread creation?
  - How to get a stack trace (other than intercepting calls, entries, exits)
  - assert with full stack trace?
  */
// end. {{{1
// vim:shiftwidth=2:softtabstop=2:expandtab
