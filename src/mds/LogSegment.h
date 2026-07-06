// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*- 
// vim: ts=8 sw=2 sts=2 expandtab

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef CEPH_LOGSEGMENT_H
#define CEPH_LOGSEGMENT_H

#include <mutex>

#include "include/elist.h"
#include "include/interval_set.h"
#include "include/Context.h"
#include "include/fs_types.h" // for inodeno_t
#include "include/types.h" // for version_t
#include "mdstypes.h" // for dirfrag_t, metareqid_t
#include "CInode.h"
#include "CDentry.h"
#include "CDir.h"

#include <unordered_set>

#include <cstdint>
#include <map>
#include <ostream>
#include <set>
#include <vector>

class CDir;
class CInode;
class CDentry;
class MDSContext;
class C_MDSInternalNoop;
using MDSGather = C_GatherBase<MDSContext, C_MDSInternalNoop>;
using MDSGatherBuilder = C_GatherBuilderBase<MDSContext, MDSGather>;
class MDSRank;
struct MDPeerUpdate;

class LogSegment {
 public:
  using seq_t = uint64_t;

  LogSegment(uint64_t _seq, loff_t off=-1) :
    seq(_seq), offset(off), end(off),
    dirty_dirfrags(member_offset(CDir, item_dirty)),
    new_dirfrags(member_offset(CDir, item_new)),
    dirty_inodes(member_offset(CInode, item_dirty)),
    dirty_dentries(member_offset(CDentry, item_dirty)),
    open_files(member_offset(CInode, item_open_file)),
    dirty_parent_inodes(member_offset(CInode, item_dirty_parent)),
    dirty_dirfrag_dir(member_offset(CInode, item_dirty_dirfrag_dir)),
    dirty_dirfrag_nest(member_offset(CInode, item_dirty_dirfrag_nest)),
    dirty_dirfrag_dirfragtree(member_offset(CInode, item_dirty_dirfrag_dirfragtree))
  {}

  void try_to_expire(MDSRank *mds, MDSGatherBuilder &gather_bld, int op_prio);
  void purge_inodes_finish(interval_set<inodeno_t>& inos);
  void set_purged_cb(MDSContext* c){
    ceph_assert(purged_cb == NULL);
    purged_cb = c;
  }
  void wait_for_expiry(MDSContext *c)
  {
    ceph_assert(c != NULL);
    expiry_waiters.push_back(c);
  }

  // Parallel sharding POC: every field below is GLOBALLY shared across
  // ALL subvolumes (there is one "current" LogSegment for the whole MDS
  // rank), so per-subvolume locking (SubvolumeState::shard_lock /
  // SubvolumeState::Guard, used by CDir/CInode's projection-stack choke
  // points) does NOT protect these — subvolume A's shard_lock and
  // subvolume B's shard_lock don't exclude each other, but both may push
  // onto the SAME LogSegment concurrently.  A single process-wide mutex
  // guards all of them across ALL LogSegment instances — simplest choice
  // since e.g. item_dirty.remove_myself() doesn't know which segment (and
  // thus which per-segment mutex) an object currently belongs to.
  static inline std::mutex g_elist_mtx;

  void mark_dirty_inode(CInode *in) {
    std::lock_guard l(g_elist_mtx);
    dirty_inodes.push_back(&in->item_dirty);
  }
  static void unmark_dirty_inode(CInode *in) {
    std::lock_guard l(g_elist_mtx);
    in->item_dirty.remove_myself();
  }
  void add_touched_session(const entity_name_t &n) {
    std::lock_guard l(g_elist_mtx);
    touched_sessions.insert(n);
  }
  void clear_touched_sessions_locked() {
    std::lock_guard l(g_elist_mtx);
    touched_sessions.clear();
  }
  template<typename F>
  void for_each_dirty_inode_locked(F&& f) {
    std::lock_guard l(g_elist_mtx);
    for (auto p = dirty_inodes.begin(); !p.end(); ++p) {
      f(*p);
    }
  }

