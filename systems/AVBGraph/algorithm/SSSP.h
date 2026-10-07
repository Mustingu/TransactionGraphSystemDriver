#ifndef SSSP_H
#define SSSP_H

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

class SSSP {
 public:
  SSSP(GraphStore* input_graph, AllVBManager* input_vbm,
       int input_thread = 64);
  void compute_sssp(uint64_t source, double delta, GraphStore* MEA = nullptr);

  inline std::vector<double>* get_raw_result() { return &distances; }
  inline std::vector<std::pair<uint64_t, double>>* get_result() {
    return &result;
  }

 private:
  GraphStore* graph;
  AllVBManager* vbm;
  std::vector<double> distances;
  uint64_t num_vertices;
  std::vector<std::pair<uint64_t, double>> result;
  int max_vid;
  int thread_num;
};

#endif  // SSSP_H