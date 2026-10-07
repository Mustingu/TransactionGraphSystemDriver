#include "BFS.h"

#include <cstdlib>
#include <iostream>

BFS::BFS(GraphStore* input_graph, AllVBManager* input_vbm, int input_thread)
    : graph(input_graph), vbm(input_vbm), thread_num(input_thread) {
  max_vid = graph->get_node_num();
  num_vertices = max_vid;
  distances.resize(max_vid);
  result.resize(max_vid);
  stats_.enabled = false;
}

int64_t BFS::init_distance(Transaction& txn) {
  uint64_t total_edge_num(0);
#pragma omp parallel for reduction(+ : total_edge_num)
  for (uint64_t n = 0; n < max_vid; n++) {
    uint64_t out_degree = graph->check_degree(n, txn.get_read_epoch());
    if (out_degree == std::numeric_limits<int64_t>::max()) [[unlikely]] {
      distances[n] = out_degree;
    } else [[likely]] {
      total_edge_num += out_degree;
      distances[n] = out_degree != 0 ? -out_degree : -1;
      if (stats_.enabled) {
        stats_.init_vertices.fetch_add(1, std::memory_order_relaxed);
        stats_.init_edges.fetch_add(out_degree, std::memory_order_relaxed);
      }
    }
  }
  return total_edge_num / 2;
}