  void mark_dirty_dirfrag(CDir *dir) {
    std::lock_guard l(g_elist_mtx);
    dirty_dirfrags.push_back(&dir->item_dirty);
  }
  static void unmark_dirty_dirfrag(CDir *dir) {
    std::lock_guard l(g_elist_mtx);
    dir->item_dirty.remove_myself();
  }
  void mark_new_dirfrag(CDir *dir) {
    std::lock_guard l(g_elist_mtx);
    new_dirfrags.push_back(&dir->item_new);
  }
  static void unmark_new_dirfrag(CDir *dir) {
    std::lock_guard l(g_elist_mtx);
    dir->item_new.remove_myself();
  }
  void mark_dirty_dentry(CDentry *dn) {
    std::lock_guard l(g_elist_mtx);
    dirty_dentries.push_back(&dn->item_dirty);
  }
  static void unmark_dirty_dentry(CDentry *dn) {
    std::lock_guard l(g_elist_mtx);
    dn->item_dirty.remove_myself();
  }
  void mark_open_file(CInode *in) {
    std::lock_guard l(g_elist_mtx);
    open_files.push_back(&in->item_open_file);
  }
  static void unmark_open_file(CInode *in) {
    std::lock_guard l(g_elist_mtx);
    in->item_open_file.remove_myself();
  }
  void mark_dirty_parent_inode(CInode *in) {
    std::lock_guard l(g_elist_mtx);
    dirty_parent_inodes.push_back(&in->item_dirty_parent);
  }
  static void unmark_dirty_parent_inode(CInode *in) {
    std::lock_guard l(g_elist_mtx);
    in->item_dirty_parent.remove_myself();
  }
  void mark_dirty_dirfrag_dir(CInode *in) {
    std::lock_guard l(g_elist_mtx);
    dirty_dirfrag_dir.push_back(&in->item_dirty_dirfrag_dir);
  }
  static void unmark_dirty_dirfrag_dir(CInode *in) {
    std::lock_guard l(g_elist_mtx);
    in->item_dirty_dirfrag_dir.remove_myself();
  }
  void mark_dirty_dirfrag_nest(CInode *in) {
    std::lock_guard l(g_elist_mtx);
    dirty_dirfrag_nest.push_back(&in->item_dirty_dirfrag_nest);
  }
  static void unmark_dirty_dirfrag_nest(CInode *in) {
    std::lock_guard l(g_elist_mtx);
    in->item_dirty_dirfrag_nest.remove_myself();
  }
  void mark_dirty_dirfrag_dirfragtree(CInode *in) {
    std::lock_guard l(g_elist_mtx);
    dirty_dirfrag_dirfragtree.push_back(&in->item_dirty_dirfrag_dirfragtree);
  }
  static void unmark_dirty_dirfrag_dirfragtree(CInode *in) {
    std::lock_guard l(g_elist_mtx);
    in->item_dirty_dirfrag_dirfragtree.remove_myself();
  }
  void insert_truncating_inode(CInode *in) {
    std::lock_guard l(g_elist_mtx);
    truncating_inodes.insert(in);
  }
  void erase_truncating_inode(CInode *in) {
    std::lock_guard l(g_elist_mtx);
    truncating_inodes.erase(in);
  }
  bool count_truncating_inode(CInode *in) {
    std::lock_guard l(g_elist_mtx);
    return truncating_inodes.count(in) != 0;
  }

  const seq_t seq;
  uint64_t offset, end;
  uint64_t num_events = 0;

  // dirty items
  elist<CDir*>    dirty_dirfrags, new_dirfrags;
  elist<CInode*>  dirty_inodes;
  elist<CDentry*> dirty_dentries;

  elist<CInode*>  open_files;
  elist<CInode*>  dirty_parent_inodes;
  elist<CInode*>  dirty_dirfrag_dir;
  elist<CInode*>  dirty_dirfrag_nest;
  elist<CInode*>  dirty_dirfrag_dirfragtree;

  std::set<CInode*> truncating_inodes;
  interval_set<inodeno_t> purging_inodes;
  MDSContext* purged_cb = nullptr;

  std::map<int, std::unordered_set<version_t>> pending_commit_tids;  // mdstable
  std::set<metareqid_t> uncommitted_leaders;
  std::set<metareqid_t> uncommitted_peers;
  std::set<dirfrag_t> uncommitted_fragments;

  // client request ids
  std::map<int, ceph_tid_t> last_client_tids;

  // potentially dirty sessions
  std::set<entity_name_t> touched_sessions;

  // table version
  version_t inotablev = 0;
  version_t sessionmapv = 0;
  std::map<int,version_t> tablev;

  std::vector<MDSContext*> expiry_waiters;
};

static inline std::ostream& operator<<(std::ostream& out, const LogSegment& ls) {
  return out << "LogSegment(" << ls.seq << "/0x" << std::hex << ls.offset
             << "~" << ls.end << std::dec << " events=" << ls.num_events << ")";
}

#endif
