// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include "SubvolumeState.h"

#include <vector>

#include "MDSRank.h"
#include "msg/Message.h"

#define dout_subsys ceph_subsys_mds
#undef dout_prefix
#define dout_prefix *_dout << "mds.subvol." << subvol_ino << " "

namespace {
// Per-thread stack of subvolume inos whose shard_lock the CURRENT thread
// already holds.  Used by SubvolumeState::Guard to make lock acquisition
// reentrant-safe: nested calls into the projection-stack choke points for
// the SAME subvolume (e.g. CInode::mark_dirty -> CDentry::mark_dirty ->
// CDir::mark_dirty, or a shard worker that already holds shard_lock for
// its whole dispatch) skip re-locking instead of self-deadlocking on the
// non-recursive fair_mutex.
thread_local std::vector<inodeno_t> tl_held_subvolume_locks;

// Fallback lock for "boundary" objects that don't belong to any single
// subvolume (sv == nullptr) but can still be touched concurrently by
// multiple subvolume shard workers — e.g. the shared ancestor directory
// linking subvolume roots to the rest of the tree. Reentrant via a
// thread-local depth counter since chains like CInode::mark_dirty ->
// CDentry::mark_dirty -> CDir::mark_dirty construct nested Guards for a
// sequence of boundary objects on the same thread.
ceph::fair_mutex g_boundary_mutex{"SubvolumeState::boundary"};
thread_local int tl_boundary_lock_depth = 0;
// See SubvolumeState::enter_sticky_boundary_mode() in the header for why
// this exists: while set, boundary Guards take g_boundary_mutex on first
// touch but do NOT release it on destruction — only
// exit_sticky_boundary_mode() does, so the lock spans a whole write's
// project-through-submit_entry() sequence instead of just one call.
thread_local bool tl_boundary_sticky_mode = false;
thread_local bool tl_boundary_sticky_held = false;
} // anonymous namespace

SubvolumeState::Guard::Guard(SubvolumeState *sv)
{
  if (!sv) {
    is_boundary_ = true;
    if (tl_boundary_sticky_mode) {
      is_sticky_ = true;
      if (!tl_boundary_sticky_held) {
        g_boundary_mutex.lock();
        tl_boundary_sticky_held = true;
      }
      return;
    }
    if (tl_boundary_lock_depth == 0) {
      g_boundary_mutex.lock();
    }
    ++tl_boundary_lock_depth;
    return;
  }
  for (auto held : tl_held_subvolume_locks) {
    if (held == sv->subvol_ino) {
      // Already held by this thread (reentrant call) — no-op.
      return;
    }
  }
  sv->shard_lock.lock();
  tl_held_subvolume_locks.push_back(sv->subvol_ino);
  locked_ = sv;
}

SubvolumeState::Guard::~Guard()
{
  if (is_boundary_) {
    if (is_sticky_) {
      // Left held for exit_sticky_boundary_mode() to release.
      return;
    }
    ceph_assert(tl_boundary_lock_depth > 0);
    if (--tl_boundary_lock_depth == 0) {
      g_boundary_mutex.unlock();
    }
    return;
  }
  if (locked_) {
    ceph_assert(!tl_held_subvolume_locks.empty() &&
                tl_held_subvolume_locks.back() == locked_->subvol_ino);
    tl_held_subvolume_locks.pop_back();
    locked_->shard_lock.unlock();
  }
}

void SubvolumeState::enter_sticky_boundary_mode()
{
  ceph_assert(!tl_boundary_sticky_mode);
  ceph_assert(!tl_boundary_sticky_held);
  ceph_assert(tl_boundary_lock_depth == 0);
  tl_boundary_sticky_mode = true;
}

void SubvolumeState::exit_sticky_boundary_mode()
{
  if (!tl_boundary_sticky_mode) {
    return;
  }
  tl_boundary_sticky_mode = false;
  if (tl_boundary_sticky_held) {
    tl_boundary_sticky_held = false;
    g_boundary_mutex.unlock();
  }
}

SubvolumeState::SubvolumeState(inodeno_t ino)
  : subvol_ino(ino),
    shard_lock(std::string("SubvolumeState::") + std::to_string(ino))
{
}

SubvolumeState::~SubvolumeState()
{
  shutdown_dispatch_worker();
}

void SubvolumeState::start_dispatch_worker(MDSRank *mds_)
{
  ceph_assert(mds_ != nullptr);
  std::lock_guard l(queue_mux);
  if (worker.joinable()) {
    return;
  }
  mds = mds_;
  stop = false;
  worker = std::thread(&SubvolumeState::worker_loop, this);
}

void SubvolumeState::shutdown_dispatch_worker()
{
  {
    std::lock_guard l(queue_mux);
    stop = true;
  }
  queue_cond.notify_all();
  if (worker.joinable()) {
    worker.join();
  }
  mds = nullptr;
}

void SubvolumeState::enqueue(const cref_t<Message> &m, bool lock_free, bool is_write)
{
  {
    std::lock_guard l(queue_mux);
    queue.push_back(QueuedMessage{m, lock_free, is_write});
  }
  queue_cond.notify_one();
}

void SubvolumeState::queue_finished(MDSContext *c)
{
  std::lock_guard l(finished_mux);
  shard_finished.push_back(c);
}

std::deque<MDSContext*> SubvolumeState::drain_finished()
{
  std::lock_guard l(finished_mux);
  std::deque<MDSContext*> out;
  out.swap(shard_finished);
  return out;
}

void SubvolumeState::worker_loop()
{
  ldout(g_ceph_context, 1) << "dispatch worker started" << dendl;
  while (true) {
    QueuedMessage qm;
    {
      std::unique_lock l(queue_mux);
      queue_cond.wait(l, [this] {
        return stop || !queue.empty();
      });
      if (stop && queue.empty()) {
        break;
      }
      qm = std::move(queue.front());
      queue.pop_front();
    }

    if (mds->is_daemon_stopping()) {
      continue;
    }

    // Use Guard (not a raw lock_guard on shard_lock) so the reentrancy
    // tracking correctly reflects that THIS thread now holds subvol_ino's
    // lock — any choke-point Guard constructed deeper in the call stack
    // for the SAME subvolume (project_fnode/mark_dirty/etc, even from
    // mds_lock-path code like CREATE that also touches this subvolume)
    // will see it's already held and skip re-locking instead of
    // deadlocking on the non-recursive shard_lock.
    Guard g(this);
    mds->dispatch_on_shard(this, qm.m, qm.lock_free, qm.is_write);
  }
  ldout(g_ceph_context, 1) << "dispatch worker stopped" << dendl;
}