void BFS::bfs(uint64_t root, int alpha, int beta, GraphStore* MEA) {
  stats_.td_calls.store(0);
  stats_.bu_calls.store(0);
  stats_.direction_switches.store(0);
  stats_.td_ms.store(0);
  stats_.bu_ms.store(0);
  stats_.init_vertices.store(0);
  stats_.init_edges.store(0);
  stats_.td_vertices.store(0);
  stats_.td_edge_checks.store(0);
  stats_.td_discoveries.store(0);
  stats_.td_repeated.store(0);
  stats_.bu_candidates.store(0);
  stats_.bu_edge_checks.store(0);
  stats_.bu_early_stops.store(0);
  stats_.bu_misses.store(0);
  stats_.bu_locks.store(0);
  stats_.bu_success_checks.store(0);
  stats_.bu_max_success_checks.store(0);

  Transaction* txn = new Transaction(1, true, false, graph);
  vbm->registerROTransaction(txn);
  epoch_t read_ts = txn->get_read_epoch();
#ifdef FINEGRAIN
  Composite fg_read_ts = txn->get_read_ts();
#endif

  if (max_vid != graph->get_node_num()) [[unlikely]] {
    max_vid = graph->get_node_num();
    distances.resize(max_vid);
    result.resize(max_vid);
  }

  gapbs::SlidingQueue<int64_t> queue(max_vid);
  queue.push_back(root);
  queue.slide_window();
  gapbs::Bitmap curr(max_vid);
  curr.reset();
  gapbs::Bitmap front(max_vid);
  front.reset();
  int64_t edges_to_check = init_distance(*txn);

  int64_t scout_count = 0;
  scout_count = graph->check_degree(root, read_ts);
  int64_t distance = 1;
  distances[root] = 0;

  while (!queue.empty()) {
    // std::cout << scout_count << " " << edges_to_check << " " << alpha << " "
    //           << beta << '\n';
    if (scout_count > edges_to_check / alpha) {
      if (stats_.enabled) {
        stats_.direction_switches.fetch_add(1, std::memory_order_relaxed);
      }
      int64_t awake_count, old_awake_count;
      // Queue to Bitmap conversion
#pragma omp parallel for
      for (auto q_iter = queue.begin(); q_iter < queue.end(); q_iter++) {
        int64_t u = *q_iter;
        front.set_bit_atomic(u);
      }

      awake_count = queue.size();
      queue.slide_window();
      do {
        // std::cout << "do_bfs_BUStep\n";
        old_awake_count = awake_count;
        awake_count = do_bfs_BUStep(*txn, distance, front, curr);
        if (stats_.enabled) stats_.bu_calls.fetch_add(1, std::memory_order_relaxed);
        front.swap(curr);
        distance++;
      } while ((awake_count >= old_awake_count) ||
               (awake_count > (int64_t)num_vertices / beta));

#pragma omp parallel
      {
        gapbs::QueueBuffer<int64_t> lqueue(queue);
#pragma omp for schedule(dynamic, 64)
        for (uint64_t n = 0; n < max_vid; n++)
          if (front.get_bit(n)) lqueue.push_back(n);
        lqueue.flush();
      }
      queue.slide_window();
      scout_count = 1;
    } else {
      // std::cout << "do_bfs_TDStep\n";
      edges_to_check -= scout_count;
      scout_count = do_bfs_TDStep(*txn, distance, queue);
      if (stats_.enabled) stats_.td_calls.fetch_add(1, std::memory_order_relaxed);
      queue.slide_window();
      distance++;
    }
  }

  // Prepare results
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

  vbm->deregisterROTransaction();

  if (stats_.enabled) {
    const auto bu_success = stats_.bu_early_stops.load();
    const auto bu_avg = bu_success ? static_cast<double>(stats_.bu_success_checks.load()) / bu_success : 0.0;
    std::cerr << "AVB_BFS_STATS init_vertices=" << stats_.init_vertices.load()
              << " init_edges=" << stats_.init_edges.load()
              << " td_calls=" << stats_.td_calls.load()
              << " bu_calls=" << stats_.bu_calls.load()
              << " direction_switches=" << stats_.direction_switches.load()
              << " td_vertices=" << stats_.td_vertices.load()
              << " td_edge_checks=" << stats_.td_edge_checks.load()
              << " td_discoveries=" << stats_.td_discoveries.load()
              << " td_repeated=" << stats_.td_repeated.load()
              << " bu_candidates=" << stats_.bu_candidates.load()
              << " bu_edge_checks=" << stats_.bu_edge_checks.load()
              << " bu_early_stops=" << bu_success
              << " bu_misses=" << stats_.bu_misses.load()
              << " bu_locks=" << stats_.bu_locks.load()
              << " bu_avg_success_checks=" << bu_avg
              << " bu_max_success_checks=" << stats_.bu_max_success_checks.load()
              << "\n";
  }

  // std::cout << root << " " << alpha << " " << beta << " " << MEA << '\\n';

  // The driver writes AlgorithmResult after timing. Retain the old standalone
  // output only when explicitly requested at compile time.
#ifdef AVB_LEGACY_RESULT_OUTPUT
  if (MEA != nullptr) {
    std::vector<std::tuple<uint64_t, int64_t>> sorted_result(max_vid);
#pragma omp parallel for
    for (uint64_t logical_id = 0; logical_id < max_vid; logical_id++) {
      sorted_result[logical_id] =
          std::make_tuple(MEA->p_mHashMap[logical_id], distances[logical_id]);
    }
    std::sort(sorted_result.begin(), sorted_result.end(),
              [](const auto& a, const auto& b) {
                return std::get<0>(a) < std::get<0>(b);
              });
    std::ofstream output_file("bfs.output");
    if (!output_file.is_open()) {
      std::cerr << "Failed to open output file" << std::endl;
      return;
    }
    for (const auto& [vid, dist] : sorted_result) {
      output_file << vid << " " << dist << std::endl;
    }
    output_file.close();
  }
#endif
  // #pragma omp parallel for
  //   for (uint64_t logical_id = 0; logical_id < max_vid; logical_id++) {
  //     std::string_view payload = txn.get_vertex(
  //         logical_id, thread_id);  // they store external vid in the vertex
  //                                  // data for experiments
  //     if (payload.empty()) [[unlikely]] {  // the vertex does not exist
  //       result[logical_id - 1] =
  //           std::make_pair(std::numeric_limits<uint64_t>::max(),
  //                          std::numeric_limits<double>::max());
  //     } else {
  //       /*if(*(reinterpret_cast<const uint64_t*>(payload.data()))==8196461){
  //           std::cout<<" max vid is "<<max_vid<<" iteration is "<<
  //       logical_id<<std::endl; std::cout<< *(reinterpret_cast<const
  //       uint64_t*>(payload.data())) <<" "<< scores[logical_id-1]<<
  //       std::endl;
  //       }*/
  //       result[logical_id - 1] =
  //           std::make_pair(*(reinterpret_cast<const
  //           uint64_t*>(payload.data())),
  //                          scores[logical_id - 1]);
  //     }
  //   }
  delete txn;
}

