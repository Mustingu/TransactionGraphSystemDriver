#include "PageRank.h"

PageRank::PageRank(GraphStore* input_graph, AllVBManager* input_vbm,
                   int input_thread)
    : graph(input_graph), vbm(input_vbm), thread_num(input_thread) {
  max_vid = graph->get_node_num();
  num_vertices = max_vid;
  scores.resize(max_vid);
  result.resize(max_vid);
}

void PageRank::compute_pagerank(uint64_t num_iterations, double damping_factor,
                                GraphStore* MEA, Satistical* global_counter) {
  Transaction* txn = new Transaction(1, true, false, graph);
  vbm->registerROTransaction(txn);
  const epoch_t read_ts = txn->get_read_epoch();
  max_vid = graph->get_node_num();
#ifdef FINEGRAIN
  const Composite fg_read_ts = txn->get_read_ts();
#ifdef PR_DEBUG
  printf("[PR] read_ts: coarse=%u, fg=(%u,%u)\n", read_ts, fg_read_ts.e, fg_read_ts.i);
#endif
#endif

  scores.resize(max_vid);
  result.resize(max_vid);

  const double init_score = 1.0 / num_vertices;
  const double base_score = (1.0 - damping_factor) / num_vertices;

  std::vector<uint64_t> degrees(max_vid);

#pragma omp parallel for
  for (uint64_t v = 0; v < max_vid; v++) {
    scores[v] = init_score;
    // StaticLoadFinalization publishes the last write epoch before this
    // reader starts. The O(1) degree metadata is therefore valid even with
    // FINEGRAIN enabled; do not rescan every adjacency list here.
    degrees[v] = graph->check_degree(v, read_ts);
  }
  std::vector<gapbs::pvector<double>> outgoing_contrib;
  outgoing_contrib.reserve(4);
  for (int i = 0; i < 4; i++) {
    outgoing_contrib.emplace_back(max_vid, 0.0);
  }

  for (uint64_t iteration = 0; iteration < num_iterations; iteration++) {
    double dangling_sum = 0.0;

#pragma omp parallel for reduction(+ : dangling_sum)
    for (uint64_t v = 0; v < max_vid; v++) {
      uint64_t out_degree = degrees[v];
      if (out_degree == 0) {
        dangling_sum += scores[v];
      } else {
        outgoing_contrib[0][v] = scores[v] / out_degree;
      }
    }
    dangling_sum /= num_vertices;

#pragma omp parallel
    {
      thread_local Satistical local_statistic;
      local_statistic.reset();
      Satistical* nw = global_counter ? &local_statistic : nullptr;

#pragma omp for schedule(dynamic, 64)
      for (uint64_t v = 0; v < max_vid; v++) {
        double incoming_total = 0;
        if (degrees[v]) {
          auto ds = graph->GetBlockByIndex(v);
          ds->getReadLock();
#ifdef FINEGRAIN
          GraphAlgorithms::for_each_edge(
              ds, fg_read_ts,
#else
          GraphAlgorithms::for_each_edge(
              ds, Composite(read_ts, 0),
#endif
              [&](EdgeWithIndex* edge) {
                incoming_total += outgoing_contrib[0][edge->e & ~DELETION_MASK];
                return false;
              },
              nw);
          ds->unleashReadLock();
        }
        scores[v] =
            base_score + damping_factor * (incoming_total + dangling_sum);
      }
#pragma omp critical
      {
        if (global_counter) {
          global_counter->record_cnt += local_statistic.record_cnt;
          global_counter->visible_record_cnt +=
              local_statistic.visible_record_cnt;
        }
      }
    }
#ifdef TVB_STATS
    {
      long long scans = tvb_scan_total.exchange(0);
      long long records = tvb_record_total.exchange(0);
      long long epoch_mismatch = tvb_scan_vertices.exchange(0);
      uint64_t accessed = 0;
      for (uint64_t v = 0; v < max_vid; v++)
        if (degrees[v]) accessed++;
      printf("[PR iter %lu] read_ts=(%u,%u) write_epoch=%u vertices=%lu "
             "TVB:%lld active(%.1f/v) records:%lld(%.1f/TVB)",
             (unsigned long)iteration, fg_read_ts.e, fg_read_ts.i,
             vbm->getCurrentEpoch(),
             (unsigned long)accessed,
             scans, accessed > 0 ? (double)scans / accessed : 0.0,
             records, scans > 0 ? (double)records / scans : 0.0);
      if (epoch_mismatch)
        printf(" epoch_mismatch:%lld", epoch_mismatch);
      printf("\n");
    }
#endif  // TVB_STATS
  }

#pragma omp parallel for num_threads(thread_num)
  for (uint64_t logical_id = 0; logical_id < max_vid; logical_id++) {
    if (MEA != nullptr && logical_id < MEA->get_node_num()) {
      result[logical_id] =
          std::make_pair(MEA->p_mHashMap[logical_id], scores[logical_id]);
    } else {
      if (scores[logical_id] == std::numeric_limits<int64_t>::max()) {
        result[logical_id] =
            std::make_pair(std::numeric_limits<uint64_t>::max(),
                           std::numeric_limits<int64_t>::max());
      } else {
        result[logical_id] = std::make_pair(logical_id, scores[logical_id]);
      }
    }
  }

  vbm->deregisterROTransaction();

  // The unified driver owns result serialization. Keeping the historical AVB
  // file output opt-in prevents a full sort plus per-line I/O from being
  // charged to the algorithm's processing time.
#ifdef AVB_LEGACY_RESULT_OUTPUT
  if (MEA != nullptr) {
    std::ofstream outfile("output_pr.result");

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
      outfile << MEA->p_mHashMap[v] << " " << scores[v] << std::endl;
    }
    outfile.close();
  }
#endif
}
