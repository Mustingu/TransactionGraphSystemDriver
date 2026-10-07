#ifndef AVBGRAPH_LCC_H
#define AVBGRAPH_LCC_H

#include <cstdint>
#include <vector>

#include "data-structure/EdgeBlock/GraphStore.h"
#include "data-structure/VersionBlock/AllVBManager.h"

class LCC {
 public:
  LCC(GraphStore* input_graph, AllVBManager* input_vbm, int input_thread = 64);

  void compute_lcc(GraphStore* MEA = nullptr);

  inline std::vector<double>* get_raw_result() { return &lcc_scores; }
  inline std::vector<std::pair<uint64_t, double>>* get_result() { return &result; }

 private:
  GraphStore* graph;
  AllVBManager* vbm;
  std::vector<double> lcc_scores;
  uint64_t num_vertices;
  std::vector<std::pair<uint64_t, double>> result;
  int max_vid;
  int thread_num;
};

#endif  // AVBGRAPH_LCC_H
