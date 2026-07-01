// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include "ShardingTelemetry.h"

#include "MDSRank.h"

ShardingTelemetryCollector::ShardingTelemetryCollector(MDSRank *mds_)
  : mds(mds_) {
}

void ShardingTelemetryCollector::record_shardable(inodeno_t subvol_ino)
{
  ++shardable_ops;
  if (subvol_ino) {
    ++metadata_ops_by_ino[subvol_ino];
  }
}

void ShardingTelemetryCollector::record_global()
{
  ++global_ops;
}

void ShardingTelemetryCollector::record_cross_subvol()
{
  ++cross_subvol_ops;
}

void ShardingTelemetryCollector::record_lock_timing(uint64_t wait_us, uint64_t hold_us)
{
  lock_wait_us += wait_us;
  lock_hold_us += hold_us;
}

void ShardingTelemetryCollector::set_subvolume_count(uint64_t count)
{
  subvolume_count = count;
}

ShardingTelemetry ShardingTelemetryCollector::flush(const MDSRankShardingState &state)
{
  ShardingTelemetry t;
  t.shardable_ops = shardable_ops;
  t.global_ops = global_ops;
  t.cross_subvol_ops = cross_subvol_ops;
  t.lock_wait_us = lock_wait_us;
  t.lock_hold_us = lock_hold_us;
  t.sharding_effective = state.effective ? 1 : 0;
  t.subvolume_count = subvolume_count;
  t.violation_count = state.violation_count;

  shardable_ops = 0;
  global_ops = 0;
  cross_subvol_ops = 0;
  lock_wait_us = 0;
  lock_hold_us = 0;

  return t;
}

std::map<inodeno_t, uint64_t> ShardingTelemetryCollector::flush_metadata_ops()
{
  std::map<inodeno_t, uint64_t> out;
  out.swap(metadata_ops_by_ino);
  return out;
}
