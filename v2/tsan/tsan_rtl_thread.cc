//===-- tsan_rtl_thread.cc --------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer (TSan), a race detector.
//
//===----------------------------------------------------------------------===//

#include "tsan_rtl.h"
#include "tsan_mman.h"
#include "tsan_placement_new.h"
#include "tsan_platform.h"
#include "tsan_report.h"
#include "tsan_sync.h"

namespace __tsan {

const int kThreadQuarantineSize = 100;

void ThreadFinalize(ThreadState *thr) {
  CHECK_GT(thr->in_rtl, 0);
  Context *ctx = CTX();
  Lock l(&ctx->thread_mtx);
  for (int i = 0; i < kMaxTid; i++) {
    ThreadContext *tctx = ctx->threads[i];
    if (tctx == 0)
      continue;
    if (tctx->detached)
      continue;
    if (tctx->status != ThreadStatusCreated
        && tctx->status != ThreadStatusRunning
        && tctx->status != ThreadStatusFinished)
      continue;
    Lock l(&ctx->report_mtx);
    ReportDesc rep;
    internal_memset(&rep, 0, sizeof(rep));
    RegionAlloc alloc(rep.alloc, sizeof(rep.alloc));
    rep.typ = ReportTypeThreadLeak;
    rep.nthread = 1;
    rep.thread = alloc.Alloc<ReportThread>(1);
    rep.thread->id = tctx->tid;
    rep.thread->running = (tctx->status != ThreadStatusFinished);
    rep.thread->stack.cnt = 0;
    PrintReport(&rep);
    ctx->nreported++;
  }
}

static void ThreadDead(ThreadState *thr, ThreadContext *tctx) {
  CHECK_GT(thr->in_rtl, 0);
  CHECK(tctx->status == ThreadStatusRunning
      || tctx->status == ThreadStatusFinished);
  DPrintf("#%d: ThreadDead uid=%lu\n", (int)thr->fast_state.tid(), tctx->uid);
  tctx->status = ThreadStatusDead;
  tctx->uid = 0;
  tctx->sync.Free(&thr->clockslab);

  // Put to dead list.
  tctx->dead_next = 0;
  if (CTX()->dead_list_size == 0)
    CTX()->dead_list_head = tctx;
  else
    CTX()->dead_list_tail->dead_next = tctx;
  CTX()->dead_list_tail = tctx;
  CTX()->dead_list_size++;
}

int ThreadCreate(ThreadState *thr, uptr pc, uptr uid, bool detached) {
  CHECK_GT(thr->in_rtl, 0);
  Lock l(&CTX()->thread_mtx);
  int tid = -1;
  ThreadContext *tctx = 0;
  if (CTX()->dead_list_size > kThreadQuarantineSize
      || CTX()->thread_seq >= kMaxTid) {
    if (CTX()->dead_list_size == 0) {
      Printf("ThreadSanitizer: %d thread limit exceeded. Dying.\n", kMaxTid);
      Die();
    }
    tctx = CTX()->dead_list_head;
    CTX()->dead_list_head = tctx->dead_next;
    CTX()->dead_list_size--;
    if (CTX()->dead_list_size == 0) {
      CHECK_EQ(tctx->dead_next, 0);
      CTX()->dead_list_head = 0;
    }
    CHECK(tctx->status == ThreadStatusDead);
    tctx->status = ThreadStatusInvalid;
    tctx->reuse_count++;
    tid = tctx->tid;
    // The point to reclain dead_info.
    // delete tctx->dead_info;
  } else {
    tid = CTX()->thread_seq++;
    tctx = new(virtual_alloc(sizeof(ThreadContext))) ThreadContext(tid);
    CTX()->threads[tid] = tctx;
  }
  CHECK(tctx != 0 && tid >= 0 && tid < kMaxTid);
  DPrintf("#%d: ThreadCreate tid=%d uid=%lu\n",
          (int)thr->fast_state.tid(), tid, uid);
  CHECK(tctx->status == ThreadStatusInvalid);
  tctx->status = ThreadStatusCreated;
  tctx->thr = 0;
  tctx->uid = uid;
  tctx->detached = detached;
  if (tid) {
    thr->fast_state.IncrementEpoch();
    thr->clock.set(thr->fast_state.tid(), thr->fast_state.epoch());
    thr->fast_synch_epoch = thr->fast_state.epoch();
    thr->clock.release(&tctx->sync, &thr->clockslab);

    tctx->creation_stack.ObtainCurrent(thr, pc);
  }
  return tid;
}

void ThreadStart(ThreadState *thr, int tid) {
  CHECK_GT(thr->in_rtl, 0);
  uptr stk_addr = 0;
  uptr stk_size = 0;
  uptr tls_addr = 0;
  uptr tls_size = 0;
  GetThreadStackAndTls(&stk_addr, &stk_size, &tls_addr, &tls_size);
  if (stk_addr && stk_size)
    MemoryResetRange(thr, /*pc=*/ 1, stk_addr, stk_size);
  // FIXME: tls seems to be pointing to same memory as stack.
  if (tls_addr && tls_size)
    MemoryResetRange(thr, /*pc=*/ 2, tls_addr, tls_size);
  Lock l(&CTX()->thread_mtx);
  ThreadContext *tctx = CTX()->threads[tid];
  CHECK(tctx && tctx->status == ThreadStatusCreated);
  tctx->status = ThreadStatusRunning;
  tctx->epoch0 = tctx->epoch1 + 1;
  tctx->epoch1 = (u64)-1;
  new(thr) ThreadState(CTX(), tid, tctx->epoch0, stk_addr, stk_size,
                       tls_addr, tls_size);
  tctx->thr = thr;
  thr->fast_synch_epoch = tctx->epoch0;
  thr->clock.set(tid, tctx->epoch0);
  thr->clock.acquire(&tctx->sync);
  DPrintf("#%d: ThreadStart epoch=%llu stk_addr=%p stk_size=%p "
      "tls_addr=%p tls_size=%p\n",
      tid, tctx->epoch0, stk_addr, stk_size, tls_addr, tls_size);
}

void ThreadFinish(ThreadState *thr) {
  CHECK_GT(thr->in_rtl, 0);
  // FIXME: Treat it as write.
  if (thr->stk_addr && thr->stk_size)
    MemoryResetRange(thr, /*pc=*/ 3, thr->stk_addr, thr->stk_size);
  if (thr->tls_addr && thr->tls_size)
    MemoryResetRange(thr, /*pc=*/ 4, thr->tls_addr, thr->tls_size);
  Lock l(&CTX()->thread_mtx);
  ThreadContext *tctx = CTX()->threads[thr->fast_state.tid()];
  CHECK(tctx && tctx->status == ThreadStatusRunning);
  if (tctx->detached) {
    ThreadDead(thr, tctx);
  } else {
    thr->fast_state.IncrementEpoch();
    thr->clock.set(thr->fast_state.tid(), thr->fast_state.epoch());
    thr->fast_synch_epoch = thr->fast_state.epoch();
    thr->clock.release(&tctx->sync, &thr->clockslab);
    tctx->status = ThreadStatusFinished;
  }

  // Save from info about the thread.
  // If dead_info will become dynamically allocated again,
  // it is the point to allocate it.
  // tctx->dead_info = new ThreadDeadInfo;
  internal_memcpy(&tctx->dead_info.trace, &thr->trace, sizeof(thr->trace));
  tctx->epoch1 = thr->clock.get(tctx->tid);

  if (kCollectStats) {
    for (int i = 0; i < StatCnt; i++)
      CTX()->stat[i] += thr->stat[i];
  }

  thr->~ThreadState();
  tctx->thr = 0;
}

void ThreadJoin(ThreadState *thr, uptr pc, uptr uid) {
  CHECK_GT(thr->in_rtl, 0);
  DPrintf("#%d: ThreadJoin uid=%lu\n",
          (int)thr->fast_state.tid(), uid);
  Lock l(&CTX()->thread_mtx);
  ThreadContext *tctx = 0;
  int tid = 0;
  for (; tid < kMaxTid; tid++) {
    if (CTX()->threads[tid] != 0
        && CTX()->threads[tid]->uid == uid
        && CTX()->threads[tid]->status != ThreadStatusInvalid) {
      tctx = CTX()->threads[tid];
      break;
    }
  }
  if (tctx == 0 || tctx->status == ThreadStatusInvalid) {
    Printf("ThreadSanitizer: join of non-existent thread\n");
    return;
  }
  CHECK(tctx->detached == false);
  CHECK(tctx->status == ThreadStatusFinished);
  thr->clock.acquire(&tctx->sync);
  ThreadDead(thr, tctx);
}

void ThreadDetach(ThreadState *thr, uptr pc, uptr uid) {
  CHECK_GT(thr->in_rtl, 0);
  Lock l(&CTX()->thread_mtx);
  ThreadContext *tctx = 0;
  for (int tid = 0; tid < kMaxTid; tid++) {
    if (CTX()->threads[tid] != 0
        && CTX()->threads[tid]->uid == uid
        && CTX()->threads[tid]->status != ThreadStatusInvalid) {
      tctx = CTX()->threads[tid];
      break;
    }
  }
  if (tctx == 0 || tctx->status == ThreadStatusInvalid) {
    Printf("ThreadSanitizer: detach of non-existent thread\n");
    return;
  }
  if (tctx->status == ThreadStatusFinished) {
    ThreadDead(thr, tctx);
  } else {
    tctx->detached = true;
  }
}

StackTrace::StackTrace()
    : n_()
    , s_() {
}

StackTrace::~StackTrace() {
  CHECK_EQ(n_, 0);
  CHECK_EQ(s_, 0);
}

void StackTrace::ObtainCurrent(ThreadState *thr, uptr toppc) {
  Free(thr);
  n_ = thr->shadow_stack_pos - &thr->shadow_stack[0] + 1;
  s_ = (uptr*)internal_alloc(thr, n_ * sizeof(s_[0]));
  for (uptr i = 0; i < n_ - 1; i++)
    s_[i] = thr->shadow_stack[i];
  s_[n_ - 1] = toppc;
}

void StackTrace::Free(ThreadState *thr) {
  if (s_) {
    CHECK_NE(n_, 0);
    internal_free(thr, s_);
    s_ = 0;
    n_ = 0;
  }
}

uptr StackTrace::Size() const {
  return n_;
}

uptr StackTrace::Get(uptr i) const {
  CHECK_LT(i, n_);
  return s_[i];
}

}  // namespace __tsan
