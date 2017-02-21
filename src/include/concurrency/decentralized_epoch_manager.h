//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// decentralized_epoch_manager.h
//
// Identification: src/include/concurrency/decentralized_epoch_manager.h
//
// Copyright (c) 2015-16, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//


#pragma once

#include <thread>
#include <vector>

#include "common/macros.h"
#include "type/types.h"
#include "common/logger.h"
#include "common/platform.h"
#include "common/init.h"
#include "common/thread_pool.h"
#include "concurrency/epoch_manager.h"

namespace peloton {
namespace concurrency {


class DecentralizedEpochManager : public EpochManager {
  DecentralizedEpochManager(const DecentralizedEpochManager&) = delete;

struct Epoch {
  std::atomic<size_t> read_only_count_;
  std::atomic<size_t> read_write_count_;

  Epoch(): 
    read_only_count_(0), 
    read_write_count_(0) {}

  Epoch(const Epoch &epoch): 
    read_only_count_(epoch.read_only_count_.load()), 
    read_write_count_(epoch.read_write_count_.load()) {}

  void Init() {
    read_only_count_ = 0;
    read_write_count_ = 0;
  }
};

struct LocalEpochContext {

  LocalEpochContext() : 
    epoch_buffer_(epoch_buffer_size_),
    head_epoch_id_(0),
    tail_epoch_id_(UINT64_MAX) {}

  bool EnterLocalEpoch(const uint64_t epoch_id) {
    // if not initiated
    if (tail_epoch_id_ == UINT64_MAX) {
      tail_epoch_id_ = epoch_id - 1;
    }

    // ideally, epoch_id should never be smaller than head_epoch id.
    // however, as we force updating head_epoch_id in GetTailEpochId() function,
    // it is possible that we find that epoch_id is smaller than head_epoch_id.
    // in this case, we should reject entering local epoch.
    // this is essentially a validation scheme.
    if (epoch_id < head_epoch_id_) {
      return false;
    }
    
    PL_ASSERT(epoch_id >= head_epoch_id_);

    // update head epoch id.
    head_epoch_id_ = epoch_id;

    // very long transactions must be timeout
    PL_ASSERT(epoch_id - tail_epoch_id_ <= epoch_buffer_size_);

    size_t epoch_idx = (size_t) epoch_id % epoch_buffer_size_;
    epoch_buffer_[epoch_idx].read_write_count_++;

    return true;
  }

  // for now, we do not support read-only transactions
  bool EnterLocalReadOnlyEpoch(const uint64_t epoch_id) {
    // if not initiated
    if (tail_epoch_id_ == UINT64_MAX) {
      tail_epoch_id_ = epoch_id - 1;
    } 

    // ideally, epoch_id should never be smaller than head_epoch id.
    // however, as we force updating head_epoch_id in GetTailEpochId() function,
    // it is possible that we find that epoch_id is smaller than head_epoch_id.
    // in this case, we should reject entering local epoch.
    // this is essentially a validation scheme.
    if (epoch_id < head_epoch_id_) {
      return false;
    }

    head_epoch_id_ = epoch_id;
    
    // very long transaction must be timeout
    PL_ASSERT(epoch_id - tail_epoch_id_ <= epoch_buffer_size_);

    size_t epoch_idx = (size_t) epoch_id % epoch_buffer_size_;
    epoch_buffer_[epoch_idx].read_only_count_++;

    return true;
  }

  void ExitLocalEpoch(const uint64_t epoch_id) {
    PL_ASSERT(tail_epoch_id_ != UINT64_MAX);

    PL_ASSERT(epoch_id > tail_epoch_id_);

    size_t epoch_idx = (size_t) epoch_id % epoch_buffer_size_;
    epoch_buffer_[epoch_idx].read_write_count_--;

    // when exiting a local epoch, we must check whether it can be reclaimed.
    IncreaseTailEpochId();
  }

  // for now, we do not support read-only transactions
  void ExitLocalReadOnlyEpoch(const uint64_t epoch_id) {
    PL_ASSERT(tail_epoch_id_ != UINT64_MAX);

    PL_ASSERT(epoch_id > tail_epoch_id_);

    size_t epoch_idx = (size_t) epoch_id % epoch_buffer_size_;
    epoch_buffer_[epoch_idx].read_only_count_--;

    // when exiting a local epoch, we must check whether it can be reclaimed.
    IncreaseTailEpochId();
  }

  void IncreaseTailEpochId() {
    // in the best case, tail_epoch_id can be increased to (head_epoch_id - 1)
    while (tail_epoch_id_ < head_epoch_id_ - 1) {
      size_t epoch_idx = (size_t) (tail_epoch_id_ + 1) % epoch_buffer_size_;
      if (epoch_buffer_[epoch_idx].read_write_count_ == 0) {
        tail_epoch_id_++;
      } else {
        break;
      }
    }
  }

