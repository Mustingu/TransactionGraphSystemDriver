#include <omp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "utils/EdgeReader.h"
#include "utils/GraphlogReplay.h"
#include "utils/MyTime.h"  // Timing helpers.
#include "utils/Parse.h"
#include "utils/Permute.h"
#include "utils/WorkloadGen.h"

// #define USE_SLT

// AVB and SLT headers define conflicting global symbols (Transaction,
// VersionedTopologyInterface, etc.), so compile this shared source into separate runners:
// bench_run (default): AVB / GTX / RS / LG / Teseo.
//   bench_run_slt (-DUSE_SLT): SLT / GTX / LG / Teseo.

#ifdef USE_SLT
#include "Drivers/SLTDriver.h"
#else
#include "Drivers/AVBDriver.h"
#include "Drivers/RSDriver.h"
#endif
#ifdef ENABLE_GTX
#include "Drivers/GTXDriver.h"
#endif
// AVBGraph/Sortledton data_types.h defines the object-like macro NO_TRANSACTION.
// Undefine it before including LiveGraph's identically named class constant.
#ifdef NO_TRANSACTION
#undef NO_TRANSACTION
#endif
#include "Drivers/LiveGraphDriver.h"
#include "Drivers/TeseoDriver.h"
#include "Drivers/CSRDriver.h"
#include "GraphDriver.h"

// Global input data.
std::vector<std::tuple<unsigned, unsigned, uint64_t>> update_tuple;
bench::GraphlogWorkload graphlog_workload;

std::atomic<int> one_finished = 0;

// Command-line arguments.
bench::Args Arg;

// Shared validation state.
unsigned ans[2000010];
std::map<unsigned, unsigned> trans;
std::map<std::pair<dst_t, dst_t>, double> edge_wight;
std::map<unsigned, std::tuple<unsigned, dst_t*, dst_t*>> vid2edgewithpro;
std::map<std::pair<dst_t, dst_t>, std::vector<unsigned>> edge_wight_versioned;

// AVBGraph's global allocator is declared extern in utils/utils.h and defined by the host.
// init(50,65) uses the bundled benchmark's allocator parameters.
#ifndef USE_SLT
MemoryAllocator* la;
#endif
thread_local int threadid;
// Declare the counter helper before BenchWrite; define it with the counters below.
extern std::atomic<uint64_t> g_ops_done;
// Unified concurrent write benchmark.
// Dispatch through GraphDriver rather than hardcoding a backend.
void BenchWrite(GraphDriver* driver, int thread_count, int repeat_count = 1,
                bool is_first = true, bool finish = true) {
  // repeat_count = 1;
  int one_edges_num = update_tuple.size();
  int total_edges = one_edges_num * repeat_count;
  std::vector<std::thread> threads;
  std::atomic<uint64_t> current_idx(0);
  std::atomic<uint64_t> inserted_edges(0);
  std::atomic<uint64_t> bigger(0);
  uint64_t chunk_size = 1 << 10;  // Work chunk size.

  auto worker = [&](int thread_id) {
    // Backend-specific worker registration occurs here when required.
    // Example: driver->RegisterThread(thread_id).
    threadid = thread_id;
    driver->RegisterThread(thread_id);
    uint64_t start;
    thread_local std::mt19937 gen(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::uniform_int_distribution<unsigned> dis(0, 10000);

    while ((start = current_idx.fetch_add(chunk_size)) < total_edges) {
      uint64_t end = std::min<uint64_t>(start + chunk_size, total_edges);
      // std::cout << "Thread " << thread_id << ": " << start << " to " << end
      //           << '\n';
      if (start >= one_edges_num) {
        bigger++;
      }
      int t;
      for (uint64_t i = start; i < end; ++i) {
        // std::cout << std::to_string(i)+'\n';
        if (i >= one_edges_num)
          t = i % one_edges_num;
        else
          t = i;
        auto& edge = update_tuple[t];
        // Dispatch to the backend adapter.
        if (is_first)
          driver->UpsertEdge(std::get<0>(edge), std::get<1>(edge),
                             std::get<2>(edge), thread_id);

        else
          driver->UpsertEdge(std::get<1>(edge), std::get<0>(edge),
                             std::get<2>(edge), thread_id);
        // Count concurrent writes for partial-throughput snapshots via --progress-file.
        g_ops_done.fetch_add(1, std::memory_order_relaxed);
      }

      inserted_edges += end - start;
      if (inserted_edges >= one_edges_num) one_finished = 1;

      if (!Arg.quiet_ && (start / chunk_size) % 10000 == 0)
        std::cout << "Finished " + std::to_string(start) + " edges\n";
    }

    driver->DeregisterThread(thread_id);
  };

  PrintFunctionTime(
      [&]() {
        for (int i = 0; i < thread_count; ++i) {
          threads.emplace_back(worker, i);
        }
        for (auto& t : threads) {
          t.join();
        }
      },
      "WritePhase");

  // Mixed loading defers finalization until all update rounds finish: AVB's
  // NoMoreTxn publishes the terminal epoch and must not precede further writes.
  if (finish) driver->FinishWrites();
  if (!Arg.quiet_) std::cout << "Inserted " << inserted_edges << " edges\n";
}

// ================= Experiment 1: Mixed-update =================

// Completed-transaction counter observed by heartbeat/STALL detection for every backend.
std::atomic<uint64_t> g_ops_done{0};
std::atomic<uint64_t> g_hotset_round_done{0};
std::mutex g_hotset_log_mutex;

// Progress reported on SIGTERM/SIGINT: writer phase and per-reader execution counts.
struct ConcurrentSnapshot {
  std::atomic<int> writer_phase{0};        // 0=not started, 1=first pass, 2=first pass done/readers released, 3=writes done.
  std::atomic<int> num_tasks{0};           // Number of reader tasks.
  std::atomic<int> reader_started[16];     // Algorithm executions started per task.
  std::atomic<int> reader_completed[16];   // Algorithm executions completed per task.
  std::atomic<long long> reader_time_us[16];  // Cumulative algorithm time per task in microseconds, including partial snapshots.
};
static ConcurrentSnapshot g_snap;
// Algorithm names point into Arg.read_tasks and remain valid throughout the process.
static const char* g_reader_algo[16];

// With --progress-file, write the atomic counters to a sidecar every 10 seconds.
// The caller must join this thread before the referenced finish flag is destroyed.
static std::thread StartProgressFile(
    std::chrono::steady_clock::time_point start,
    const std::atomic<bool>& finish) {
  if (Arg.progress_file_.empty()) return std::thread();
  return std::thread([start, &finish]() {
    while (!finish.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      // Truncate and rewrite each frame; fflush preserves the latest frame on crash or hang.
      FILE* f = fopen(Arg.progress_file_.c_str(), "w");
      if (!f) continue;
      double elapsed_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start)
                              .count();
      int ntasks = g_snap.num_tasks.load(std::memory_order_relaxed);
      fprintf(f, "PROGRESS elapsed_ms=%.0f writer_ops=%llu num_tasks=%d\n",
              elapsed_ms,
              static_cast<unsigned long long>(
                  g_ops_done.load(std::memory_order_relaxed)),
              ntasks);
      for (int i = 0; i < ntasks && i < 16; ++i) {
        fprintf(f,
                "TASK %d algo=%s started=%d completed=%d elapsed_us=%lld\n", i,
                g_reader_algo[i] ? g_reader_algo[i] : "?",
                g_snap.reader_started[i].load(std::memory_order_relaxed),
                g_snap.reader_completed[i].load(std::memory_order_relaxed),
                static_cast<long long>(
                    g_snap.reader_time_us[i].load(std::memory_order_relaxed)));
      }
      fflush(f);
      fclose(f);
    }
  });
}

// Signal-triggered progress dump using bounded snprintf plus write/_exit.
// Only SIGTERM (timeout) or SIGINT (Ctrl-C) triggers output; normal timing does not.
static void progress_dump_handler(int sig) {
  char buf[1024];
  int n = 0;
  int wphase = g_snap.writer_phase.load(std::memory_order_relaxed);
  int ntasks = g_snap.num_tasks.load(std::memory_order_relaxed);
  auto append = [&](const char* format, auto... args) {
    if (n >= static_cast<int>(sizeof(buf))) return;
    int written = std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n),
                                format, args...);
    if (written > 0) n += std::min(written, static_cast<int>(sizeof(buf)) - n);
  };
  append("\n== PROGRESS DUMP (signal %d) ==\n", sig);
  append("writer_phase=%d (%s) writer_ops=%llu\n", wphase,
         wphase == 0 ? "not-started"
         : wphase == 1 ? "first-pass"
         : wphase == 2 ? "repeat-write/admit-readers"
                       : "write-done",
         static_cast<unsigned long long>(
             g_ops_done.load(std::memory_order_relaxed)));
  for (int i = 0; i < ntasks && i < 16; ++i) {
    int s = g_snap.reader_started[i].load(std::memory_order_relaxed);
    int c = g_snap.reader_completed[i].load(std::memory_order_relaxed);
    append("reader[%d] algo=%s started=%d completed=%d in_flight=%d\n", i,
           g_reader_algo[i] ? g_reader_algo[i] : "?", s, c, s - c);
  }
  append("== END PROGRESS DUMP ==\n");
  if (n > 0) write(STDERR_FILENO, buf, static_cast<size_t>(n));
  _exit(128 + sig);
}

// Sample g_ops_done every 10 seconds; 12 consecutive unchanged samples (120s)
// report STALL. Zero write progress alone does not prove a deadlock. --stall-abort raises SIGABRT.
// If supplied, active_flag stops monitoring when false, so HotSet readers can finish
// after writes end without being misclassified by this writer-only progress counter.
void StartHeartbeat(bool stall_abort,
                    std::shared_ptr<std::atomic<bool>> active_flag = nullptr) {
  std::thread([stall_abort, active_flag]() {
    uint64_t last = g_ops_done.load();
    int zero_rounds = 0;
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      if (active_flag && !active_flag->load(std::memory_order_acquire)) {
        return;
      }
      uint64_t cur = g_ops_done.load();
      uint64_t delta = cur - last;
      if (delta == 0)
        zero_rounds++;
      else
        zero_rounds = 0;
      std::cout << "[Heartbeat] ops done: " + std::to_string(cur) +
                       " rate: " + std::to_string(delta / 10) + " ops/s\n";
      if (zero_rounds >= 12) {  // No write progress for 120 seconds.
        std::cout << "[STALL] no progress for 120s, ops=" << cur << std::endl;
        zero_rounds = 0;  // Report at most once per 120 seconds.
        if (stall_abort) {
          std::cout << "[STALL] raising SIGABRT for core dump" << std::endl;
          raise(SIGABRT);
        }
      }
      last = cur;
    }
  }).detach();
}

