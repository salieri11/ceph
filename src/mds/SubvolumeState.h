// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#pragma once

#include "include/types.h"

// Per-subvolume state bucket.  Future phases will migrate subvolume-scoped
// MDCache / request state here so dispatch can decide whether mds_lock is
// required for a given client operation.
struct SubvolumeState {
  explicit SubvolumeState(inodeno_t ino) : subvol_ino(ino) {}

  inodeno_t subvol_ino;
};

enum class ShardingClass {
  Global,
  Shardable,
  CrossSubvolume,
};

struct ClientRequestClassification {
  ShardingClass cls = ShardingClass::Global;
  inodeno_t subvol_ino;
};
