#include "WCC.h"

#include <omp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>

WCC::WCC(GraphStore* input_graph, AllVBManager* input_vbm, int input_thread)
    : graph(input_graph), vbm(input_vbm), thread_num(input_thread) {
  max_vid = graph->get_node_num();
  num_vertices = max_vid;
  components.resize(max_vid);
  result.resize(max_vid);
}

void WCC::compute_wcc(GraphStore* MEA) {
  Transaction* txn = new Transaction(1, true, false, graph);
  vbm->registerROTransaction(txn);
  epoch_t read_ts = txn->get_read_epoch();

  max_vid = graph->get_node_num();
  components.resize(max_vid);
  // Initialize: each vertex is its own component
#pragma omp parallel for
  for (uint64_t v = 0; v < max_vid; v++)
    components[v] = v;

  std::atomic<bool> changed(true);
  int iter = 0;

  while (changed.load() && iter < 100) {
    changed.store(false);
    iter++;

    // Forward propagation: u → v
#pragma omp parallel for schedule(dynamic, 64)
    for (uint64_t u = 0; u < max_vid; u++) {
      uint64_t deg = graph->check_degree(u, read_ts);
      if (deg == 0) continue;
      auto* ds = graph->GetBlockByIndex(u);
      ds->getReadLock();
      uint64_t my = components[u];
      GraphAlgorithms::for_each_edge(ds, Composite(read_ts, INTRA_MAX),
          [&](EdgeWithIndex* edge) {
            uint64_t v = edge->e & ~DELETION_MASK;
            uint64_t other = components[v];
            if (my < other) {
              components[v] = my;
              changed.store(true);
            }
          }, nullptr);
      ds->unleashReadLock();
    }

    // Backward propagation: ensure both directions converge
#pragma omp parallel for schedule(dynamic, 64)
    for (uint64_t u = 0; u < max_vid; u++) {
      auto* ds = graph->GetBlockByIndex(u);
      ds->getReadLock();
      GraphAlgorithms::for_each_edge(ds, Composite(read_ts, INTRA_MAX),
          [&](EdgeWithIndex* edge) {
            uint64_t v = edge->e & ~DELETION_MASK;
            uint64_t a = components[u];
            uint64_t b = components[v];
            uint64_t m = std::min(a, b);
            if (a != m) { components[u] = m; changed.store(true); }
            if (b != m) { components[v] = m; changed.store(true); }
          }, nullptr);
      ds->unleashReadLock();
    }
  }

  // Flatten: use parent pointers to finalize component IDs
  std::vector<uint64_t> parent(max_vid);
#pragma omp parallel for
  for (uint64_t v = 0; v < max_vid; v++) {
    uint64_t cur = v;
    while (components[cur] != cur) cur = components[cur];
    parent[v] = cur;
  }

  vbm->deregisterROTransaction();
  delete txn;

#pragma omp parallel for num_threads(thread_num)
  for (uint64_t i = 0; i < max_vid; i++) {
    if (MEA != nullptr && i < MEA->get_node_num())
      result[i] = std::make_pair(MEA->p_mHashMap[i], parent[i]);
    else
      result[i] = std::make_pair(i, parent[i]);
  }
}