// Mixed executor: use BenchWrite's work-stealing chunks (1<<10) and dispatch by job type.
// MODIFY -> UpsertEdge (one transaction through the backend's update path).
// REINSERT -> DeleteEdge then UpsertEdge on the same worker (two transactions).
void BenchMixed(GraphDriver* driver, const std::vector<bench::WOp>& ops,
                int thread_count) {
  std::vector<std::thread> threads;
  std::atomic<uint64_t> current_idx(0);
  const uint64_t total = ops.size();
  const uint64_t chunk_size = 1 << 10;

  auto worker = [&](int thread_id) {
    threadid = thread_id;
    driver->RegisterThread(thread_id);
    uint64_t start;
    while ((start = current_idx.fetch_add(chunk_size)) < total) {
      uint64_t end = std::min<uint64_t>(start + chunk_size, total);
      for (uint64_t i = start; i < end; ++i) {
        const auto& op = ops[i];
        if (op.type == bench::OpType::MODIFY) {
          driver->UpsertEdge(op.src, op.dst, op.weight, thread_id);
          g_ops_done.fetch_add(1, std::memory_order_relaxed);
        } else {  // REINSERT
          driver->DeleteEdge(op.src, op.dst, thread_id);
          driver->UpsertEdge(op.src, op.dst, op.weight, thread_id);
          g_ops_done.fetch_add(2, std::memory_order_relaxed);
        }
      }
    }
    driver->DeregisterThread(thread_id);
  };

  PrintFunctionTime(
      [&]() {
        for (int i = 0; i < thread_count; ++i) {
          threads.emplace_back(worker, i);
        }
        for (auto& t : threads) {
          t.join();
        }
      },
      "MixedRound");
}

void BenchChurn(GraphDriver* driver, const std::vector<bench::ChurnOp>& ops,
                int thread_count) {
  std::vector<std::thread> threads;
  std::atomic<uint64_t> current_idx{0};
  const uint64_t chunk_size = 1 << 10;

  auto worker = [&](int thread_id) {
    threadid = thread_id;
    driver->RegisterThread(thread_id);
    uint64_t start;
    while ((start = current_idx.fetch_add(chunk_size)) < ops.size()) {
      uint64_t end = std::min<uint64_t>(start + chunk_size, ops.size());
      for (uint64_t index = start; index < end; ++index) {
        const auto& operation = ops[index];
        driver->DeleteEdge(operation.remove_src, operation.remove_dst,
                           thread_id);
        driver->UpsertEdge(operation.insert_src, operation.insert_dst,
                           operation.weight, thread_id);
      }
    }
    driver->DeregisterThread(thread_id);
  };

  PrintFunctionTime(
      [&]() {
        for (int thread_id = 0; thread_id < thread_count; ++thread_id) {
          threads.emplace_back(worker, thread_id);
        }
        for (auto& thread : threads) thread.join();
      },
      "ChurnPhase");
}

struct TeseoChurnRunResult {
  uint64_t logical_updates = 0;
  uint64_t graphlog_operations = 0;
  uint64_t checksum = 0;
  uint64_t order_checksum = 0;
  double plan_ms = 0.0;
  double shuffle_ms = 0.0;
  double replay_ms = 0.0;
  bool completed = true;
};

enum class TeseoChurnWriterStage : uint8_t {
  IDLE = 0,
  DELETE_EDGE = 1,
  INSERT_EDGE = 2,
  DONE = 3,
};

struct TeseoChurnWriterState {
  std::atomic<uint64_t> current_index{std::numeric_limits<uint64_t>::max()};
  std::atomic<uint64_t> completed_replacements{0};
  std::atomic<uint64_t> last_progress_ns{0};
  std::atomic<uint8_t> stage{
      static_cast<uint8_t>(TeseoChurnWriterStage::IDLE)};
};

uint64_t SteadyClockNanos() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

const char* TeseoChurnStageName(uint8_t stage) {
  switch (static_cast<TeseoChurnWriterStage>(stage)) {
    case TeseoChurnWriterStage::IDLE:
      return "idle";
    case TeseoChurnWriterStage::DELETE_EDGE:
      return "delete";
    case TeseoChurnWriterStage::INSERT_EDGE:
      return "insert";
    case TeseoChurnWriterStage::DONE:
      return "done";
  }
  return "unknown";
}

TeseoChurnRunResult BenchTeseoChurn(
    GraphDriver* driver, int thread_count, uint64_t rounds) {
  TeseoChurnRunResult result;
  if (update_tuple.empty()) {
    result.completed = false;
    return result;
  }

  const uint64_t edge_count = update_tuple.size();
  const uint64_t chunk_size = 1 << 10;
  auto* teseo_driver = dynamic_cast<TeseoDriver*>(driver);
  std::unique_ptr<TeseoChurnWriterState[]> writer_states(
      new TeseoChurnWriterState[thread_count]);
  std::vector<std::thread> workers;
  std::atomic<uint64_t> current_index{0};
  std::atomic<bool> failed{false};
  std::atomic<bool> replay_active{false};
  std::atomic<uint64_t> active_round_number{0};
  std::mutex round_mutex;
  std::condition_variable round_start_cv;
  std::condition_variable round_done_cv;
  uint64_t generation = 0;
  const std::vector<bench::ChurnOp>* active_operations = nullptr;
  const std::vector<uint64_t>* active_execution_order = nullptr;
  int completed_workers = 0;
  bool stopping = false;
  std::mutex monitor_mutex;
  std::condition_variable monitor_cv;
  bool monitor_stopping = false;

  auto worker = [&](int thread_id) {
    driver->RegisterThread(thread_id);
    uint64_t observed_generation = 0;
    while (true) {
      {
        std::unique_lock<std::mutex> lock(round_mutex);
        round_start_cv.wait(lock, [&]() {
          return stopping || generation > observed_generation;
        });
        if (stopping) break;
        observed_generation = generation;
      }

      uint64_t start;
      while (!failed.load(std::memory_order_acquire) &&
             (start = current_index.fetch_add(chunk_size,
                                              std::memory_order_relaxed)) <
                 edge_count) {
        uint64_t end = std::min<uint64_t>(start + chunk_size, edge_count);
        for (uint64_t index = start; index < end; ++index) {
          if (failed.load(std::memory_order_acquire)) break;
          uint64_t operation_index = (*active_execution_order)[index];
          const auto& operation = (*active_operations)[operation_index];
          writer_states[thread_id].current_index.store(
              operation_index, std::memory_order_release);
          writer_states[thread_id].stage.store(
              static_cast<uint8_t>(TeseoChurnWriterStage::DELETE_EDGE),
              std::memory_order_release);
          if (!driver->DeleteEdge(operation.remove_src, operation.remove_dst,
                                  thread_id)) {
            failed.store(true, std::memory_order_release);
            break;
          }
          writer_states[thread_id].stage.store(
              static_cast<uint8_t>(TeseoChurnWriterStage::INSERT_EDGE),
              std::memory_order_release);
          if (!driver->InsertEdgeIfAbsent(operation.insert_src,
                                          operation.insert_dst,
                                          operation.weight, thread_id)) {
            failed.store(true, std::memory_order_release);
            break;
          }
          update_tuple[operation_index] = {
              operation.insert_src, operation.insert_dst, operation.weight};
          writer_states[thread_id].completed_replacements.fetch_add(
              1, std::memory_order_relaxed);
          writer_states[thread_id].last_progress_ns.store(
              SteadyClockNanos(), std::memory_order_release);
          g_ops_done.fetch_add(2, std::memory_order_relaxed);
        }
      }

      writer_states[thread_id].stage.store(
          static_cast<uint8_t>(TeseoChurnWriterStage::DONE),
          std::memory_order_release);

      {
        std::lock_guard<std::mutex> lock(round_mutex);
        completed_workers++;
        if (completed_workers == thread_count) round_done_cv.notify_one();
      }
    }
    driver->DeregisterThread(thread_id);
  };

  for (int thread_id = 0; thread_id < thread_count; ++thread_id) {
    workers.emplace_back(worker, thread_id);
  }

  g_ops_done.store(0, std::memory_order_release);
  std::thread monitor([&]() {
    uint64_t last_operations = 0;
    uint64_t last_delete_conflicts = 0;
    uint64_t last_insert_conflicts = 0;
    int zero_progress_samples = 0;
    std::unique_lock<std::mutex> lock(monitor_mutex);
    while (!monitor_cv.wait_for(lock, std::chrono::seconds(10),
                                [&]() { return monitor_stopping; })) {
      lock.unlock();
      uint64_t operations = g_ops_done.load(std::memory_order_acquire);
      uint64_t delete_conflicts = 0;
      uint64_t insert_conflicts = 0;
      uint64_t min_completed = std::numeric_limits<uint64_t>::max();
      uint64_t max_completed = 0;
      int stage_counts[4] = {0, 0, 0, 0};
      for (int thread_id = 0; thread_id < thread_count; ++thread_id) {
        uint64_t completed = writer_states[thread_id]
                                 .completed_replacements.load(
                                     std::memory_order_acquire);
        min_completed = std::min(min_completed, completed);
        max_completed = std::max(max_completed, completed);
        uint8_t stage =
            writer_states[thread_id].stage.load(std::memory_order_acquire);
        if (stage < 4) stage_counts[stage]++;
        if (teseo_driver != nullptr) {
          delete_conflicts += teseo_driver->DeleteConflictCount(thread_id);
          insert_conflicts += teseo_driver->InsertConflictCount(thread_id);
        }
      }
      if (min_completed == std::numeric_limits<uint64_t>::max()) {
        min_completed = 0;
      }

      bool in_replay = replay_active.load(std::memory_order_acquire);
      uint64_t operation_delta = operations - last_operations;
      std::cout << "[TeseoChurn Heartbeat] round="
                << active_round_number.load(std::memory_order_acquire)
                << " phase=" << (in_replay ? "replay" : "planning")
                << " ops_done=" << operations
                << " rate=" << operation_delta / 10 << " ops/s"
                << " writer_completed_min=" << min_completed
                << " writer_completed_max=" << max_completed
                << " stages=idle:" << stage_counts[0]
                << ",delete:" << stage_counts[1]
                << ",insert:" << stage_counts[2]
                << ",done:" << stage_counts[3]
                << " delete_conflicts=" << delete_conflicts
                << "(+" << delete_conflicts - last_delete_conflicts << ")"
                << " insert_conflicts=" << insert_conflicts
                << "(+" << insert_conflicts - last_insert_conflicts << ")"
                << std::endl;

      if (in_replay && operation_delta == 0)
        zero_progress_samples++;
      else
        zero_progress_samples = 0;

      if (zero_progress_samples >= 12) {
        uint64_t now_ns = SteadyClockNanos();
        std::cout << "[TeseoChurn STALL] no replay progress for 120s, "
                     "dumping writer states"
                  << std::endl;
        for (int thread_id = 0; thread_id < thread_count; ++thread_id) {
          uint64_t last_progress = writer_states[thread_id]
                                       .last_progress_ns.load(
                                           std::memory_order_acquire);
          uint64_t age_ms = last_progress == 0
                                ? 0
                                : (now_ns - last_progress) / 1'000'000;
          std::cout << "[TeseoChurn Writer] id=" << thread_id
                    << " index="
                    << writer_states[thread_id].current_index.load(
                           std::memory_order_acquire)
                    << " stage="
                    << TeseoChurnStageName(writer_states[thread_id].stage.load(
                           std::memory_order_acquire))
                    << " completed="
                    << writer_states[thread_id]
                           .completed_replacements.load(
                               std::memory_order_acquire)
                    << " delete_conflicts="
                    << (teseo_driver == nullptr
                            ? 0
                            : teseo_driver->DeleteConflictCount(thread_id))
                    << " insert_conflicts="
                    << (teseo_driver == nullptr
                            ? 0
                            : teseo_driver->InsertConflictCount(thread_id))
                    << " last_progress_age_ms=" << age_ms << std::endl;
        }
        zero_progress_samples = 0;
        if (Arg.stall_abort_) {
          std::cout << "[TeseoChurn STALL] raising SIGABRT for core dump"
                    << std::endl;
          raise(SIGABRT);
        }
      }

      last_operations = operations;
      last_delete_conflicts = delete_conflicts;
      last_insert_conflicts = insert_conflicts;
      lock.lock();
    }
  });

  for (uint64_t round = 1; round <= rounds; ++round) {
    std::vector<bench::ChurnOp> operations;
    active_round_number.store(round, std::memory_order_release);
    auto plan_start = std::chrono::high_resolution_clock::now();
    try {
      operations = bench::GenerateGraphlogLikeChurn(
          update_tuple, Arg.wseed_ ^ bench::MixHash64(round));
    } catch (const std::exception& error) {
      std::cerr << "TeseoChurn graphlog generation failed in round " << round
                << ": " << error.what() << "\n";
      result.completed = false;
      break;
    }
    auto plan_end = std::chrono::high_resolution_clock::now();
    if (operations.size() != edge_count) {
      std::cerr << "TeseoChurn graphlog generation returned "
                << operations.size() << " replacements, expected "
                << edge_count << "\n";
      result.completed = false;
      break;
    }

    std::vector<uint64_t> execution_order(operations.size());
    std::iota(execution_order.begin(), execution_order.end(), 0);
    uint64_t shuffle_seed = bench::MixHash64(Arg.wseed_ ^ round);
    auto shuffle_start = std::chrono::high_resolution_clock::now();
    std::mt19937_64 shuffle_rng(shuffle_seed);
    std::shuffle(execution_order.begin(), execution_order.end(), shuffle_rng);
    auto shuffle_end = std::chrono::high_resolution_clock::now();

    uint64_t operation_checksum = bench::MixHash64(Arg.wseed_ ^ round);
    uint64_t order_checksum = bench::MixHash64(shuffle_seed);
    for (size_t index = 0; index < operations.size(); ++index) {
      const auto& operation = operations[index];
      uint64_t value = bench::MixHash64(bench::UndirectedEdgeKey(
          operation.insert_src, operation.insert_dst));
      operation_checksum ^= value;
      const auto& ordered_operation = operations[execution_order[index]];
      uint64_t ordered_value = bench::MixHash64(bench::UndirectedEdgeKey(
          ordered_operation.insert_src, ordered_operation.insert_dst));
      order_checksum = bench::MixHash64(
          order_checksum ^ ordered_value ^ bench::MixHash64(index));
    }

    double plan_ms =
        std::chrono::duration<double, std::milli>(plan_end - plan_start)
            .count();
    double shuffle_ms =
        std::chrono::duration<double, std::milli>(shuffle_end - shuffle_start)
            .count();
    result.plan_ms += plan_ms;
    result.shuffle_ms += shuffle_ms;

    uint64_t replay_start_ns = SteadyClockNanos();
    if (teseo_driver != nullptr) {
      teseo_driver->ResetChurnConflictStats(thread_count);
    }
    for (int thread_id = 0; thread_id < thread_count; ++thread_id) {
      writer_states[thread_id].current_index.store(
          std::numeric_limits<uint64_t>::max(), std::memory_order_release);
      writer_states[thread_id].completed_replacements.store(
          0, std::memory_order_release);
      writer_states[thread_id].last_progress_ns.store(
          replay_start_ns, std::memory_order_release);
      writer_states[thread_id].stage.store(
          static_cast<uint8_t>(TeseoChurnWriterStage::IDLE),
          std::memory_order_release);
    }

    auto start = std::chrono::high_resolution_clock::now();
    {
      std::lock_guard<std::mutex> lock(round_mutex);
      current_index.store(0, std::memory_order_release);
      completed_workers = 0;
      active_operations = &operations;
      active_execution_order = &execution_order;
      generation++;
    }
    replay_active.store(true, std::memory_order_release);
    round_start_cv.notify_all();
    {
      std::unique_lock<std::mutex> lock(round_mutex);
      round_done_cv.wait(lock, [&]() { return completed_workers == thread_count; });
    }
    auto end = std::chrono::high_resolution_clock::now();
    replay_active.store(false, std::memory_order_release);
    uint64_t completed = g_ops_done.load(std::memory_order_acquire);
    uint64_t round_operations = completed - result.graphlog_operations;
    uint64_t round_updates = round_operations / 2;
    result.graphlog_operations = completed;
    result.logical_updates += round_updates;
    result.checksum ^= operation_checksum;
    result.order_checksum ^= order_checksum;
    double elapsed_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    result.replay_ms += elapsed_ms;
    std::cout << "TeseoChurn Round " << round
              << ": graphlog_operations=" << round_operations
              << " replacements=" << round_updates
              << " plan_ms=" << std::fixed << std::setprecision(3) << plan_ms
              << " shuffle_ms=" << shuffle_ms
              << " replay_ms=" << elapsed_ms
              << " replay_throughput="
              << (elapsed_ms > 0.0 ? round_operations / (elapsed_ms / 1000.0)
                                   : 0.0)
              << " operation_checksum=" << operation_checksum
              << " order_checksum=" << order_checksum << std::defaultfloat
              << "\n";
    if (failed.load(std::memory_order_acquire)) {
      result.completed = false;
      break;
    }
  }

  {
    std::lock_guard<std::mutex> lock(round_mutex);
    stopping = true;
  }
  round_start_cv.notify_all();
  for (auto& worker_thread : workers) worker_thread.join();
  {
    std::lock_guard<std::mutex> lock(monitor_mutex);
    monitor_stopping = true;
  }
  monitor_cv.notify_all();
  monitor.join();
  return result;
}

