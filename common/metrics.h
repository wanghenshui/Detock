#pragma once

#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/configuration.h"
#include "common/spin_latch.h"
#include "glog/logging.h"
#include "proto/transaction.pb.h"

namespace slog {

class Sampler {
  constexpr static uint32_t kSampleMaskSize = 1 << 8;
  using sample_mask_t = std::array<bool, kSampleMaskSize>;

  sample_mask_t sample_mask_;
  std::vector<uint8_t> sample_count_;

 public:
  Sampler(int sample_rate, size_t num_keys) : sample_count_(num_keys, 0) {
    sample_mask_.fill(false);
    for (uint32_t i = 0; i < sample_rate * kSampleMaskSize / 100; i++) {
      sample_mask_[i] = true;
    }
    auto rd = std::random_device{};
    auto rng = std::default_random_engine{rd()};
    std::shuffle(sample_mask_.begin(), sample_mask_.end(), rng);
  }

  bool IsChosen(size_t key) {
    DCHECK_LT(sample_count_[key], sample_mask_.size());
    return sample_mask_[sample_count_[key]++];
  }
};

class TransactionEventMetrics {
 public:
  TransactionEventMetrics(int sample_rate, uint32_t local_replica, uint32_t local_partition)
      : sampler_(sample_rate, TransactionEvent_descriptor()->value_count()),
        local_replica_(local_replica),
        local_partition_(local_partition) {}

  std::chrono::system_clock::time_point Record(TxnId txn_id, TransactionEvent event) {
    auto now = std::chrono::system_clock::now();
    auto sample_index = static_cast<size_t>(event);
    if (sampler_.IsChosen(sample_index)) {
      txn_events_.push_back({.time = now.time_since_epoch().count(),
                             .replica = local_replica_,
                             .partition = local_partition_,
                             .txn_id = txn_id,
                             .event = event});
    }
    return now;
  }

  struct Data {
    int64_t time;  // nanosecond since epoch
    uint32_t replica;
    uint32_t partition;
    TxnId txn_id;
    TransactionEvent event;
  };

  std::list<Data>& data() { return txn_events_; }

 private:
  Sampler sampler_;
  uint32_t local_replica_;
  uint32_t local_partition_;
  std::list<Data> txn_events_;
};

class DeadlockResolverRunMetrics {
 public:
  DeadlockResolverRunMetrics(int sample_rate, uint32_t local_replica, uint32_t local_partition)
      : sampler_(sample_rate, 2), local_replica_(local_replica), local_partition_(local_partition) {}

  void Record(int64_t runtime, size_t unstable_graph_sz, size_t stable_graph_sz, size_t deadlocks_resolved) {
    if (sampler_.IsChosen(0)) {
      data_.push_back({.time = std::chrono::system_clock::now().time_since_epoch().count(),
                       .partition = local_partition_,
                       .replica = local_replica_,
                       .runtime = runtime,
                       .unstable_graph_sz = unstable_graph_sz,
                       .stable_graph_sz = stable_graph_sz,
                       .deadlocks_resolved = deadlocks_resolved});
    }
  }

  struct Data {
    int64_t time;  // nanosecond since epoch
    uint32_t partition;
    uint32_t replica;
    int64_t runtime;  // nanosecond
    size_t unstable_graph_sz;
    size_t stable_graph_sz;
    size_t deadlocks_resolved;
  };
  std::list<Data>& data() { return data_; }

 private:
  Sampler sampler_;
  uint32_t local_replica_;
  uint32_t local_partition_;
  std::list<Data> data_;
};

class DeadlockResolverDeadlockMetrics {
 public:
  DeadlockResolverDeadlockMetrics(int sample_rate, uint32_t local_replica, uint32_t local_partition)
      : sampler_(sample_rate, 2), local_replica_(local_replica), local_partition_(local_partition) {}

  void Record(int num_vertices, const std::vector<std::pair<uint64_t, uint64_t>>& edges_removed,
              const std::vector<std::pair<uint64_t, uint64_t>>& edges_added) {
    if (sampler_.IsChosen(1)) {
      data_.push_back({.time = std::chrono::system_clock::now().time_since_epoch().count(),
                       .partition = local_partition_,
                       .replica = local_replica_,
                       .num_vertices = num_vertices,
                       .edges_removed = edges_removed,
                       .edges_added = edges_added});
    }
  }

