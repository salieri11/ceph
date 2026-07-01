// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#pragma once

#include <map>

#include "include/types.h"

class MDSRank;

struct ShardingTelemetry {
  uint64_t shardable_ops = 0;
  uint64_t global_ops = 0;
  uint64_t cross_subvol_ops = 0;
  uint64_t lock_wait_us = 0;
  uint64_t lock_hold_us = 0;
  uint64_t sharding_effective = 0;
  uint64_t subvolume_count = 0;
  uint64_t violation_count = 0;
};

struct MDSRankShardingState {
  uint64_t subvolume_count = 0;
  uint64_t violation_count = 0;
};

class ShardingTelemetryCollector {
public:
  explicit ShardingTelemetryCollector(MDSRank *mds);

  void record_shardable(inodeno_t subvol_ino);
  void record_global();
  void record_cross_subvol();
  void record_lock_timing(uint64_t wait_us, uint64_t hold_us);

  void set_subvolume_count(uint64_t count);

  ShardingTelemetry flush(const MDSRankShardingState &state);
  std::map<inodeno_t, uint64_t> flush_metadata_ops();

private:
  MDSRank *mds;

  uint64_t shardable_ops = 0;
  uint64_t global_ops = 0;
  uint64_t cross_subvol_ops = 0;
  uint64_t lock_wait_us = 0;
  uint64_t lock_hold_us = 0;
  uint64_t subvolume_count = 0;

  std::map<inodeno_t, uint64_t> metadata_ops_by_ino;
};
