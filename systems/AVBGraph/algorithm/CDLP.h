#ifndef AVBGRAPH_CDLP_H
#define AVBGRAPH_CDLP_H

#include <cstdint>
#include <vector>

#include "data-structure/EdgeBlock/GraphStore.h"
#include "data-structure/VersionBlock/AllVBManager.h"

class CDLP {
 public:
  CDLP(GraphStore* input_graph, AllVBManager* input_vbm, int input_thread = 64);

  void compute_cdlp(uint64_t max_iterations = 10, GraphStore* MEA = nullptr);

  inline std::vector<uint64_t>* get_raw_result() { return &labels; }
  inline std::vector<std::pair<uint64_t, uint64_t>>* get_result() { return &result; }

 private:
  GraphStore* graph;
  AllVBManager* vbm;
  std::vector<uint64_t> labels;
  uint64_t num_vertices;
  std::vector<std::pair<uint64_t, uint64_t>> result;
  int max_vid;
  int thread_num;
};

#endif  // AVBGRAPH_CDLP_H