int64_t BFS::do_bfs_TDStep(Transaction& txn, int64_t distance,
                           gapbs::SlidingQueue<int64_t>& queue) {
  int64_t scout_count = 0;
#pragma omp parallel reduction(+ : scout_count)
  {
    gapbs::QueueBuffer<int64_t> lqueue(queue);
#pragma omp for schedule(dynamic, 64)
    for (auto q_iter = queue.begin(); q_iter < queue.end(); q_iter++) {
      int64_t u = *q_iter;
      if (stats_.enabled) {
        stats_.td_vertices.fetch_add(1, std::memory_order_relaxed);
      }

      auto ds = graph->GetBlockByIndex(u);
      ds->getReadLock();

      GraphAlgorithms::for_each_edge(
          ds, Composite(txn.get_read_epoch(), INTRA_MAX),
          [&](EdgeWithIndex* edge) {
            if (stats_.enabled) {
              stats_.td_edge_checks.fetch_add(1, std::memory_order_relaxed);
            }
            auto dst = edge->e & ~DELETION_MASK;
            // std::cout << "dst " << dst << '\n';
            int64_t curr_val = distances[dst];
            if (curr_val < 0 &&
                gapbs::compare_and_swap(distances[dst], curr_val, distance)) {
              // Add to local queue buffer
              lqueue.push_back(dst);
              stats_.td_discoveries.fetch_add(1, std::memory_order_relaxed);
              scout_count += -curr_val;
            } else if (stats_.enabled) {
              stats_.td_repeated.fetch_add(1, std::memory_order_relaxed);
            }
          },
          nullptr);

      ds->unleashReadLock();
    }
    lqueue.flush();
  }
  return scout_count;
}

int64_t BFS::do_bfs_BUStep(Transaction& txn, int64_t distance,
                           gapbs::Bitmap& front, gapbs::Bitmap& next) {
  int64_t awake_count = 0;
  next.reset();

  uint64_t local_checks = 0;
  uint64_t local_candidates = 0;
#pragma omp parallel for reduction(+ : awake_count)
  for (uint64_t u = 0; u < max_vid; u++) {
    // std::cout << "do" + std::to_string(u) + '\n';
    if (distances[u] == std::numeric_limits<int64_t>::max())
      continue;              // the vertex does not exist
    if (distances[u] < 0) {  // the node has not been visited yet
      auto ds = graph->GetBlockByIndex(u);
      ds->getReadLock();
      if (stats_.enabled) {
        stats_.bu_candidates.fetch_add(1, std::memory_order_relaxed);
        stats_.bu_locks.fetch_add(1, std::memory_order_relaxed);
      }

      uint64_t checked = 0;
      bool found_neighbor = false;
      GraphAlgorithms::for_each_edge_condition(
          ds, Composite(txn.get_read_epoch(), INTRA_MAX), [&](EdgeWithIndex* edge) -> bool {
            if (stats_.enabled) ++checked;
            if (front.get_bit(edge->e & ~DELETION_MASK)) {
              found_neighbor = true;
              distances[u] = distance;
              awake_count++;
              next.set_bit(u);
              // std::cout << "setu " + std::to_string(edge->e & ~DELETION_MASK)
              // +
              //                  " " + std::to_string(u) + '\n';
              return true;
            }
            return false;
          });

      ds->unleashReadLock();
      if (stats_.enabled) {
        stats_.bu_edge_checks.fetch_add(checked, std::memory_order_relaxed);
        if (found_neighbor) {
          stats_.bu_early_stops.fetch_add(1, std::memory_order_relaxed);
          stats_.bu_success_checks.fetch_add(checked, std::memory_order_relaxed);
          uint64_t old = stats_.bu_max_success_checks.load(std::memory_order_relaxed);
          while (old < checked && !stats_.bu_max_success_checks.compare_exchange_weak(
              old, checked, std::memory_order_relaxed)) {}
        } else {
          stats_.bu_misses.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
  }
  return awake_count;
}
