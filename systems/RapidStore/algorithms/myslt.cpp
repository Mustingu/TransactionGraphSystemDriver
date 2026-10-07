#include "PageRank.h"

PageRank::PageRank(MyEdgeArray* input_graph, AllVBManager* input_vbm,
                   int input_thread)
    : graph(input_graph), vbm(input_vbm), thread_num(input_thread) {
  max_vid = graph->get_node_num();
  num_vertices = max_vid;
  scores.resize(max_vid);
  result.resize(max_vid);
}

void PageRank::compute_pagerank(uint64_t num_iterations, double damping_factor,
                                MyEdgeArray* MEA) {
  Transaction* txn = new Transaction(1, true, false, graph);
  vbm->registerROTransaction(txn);
  auto read_ts = txn->get_read_epoch();
  // if (max_vid != graph->get_node_num()) [[unlikely]] {
  max_vid = graph->get_node_num();

  scores.resize(max_vid);
  result.resize(max_vid);
  // }

  const double init_score = 1.0 / num_vertices;
  const double base_score = (1.0 - damping_factor) / num_vertices;

  std::vector<uint64_t> degrees(max_vid);

  // std::cout << "read epoch: " << read_ts
  //           << " number of iterations : " << num_iterations
  //           << " number of vertices: " << num_vertices << std::endl;

#pragma omp parallel for  // num_threads(thread_num)
  for (uint64_t v = 0; v < max_vid; v++) {
    scores[v] = init_score;
    degrees[v] = graph->check_degree(v, read_ts);
  }
  // std::cout<<"neighborhood scanned"<<std::endl;
  std::vector<gapbs::pvector<double>> outgoing_contrib;
  outgoing_contrib.reserve(4);
  for (int i = 0; i < 4; i++) {
    outgoing_contrib.emplace_back(max_vid, 0.0);
  }

  // pagerank iteration start
  // std::cout << num_iterations << " iterations" << " " << max_vid <<
  // std::endl;
  for (uint64_t iteration = 0; iteration < num_iterations; iteration++) {
    // std::cout << "iteration: " << iteration << std::endl;
    double dangling_sum = 0.0;
#pragma omp parallel for reduction(+ : dangling_sum)  // num_threads(thread_num)
    for (uint64_t v = 0; v < max_vid; v++) {
      uint64_t out_degree = degrees[v];
      if (out_degree == 0) {  // this is a sink
        dangling_sum += scores[v];
      } else {
        outgoing_contrib[0][v] = scores[v] / out_degree;
        // std::cout << "outgoing_contrib[v]: " << outgoing_contrib[v]
        //           << " v: " << v << " " << "out_degree: " << out_degree
        //           << std::endl;
      }
    }
    dangling_sum /= num_vertices;

    // #pragma omp parallel for
    // for (int copy_idx = 1; copy_idx < 4; copy_idx++) {
    //   std::memcpy(outgoing_contrib[copy_idx].data(),
    //   outgoing_contrib[0].data(),
    //               max_vid * sizeof(double));
    // }

    // gapbs::pvector<double> neighbors(max_vid, 0.0);
#pragma omp parallel for schedule(dynamic, 64)  // num_threads(thread_num)
    for (uint64_t v = 0; v < max_vid; v++) {
      double incoming_total = 0;
      if (degrees[v]) {
        auto x = outgoing_contrib[0][v];
        auto ds = graph->GetBlockByIndex(v);
        thread_local gapbs::pvector<double> neighbors(max_vid, 0.0);
        // auto& local_outgoing = outgoing_contrib[omp_get_thread_num() & 3];
        // neighbors.clear();

        ds->getReadLock();
        // GraphAlgorithms::for_each_edge(ds, read_ts, [&](EdgeWithIndex* edge)
        // {
        //   incoming_total += edge->e & ~DELETION_MASK;
        // });
        GraphAlgorithms::for_each_edge(ds, read_ts, [&](EdgeWithIndex* edge) {
          incoming_total += outgoing_contrib[0][edge->e & ~DELETION_MASK];
          // neighbors[edge->e & ~DELETION_MASK] += x;
        });
        ds->unleashReadLock();

        // std::sort(neighbors.begin(), neighbors.end());

        // for (int i = 0; i < neighbors.size(); i++) {
        //   incoming_total += local_outgoing[neighbors[i]];
        // }
      }
      scores[v] = base_score + damping_factor * (incoming_total + dangling_sum);
    }
  }
  vbm->deregisterROTransaction();

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
  // pagerank iteration end

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
}
