#ifndef BFS_H
#define BFS_H

#include <omp.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "AllVBManager.h"
#include "data-structure/EdgeBlock/GraphStore.h"
#include "gapbs.h"

class BFS {
 public:
  BFS(GraphStore* input_graph, AllVBManager* input_vbm, int input_thread = 64);
  void bfs(uint64_t root, int alpha = 15, int beta = 18,
           GraphStore* MEA = nullptr);
  int64_t init_distance(Transaction& txn);

  inline std::vector<int64_t>* get_raw_result() { return &distances; }
  inline std::vector<std::pair<uint64_t, int64_t>>* get_result() {
    return &result;
  }

 private:
  int64_t do_bfs_TDStep(Transaction& txn, int64_t distance,
                        gapbs::SlidingQueue<int64_t>& queue);
  int64_t do_bfs_BUStep(Transaction& txn, int64_t distance,
                        gapbs::Bitmap& front, gapbs::Bitmap& next);

  struct Stats {
    bool enabled = false;
    std::atomic<uint64_t> init_vertices{0};
    std::atomic<uint64_t> init_edges{0};
    std::atomic<uint64_t> td_vertices{0};
    std::atomic<uint64_t> td_edge_checks{0};
    std::atomic<uint64_t> td_discoveries{0};
    std::atomic<uint64_t> td_repeated{0};
    std::atomic<uint64_t> bu_candidates{0};
    std::atomic<uint64_t> bu_edge_checks{0};
    std::atomic<uint64_t> bu_early_stops{0};
    std::atomic<uint64_t> bu_misses{0};
    std::atomic<uint64_t> bu_locks{0};
    std::atomic<uint64_t> bu_success_checks{0};
    std::atomic<uint64_t> bu_max_success_checks{0};
    std::atomic<uint64_t> td_calls{0};
    std::atomic<uint64_t> bu_calls{0};
    std::atomic<uint64_t> direction_switches{0};
    std::atomic<uint64_t> td_ms{0};
    std::atomic<uint64_t> bu_ms{0};
  };

  Stats stats_;

  GraphStore* graph;
  AllVBManager* vbm;
  std::vector<int64_t> distances;
  uint64_t num_vertices;
  std::vector<std::pair<uint64_t, int64_t>> result;
  int max_vid;
  int thread_num;
};

#endif  // BFS_H