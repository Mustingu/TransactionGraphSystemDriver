#include "CDLP.h"

#include <omp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <unordered_map>

CDLP::CDLP(GraphStore* input_graph, AllVBManager* input_vbm, int input_thread)
    : graph(input_graph), vbm(input_vbm), thread_num(input_thread) {
  max_vid = graph->get_node_num();
  num_vertices = max_vid;
  labels.resize(max_vid);
  result.resize(max_vid);
}

void CDLP::compute_cdlp(uint64_t max_iterations, GraphStore* MEA) {
  Transaction* txn = new Transaction(1, true, false, graph);
  vbm->registerROTransaction(txn);
  epoch_t read_ts = txn->get_read_epoch();

  max_vid = graph->get_node_num();
  labels.resize(max_vid);

  // Initialize: each vertex gets its own EXTERNAL id as label. The official
  // convention breaks propagation ties by comparing label values numerically
  // against every OTHER vertex's label, so this must operate in external-id
  // space throughout — internal ids are assigned in load order and generally
  // do not preserve external-id relative order, which would converge the
  // propagation to a different (wrong) partition, not just mislabel the
  // output.
#pragma omp parallel for
  for (uint64_t v = 0; v < max_vid; v++)
    labels[v] = graph->p_mHashMap[v];

  std::vector<uint64_t> next_labels(max_vid);
  std::atomic<bool> changed(true);

  for (uint64_t iter = 0; iter < max_iterations && changed.load(); iter++) {
    changed.store(false);

#pragma omp parallel
    {
      // Thread-local scratch buffer for one vertex's neighbor labels.
      std::vector<uint64_t> nbr_labels;
#pragma omp for schedule(dynamic, 64)
      for (uint64_t v = 0; v < max_vid; v++) {
        uint64_t deg = graph->check_degree(v, read_ts);
        if (deg == 0) { next_labels[v] = labels[v]; continue; }

        // for_each_edge_sorted orders by neighbor INTERNAL id, not by label
        // value, so equal labels are not guaranteed adjacent — collect then
        // sort the actual label values before run-length counting, or
        // repeated labels get undercounted (silently corrupting tie-breaks).
        nbr_labels.clear();
        nbr_labels.reserve(deg);
        auto* ds = graph->GetBlockByIndex(v);
        ds->getReadLock();
        ds->for_each_edge_sorted(read_ts,
            [&](EdgeWithIndex* edge) {
              nbr_labels.push_back(labels[edge->e & ~DELETION_MASK]);
            });
        ds->unleashReadLock();
        std::sort(nbr_labels.begin(), nbr_labels.end());

        uint64_t best_label = nbr_labels[0];
        uint64_t best_count = 0;
        uint64_t i = 0;
        while (i < nbr_labels.size()) {
          uint64_t j = i;
          while (j < nbr_labels.size() && nbr_labels[j] == nbr_labels[i]) j++;
          uint64_t count = j - i;
          if (count > best_count) {
            best_count = count;
            best_label = nbr_labels[i];
          }
          i = j;
        }

        if (best_label != labels[v]) {
          changed.store(true);
          next_labels[v] = best_label;
        } else {
          next_labels[v] = labels[v];
        }
      }
    }

    std::swap(labels, next_labels);
  }

  vbm->deregisterROTransaction();
  delete txn;

#pragma omp parallel for num_threads(thread_num)
  for (uint64_t i = 0; i < max_vid; i++) {
    if (MEA != nullptr && i < MEA->get_node_num())
      result[i] = std::make_pair(MEA->p_mHashMap[i], labels[i]);
    else
      result[i] = std::make_pair(i, labels[i]);
  }
}