  struct Data {
    int64_t time;  // nanosecond since epoch
    uint32_t partition;
    uint32_t replica;
    int num_vertices;
    std::vector<std::pair<uint64_t, uint64_t>> edges_removed;
    std::vector<std::pair<uint64_t, uint64_t>> edges_added;
  };
  std::list<Data>& data() { return data_; }

 private:
  Sampler sampler_;
  uint32_t local_replica_;
  uint32_t local_partition_;
  std::list<Data> data_;
};

/**
 * Repository of metrics per thread
 */
class MetricsRepository {
 public:
  struct AllMetrics {
    TransactionEventMetrics txn_event_metrics;
    DeadlockResolverRunMetrics deadlock_resolver_run_metrics;
    DeadlockResolverDeadlockMetrics deadlock_resolver_deadlock_metrics;
  };

  MetricsRepository(const ConfigurationPtr& config);

  std::chrono::system_clock::time_point RecordTxnEvent(TxnId txn_id, TransactionEvent event);
  void RecordDeadlockResolverRun(int64_t runtime, size_t unstable_graph_sz, size_t stable_graph_sz,
                                 size_t deadlocks_resolved);
  void RecordDeadlockResolverDeadlock(int num_vertices, const std::vector<std::pair<uint64_t, uint64_t>>& edges_removed,
                                      const std::vector<std::pair<uint64_t, uint64_t>>& edges_added);
  std::unique_ptr<AllMetrics> Reset();

 private:
  const ConfigurationPtr config_;
  SpinLatch latch_;

  std::unique_ptr<AllMetrics> metrics_;
};

extern thread_local std::shared_ptr<MetricsRepository> per_thread_metrics_repo;

/**
 * Handles thread registering, aggregates results, and output results to files
 */
class MetricsRepositoryManager {
 public:
  MetricsRepositoryManager(const ConfigurationPtr& config);
  void RegisterCurrentThread();
  void AggregateAndFlushToDisk(const std::string& dir);

 private:
  const ConfigurationPtr config_;
  std::unordered_map<std::thread::id, std::shared_ptr<MetricsRepository>> metrics_repos_;
  std::mutex mut_;
};

using MetricsRepositoryManagerPtr = std::shared_ptr<MetricsRepositoryManager>;

extern uint32_t gLocalMachineId;
extern uint64_t gDisabledEvents;

void InitializeRecording(const ConfigurationPtr& config);

template <typename TxnOrBatchPtr>
inline void RecordTxnEvent(TxnOrBatchPtr txn, TransactionEvent event) {
  using Clock = std::chrono::system_clock;
  auto now = duration_cast<microseconds>(Clock::now().time_since_epoch()).count();
  if ((gDisabledEvents >> event) & 1) {
    return;
  }
  TxnId txn_id = 0;
  if (txn != nullptr) {
    txn->mutable_events()->Add(event);
    txn->mutable_event_times()->Add(now);
    txn->mutable_event_machines()->Add(gLocalMachineId);
    txn_id = txn->id();
  }
  if (per_thread_metrics_repo != nullptr) {
    per_thread_metrics_repo->RecordTxnEvent(txn_id, event);
  }
}

#ifdef ENABLE_TXN_EVENT_RECORDING
#define INIT_RECORDING(config) slog::InitializeRecording(config)
#define RECORD(txn, event) RecordTxnEvent(txn, event)
#else
#define INIT_RECORDING(config)
#define RECORD(txn, event)
#endif

// Helper function for quickly monitor throughput at a certain place
// TODO: use thread_local instead of static
#define MONITOR_THROUGHPUT()                                                                                \
  static int TP_COUNTER = 0;                                                                                \
  static int TP_LAST_COUNTER = 0;                                                                           \
  static std::chrono::steady_clock::time_point TP_LAST_LOG_TIME;                                            \
  TP_COUNTER++;                                                                                             \
  auto TP_LOG_SPAN = std::chrono::steady_clock::now() - TP_LAST_LOG_TIME;                                   \
  if (TP_LOG_SPAN > 1s) {                                                                                   \
    LOG(INFO) << "Throughput: "                                                                             \
              << (TP_COUNTER - TP_LAST_COUNTER) / std::chrono::duration_cast<seconds>(TP_LOG_SPAN).count(); \
    TP_LAST_COUNTER = TP_COUNTER;                                                                           \
    TP_LAST_LOG_TIME = std::chrono::steady_clock::now();                                                    \
  }

}  // namespace slog