struct HotSetRunResult {
  double total_ms = 0.0;
  uint64_t txns = 0;
  uint64_t updates = 0;
  uint64_t deletes = 0;
  uint64_t inserts = 0;
};

// Keep HotSet writers alive across rounds to avoid reallocating GTX worker IDs.
// Each writer performs a real first-round update to occupy the writer-ID prefix;
// ConcurrentHotSetTest then releases the remaining writes and readers together.
HotSetRunResult BenchHotSetConcurrent(
    GraphDriver* driver, size_t hot_edge_count, int thread_count,
    uint64_t rounds, std::atomic<int>& writers_ready,
    std::atomic<bool>& readers_started) {
  HotSetRunResult result;
  std::vector<std::thread> threads;
  std::atomic<uint64_t> current_idx{0};
  const uint64_t chunk_size = 1 << 10;
  std::mutex round_mutex;
  std::condition_variable round_start_cv;
  std::condition_variable round_done_cv;
  uint64_t generation = 0;
  uint64_t active_round = 0;
  int completed_workers = 0;
  bool stopping = false;
  const bool mixed = Arg.hotset_update_pct_ < 100;
  bench::HotSetMixedPlan active_plan;

  // Keep the selected hot-set edges stable. Each update position deterministically
  // selects one recorded hot edge and derives a fresh property value, so all
  // round preparation is done inline with the writer instead of pausing writers
  // for shuffle/weight generation between rounds.
  const uint64_t hot_seed = Arg.wseed_;
  auto hot_edge_index = [&](uint64_t position) -> size_t {
    return static_cast<size_t>(bench::MixHash64(hot_seed ^ position) %
                               hot_edge_count);
  };
  auto hot_edge_weight = [&](uint64_t position) -> uint64_t {
    const uint64_t value = bench::MixHash64(
        hot_seed ^ (position + 0xd6e8feb86659fd93ULL));
    const double weight = 1.0 + static_cast<double>(value % 9900) / 100.0;
    uint64_t encoded_weight;
    std::memcpy(&encoded_weight, &weight, sizeof(encoded_weight));
    return encoded_weight;
  };

  auto worker = [&](int thread_id) {
    threadid = thread_id;
    driver->RegisterThread(thread_id);
    uint64_t observed_generation = 0;
    bool announced_ready = false;

    while (true) {
      uint64_t round;
      bench::HotSetMixedPlan plan;
      {
        std::unique_lock<std::mutex> lock(round_mutex);
        round_start_cv.wait(lock, [&]() {
          return stopping || generation > observed_generation;
        });
        if (stopping) break;
        observed_generation = generation;
        round = active_round;
        if (mixed) plan = active_plan;
      }

      if (round == 1 && !announced_ready) {
        // U0 is strictly Delete/Insert. Other mixed ratios retain the
        // per-writer warmup Update, which is included in their planned ratio.
        if (!mixed || Arg.hotset_update_pct_ > 0) {
          const auto& edge = update_tuple[thread_id];
          driver->UpsertEdge(std::get<0>(edge), std::get<1>(edge),
                             std::get<2>(edge), thread_id);
          g_ops_done.fetch_add(1, std::memory_order_relaxed);
        }
        announced_ready = true;
        writers_ready.fetch_add(1, std::memory_order_release);
        while (!readers_started.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
      }

      uint64_t start;
      const uint64_t job_count = mixed ? plan.jobs : hot_edge_count;
      while ((start = current_idx.fetch_add(chunk_size)) < job_count) {
        uint64_t end =
            std::min<uint64_t>(start + chunk_size, job_count);
        for (uint64_t i = start; i < end; ++i) {
          const uint64_t position = (round - 1) * hot_edge_count + i;
          if (mixed) {
            const auto job = plan.JobAt(i);
            const auto& edge = update_tuple[job.edge_index];
            if (job.reinsert) {
              driver->DeleteEdge(std::get<0>(edge), std::get<1>(edge),
                                 thread_id);
              g_ops_done.fetch_add(1, std::memory_order_relaxed);
              driver->UpsertEdge(std::get<0>(edge), std::get<1>(edge),
                                 std::get<2>(edge), thread_id);
              g_ops_done.fetch_add(1, std::memory_order_relaxed);
            } else {
              driver->UpsertEdge(std::get<0>(edge), std::get<1>(edge),
                                 hot_edge_weight(position), thread_id);
              g_ops_done.fetch_add(1, std::memory_order_relaxed);
            }
          } else {
            const auto& edge = update_tuple[hot_edge_index(position)];
            driver->UpsertEdge(std::get<0>(edge), std::get<1>(edge),
                               hot_edge_weight(position), thread_id);
            g_ops_done.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }

      {
        std::lock_guard<std::mutex> lock(round_mutex);
        completed_workers++;
        if (completed_workers == thread_count) round_done_cv.notify_one();
      }
    }
    driver->DeregisterThread(thread_id);
  };

  for (int i = 0; i < thread_count; ++i) {
    threads.emplace_back(worker, i);
  }

  for (uint64_t round = 1; round <= rounds; ++round) {
    bench::HotSetMixedPlan next_plan;
    if (mixed) {
      next_plan = bench::MakeHotSetMixedPlan(
          hot_edge_count, thread_count, Arg.hotset_update_pct_, hot_seed, round);
    }
    auto round_start = std::chrono::high_resolution_clock::now();
    {
      std::lock_guard<std::mutex> lock(round_mutex);
      current_idx.store(mixed ? 0 : (round == 1 ? thread_count : 0),
                        std::memory_order_release);
      completed_workers = 0;
      active_round = round;
      if (mixed) active_plan = next_plan;
      generation++;
    }
    round_start_cv.notify_all();

    {
      std::unique_lock<std::mutex> lock(round_mutex);
      round_done_cv.wait(
          lock, [&]() { return completed_workers == thread_count; });
    }
    auto round_end = std::chrono::high_resolution_clock::now();
    double elapsed_ms =
        std::chrono::duration<double, std::milli>(round_end - round_start)
            .count();
    result.total_ms += elapsed_ms;
    if (mixed) {
      result.updates += next_plan.updates;
      result.deletes += next_plan.reinserts;
      result.inserts += next_plan.reinserts;
    }
    g_hotset_round_done.store(round, std::memory_order_release);
    double throughput =
        elapsed_ms > 0.0 ? hot_edge_count / (elapsed_ms / 1000.0) : 0.0;
    std::ostringstream round_log;
    round_log << "HotSet Round " << round << ": " << hot_edge_count
              << " txns in " << elapsed_ms << "ms, Throughput: "
              << std::fixed << std::setprecision(2) << throughput
              << " txns/s";
    {
      std::lock_guard<std::mutex> lock(g_hotset_log_mutex);
      std::cout << round_log.str() << std::endl;
    }
  }

  {
    std::lock_guard<std::mutex> lock(round_mutex);
    stopping = true;
  }
  round_start_cv.notify_all();
  for (auto& worker_thread : threads) {
    worker_thread.join();
  }
  result.txns = g_ops_done.load();
  return result;
}

// Load once, execute mixed-write rounds, then finalize writes.
void RunMixed(GraphDriver* driver) {
  const int wt = Arg.Spruce_wt;
  const size_t n = update_tuple.size();

  std::cout << "Mixed Load Start" << std::endl;
  driver->ResetWorkerThreads();  // Reset IDs before loading for backends with fixed worker tables, such as GTX.
  driver->BeginBulkLoad();
  PrintFunctionTime([&]() { BenchWrite(driver, wt, 1, true, false); },
                    "MixedLoad");
  driver->EndBulkLoad();
  // Keep writes open for the mixed rounds; finalize only after their completion.
  std::cout << "Graph loaded: " << n << " edges" << std::endl;

  // Exclude loading operations: total_ms measures only the mixed-write rounds.
  g_ops_done.store(0);

  StartHeartbeat(Arg.stall_abort_);
  std::cout << "Mixed Update Start" << std::endl;
  double total_ms = 0;
  for (int r = 1; r <= Arg.rounds_; r++) {
    bench::RoundStats st;
    auto ops = bench::GenerateMixedRound(update_tuple, Arg.mix_,
                                         Arg.wseed_ * 1000 + r, &st);
    driver->ResetWorkerThreads();  // Reset before each round because BenchMixed spawns a new worker set.
    double t = PrintFunctionTime([&]() { BenchMixed(driver, ops, wt); },
                                 "MixedRound");
    total_ms += t;
    std::cout << "Mixed Round " << r << ": " << st.txns() << " txns ("
              << st.reinserts << " reinserts, " << st.modifies
              << " modifies) in " << t << "ms" << std::endl;
  }
  std::cout << "Mixed Update Done" << std::endl;
  driver->FinishWrites();  // Finalize once after all write rounds, matching the existing write path.
  std::cout << "Mixed Total: " << g_ops_done.load() << " txns in " << total_ms
            << "ms, Throughput: " << std::fixed << std::setprecision(2)
            << g_ops_done.load() / (total_ms / 1000.0) << " txns/s"
            << std::endl;
}

void RunAlgorithmTask(GraphDriver* driver, const std::string& algo, int threads,
                      unsigned root, S_Driver* global_counter = nullptr) {
  if (algo == "pr") {
    driver->RunPageRank(10, 0.85, global_counter);
  } else if (algo == "bfs") {
    driver->RunBFS(Arg.root, global_counter);
  } else if (algo == "sssp") {
    driver->RunSSSP(Arg.root, global_counter);
  } else {
    std::cerr << "Unknown algorithm: " << algo << "\n";
  }
}

void ConfigureTeseoChurnReadTask(int task_id, const std::string& algorithm,
                                 int requested_threads) {
  omp_set_dynamic(0);
  omp_set_num_threads(requested_threads);
  std::lock_guard<std::mutex> lock(g_hotset_log_mutex);
  std::cout << "TeseoChurn Read Task Config: task=" << task_id
            << " algo=" << algorithm
            << " requested_threads=" << requested_threads
            << " omp_max_threads=" << omp_get_max_threads()
            << " omp_dynamic=" << omp_get_dynamic() << std::endl;
}

struct GraphlogRunResult {
  uint64_t operations = 0;
  double elapsed_ms = 0.0;
};

GraphlogRunResult ReplayGraphlog(GraphDriver* driver, int thread_count) {
  std::vector<std::vector<const bench::GraphlogOperation*>> worker_operations(
      thread_count);
  for (const auto& operation : graphlog_workload.operations) {
    size_t owner = std::hash<uint64_t>{}(operation.source +
                                         operation.destination) %
                   thread_count;
    worker_operations[owner].push_back(&operation);
  }

  g_ops_done.store(0, std::memory_order_release);
  auto start = std::chrono::high_resolution_clock::now();
  std::vector<std::thread> workers;
  for (int thread_id = 0; thread_id < thread_count; ++thread_id) {
    workers.emplace_back([&, thread_id]() {
      driver->RegisterThread(thread_id);
      for (const auto* operation : worker_operations[thread_id]) {
        if (operation->weight >= 0.0) {
          uint64_t property;
          std::memcpy(&property, &operation->weight, sizeof(property));
          driver->UpsertEdge(operation->source, operation->destination,
                             property, thread_id);
        } else {
          driver->DeleteEdge(operation->source, operation->destination,
                             thread_id);
        }
        g_ops_done.fetch_add(1, std::memory_order_release);
      }
      driver->DeregisterThread(thread_id);
    });
  }
  for (auto& worker : workers) worker.join();
  auto end = std::chrono::high_resolution_clock::now();

  GraphlogRunResult result;
  result.operations = g_ops_done.load(std::memory_order_acquire);
  result.elapsed_ms =
      std::chrono::duration<double, std::milli>(end - start).count();
  return result;
}

void ConcurrentGraphlogTest(GraphDriver* driver) {
  const int write_threads = Arg.Spruce_wt;
  const int read_threads = Arg.read_thread;
  if (driver->Name() != "Teseo" || Arg.read_tasks.size() != 1 ||
      write_threads <= 0 || read_threads <= 0) {
    std::cerr << "--graphlog requires --system teseo, positive thread counts, "
                 "and exactly one read task.\n";
    return;
  }
  if (Arg.permute_) {
    std::cerr << "--graphlog cannot be combined with -p; operation order is "
                 "defined by the log.\n";
    return;
  }

  std::cout << "========================================\n";
  std::cout << "Starting Concurrent Graphlog Benchmark\n";
  std::cout << "Write Threads: " << write_threads
            << ", Read Threads: " << read_threads << "\n";
  std::cout << "Read Task: " << Arg.read_tasks[0] << "\n";
  std::cout << "Read Schedule: "
            << (Arg.reference_schedule_ ? "reference-inline"
                                        : "async-per-task")
            << "\n";
  std::cout << "Graphlog Config: operations="
            << graphlog_workload.operations.size()
            << " final_edges=" << graphlog_workload.final_edges
            << " read_window=10%-90%\n";
  std::cout << "========================================\n";

  std::atomic<bool> writer_finished{false};
  auto benchmark_start = std::chrono::high_resolution_clock::now();
  auto writer_future = std::async(std::launch::async, [&]() {
    GraphlogRunResult result = ReplayGraphlog(driver, write_threads);
    writer_finished.store(true, std::memory_order_release);
    return result;
  });

  long long total_time = 0;
  int execution_count = 0;
  S_Driver global_counter;
  const uint64_t operation_count = graphlog_workload.operations.size();
  const uint64_t read_start_threshold = operation_count / 10;
  const uint64_t read_stop_threshold = operation_count * 9 / 10;

  auto read_loop = [&]() {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    while (!writer_finished.load(std::memory_order_acquire) &&
           g_ops_done.load(std::memory_order_acquire) <=
               read_start_threshold) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    omp_set_num_threads(read_threads);
    S_Driver local_counter;
    while (!writer_finished.load(std::memory_order_acquire) &&
           g_ops_done.load(std::memory_order_acquire) < read_stop_threshold) {
      uint64_t progress_start = g_ops_done.load(std::memory_order_acquire);
      auto start = std::chrono::high_resolution_clock::now();
      local_counter.reset();
      RunAlgorithmTask(driver, Arg.read_tasks[0], read_threads, Arg.root,
                       &local_counter);
      auto end = std::chrono::high_resolution_clock::now();
      uint64_t progress_end = g_ops_done.load(std::memory_order_acquire);
      long long elapsed_us =
          std::chrono::duration_cast<std::chrono::microseconds>(end - start)
              .count();
      total_time += elapsed_us;
      execution_count++;
      global_counter += local_counter;
      std::cout << "Graphlog Read: algo=" << Arg.read_tasks[0]
                << " execution=" << execution_count
                << " progress_start=" << progress_start
                << " progress_end=" << progress_end << " elapsed_ms="
                << std::fixed << std::setprecision(3) << elapsed_us / 1000.0
                << std::defaultfloat << "\n";
    }
  };

  std::future<void> reader_future;
  if (Arg.reference_schedule_) {
    read_loop();
  } else {
    reader_future = std::async(std::launch::async, read_loop);
  }

  GraphlogRunResult write_result = writer_future.get();
  auto writer_end = std::chrono::high_resolution_clock::now();
  if (reader_future.valid()) reader_future.get();
  driver->FinishWrites();

  double wall_seconds =
      std::chrono::duration<double>(writer_end - benchmark_start).count();
  double throughput = write_result.elapsed_ms > 0.0
                          ? write_result.operations /
                                (write_result.elapsed_ms / 1000.0)
                          : 0.0;
  double average_ms = execution_count > 0
                          ? (total_time / execution_count) / 1000.0
                          : 0.0;
  std::cout << "Graphlog Update Done: " << write_result.operations
            << " operations in " << write_result.elapsed_ms
            << "ms, Throughput: " << std::fixed << std::setprecision(2)
            << throughput << " ops/s\n";
  std::cout << "Concurrent Benchmark Total Time: " << wall_seconds << " s\n";
  std::cout << "Algorithm [" << Arg.read_tasks[0] << "]: " << average_ms
            << " ms avg (" << execution_count << " executions)\n"
            << std::defaultfloat;
}

void ConcurrentHotSetTest(GraphDriver* driver) {
  const int requested_write_threads = Arg.Spruce_wt;
  const int total_read_threads = Arg.read_thread;
  const int num_tasks = Arg.read_tasks.size();
  uint64_t rounds = Arg.rounds_explicit_
                        ? static_cast<uint64_t>(Arg.rounds_)
                        : (Arg.hotset_ > 0.0
                               ? bench::DefaultHotSetRounds(Arg.hotset_)
                               : 0);

  if (num_tasks == 0) {
    std::cerr << "No read tasks specified. Use --tasks to specify algorithms.\n";
    return;
  }
  if (Arg.reference_schedule_ &&
      (driver->Name() != "Teseo" || num_tasks != 1)) {
    std::cerr << "--reference-schedule requires --system teseo and exactly "
                 "one read task.\n";
    return;
  }
  if (requested_write_threads <= 0 || total_read_threads <= 0 ||
      total_read_threads < num_tasks ||
      requested_write_threads + total_read_threads > 64) {
    std::cerr << "HotSet concurrent mode requires positive thread counts, "
                 "read threads >= task count, and write+read <= 64.\n";
    return;
  }

  const int threads_per_task = total_read_threads / num_tasks;
  if (total_read_threads % num_tasks != 0) {
    std::cout << "[Warning] Read threads (" << total_read_threads
              << ") not perfectly divisible by tasks (" << num_tasks
              << ").\n";
  }

  std::cout << "========================================\n";
  std::cout << "Starting Concurrent HotSet Benchmark\n";
  std::cout << "Write Threads: " << requested_write_threads
            << ", Read Threads: " << total_read_threads << "\n";
  std::cout << "Read Tasks: " << num_tasks << " (" << threads_per_task
            << " threads per task)\n";
  std::cout << "Read Schedule: "
            << (Arg.reference_schedule_ ? "reference-inline"
                                        : "async-per-task")
            << "\n";
  std::cout << "========================================\n";

  std::cout << "HotSet Load Start" << std::endl;
  driver->ResetWorkerThreads();
  driver->BeginBulkLoad();
  PrintFunctionTime(
      [&]() {
        BenchWrite(driver, requested_write_threads, 1, true, false);
      },
      "HotSetLoad");
  driver->EndBulkLoad();

  bench::HotSetInfo hotset;
  const size_t graph_edge_count =
      update_tuple.size();  // Original edge count E, captured before LoadHotSet.
  if (!Arg.hotset_file_.empty()) {
    // Load the candidate hot-edge file after HotsetLoad inserts the complete graph,
    // then rebuild update_tuple to contain only candidate edges.
    hotset = bench::LoadHotSetFile(update_tuple, Arg.hotset_file_);
    std::cout << "HotSet mode: vertex-degree (file=" << Arg.hotset_file_
              << ")\n";
    if (!Arg.rounds_explicit_) {
      // Default budget: 5*|E| updates; rounds = ceil(5E / candidate_edges).
      rounds = static_cast<uint64_t>(std::ceil(
          5.0L * static_cast<long double>(graph_edge_count) /
          static_cast<long double>(hotset.edge_count)));
    } else {
      rounds = static_cast<uint64_t>(Arg.rounds_);
    }
  } else {
    hotset = bench::SelectHotSet(update_tuple, Arg.hotset_, Arg.wseed_);
  }
  const int write_threads = std::min<size_t>(requested_write_threads,
                                             hotset.edge_count);
  if (write_threads != requested_write_threads) {
    std::cout << "[Warning] Hot edge count is smaller than write threads; "
                 "using "
              << write_threads << " writer threads.\n";
  }
  if (Arg.hotset_update_pct_ < 100) {
    try {
      bench::MakeHotSetMixedPlan(hotset.edge_count, write_threads,
                                 Arg.hotset_update_pct_, Arg.wseed_, 1);
    } catch (const std::invalid_argument& error) {
      std::cerr << error.what() << '\n';
      std::exit(2);
    }
  }

  driver->ResetWorkerThreads();
  if (driver->Name() == "GTX") {
    driver->SetWRThread(write_threads, total_read_threads);
  }

  std::cout << "HotSet Config: fraction=" << std::setprecision(8)
            << Arg.hotset_ << " hot_edges=" << hotset.edge_count
            << " graph_edges=" << update_tuple.size() << " rounds=" << rounds
            << " seed=" << Arg.wseed_ << " checksum=0x" << std::hex
            << hotset.checksum << std::dec << std::defaultfloat << std::endl;
  std::cout << "Graph loading done." << std::endl;

  g_ops_done.store(0);
  g_hotset_round_done.store(0);
  g_snap.writer_phase.store(0, std::memory_order_release);
  g_snap.num_tasks.store(num_tasks, std::memory_order_release);
  for (int i = 0; i < num_tasks && i < 16; ++i) {
    g_snap.reader_started[i].store(0, std::memory_order_relaxed);
    g_snap.reader_completed[i].store(0, std::memory_order_relaxed);
    g_snap.reader_time_us[i].store(0, std::memory_order_relaxed);
    g_reader_algo[i] = Arg.read_tasks[i].c_str();
  }
  std::atomic<bool> finish{false};
  std::atomic<bool> readers_started{false};
  std::atomic<int> writers_ready{0};
  std::vector<long long> total_time(num_tasks, 0);
  std::vector<int> execution_count(num_tasks, 0);
  std::vector<S_Driver> global_counter(num_tasks);
  // g_ops_done tracks only writers. Readers, particularly concurrent PR tasks,
  // may take much longer to finish; stop writer-stall checks once writes end.
  auto hotset_heartbeat_active = std::make_shared<std::atomic<bool>>(true);

  // A driver has one Graphalytics last-result buffer, while this workload can
  // run several readers concurrently. Hot-set reports timing/counters only;
  // suppress result materialisation to avoid racing on that shared buffer.
  const bool previous_result_collection =
      driver->SetAlgorithmResultCollectionEnabled(false);

  StartHeartbeat(Arg.stall_abort_, hotset_heartbeat_active);
  g_snap.writer_phase.store(1, std::memory_order_release);  // HotSet writes are in progress.
  std::cout << "HotSet Update Start" << std::endl;
  auto concurrent_start = std::chrono::steady_clock::now();
  std::thread progress_thread = StartProgressFile(concurrent_start, finish);
  auto writer_future = std::async(std::launch::async, [&]() {
    return BenchHotSetConcurrent(driver, hotset.edge_count, write_threads,
                                 rounds, writers_ready, readers_started);
  });

  while (writers_ready.load(std::memory_order_acquire) < write_threads) {
    std::this_thread::yield();
  }

  auto run_read_once = [&](int task_id, const std::string& algorithm) {
    uint64_t round_start =
        g_hotset_round_done.load(std::memory_order_acquire);
    auto read_start = std::chrono::high_resolution_clock::now();
    g_snap.reader_started[task_id]++;
    S_Driver local_counter;
    local_counter.reset();
    RunAlgorithmTask(driver, algorithm, threads_per_task, Arg.root,
                     &local_counter);
    g_snap.reader_completed[task_id]++;
    auto read_end = std::chrono::high_resolution_clock::now();
    uint64_t round_end =
        g_hotset_round_done.load(std::memory_order_acquire);
    long long elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(read_end -
                                                              read_start)
            .count();
    total_time[task_id] += elapsed_us;
    execution_count[task_id]++;
    global_counter[task_id] += local_counter;
    g_snap.reader_time_us[task_id].store(
        total_time[task_id], std::memory_order_relaxed);

    // std::ostringstream read_log;
    // read_log << "HotSet Read: task=" << task_id << " algo=" << algorithm
    //          << " execution=" << execution_count[task_id]
    //          << " round_start=" << round_start << " round_end=" << round_end
    //          << " elapsed_ms=" << std::fixed << std::setprecision(3)
    //          << elapsed_us / 1000.0;
    // std::lock_guard<std::mutex> lock(g_hotset_log_mutex);
    // std::cout << read_log.str() << std::endl;
  };

  std::vector<std::future<void>> reader_futures;
  if (!Arg.reference_schedule_) {
    for (int task_id = 0; task_id < num_tasks; ++task_id) {
      std::string algorithm = Arg.read_tasks[task_id];
      reader_futures.push_back(std::async(
          std::launch::async,
          [&, task_id, algorithm]() {
            bool registered = driver->Name() == "AVB" ||
                              driver->Name() == "Sortledton";
            if (registered) driver->RegisterThread(task_id + write_threads);
            omp_set_num_threads(threads_per_task);
            while (!readers_started.load(std::memory_order_acquire)) {
              std::this_thread::yield();
            }
            while (!finish.load(std::memory_order_acquire)) {
              run_read_once(task_id, algorithm);
            }
            if (registered) driver->DeregisterThread(task_id + write_threads);
          }));
    }
  }

  readers_started.store(true, std::memory_order_release);
  g_snap.writer_phase.store(2, std::memory_order_release);  // Readers start concurrently with writers.
  if (Arg.reference_schedule_) {
    omp_set_num_threads(threads_per_task);
    while (writer_future.wait_for(std::chrono::seconds(0)) !=
           std::future_status::ready) {
      run_read_once(0, Arg.read_tasks[0]);
    }
  }
  HotSetRunResult write_result = writer_future.get();
  std::cout << "HotSet Writer Done" << std::endl;
  auto writer_end = std::chrono::steady_clock::now();
  g_snap.writer_phase.store(3, std::memory_order_release);  // Writes are complete; wait for readers to finish.
  hotset_heartbeat_active->store(false, std::memory_order_release);
  finish.store(true, std::memory_order_release);
  for (auto& future : reader_futures) future.get();
  if (progress_thread.joinable()) progress_thread.join();
  driver->FinishWrites();
  driver->SetAlgorithmResultCollectionEnabled(previous_result_collection);

  std::chrono::duration<double> concurrent_duration =
      writer_end - concurrent_start;
  double update_throughput =
      write_result.total_ms > 0.0
          ? write_result.txns / (write_result.total_ms / 1000.0)
          : 0.0;
  double wall_throughput = concurrent_duration.count() > 0.0
                               ? write_result.txns / concurrent_duration.count()
                               : 0.0;

  std::cout << "HotSet Update Done" << std::endl;
  std::cout << "HotSet Total: " << write_result.txns << " txns in "
            << write_result.total_ms << "ms, Throughput: " << std::fixed
            << std::setprecision(2) << update_throughput << " txns/s"
            << std::defaultfloat << std::endl;
  if (Arg.hotset_update_pct_ < 100) {
    std::cout << "HotSet Mix: update_pct=" << Arg.hotset_update_pct_
              << " updates=" << write_result.updates
              << " deletes=" << write_result.deletes
              << " inserts=" << write_result.inserts << std::endl;
  }
  std::cout << "Concurrent Benchmark Total Time: "
            << concurrent_duration.count() << " s\n";
  std::cout << "Write Throughput: " << std::fixed << std::setprecision(2)
            << wall_throughput << " ops\n" << std::defaultfloat;

  std::map<std::string, long long> algorithm_total_time;
  std::map<std::string, long long> algorithm_total_record;
  std::map<std::string, long long> algorithm_total_visible;
  std::map<std::string, int> algorithm_execution_count;
  for (int task_id = 0; task_id < num_tasks; ++task_id) {
    const std::string& algorithm = Arg.read_tasks[task_id];
    algorithm_total_time[algorithm] += total_time[task_id];
    algorithm_execution_count[algorithm] += execution_count[task_id];
    algorithm_total_record[algorithm] += global_counter[task_id].record_cnt;
    algorithm_total_visible[algorithm] +=
        global_counter[task_id].visible_record_cnt;
  }

  std::cout << "\n========== Algorithm Summary ==========\n";
  for (const auto& [algorithm, total] : algorithm_total_time) {
    int count = algorithm_execution_count[algorithm];
    double average_ms = count > 0 ? (total / count) / 1000.0 : 0.0;
    std::cout << "Algorithm [" << algorithm << "]: " << average_ms
              << " ms avg (" << count << " executions)\n";
    std::cout << "Algorithm [" << algorithm << "]: "
              << algorithm_total_record[algorithm] << " records ("
              << algorithm_total_visible[algorithm] << " visible)\n";
  }
  std::cout << "========================================\n";
}

void ConcurrentTest(GraphDriver* driver) {
  if (Arg.hotset_ > 0.0 || !Arg.hotset_file_.empty()) {
    ConcurrentHotSetTest(driver);
    return;
  }

  int write_threads = Arg.Spruce_wt;         // For example, 32 writers.
  int total_read_threads = Arg.read_thread;  // For example, 32 total reader threads.
  int repeat_count = Arg.repeat_count;       // Number of input passes, for example 10.

  const bool use_teseo_churn = Arg.teseo_churn_;
  if (use_teseo_churn) {
    if (driver->Name() != "Teseo" || update_tuple.empty()) {
      std::cerr << "--teseo-churn requires --system teseo and a non-empty "
                   "input graph.\n";
      return;
    }
  }

  const bool use_replacement_churn = Arg.churn_;
  const bool use_split_update = Arg.split_update_;
  const bool use_split_write = use_replacement_churn || use_split_update;
  std::vector<bench::ChurnOp> churn_operations;
  if (use_split_write) {
    if (driver->Name() != "Teseo" || repeat_count != 2 ||
        update_tuple.size() > 1'000'000) {
      std::cerr << "--churn/--split-update require --system teseo, --repeat "
                   "2, and at most 1,000,000 input edges.\n";
      return;
    }
    try {
      if (use_replacement_churn) {
        churn_operations =
            bench::GenerateGraphlogLikeChurn(update_tuple, Arg.wseed_);
      } else {
        churn_operations.reserve(update_tuple.size());
        for (const auto& [source, destination, weight] : update_tuple) {
          churn_operations.push_back(
              {source, destination, source, destination, weight});
        }
      }
    } catch (const std::exception& error) {
      std::cerr << "Split-write generation failed: " << error.what() << "\n";
      return;
    }
  }

  assert(write_threads + total_read_threads <= 64);
  std::cout << write_threads << " " << total_read_threads << '\n';

  if (Arg.system_ == "GTX" || Arg.system_ == "gtx") {
    driver->SetWRThread(write_threads, total_read_threads);
  }

  // Distribute reader threads among tasks.
  int num_tasks = Arg.read_tasks.size();
  if (num_tasks == 0) {
    std::cerr
        << "No read tasks specified. Use --tasks to specify algorithms.\n";
    return;
  }
  g_snap.num_tasks = num_tasks;
  g_snap.writer_phase.store(0, std::memory_order_release);
  for (int i = 0; i < num_tasks && i < 16; ++i) {
    g_reader_algo[i] = Arg.read_tasks[i].c_str();
    g_snap.reader_started[i].store(0, std::memory_order_relaxed);
    g_snap.reader_completed[i].store(0, std::memory_order_relaxed);
  }
  if (Arg.reference_schedule_ &&
      (driver->Name() != "Teseo" || num_tasks != 1)) {
    std::cerr << "--reference-schedule requires --system teseo and exactly "
                 "one read task.\n";
    return;
  }

  // Require a task count that divides the available reader threads.
  int threads_per_task = total_read_threads / num_tasks;
  if (total_read_threads % num_tasks != 0) {
    std::cout << "[Warning] Read threads (" << total_read_threads
              << ") not perfectly divisible by tasks (" << num_tasks << ").\n";
  }

  std::cout << "========================================\n";
  std::cout << "Starting Concurrent Read/Write Benchmark\n";
  std::cout << "Write Threads: " << write_threads
            << ", Data amplification: " << repeat_count << "x\n";
  std::cout << "Read Tasks: " << num_tasks << " (" << threads_per_task
            << " threads per task)\n";
  std::cout << "Read Schedule: "
            << (Arg.reference_schedule_ ? "reference-inline"
                                        : "async-per-task")
            << "\n";
  if (use_teseo_churn) {
    std::cout << "Update Mode: teseo-graphlog-like-churn\n";
    std::cout << "TeseoChurn Config: rounds=" << Arg.rounds_
              << " logical_updates_per_round=" << update_tuple.size()
              << " logical_updates_total="
              << static_cast<uint64_t>(Arg.rounds_) * update_tuple.size()
              << " successful_low_level_transactions_total="
              << static_cast<uint64_t>(Arg.rounds_) * update_tuple.size() * 2
              << " generation=prevalidated-graphlog-rounds"
              << " order=seeded-shuffle"
              << " seed=" << Arg.wseed_ << "\n";
  }
  if (use_split_write) {
    std::cout << "Update Mode: "
              << (use_replacement_churn ? "graphlog-like-churn"
                                         : "split-same-edge")
              << "\n";
    std::cout << "Churn Config: replacements=" << churn_operations.size()
              << " low_level_transactions=" << churn_operations.size() * 2
              << " seed=" << Arg.wseed_ << "\n";
  }
  std::cout << "========================================\n";

  // Use futures to collect return values/exceptions and timings in the main thread.
  std::vector<std::future<void>> futures;
  TeseoChurnRunResult teseo_churn_result;

  one_finished = 0;

  std::atomic<bool> finish(false);
  long long total_time[10];  // Cumulative execution time in microseconds.
  int execution_count[10];   // Number of completed executions.
  S_Driver global_counter[10];

  for (int i = 0; i < num_tasks; i++) total_time[i] = execution_count[i] = 0;

  // A driver has one Graphalytics last-result buffer, while this workload can
  // run several readers concurrently. Non-hotset readers each rebuild the
  // shared last_result_ on every algorithm execution; that is a data race and
  // deterministically SIGSEGVs Teseo at pr×2 (double-free of the result
  // vector). Same suppression the HotSet path applies above.
  const bool previous_result_collection =
      driver->SetAlgorithmResultCollectionEnabled(false);

  // 1. Start concurrent writes.
  if (Arg.system_ != "rs" && Arg.system_ != "RS") {
    g_snap.writer_phase = 1;  // First loading pass.
    futures.push_back(std::async(std::launch::async, [&]() {
      if (use_teseo_churn) {
        PrintFunctionTime(
            [&]() {
              BenchWrite(driver, write_threads, 1, true, false);
              std::cout << "TeseoChurn Update Start\n";
              teseo_churn_result = BenchTeseoChurn(
                  driver, write_threads, Arg.rounds_);
              driver->FinishWrites();
              std::cout << "TeseoChurn Update Done\n";
            },
            "Concurrent TeseoChurn Write");
      } else if (use_split_write) {
        PrintFunctionTime(
            [&]() {
              BenchWrite(driver, write_threads, 1, true, false);
              BenchChurn(driver, churn_operations, write_threads);
              driver->FinishWrites();
            },
            "Concurrent Churn Write");
      } else {
        PrintFunctionTime(
            [&]() { BenchWrite(driver, write_threads, repeat_count); },
            "Concurrent Write");
      }
      g_snap.writer_phase = 3;  // Writes are complete.
      finish = true;
    }));

    while (!one_finished);
    // Reset after loading so g_ops_done counts only the repeat_count-1 concurrent passes,
    // matching write_transactions = update_tuple.size()*(repeat_count-1).
    g_ops_done.store(0, std::memory_order_release);
    g_snap.writer_phase = 2;  // Loading is complete and readers have been released.
    std::cout << "Graph loading done.\n" << std::endl;
  }

  else {
    g_snap.writer_phase = 1;  // First loading pass.
    BenchWrite(driver, write_threads, 1);
    one_finished = true;
    g_snap.writer_phase = 2;  // First pass is complete.
    std::cout << "Graph loading done.\n" << std::endl;

    // driver->RunPageRank(10, 0.85);

    futures.push_back(std::async(std::launch::async, [&]() {
      PrintFunctionTime(
          [&]() { BenchWrite(driver, write_threads, repeat_count - 1, false); },
          "Concurrent Write");
      g_snap.writer_phase = 3;  // Writes are complete.
      finish = true;
    }));
  }

  auto start_time = std::chrono::steady_clock::now();
  // The background sidecar records write progress for partial results after a crash or hang.
  // num_tasks and g_reader_algo have already been initialized above.
  std::thread progress_thread = StartProgressFile(start_time, finish);
  // 2. Start concurrent reader tasks.
  if (Arg.reference_schedule_) {
    if (use_teseo_churn) {
      ConfigureTeseoChurnReadTask(0, Arg.read_tasks[0], threads_per_task);
    } else {
      omp_set_num_threads(threads_per_task);
    }
    S_Driver local_counter;
    while (!finish.load(std::memory_order_acquire)) {
      const std::string& algo = Arg.read_tasks[0];
      g_snap.reader_started[0]++;  // Mark the start of an algorithm execution for task 0.
      if (!Arg.quiet_) std::cout << "Starting Task 0 (" << algo << ")\n";
      auto start = std::chrono::high_resolution_clock::now();
      local_counter.reset();
      RunAlgorithmTask(driver, algo, threads_per_task, Arg.root,
                       &local_counter);
      g_snap.reader_completed[0]++;
      auto end = std::chrono::high_resolution_clock::now();
      auto duration =
          std::chrono::duration_cast<std::chrono::microseconds>(end - start)
              .count();
      total_time[0] += duration;
      g_snap.reader_time_us[0].store(total_time[0], std::memory_order_relaxed);
      execution_count[0]++;
      global_counter[0] += local_counter;
      if (!Arg.quiet_)
        std::cout << "Task 0: " << execution_count[0] << " execution with "
                  << total_time[0] / 1000.0 << "ms\n";
    }
  } else {
    for (int i = 0; i < num_tasks; ++i) {
      std::string algo = Arg.read_tasks[i];
      futures.push_back(std::async(
          std::launch::async,
          [driver, algo, threads_per_task, &Arg, i, &finish, &total_time,
           &execution_count, &global_counter, write_threads,
           use_teseo_churn]() {
          //   std::string log_name =
          //       "Task " + std::to_string(i) + " (" + algo + ")";
          //   PrintFunctionTime(
          if (driver->Name() == "Sortledton") {
            driver->RegisterThread(i + write_threads);
          }
          if (Arg.system_ == "AVB" || Arg.system_ == "avb") {
            driver->RegisterThread(i + write_threads);
          }
          if (use_teseo_churn) {
            ConfigureTeseoChurnReadTask(i, algo, threads_per_task);
          } else {
            omp_set_num_threads(threads_per_task);
          }
          S_Driver local_counter;
          while (!finish) {
            if (!Arg.quiet_)
              std::cout << "Starting Task " + std::to_string(i) + " (" + algo +
                               ")\n";
            auto start = std::chrono::high_resolution_clock::now();

            g_snap.reader_started[i]++;
            local_counter.reset();
            RunAlgorithmTask(driver, algo, threads_per_task, Arg.root,
                             &local_counter);
            g_snap.reader_completed[i]++;
            auto end = std::chrono::high_resolution_clock::now();
            auto duration =
                std::chrono::duration_cast<std::chrono::microseconds>(end -
                                                                      start)
                    .count();

            // Accumulate elapsed time and execution count.
            total_time[i] += duration;
            g_snap.reader_time_us[i].store(
                total_time[i], std::memory_order_relaxed);
            execution_count[i]++;
            if (!Arg.quiet_)
              std::cout << "Task " + std::to_string(i) + ": " +
                               std::to_string(execution_count[i]) +
                               " execution with " +
                               std::to_string(total_time[i] / 1000.0) + "ms\n";
            // global_counter[i] += local_counter;
            //  break;
          }
          //   log_name);
          }));
    }
  }

  // 3. Wait for all writers.
  futures[0].get();
  auto end_time = std::chrono::steady_clock::now();
  std::chrono::duration<double> diff = end_time - start_time;
  double times = diff.count();
  std::cout << "Concurrent Benchmark Total Time: " << diff.count() << " s\n";

  uint64_t write_transactions =
      use_teseo_churn
          ? teseo_churn_result.logical_updates * 2
          : (use_split_write ? churn_operations.size() * 2
                             : update_tuple.size() * (repeat_count - 1));
  std::cout << "Write Throughput: " << std::fixed << std::setprecision(2)
            << (use_teseo_churn && teseo_churn_result.replay_ms > 0.0
                    ? static_cast<double>(write_transactions) /
                          (teseo_churn_result.replay_ms / 1000.0)
                    : static_cast<double>(write_transactions) / times)
            << " ops\n";
  if (use_teseo_churn) {
    std::cout << "TeseoChurn Total: logical_updates="
              << teseo_churn_result.logical_updates
              << " successful_low_level_transactions=" << write_transactions
              << " graphlog_operations="
              << teseo_churn_result.graphlog_operations
              << " graphlog_checksum=" << teseo_churn_result.checksum
              << " order_checksum=" << teseo_churn_result.order_checksum
              << " plan_ms=" << teseo_churn_result.plan_ms
              << " shuffle_ms=" << teseo_churn_result.shuffle_ms
              << " replay_ms=" << teseo_churn_result.replay_ms
              << " completed=" << std::boolalpha << teseo_churn_result.completed
              << std::noboolalpha << "\n";
  }

  // 4. Wait for all readers.
  for (int i = 1; i < futures.size(); i++) {
    futures[i].get();
  }

  // The writer set finish; stop the progress thread (at most one 10-second interval).
  if (progress_thread.joinable()) progress_thread.join();

  // Aggregate statistics by algorithm.
  std::map<std::string, long long> algo_total_time;
  std::map<std::string, long long> algo_total_record;
  std::map<std::string, long long> algo_total_visible;
  std::map<std::string, int> algo_execution_count;

  for (int i = 0; i < num_tasks; i++) {
    std::string algo = Arg.read_tasks[i];
    algo_total_time[algo] += total_time[i];
    algo_execution_count[algo] += execution_count[i];
    algo_total_record[algo] += global_counter[i].record_cnt;
    algo_total_visible[algo] += global_counter[i].visible_record_cnt;
  }

  std::cout << "\n========== Algorithm Summary ==========\n";
  for (const auto& [algo, total] : algo_total_time) {
    int count = algo_execution_count[algo];
    double avg_ms = (count > 0) ? (total / count) / 1000.0 : 0.0;
    std::cout << "Algorithm [" << algo << "]: " << avg_ms << " ms avg ("
              << count << " executions)\n";
    std::cout << "Algorithm [" << algo << "]: " << algo_total_record[algo]
              << " records (" << algo_total_visible[algo] << " visible)\n";
  }
  std::cout << "========================================\n";

  // Restore the prior result-collection setting after the concurrent phase
  // (disabled above so concurrent readers don't race on last_result_).
  driver->SetAlgorithmResultCollectionEnabled(previous_result_collection);
}

// ================= Graphalytics-like static evaluation =================

// Structured timer: prints nothing, returns elapsed milliseconds.
static double MeasureMs(std::function<void()> fn) {
  auto start = std::chrono::high_resolution_clock::now();
  fn();
  auto end = std::chrono::high_resolution_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
      .count();
}

// Write a (vertex_id, value) result vector to a file in the standard
// Graphalytics output format: one <vertex_id> <value> per line, sorted.
static bool WriteResultToFile(const std::string& path,
                              const AlgorithmResult& result) {
  std::ofstream ofs(path);
  if (!ofs) {
    std::cerr << "[Graphalytics] Cannot open output file: " << path << "\n";
    return false;
  }
  if (result.type == AlgorithmResult::kDouble) {
    auto sorted = result.double_vals;
    std::sort(sorted.begin(), sorted.end());
    for (const auto& p : sorted) {
      ofs << p.first << " " << std::setprecision(15) << p.second << "\n";
    }
    std::cout << "[Graphalytics] Wrote " << sorted.size()
              << " results to " << path << "\n";
    return true;
  }
  if (result.type == AlgorithmResult::kLong) {
    auto sorted = result.long_vals;
    std::sort(sorted.begin(), sorted.end());
    for (const auto& p : sorted) ofs << p.first << " " << p.second << "\n";
    std::cout << "[Graphalytics] Wrote " << sorted.size()
              << " results to " << path << "\n";
    return true;
  }
  return false;
}

// Entry point for static (Graphalytics-like) evaluation.
// Reuses the existing driver, UpdateDataConcurrent, and BenchWrite paths.
// Returns 0 on success, nonzero on error.
static int RunGraphalyticsBenchmark(GraphDriver* driver,
    const std::vector<std::tuple<unsigned, unsigned, uint64_t>>& edges,
    const bench::Args& args) {
  if (args.algorithm_.empty()) {
    std::cerr << "[Graphalytics] No algorithm specified (--algorithm).\n";
    return 1;
  }
  std::vector<std::string> algorithms;
  if (args.algorithm_ == "all") {
    algorithms = {"pr", "bfs", "sssp", "wcc", "lcc", "cdlp"};
  } else if (args.algorithm_.find(',') != std::string::npos) {
    std::istringstream input(args.algorithm_);
    std::string algorithm;
    while (std::getline(input, algorithm, ',')) {
      if (!algorithm.empty()) algorithms.push_back(algorithm);
    }
    const std::vector<std::string> supported =
        {"pr", "bfs", "sssp", "wcc", "lcc", "cdlp"};
    if (algorithms.empty() ||
        std::any_of(algorithms.begin(), algorithms.end(), [&](const auto& algo) {
          return std::find(supported.begin(), supported.end(), algo) ==
              supported.end();
        })) {
      std::cerr << "[Graphalytics] Invalid comma-separated algorithm list: "
                << args.algorithm_ << "\n";
      return 1;
    }
  } else {
    algorithms = {args.algorithm_};
  }
  const std::string algorithm_label = args.algorithm_;

  // --no-collect-results: keep processing_ms to pure algorithm time. All drivers
  // skip last_result_ materialization via ShouldCollectAlgorithmResults(), so the
  // result-file/correctness paths below must also be gated on the same flag.
  driver->SetAlgorithmResultCollectionEnabled(args.collect_results_);

  const std::string root_field = std::to_string(args.root);

  // 1. Check capability. A directed request must never silently use the
  // historical bidirectional loader of a driver that cannot preserve it.
  for (const auto& algo : algorithms) {
    if (!driver->SupportsAlgorithm(algo)) {
      std::cout << "GRAPHALYTICS_RESULT system=" << driver->Name()
                << " algorithm=" << algo
                << " root=" << root_field
                << " edge_mode=" << args.edge_mode_
                << " status=unsupported"
                << " reason=algorithm-not-implemented\n";
      return args.algorithm_ == "all" ? 1 : 0;
    }
  }
  if (args.edge_mode_ == "directed" && !driver->SupportsDirected()) {
    for (const auto& algo : algorithms) {
      std::cout << "GRAPHALYTICS_RESULT system=" << driver->Name()
                << " algorithm=" << algo
                << " root=" << root_field
                << " edge_mode=directed"
                << " status=unsupported-directed"
                << " reason=driver-direction-unsupported\n";
    }
    return 0;
  }

  // 2. Load graph via existing edge insertion
  std::cout << "[Graphalytics] System=" << driver->Name()
            << " Graph=(" << edges.size() << " edges)"
            << " Algorithm=" << algorithm_label
            << " EdgeMode=" << args.edge_mode_
            << " Repetitions=" << args.repetitions_ << "\n";

  bool load_finalized = true;
  double load_ms = MeasureMs([&]() {
    driver->BeginBulkLoad();
    BenchWrite(driver, args.Spruce_wt, 1, true, false);
    driver->EndBulkLoad();
    load_finalized = driver->FinalizeStaticLoad();
  });
  if (!load_finalized) {
    std::cout << "GRAPHALYTICS_RESULT system=" << driver->Name()
              << " algorithm=" << algorithm_label
              << " root=" << root_field
              << " edge_mode=" << args.edge_mode_
              << " load_ms=" << std::fixed << std::setprecision(1) << load_ms
              << " status=load-finalize-failed"
              << " reason=static-load-not-visible\n";
    return 1;
  }

  // Match the ordinary path: configure GTX's algorithm worker after the
  // write/load phase, not before its transaction table has been populated.
  if (driver->Name() == "GTX") driver->SetWorkThread(args.read_thread);
  // Static algorithms use OpenMP internally. Keep the OpenMP worker count in
  // sync with the requested reader count; GTX has a fixed 64-slot worker table
  // and otherwise the machine's default OpenMP pool can exhaust it.
  omp_set_num_threads(args.read_thread);

  // Optional profiling pause used to isolate the read algorithm from loading.
  // It is disabled by default and only applies when explicitly requested.
  // 3. Execute algorithm with repetitions
  // Each repetition is a fresh call; the driver is responsible for any
  // internal state reset needed between repetitions.
  for (const auto& algo : algorithms) {
    if (std::getenv("AVB_PROFILE_PAUSE") != nullptr &&
        driver->Name() == "AVB" && algo == "bfs") {
      std::cerr << "[profile] ready before BFS, pid=" << getpid() << "\n";
      std::cerr.flush();
      std::this_thread::sleep_for(std::chrono::seconds(10));
    }
    for (int r = 0; r < args.repetitions_; r++) {
    // We track processing time (what the algorithm call took) and makespan
    // (wall clock including runner overhead — for single-algorithm jobs
    // these are the same).
    double processing_ms = 0.0;
    double makespan_ms = 0.0;
    std::string status = "ok";
    std::string reason = "";

    // Timed run; repetitions count only measured executions.

    makespan_ms = MeasureMs([&]() {
      processing_ms = MeasureMs([&]() {
        const bool common_allowed =
            args.graphalytics_ && args.common_algorithms_ &&
            driver->SupportsCommonAlgorithm(algo);
        if (algo == "pr") {
          if (common_allowed) {
            if (!driver->RunPageRankCommon(args.pr_iterations_, args.pr_damping_)) {
              status = "algorithm-failed";
              reason = "common-pr-failed";
            }
          } else {
            driver->RunPageRank(args.pr_iterations_, args.pr_damping_);
          }
        } else if (algo == "bfs") {
          if (common_allowed) {
            if (!driver->RunBFSCommon(args.root)) {
              status = "algorithm-failed";
              reason = "common-bfs-failed";
            }
          } else {
            driver->RunBFS(args.root);
          }
        } else if (algo == "sssp") {
          if (common_allowed) {
            if (!driver->RunSSSPCommon(args.root)) {
              status = "algorithm-failed";
              reason = "common-sssp-failed";
            }
          } else {
            driver->RunSSSP(args.root);
          }
        } else if (algo == "wcc") {
          if (common_allowed) {
            if (!driver->RunWCCCommon()) {
              status = "algorithm-failed";
              reason = "common-wcc-failed";
            }
          } else {
            driver->RunWCC();
          }
        } else if (algo == "lcc") {
          if (args.graphalytics_ && args.common_algorithms_ &&
              driver->SupportsCommonAlgorithm(algo)) {
            if (!driver->RunLCCCommon()) {
              status = "algorithm-failed";
              reason = "common-lcc-failed";
            }
          } else {
            driver->RunLCC();
          }
        } else if (algo == "cdlp") {
          if (args.graphalytics_ && args.common_algorithms_ &&
              driver->SupportsCommonAlgorithm(algo)) {
            if (!driver->RunCDLPCommon(args.cdlp_iterations_)) {
              status = "algorithm-failed";
              reason = "common-cdlp-failed";
            }
          } else {
            driver->RunCDLP(args.cdlp_iterations_);
          }
        } else {
          status = "unsupported";
          reason = "unknown-algorithm";
        }
      });
    });

    // A driver can reject an invalid source without throwing. Do not emit an
    // apparently successful static result when no algorithm result exists,
    // even when the caller did not request an output file.
    // 3. Validate result existence. Only meaningful when result collection is
    // enabled: under --no-collect-results last_result_ stays kNone by design.
    if (args.collect_results_ && status == "ok" &&
        driver->GetLastResult().type == AlgorithmResult::kNone) {
      status = "result-unavailable";
      reason = "driver-did-not-expose-result";
    }

    // 4. Write result if requested. A missing result is a distinct status,
    // not a successful validation result.
    if (args.collect_results_ && !args.output_file_.empty() && status == "ok") {
      std::string output_path = args.output_file_;
      if (algorithms.size() > 1) output_path += "." + algo;
      if (args.repetitions_ > 1) {
        output_path += ".rep" + std::to_string(r + 1);
      }
      if (!WriteResultToFile(output_path, driver->GetLastResult())) {
        status = "result-unavailable";
        reason = "driver-did-not-expose-result";
      }
    }

    std::cout << "GRAPHALYTICS_RESULT"
              << " system=" << driver->Name()
              << " algorithm=" << algo
              << " repetition=" << (r + 1)
              << " root=" << root_field
              << " edge_mode=" << args.edge_mode_
              << " load_ms=" << std::fixed << std::setprecision(1) << load_ms
              << " processing_ms=" << std::fixed << std::setprecision(1) << processing_ms
              << " makespan_ms=" << std::fixed << std::setprecision(1) << makespan_ms
              << " status=" << status
              << (reason.empty() ? "" : " reason=" + reason)
              << "\n";
    }
  }
  return 0;
}

int main(int argc, char** argv) {
  // 1. Parse arguments.
  Arg.ParseArgs(argc, argv);

  // SIGTERM from timeout or SIGINT from Ctrl-C dumps writer/reader progress
  // to diagnose a hang. Normal timed execution does not trigger this output.
  {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = progress_dump_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
  }

  // 4. Load input data.
  if (Arg.graphlog_.empty()) {
    PrintFunctionTime(
        [&]() {
          bench::UpdateDataConcurrent(Arg.input_file, update_tuple,
                                      Arg.limit_edge_nums);
        },
        "Input Edge Data");
  } else {
    try {
      PrintFunctionTime(
          [&]() { graphlog_workload = bench::LoadGraphlog(Arg.graphlog_); },
          "Input Graphlog Data");
    } catch (const std::exception& error) {
      std::cerr << "Graphlog load failed: " << error.what() << "\n";
      return 1;
    }
  }

  if (Arg.limit_edge_nums > 0) {
    update_tuple.resize(Arg.limit_edge_nums);
  }

  if (Arg.graphalytics_ && !Arg.permute_explicit_) {
    Arg.permute_ = true;
    std::cout << "[Graphalytics] Permuting input edge order by default (-p)\n";
  }
  if (Arg.permute_) {
    permute(update_tuple.size(), update_tuple);
  }
  if (Arg.graphalytics_ && !Arg.root_explicit_ && !update_tuple.empty()) {
    Arg.root = static_cast<int>(std::get<0>(update_tuple.front()));
  }
  std::cout << "Finish Graph Data Loading..." << std::endl;
  // LoadData(Arg.input_file);

  // Select the backend requested by --system.
  std::unique_ptr<GraphDriver> driver;

  std::string sys = Arg.system_ == "" ? "AVB" : Arg.system_;
#ifndef ENABLE_GTX
  if (sys == "gtx" || sys == "GTX") {
    std::cerr << "GTX backend is not included in this checkout. Reconfigure "
                 "with -DENABLE_GTX=ON and provide systems/GTX.\n";
    return 2;
  }
#endif
#if !defined(USE_SLT)
  if (sys == "slt" || sys == "SLT" || sys == "Sortledton" ||
      sys == "sortledton") {
    std::cerr << "Sortledton backend is available only in the optional "
                 "bench_run_slt target. Reconfigure with "
                 "-DENABLE_SORTLEDTON=ON and provide systems/Sortledton.\n";
    return 2;
  }
#endif
  if (sys == "gtx" || sys == "GTX") {
#ifdef ENABLE_GTX
    driver = std::make_unique<GTXDriver>();
#endif
  } else if (sys == "lg" || sys == "livegraph" || sys == "LiveGraph") {
    driver = std::make_unique<LiveGraphDriver>();
  } else if (sys == "teseo" || sys == "Teseo" || sys == "TESEO") {
    driver = std::make_unique<TeseoDriver>();
  } else if (sys == "csr" || sys == "CSR") {
    driver = std::make_unique<CSRDriver>();
  }
#ifdef USE_SLT
  else if (sys == "slt" || sys == "SLT" || sys == "Sortledton" ||
           sys == "sortledton")
    driver = std::make_unique<SLTDriver>();
#else
  // driver = std::make_unique<AVBDriver>(); // Default backend.
  else if (sys == "rapidstore" || sys == "RapidStore" || sys == "RS" ||
           sys == "rs")
    driver = std::make_unique<RSDriver>();
  else
    driver = std::make_unique<AVBDriver>();  // Default backend.
#endif

  if (!driver) {
    std::cerr << "Backend " << sys << " is not available in this executable. "
                 "Use the runner listed in README.md.\n";
    return 2;
  }

  std::cout << "Target System: " << driver->Name() << std::endl;
  driver->Init(64);

  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  std::cout << "Finish System Initialization..." << std::endl;

  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  if (sys == "rapidstore" || sys == "RapidStore" || sys == "RS" ||
      sys == "rs") {
    for (int i = 0; i < update_tuple.size(); i++) {
      driver->InsertVertex(std::get<0>(update_tuple[i]));
      driver->InsertVertex(std::get<1>(update_tuple[i]));
    }
    std::cout << "RapidStore: Insert Vertex Done" << std::endl;
  }

  // Graphalytics-like static evaluation: early exit, bypasses all mixed/hotset/concurrent paths.
  if (Arg.graphalytics_) {
    return RunGraphalyticsBenchmark(driver.get(), update_tuple, Arg);
  }

  // return 0;
  if (Arg.con_wr_test) {
    // Run the concurrent read/write benchmark.
    std::cout << "Starting Concurrent Write Benchmark..." << std::endl;
    if (!Arg.graphlog_.empty())
      ConcurrentGraphlogTest(driver.get());
    else
      ConcurrentTest(driver.get());
    return 0;
  } else if (Arg.mix_ != "") {
    RunMixed(driver.get());  // Load, execute mixed rounds, then continue to the optional -a algorithm.
  } else {
  // 5. Run the write benchmark.
  std::cout << Arg.repeat_count << " times\n";
  std::cout << "Starting Write Benchmark..." << std::endl;

  if (Arg.system_ != "rs" && Arg.system_ != "RS") {
    double tim = PrintFunctionTime(
        [&]() { BenchWrite(driver.get(), Arg.Spruce_wt, Arg.repeat_count); },
        "Concurrent Write");

    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    std::cout << "Write Throughput: " << std::fixed << std::setprecision(2)
              << (double)(1.0) * update_tuple.size() / tim * 1000 << " ops\n";
  } else {
    double tim =
        PrintFunctionTime([&]() { BenchWrite(driver.get(), Arg.Spruce_wt, 1); },
                          "Concurrent Write");
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    std::cout << "Write Throughput: " << std::fixed << std::setprecision(2)
              << (double)(1.0) * update_tuple.size() / tim * 1000 << " ops\n";
    PrintFunctionTime(
        [&]() {
          BenchWrite(driver.get(), Arg.Spruce_wt, Arg.repeat_count - 1, false);
        },
        "Concurrent Write");
  };
  }  // end else: ordinary write path, without --mix or --con.

  if (Arg.algorithm_ != "") {
    if (Arg.system_ == "gtx" || Arg.system_ == "GTX") driver->SetWorkThread(Arg.read_thread);
    omp_set_num_threads(Arg.read_thread);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  // 6. Run reads or graph algorithms.
  if (Arg.algorithm_ == "pr") {
    std::cout << "Starting PageRank..." << std::endl;
    PrintFunctionTime([&]() { driver->RunPageRank(10, 0.85); }, "PageRank");
  } else if (Arg.algorithm_ == "bfs") {
    std::cout << "Starting BFS..." << std::endl;
    PrintFunctionTime(
        [&]() {
          driver->RunBFS(Arg.root);  // Use the root supplied through Args.
        },
        "BFS");
  } else if (Arg.algorithm_ == "sssp") {
    std::cout << "Starting SSSP..." << std::endl;
    PrintFunctionTime(
        [&]() {
          driver->RunSSSP(Arg.root);  // Use the root supplied through Args.
        },
        "SSSP");
  }

  return 0;
}
