// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#pragma once

// Thread-safety wrappers for MDS shared data structures.
// Used by per-subvolume shard workers to access global state without
// holding the global mds_lock.
//
// Design: readers take shared_lock (parallel), writers take unique_lock.
// Shard workers use these wrappers; the global mds_lock path goes through
// them too, so we have a single source of truth.

#include <shared_mutex>
#include <unordered_map>

#include "include/fs_types.h"

class CInode;
struct MDRequestImpl;
typedef boost::intrusive_ptr<MDRequestImpl> MDRequestRef;
struct metareqid_t;

class ThreadSafeInodeMap {
public:
  CInode* get(inodeno_t ino) const {
    std::shared_lock l(mtx_);
    auto it = map_.find(ino);
    return it != map_.end() ? it->second : nullptr;
  }

  bool contains(inodeno_t ino) const {
    std::shared_lock l(mtx_);
    return map_.count(ino);
  }

  void insert(inodeno_t ino, CInode *in) {
    std::unique_lock l(mtx_);
    auto &p = map_[ino];
    ceph_assert(!p);
    p = in;
  }

  void erase(inodeno_t ino) {
    std::unique_lock l(mtx_);
    map_.erase(ino);
  }

  size_t size() const {
    std::shared_lock l(mtx_);
    return map_.size();
  }

  bool empty() const {
    std::shared_lock l(mtx_);
    return map_.empty();
  }

  // Exclusive access for iteration — callers that need to walk the whole
  // map (e.g. shutdown, scrub) must hold exclusive lock.
  template<typename F>
  void for_each_exclusive(F&& f) {
    std::unique_lock l(mtx_);
    for (auto &p : map_) {
      f(p.first, p.second);
    }
  }

  template<typename F>
  void for_each_shared(F&& f) const {
    std::shared_lock l(mtx_);
    for (auto &p : map_) {
      f(p.first, p.second);
    }
  }

  // Random pick (used by MDCache::get_random_inode)
  CInode* random_pick() const {
    std::shared_lock l(mtx_);
    if (map_.empty()) return nullptr;
    int n = rand() % map_.size();
    auto it = map_.begin();
    std::advance(it, n);
    return it->second;
  }

  // Direct access to underlying map — ONLY use under mds_lock when
  // thread-safety is guaranteed externally (legacy compatibility).
  std::unordered_map<inodeno_t, CInode*>& unsafe_get() { return map_; }
  const std::unordered_map<inodeno_t, CInode*>& unsafe_get() const { return map_; }

private:
  mutable std::shared_mutex mtx_;
  std::unordered_map<inodeno_t, CInode*> map_;
};

class ThreadSafeActiveRequests {
public:
  MDRequestRef get(const metareqid_t &reqid) const {
    std::shared_lock l(mtx_);
    auto it = map_.find(reqid);
    return it != map_.end() ? it->second : MDRequestRef();
  }

  bool contains(const metareqid_t &reqid) const {
    std::shared_lock l(mtx_);
    return map_.count(reqid);
  }

  void insert(const metareqid_t &reqid, MDRequestRef mdr) {
    std::unique_lock l(mtx_);
    map_[reqid] = std::move(mdr);
  }

  void erase(const metareqid_t &reqid) {
    std::unique_lock l(mtx_);
    map_.erase(reqid);
  }

  size_t size() const {
    std::shared_lock l(mtx_);
    return map_.size();
  }

  bool empty() const {
    std::shared_lock l(mtx_);
    return map_.empty();
  }

  template<typename F>
  void for_each_exclusive(F&& f) {
    std::unique_lock l(mtx_);
    for (auto &p : map_) {
      f(p.first, p.second);
    }
  }

  // Direct access — ONLY under mds_lock.
  std::unordered_map<metareqid_t, MDRequestRef>& unsafe_get() { return map_; }
  const std::unordered_map<metareqid_t, MDRequestRef>& unsafe_get() const { return map_; }

private:
  mutable std::shared_mutex mtx_;
  std::unordered_map<metareqid_t, MDRequestRef> map_;
};