  // increase tail epoch id using the current epoch id obtained from
  // centralized epoch thread.
  void IncreaseTailEpochId(const uint64_t current_epoch_id) {
    head_epoch_id_ = current_epoch_id;

    // this thread never started executing transactions.
    if (tail_epoch_id_ == UINT64_MAX) {
      uint64_t new_tail_epoch_id = head_epoch_id_ - 1;

      // force updating tail epoch id to head_epoch_id - 1.
      // it is ok if this operation fails.
      // if fails, then if means that the local thread started a new transaction.
      __sync_bool_compare_and_swap(&tail_epoch_id_, UINT64_MAX, new_tail_epoch_id);
    
    }
    IncreaseTailEpochId();
  }

  // this function is periodically invoked.
  // the centralized epoch thread must check the status of each local epoch.
  // the centralized epoch thread should also tell each local epoch the latest time.
  uint64_t GetTailEpochId(const uint64_t current_epoch_id) {
    IncreaseTailEpochId(current_epoch_id);
    return tail_epoch_id_;
  }

  // queue size
  static const size_t epoch_buffer_size_ = 4096;
  std::vector<Epoch> epoch_buffer_;

  uint64_t head_epoch_id_; // point to the latest epoch that the thread is aware of.
  uint64_t tail_epoch_id_; // point to the oldest epoch that we can reclaim.
};

/////////////////////////////////////////////////////////////////

public:
  DecentralizedEpochManager() : 
    current_global_epoch_(1), 
    next_txn_id_(0),
    is_running_(false) {}


  static DecentralizedEpochManager &GetInstance() {
    static DecentralizedEpochManager epoch_manager;
    return epoch_manager;
  }

  virtual void Reset(const size_t &current_epoch) override {
    current_global_epoch_ = (uint64_t) current_epoch;
  }

  virtual void StartEpoch(std::unique_ptr<std::thread> &epoch_thread) override {
    LOG_TRACE("Starting epoch");
    this->is_running_ = true;
    epoch_thread.reset(new std::thread(&DecentralizedEpochManager::Running, this));
  }

  virtual void StartEpoch() override {
    LOG_TRACE("Starting epoch");
    this->is_running_ = true;
    thread_pool.SubmitDedicatedTask(&DecentralizedEpochManager::Running, this);
  }

  virtual void StopEpoch() override {
    LOG_TRACE("Stopping epoch");
    this->is_running_ = false;
  }

  virtual void RegisterThread(const size_t thread_id) override {
    local_epoch_context_lock_.Lock();

    local_epoch_contexts_[thread_id].reset(new LocalEpochContext());

    local_epoch_context_lock_.Unlock();
  }

  virtual void DeregisterThread(const size_t thread_id) override {
    local_epoch_context_lock_.Lock();

    local_epoch_contexts_.erase(thread_id);

    local_epoch_context_lock_.Unlock();
  }

  // enter epoch with thread id
  virtual cid_t EnterEpochD(const size_t thread_id) override {

    PL_ASSERT(local_epoch_contexts_.find(thread_id) != local_epoch_contexts_.end());

    while (true) {
      uint64_t epoch_id = GetCurrentGlobalEpoch();

      // enter the corresponding local epoch.
      bool rt = local_epoch_contexts_.at(thread_id)->EnterLocalEpoch(epoch_id);
      // if successfully enter local epoch
      if (rt == true) {
    
        uint32_t next_txn_id = GetNextTransactionId();

        return (epoch_id << 32) | next_txn_id;
      }
    }
  }

  virtual void ExitEpochD(const size_t thread_id, const size_t begin_cid) override {

    PL_ASSERT(local_epoch_contexts_.find(thread_id) != local_epoch_contexts_.end());

    uint64_t epoch_id = ExtractEpochId(begin_cid);

    // enter the corresponding local epoch.
    local_epoch_contexts_.at(thread_id)->ExitLocalEpoch(epoch_id);
 
  }


  virtual uint64_t GetTailEpochId() override {
    uint64_t min_epoch_id = std::numeric_limits<uint64_t>::max();
    
    for (auto &local_epoch_context : local_epoch_contexts_) {
      
      uint64_t local_epoch_id = local_epoch_context.second->GetTailEpochId(current_global_epoch_);
      
      if (local_epoch_id < min_epoch_id) {
        min_epoch_id = local_epoch_context.second->tail_epoch_id_;
      }
    }

    return min_epoch_id;
  }

private:

  inline uint64_t ExtractEpochId(const cid_t cid) {
    return (cid >> 32);
  }

  inline uint64_t GetCurrentGlobalEpoch() {
    return current_global_epoch_.load();
  }

  inline uint32_t GetNextTransactionId() {
    return next_txn_id_.fetch_add(1, std::memory_order_relaxed);
  }


  void Running() {

    PL_ASSERT(is_running_ == true);

    while (is_running_ == true) {
      // the epoch advances every EPOCH_LENGTH milliseconds.
      std::this_thread::sleep_for(std::chrono::milliseconds(EPOCH_LENGTH));

    }
  }

private:

  // each thread holds a pointer to a local epoch context.
  // it updates the local epoch context to report their local time.
  Spinlock local_epoch_context_lock_;
  std::unordered_map<int, std::unique_ptr<LocalEpochContext>> local_epoch_contexts_;
  
  // the global epoch reflects the true time of the system.
  std::atomic<uint64_t> current_global_epoch_;
  std::atomic<uint32_t> next_txn_id_;
  
  bool is_running_;

};

}
}

