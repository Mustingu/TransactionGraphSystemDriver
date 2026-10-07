#include "LCC.h"

#include <omp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>

LCC::LCC(GraphStore* input_graph, AllVBManager* input_vbm, int input_thread)
    : graph(input_graph), vbm(input_vbm), thread_num(input_thread) {
  max_vid = graph->get_node_num();
  num_vertices = max_vid;
  lcc_scores.resize(max_vid, 0.0);
  result.resize(max_vid);
}

void LCC::compute_lcc(GraphStore* MEA) {
  Transaction* txn = new Transaction(1, true, false, graph);
  vbm->registerROTransaction(txn);
  epoch_t read_ts = txn->get_read_epoch();

  max_vid = graph->get_node_num();
  lcc_scores.resize(max_vid, 0.0);

  // Phase 1: materialize + sort every vertex's neighbor list exactly once.
  // The previous version re-fetched and re-scanned u's raw edge block (with
  // its own lock) for every v that has u as a neighbor, i.e. O(deg(u)) work
  // repeated deg(u) times per hub vertex. dota-league has ~27k vertices with
  // degree > 1000 (max 17004), so that repeated re-locking/re-scanning is
  // what made this algorithm an outlier vs. the other systems' LCC, not the
  // O(sum deg^2) complexity itself (which is the same for the merge below).
  std::vector<std::vector<uint64_t>> all_neighbors(max_vid);
#pragma omp parallel for schedule(dynamic, 64)
  for (uint64_t v = 0; v < max_vid; v++) {
    auto* ds = graph->GetBlockByIndex(v);
    ds->getReadLock();
    std::vector<uint64_t> neighbors;
    GraphAlgorithms::for_each_edge(ds, Composite(read_ts, INTRA_MAX),
        [&](EdgeWithIndex* edge) {
          neighbors.push_back(edge->e & ~DELETION_MASK);
        }, nullptr);
    ds->unleashReadLock();
    std::sort(neighbors.begin(), neighbors.end());
    all_neighbors[v] = std::move(neighbors);
  }

  // Orient every edge from lower degree to higher degree (breaking ties by
  // vertex id).  Each triangle then has exactly one directed apex pattern,
  // so its three vertices are counted once instead of intersecting every
  // undirected adjacency pair.
  std::vector<std::vector<uint64_t>> forward(max_vid);
  for (uint64_t u = 0; u < max_vid; u++) {
    auto& fu = forward[u];
    fu.reserve(all_neighbors[u].size());
    const uint64_t du = all_neighbors[u].size();
    for (uint64_t v : all_neighbors[u]) {
      const uint64_t dv = all_neighbors[v].size();
      if (du < dv || (du == dv && u < v)) fu.push_back(v);
    }
  }

  // Phase 2: intersect only forward neighborhoods.  Atomic relaxed adds are
  // used because multiple oriented edges can discover a vertex concurrently.
  std::vector<std::atomic<uint64_t>> triangles(max_vid);
  for (auto& count : triangles) count.store(0, std::memory_order_relaxed);
#pragma omp parallel for schedule(dynamic, 64)
  for (uint64_t u = 0; u < max_vid; u++) {
    const auto& fu = forward[u];
    for (uint64_t v : fu) {
      const auto& fv = forward[v];
      size_t i = 0, j = 0;
      while (i < fu.size() && j < fv.size()) {
        if (fu[i] < fv[j]) {
          ++i;
        } else if (fu[i] > fv[j]) {
          ++j;
        } else {
          const uint64_t w = fu[i];
          triangles[u].fetch_add(2, std::memory_order_relaxed);
          triangles[v].fetch_add(2, std::memory_order_relaxed);
          triangles[w].fetch_add(2, std::memory_order_relaxed);
          ++i;
          ++j;
        }
      }
    }
  }

#pragma omp parallel for
  for (uint64_t v = 0; v < max_vid; v++) {
    const uint64_t deg = all_neighbors[v].size();
    if (deg >= 2) {
      lcc_scores[v] = static_cast<double>(
          triangles[v].load(std::memory_order_relaxed)) /
          (static_cast<double>(deg) * (deg - 1));
    }
  }

  vbm->deregisterROTransaction();
  delete txn;

#pragma omp parallel for num_threads(thread_num)
  for (uint64_t i = 0; i < max_vid; i++) {
    if (MEA != nullptr && i < MEA->get_node_num())
      result[i] = std::make_pair(MEA->p_mHashMap[i], lcc_scores[i]);
    else
      result[i] = std::make_pair(i, lcc_scores[i]);
  }
}
