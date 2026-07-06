// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "common/fair_mutex.h"
#include "common/ref.h"
#include "include/fs_types.h"

class MDSRank;
class Message;

// Per-subvolume state bucket.  POC: optional dedicated dispatch worker +
// shard_lock so classified client ops can run without the global mds_lock.
struct SubvolumeState {
  explicit SubvolumeState(inodeno_t ino);
  ~SubvolumeState();

  SubvolumeState(const SubvolumeState&) = delete;
  SubvolumeState& operator=(const SubvolumeState&) = delete;

  void start_dispatch_worker(MDSRank *mds);
  void shutdown_dispatch_worker();
  // lock_free: true if this request was classified (Phase 5) as safe to
  // process without mds_lock (read-only, or an audited write like SETATTR).
  // is_write: true if it's specifically an audited lock-free write (needs
  // the post-dispatch durability wait to preserve sequential semantics).
  void enqueue(const cref_t<Message> &m, bool lock_free, bool is_write);

  // Per-shard finished queue: shard worker pushes contexts here instead
  // of touching the global ls_.finished_queue.  Drained under mds_lock.
  void queue_finished(class MDSContext *c);
  std::deque<class MDSContext*> drain_finished();

  inodeno_t subvol_ino;
  ceph::fair_mutex shard_lock;

  // Phase 6: reentrant-safe RAII lock for the choke-point functions in
  // CDir/CInode's projection stack (project_fnode/project_inode/
  // pre_dirty/mark_dirty/pop_and_dirty_projected_*).  Takes `sv`'s
  // shard_lock UNLESS the current thread already holds it (tracked via
  // thread_local state) — needed because these functions call each other
  // in chains for the SAME subvolume (e.g. CInode::mark_dirty ->
  // CDentry::mark_dirty -> CDir::mark_dirty) and also because a shard
  // worker already holds shard_lock for its entire dispatch before ever
  // reaching these functions.  `sv` may be nullptr — either the POC is
  // disabled, or the object is a "boundary" object that sits
  // ABOVE/OUTSIDE any single subvolume (e.g. the shared ancestor
  // directory that all subvolume roots live in, like .../_nogroup/, or
  // the fs root itself). Boundary objects are NOT exclusive to one
  // subvolume's shard — e.g. marking a subvolume root's own linking
  // dentry dirty (DIRTYPARENT propagation) touches the *shared* parent
  // directory, which every other subvolume's shard worker can also
  // reach concurrently. So when sv is nullptr because of this boundary
  // case, Guard falls back to one process-wide mutex instead of doing
  // nothing, to avoid leaving these shared objects' projection stacks
  // completely unprotected against concurrent lock-free shard workers.
  class Guard {
  public:
    explicit Guard(SubvolumeState *sv);
    ~Guard();
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
  private:
    SubvolumeState *locked_ = nullptr;
    bool is_boundary_ = false;
    bool is_sticky_ = false;
  };

  // "Sticky" boundary mode: the boundary fallback lock normally releases
  // as soon as each individual choke-point call (project_fnode/mark_dirty/
  // etc) returns. That's NOT enough for correctness on its own: a write's
  // synchronous half typically does several *separate* boundary touches
  // (e.g. project_inode() for the subvolume root, then later mark_dirty()
  // cascading into it) before finally calling mds->mdlog->submit_entry().
  // If the boundary lock isn't held continuously across that whole span,
  // two different subvolume shard threads can interleave their push
  // (project) and submit_entry() calls in different relative orders,
  // which corrupts the FIFO invariant CDir::mark_dirty() asserts on
  // (`pv <= projected_fnode.front()->version`) even though each
  // individual touch was itself race-free.
  //
  // Callers (MDSRank::dispatch_on_shard(), for the synchronous half of a
  // lock-free write) bracket the whole dispatch with enter/exit. While
  // sticky mode is on for this thread, the FIRST boundary Guard
  // constructed takes g_boundary_mutex and every Guard (including that
  // first one) leaves it held on destruction; exit_sticky_boundary_mode()
  // is what actually releases it, guaranteeing push-order == submit-order
  // for any single thread's write.
  static void enter_sticky_boundary_mode();
  static void exit_sticky_boundary_mode();

private:
  void worker_loop();

  struct QueuedMessage {
    cref_t<Message> m;
    bool lock_free = false;
    bool is_write = false;
  };

  std::deque<QueuedMessage> queue;
  std::mutex queue_mux;
  std::condition_variable queue_cond;
  std::deque<class MDSContext*> shard_finished;
  std::mutex finished_mux;
  std::thread worker;
  std::atomic<bool> stop{false};
  MDSRank *mds = nullptr;
};

enum class ShardingClass {
  Global,
  Shardable,
  CrossSubvolume,
};

struct ClientRequestClassification {
  ShardingClass cls = ShardingClass::Global;
  inodeno_t subvol_ino;
  // Phase 5.1: true for ops that never touch the journal/InoTable/stray
  // dirs (GETATTR, LOOKUP family, read-only OPEN, etc).  Used to decide
  // whether a shard worker can process the request without mds_lock.
  bool read_only = false;
  // Phase 5.2: true for write ops audited as safe to run lock-free given
  // Phase 4's wrlock/xlock, SessionMap, and predirty-boundary safety work
  // (currently just SETATTR — no InoTable/stray dirs involved).
  bool write_lockfree = false;
};
