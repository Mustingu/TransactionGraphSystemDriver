#include "SSSP.h"

SSSP::SSSP(GraphStore* input_graph, AllVBManager* input_vbm, int input_thread)
    : graph(input_graph), vbm(input_vbm), thread_num(input_thread) {
  max_vid = graph->get_node_num();
  num_vertices = max_vid;
  distances.resize(max_vid);
  result.resize(max_vid);
}

void SSSP::compute_sssp(uint64_t source, double delta, GraphStore* MEA) {
  Transaction* txn = new Transaction(1, true, false, graph);
  vbm->registerROTransaction(txn);
  epoch_t read_ts = txn->get_read_epoch();
#ifdef FINEGRAIN
  Composite fg_read_ts = txn->get_read_ts();
#endif

  max_vid = graph->get_node_num();
  distances.resize(max_vid);
  result.resize(max_vid);

  // Initialize distances.
  const double INF = std::numeric_limits<double>::infinity();
#pragma omp parallel for
  for (int i = 0; i < max_vid; i++) {
    distances[i] = INF;
  }

  // Set the source distance to zero.
  distances[source] = 0.0;

  // Read each vertex's outdegree.
  std::vector<uint64_t> degrees(max_vid);
#pragma omp parallel for
  for (int v = 0; v < max_vid; v++) {
    degrees[v] = graph->check_degree(v, read_ts);
  }

  // Count edges and initialize the frontier queue.
  uint64_t num_edges = 0;
  for (int v = 0; v < max_vid; v++) {
    num_edges += degrees[v];
  }

  gapbs::pvector<uint64_t> frontier(num_edges);
  size_t shared_indexes[2] = {0, std::numeric_limits<size_t>::max() / 2};
  size_t frontier_tails[2] = {1, 0};
  frontier[0] = source;

  using NodeID = uint64_t;
  using WeightT = double;

#pragma omp parallel
  {
    uint8_t thread_id = omp_get_thread_num();
    std::vector<std::vector<uint64_t>> local_bins(0);
    size_t iter = 0;

    while (shared_indexes[iter & 1] != std::numeric_limits<size_t>::max() / 2) {
      size_t& curr_bin_index = shared_indexes[iter & 1];
      size_t& next_bin_index = shared_indexes[(iter + 1) & 1];
      size_t& curr_frontier_tail = frontier_tails[iter & 1];
      size_t& next_frontier_tail = frontier_tails[(iter + 1) & 1];

#pragma omp for nowait schedule(dynamic, 64)
      for (size_t i = 0; i < curr_frontier_tail; i++) {
        NodeID u = frontier[i];

        if (distances[u] >= delta * static_cast<WeightT>(curr_bin_index)) {
          auto ds = graph->GetBlockByIndex(u);

          ds->getReadLock();
          GraphAlgorithms::for_each_edge_with_property(
#ifdef FINEGRAIN
              ds, fg_read_ts, [&](EdgeWithIndex* edge, EdgeWithIndex* prop) {
#else
              ds, Composite(read_ts, 0), [&](EdgeWithIndex* edge, EdgeWithIndex* prop) {
#endif
                auto v = edge->e & ~DELETION_MASK;
                double w = *reinterpret_cast<double*>(&prop->properties);

                WeightT old_dist = distances[v];
                WeightT new_dist = distances[u] + w;

                if (new_dist < old_dist) {
                  bool changed_dist = true;
                  while (!gapbs::compare_and_swap(distances[v], old_dist,
                                                  new_dist)) {
                    old_dist = distances[v];
                    if (old_dist <= new_dist) {
                      changed_dist = false;
                      break;
                    }
                  }

                  if (changed_dist) {
                    size_t dest_bin = new_dist / delta;
                    if (dest_bin >= local_bins.size()) {
                      local_bins.resize(dest_bin + 1);
                    }
                    local_bins[dest_bin].push_back(v);
                  }
                }
              });
          ds->unleashReadLock();
        }
      }

      // Update the global bin index.
      for (size_t i = curr_bin_index; i < local_bins.size(); i++) {
        if (!local_bins[i].empty()) {
#pragma omp critical
          next_bin_index = std::min(next_bin_index, i);
          break;
        }
      }

#pragma omp barrier
#pragma omp single nowait
      {
        curr_bin_index = std::numeric_limits<size_t>::max() / 2;
        curr_frontier_tail = 0;
      }

      if (next_bin_index < local_bins.size()) {
        size_t copy_start = gapbs::fetch_and_add(
            next_frontier_tail, local_bins[next_bin_index].size());
        std::copy(local_bins[next_bin_index].begin(),
                  local_bins[next_bin_index].end(),
                  frontier.data() + copy_start);
        local_bins[next_bin_index].clear();
      }

      iter++;
#pragma omp barrier
    }
  }

  vbm->deregisterROTransaction();

#pragma omp parallel for
  for (uint64_t logical_id = 0; logical_id < max_vid; logical_id++) {
    if (MEA != nullptr && logical_id < MEA->get_node_num()) {
      result[logical_id] =
          std::make_pair(MEA->p_mHashMap[logical_id], distances[logical_id]);
    } else {
      if (distances[logical_id] == std::numeric_limits<int64_t>::max()) {
        result[logical_id] =
            std::make_pair(std::numeric_limits<uint64_t>::max(),
                           std::numeric_limits<int64_t>::max());
      } else {
        result[logical_id] = std::make_pair(logical_id, distances[logical_id]);
      }
    }
  }

  // The driver writes AlgorithmResult after timing. Retain the old standalone
  // output only when explicitly requested at compile time.
#ifdef AVB_LEGACY_RESULT_OUTPUT
  if (MEA != nullptr) {
    std::ofstream outfile("output_sssp.result");

    // Create an index vector for sorted output.
    std::vector<uint64_t> indices(max_vid);
    std::iota(indices.begin(), indices.end(), 0);

    // Sort indices by the external IDs in MEA->p_mHashMap.
    std::sort(indices.begin(), indices.end(), [&](uint64_t a, uint64_t b) {
      return MEA->p_mHashMap[a] < MEA->p_mHashMap[b];
    });

    // Write results in sorted order.
    for (uint64_t i = 0; i < max_vid; i++) {
      uint64_t v = indices[i];
      outfile << MEA->p_mHashMap[v] << " " << distances[v] << std::endl;
    }
    outfile.close();
  }
#endif
}
