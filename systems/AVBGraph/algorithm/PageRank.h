#ifndef PAGE_RANK_H
#define PAGE_RANK_H

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

class PageRank {
 public:
  PageRank(GraphStore* input_graph, AllVBManager* input_vbm,
           int input_thread = 64);
  void compute_pagerank(uint64_t num_iterations, double damping_factor,
                        GraphStore* MEA = nullptr,
                        Satistical* global_counter = nullptr);

  inline std::vector<double>* get_raw_result() { return &scores; }
  inline std::vector<std::pair<uint64_t, double>>* get_result() {
    return &result;
  }

 private:
  GraphStore* graph;
  AllVBManager* vbm;
  std::vector<double> scores;
  uint64_t num_vertices;
  std::vector<std::pair<uint64_t, double>> result;
  int max_vid;
  int thread_num;
};
#endif  // PAGE_RANK_H