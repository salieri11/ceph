// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace mds {

/**
 * Copy-on-write snapshot holder for read-mostly MDS state.
 *
 * Readers load an immutable shared_ptr<const T> without holding mds_lock.
 * Writers clone the current snapshot, mutate the copy, and publish it.
 * All writes must still be serialized by the caller (typically mds_lock).
 */
template<typename T>
class CowSnapshot {
public:
  using Snap = std::shared_ptr<const T>;

  CowSnapshot() : snap_(std::make_shared<T>()) {}

  explicit CowSnapshot(T initial)
    : snap_(std::make_shared<T>(std::move(initial))) {}

  Snap read() const {
#ifdef __cpp_lib_atomic_shared_ptr
    return snap_.load(std::memory_order_acquire);
#else
    return std::atomic_load_explicit(&snap_, std::memory_order_acquire);
#endif
  }

  void publish(Snap next) {
#ifdef __cpp_lib_atomic_shared_ptr
    snap_.store(std::move(next), std::memory_order_release);
#else
    std::atomic_store_explicit(&snap_, std::move(next), std::memory_order_release);
#endif
  }

  template<typename Mutator>
  void mutate(Mutator&& mutator) {
    auto cur = read();
    auto next = std::make_shared<T>(*cur);
    mutator(*next);
    publish(next);
  }

private:
#ifdef __cpp_lib_atomic_shared_ptr
  std::atomic<Snap> snap_;
#else
  Snap snap_;
#endif
};

} // namespace mds
