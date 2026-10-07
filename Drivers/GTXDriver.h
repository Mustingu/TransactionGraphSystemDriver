#pragma once
#include <omp.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../GraphDriver.h"
#include "../common/algorithms/GraphKernels.h"
#include "GTX.hpp"
#include "tbb/concurrent_hash_map.h"

class GTXDriver : public GraphDriver {
 private:
  gt::Graph* m_pImpl;

  // Map external vertex IDs to GTX internal IDs.
  using vertex_dictionary_t = tbb::concurrent_hash_map<uint64_t, uint64_t>;
  vertex_dictionary_t* m_pHashMap;

  std::atomic<int> m_num_vertices{0};
  std::atomic<int> m_num_edges{0};
  AlgorithmResult last_result_;

 public:
  void SetWRThread(int writer_threads, int reader_threads) override {
    m_pImpl->configure_distinct_readers_and_writers(reader_threads,
                                                    writer_threads);
  }
  void SetWorkThread(int thread_num) {
    m_pImpl->set_worker_thread_num(thread_num);
  }

  // GTX hardcodes worker_thread_num to 64 (graph_global.hpp); thread IDs
  // are allocated monotonically and are not reclaimed when threads exit. Spawning
  // wt workers for both loading and mixed phases can exceed 64 and overrun get_table.
  // Reset before each phase; configure resets worker IDs and rebuilds transaction tables.
  // The configure argument order is (reader_count, writer_count).
  void ResetWorkerThreads() override {
    m_pImpl->configure_distinct_readers_and_writers(0, 64);
  }
  std::string Name() const override { return "GTX"; }

  const AlgorithmResult& GetLastResult() const override { return last_result_; }

  void Init(int thread_num) override {
    m_pImpl = new gt::Graph();
    m_pHashMap = new vertex_dictionary_t();

    printf("[GTX] Initialized.\n");
  }

  bool UpsertEdge(uint64_t src, uint64_t dst, uint64_t property,
                  int thread_id) override {
    uint64_t internal_source_id = std::numeric_limits<uint64_t>::max();
    uint64_t internal_destination_id = 0;
    bool insert_source = false;
    bool insert_destination = false;
    vertex_dictionary_t::const_accessor slock1, slock2;
    vertex_dictionary_t::accessor xlock1, xlock2;
    bool result = false;
    if (m_pHashMap->find(slock1, src)) {  // insert the vertex e.m_source
      internal_source_id = slock1->second;
    } else {
      slock1.release();
      if (m_pHashMap->insert(xlock1, src)) {
        insert_source = true;
      } else {
        internal_source_id = xlock1->second;
      }
    }

    if (m_pHashMap->find(slock2,
                         dst)) {  // insert the vertex e.m_destination
      internal_destination_id = slock2->second;
    } else {
      slock2.release();
      if (m_pHashMap->insert(xlock2, dst)) {
        insert_destination = true;
      } else {
        internal_destination_id = xlock2->second;
      }
    }

    bool done = false;
    do {
      auto tx = m_pImpl->begin_read_write_transaction();
      try {
        // create the vertices in GTX
        if (insert_source) {
          internal_source_id = tx.new_vertex();
          std::string_view data{(char*)&src, sizeof(src)};
          tx.put_vertex(internal_source_id, data);
        }
        if (insert_destination) {
          internal_destination_id = tx.new_vertex();
          std::string_view data{(char*)&dst, sizeof(dst)};
          tx.put_vertex(internal_destination_id, data);
        }

        // insert the edge
        std::string_view weight{(char*)&property, sizeof(property)};
        result = tx.checked_put_edge(internal_source_id, /* label */ 1,
                                     internal_destination_id, weight);

        if (tx.commit()) {
          if (result) m_num_edges++;
          done = true;
        }
      } catch (gt::RollbackExcept& e) {
        tx.abort();
        // retry ...
      }
    } while (!done);

    if (insert_source) {
      // assert(internal_source_id != numeric_limits<uint64_t>::max());
      xlock1->second = internal_source_id;
      m_num_vertices++;
    }
    if (insert_destination) {
      // assert(internal_destination_id != numeric_limits<uint64_t>::max());
      xlock2->second = internal_destination_id;
      m_num_vertices++;
    }

    return result;
  }


  // Look up both existing IDs without creating vertices on the deletion path.
  bool FindIDs(uint64_t src, uint64_t dst, uint64_t& s, uint64_t& d) {
    vertex_dictionary_t::const_accessor a1, a2;
    if (!m_pHashMap->find(a1, src)) return false;
    if (!m_pHashMap->find(a2, dst)) return false;
    s = a1->second;
    d = a2->second;
    return true;
  }

  // checked_delete_edge also removes the reverse edge when DIRECTED_GRAPH is true
  // (see bind/GTX.cpp); one call therefore deletes both directions.
  // Missing edges return false; retry RollbackExcept on conflicts.
  bool DeleteEdge(uint64_t src, uint64_t dst, int thread_id) override {
    uint64_t s, d;
    if (!FindIDs(src, dst, s, d)) return false;
    bool done = false;
    do {
      auto tx = m_pImpl->begin_read_write_transaction();
      try {
        tx.checked_delete_edge(s, 1, d);
        tx.commit();
        done = true;
      } catch (gt::RollbackExcept& e) {
        tx.abort();
        // retry
      }
    } while (!done);
    return true;
  }

  void RunPageRank(int iterations, double damping_factor,
                   S_Driver* global_counter) override {
    uint64_t v_num = m_num_vertices;
    auto handler = m_pImpl->get_pagerank_handler(v_num);
    handler.compute(iterations, damping_factor);
    if (!ShouldCollectAlgorithmResults()) return;
    last_result_.clear();
    auto* res = handler.get_result();
    if (res) {
      last_result_.type = AlgorithmResult::kDouble;
      last_result_.double_vals = *res;
    }
  }

  void RunBFS(uint64_t root, S_Driver* global_counter) override {
    vertex_dictionary_t::const_accessor a;
    if (!m_pHashMap->find(a, root)) {
      printf("BFS Root %lu not found!\n", root);
      return;
    }
    uint64_t internal_root = a->second;

    auto handler = m_pImpl->get_bfs_handler(m_num_vertices);
    handler.compute(internal_root);
    if (!ShouldCollectAlgorithmResults()) return;
    last_result_.clear();
    auto* res = handler.get_result();
    if (res) {
      last_result_.type = AlgorithmResult::kLong;
      for (const auto& p : *res) {
        if (p.first != std::numeric_limits<uint64_t>::max())
          last_result_.long_vals.emplace_back(p.first, p.second);
      }
    }
  }

  void RunSSSP(uint64_t root, S_Driver* global_counter) override {
    vertex_dictionary_t::const_accessor accessor;
    if (m_pHashMap->find(accessor, root)) {
      root = accessor->second;
    } else {
      std::cout << "unable to find the vertex " << root << std::endl;
      throw std::runtime_error("force crash");
      return;
    }
    auto handler = m_pImpl->get_sssp_handler(/*max_vertex_id*/);
    double delta = 2.0;
    handler.compute(root, delta);
    if (!ShouldCollectAlgorithmResults()) return;
    last_result_.clear();
    auto* res = handler.get_result();
    if (res) {
      last_result_.type = AlgorithmResult::kDouble;
      last_result_.double_vals = *res;
    }
  }

  struct CommonView {
    std::unique_ptr<gt::SharedROTransaction> tx;
    std::vector<uint64_t> external;
    std::vector<uint64_t> internal;
    // Maps a GTX storage vertex id (dst_id()) to the dense rank in internal/
    // external. The shared kernels index their dense arrays by this rank, so a
    // neighbor returned by TraverseEdges() must be remapped through it;
    // passing the raw storage id would index out of bounds (WCC/LCC crash).
    std::unordered_map<uint64_t, uint64_t> internal_to_rank;

    uint64_t vertex_count() const { return external.size(); }
    uint64_t external_id(uint64_t v) const { return external[v]; }
    uint64_t degree(uint64_t v) const {
      auto it = tx->static_get_edges(internal[v], 1);
      return it.vertex_degree();
    }
    template <typename Fn>
    void TraverseEdges(uint64_t v, Fn&& fn) const {
      auto it = tx->static_get_edges(internal[v], 1);
      while (it.valid()) {
        auto r = internal_to_rank.find(it.dst_id());
        if (r != internal_to_rank.end()) fn(r->second);
      }
    }
    template <typename Fn>
    bool TraverseEdgesUntil(uint64_t v, Fn&& fn) const {
      auto it = tx->static_get_edges(internal[v], 1);
      while (it.valid()) {
        auto r = internal_to_rank.find(it.dst_id());
        if (r != internal_to_rank.end() && fn(r->second)) return true;
      }
      return false;
    }
    template <typename Fn>
    bool TraverseEdgesSorted(uint64_t, Fn&&) const {
      // GTX adjacency/delta iterators have no ordering contract.
      return false;
    }
    template <typename Fn>
    void TraverseEdgesWithProperty(uint64_t v, Fn&& fn) const {
      auto it = tx->static_get_edges(internal[v], 1);
      while (it.valid()) {
        auto r = internal_to_rank.find(it.dst_id());
        if (r != internal_to_rank.end()) fn(r->second, it.get_weight());
      }
    }
    uint64_t resolve_root(uint64_t external_id) const {
      for (uint64_t v = 0; v < external.size(); ++v)
        if (external[v] == external_id) return v;
      return std::numeric_limits<uint64_t>::max();
    }
  };

  CommonView BuildCommonView() const {
    CommonView view;
    view.tx = std::make_unique<gt::SharedROTransaction>(
        m_pImpl->begin_shared_read_only_transaction());
    const uint64_t max_vid = m_pImpl->get_max_allocated_vid();
    uint64_t rank = 0;
    for (uint64_t id = 1; id <= max_vid; ++id) {
      const auto payload = view.tx->static_get_vertex(id);
      if (!payload.empty() && payload.size() >= sizeof(uint64_t)) {
        view.internal.push_back(id);
        view.external.push_back(*reinterpret_cast<const uint64_t*>(payload.data()));
        view.internal_to_rank.emplace(id, rank++);
      }
    }
    return view;
  }

  bool SupportsCommonAlgorithms() const override { return true; }
  bool SupportsCommonAlgorithm(const std::string& algorithm) const override {
    return algorithm == "cdlp" || algorithm == "lcc" || algorithm == "pr" ||
           algorithm == "bfs" || algorithm == "sssp" || algorithm == "wcc";
  }
  bool RunCDLPCommon(int max_iters) override {
    auto view = BuildCommonView();
    auto labels = common::algorithms::cdlp(view, max_iters);
    if (ShouldCollectAlgorithmResults()) {
      last_result_.clear(); last_result_.type = AlgorithmResult::kLong;
      for (uint64_t v = 0; v < labels.size(); ++v)
        last_result_.long_vals.emplace_back(view.external_id(v), static_cast<int64_t>(labels[v]));
    }
    return true;
  }
  bool RunLCCCommon() override {
    auto view = BuildCommonView();
    auto values = common::algorithms::lcc(view);
    if (ShouldCollectAlgorithmResults()) {
      last_result_.clear(); last_result_.type = AlgorithmResult::kDouble;
      for (uint64_t v = 0; v < values.size(); ++v)
        last_result_.double_vals.emplace_back(view.external_id(v), values[v]);
    }
    return true;
  }
  bool RunPageRankCommon(int iterations, double damping) override {
    auto view = BuildCommonView();
    auto scores = common::algorithms::pagerank(view, iterations, damping);
    if (ShouldCollectAlgorithmResults()) {
      last_result_.clear(); last_result_.type = AlgorithmResult::kDouble;
      for (uint64_t v = 0; v < scores.size(); ++v)
        last_result_.double_vals.emplace_back(view.external_id(v), scores[v]);
    }
    return true;
  }
  bool RunBFSCommon(uint64_t root) override {
    auto view = BuildCommonView();
    const uint64_t r = view.resolve_root(root);
    if (r == std::numeric_limits<uint64_t>::max()) return false;
    auto dist = common::algorithms::bfs(view, r);
    if (ShouldCollectAlgorithmResults()) {
      last_result_.clear(); last_result_.type = AlgorithmResult::kLong;
      for (uint64_t v = 0; v < dist.size(); ++v)
        last_result_.long_vals.emplace_back(view.external_id(v), dist[v]);
    }
    return true;
  }
  bool RunSSSPCommon(uint64_t root) override {
    auto view = BuildCommonView();
    const uint64_t r = view.resolve_root(root);
    if (r == std::numeric_limits<uint64_t>::max()) return false;
    auto dist = common::algorithms::sssp_weighted(view, r);
    if (ShouldCollectAlgorithmResults()) {
      last_result_.clear(); last_result_.type = AlgorithmResult::kDouble;
      for (uint64_t v = 0; v < dist.size(); ++v)
        last_result_.double_vals.emplace_back(view.external_id(v), dist[v]);
    }
    return true;
  }
  bool RunWCCCommon() override {
    auto view = BuildCommonView();
    auto comp = common::algorithms::wcc(view);
    if (ShouldCollectAlgorithmResults()) {
      last_result_.clear(); last_result_.type = AlgorithmResult::kLong;
      for (uint64_t v = 0; v < comp.size(); ++v)
        last_result_.long_vals.emplace_back(view.external_id(v), static_cast<int64_t>(comp[v]));
    }
    return true;
  }

  // GTX has no native WCC/LCC/CDLP handler. Use an ordinary read-only
  // gt::ROTransaction per thread; never share it because its
  // accessed_edge_label_entry_cache is not atomic. This also avoids the opaque
  // handlers' SharedROTransaction and OpenMP worker slots, which are acquired
  // and released per parallel region. Decode external IDs from the vertex
  // payload written by UpsertEdge (reinterpret tx.get_vertex(v) as uint64_t);
  // no separate reverse map is maintained. EdgeDeltaIterator::valid() calls next()
  // internally in bind/GTX.cpp; do not call next() again in the loop body.

  void RunWCC() override {
    last_result_.clear();
    const uint64_t max_vid = m_pImpl->get_max_allocated_vid();
    if (max_vid == 0) return;

    std::vector<uint64_t> parent(max_vid + 1);
    for (uint64_t v = 1; v <= max_vid; v++) parent[v] = v;

    auto find = [&](uint64_t x) {
      while (parent[x] != x) {
        parent[x] = parent[parent[x]];  // path halving
        x = parent[x];
      }
      return x;
    };

    auto tx = m_pImpl->begin_read_only_transaction();
    for (uint64_t v = 1; v <= max_vid; v++) {
      auto it = tx.get_edges(v, 1);
      while (it.valid()) {
        uint64_t u = it.dst_id();
        uint64_t rv = find(v);
        uint64_t ru = find(u);
        if (ru != rv) parent[std::max(ru, rv)] = std::min(ru, rv);
      }
    }

    last_result_.type = AlgorithmResult::kLong;
    for (uint64_t v = 1; v <= max_vid; v++) {
      std::string_view payload = tx.get_vertex(v);
      if (payload.empty()) continue;
      uint64_t ext = *reinterpret_cast<const uint64_t*>(payload.data());
      last_result_.long_vals.emplace_back(ext, static_cast<int64_t>(find(v)));
    }
  }

  void RunLCC() override {
    last_result_.clear();
    const uint64_t max_vid = m_pImpl->get_max_allocated_vid();
    if (max_vid == 0) return;

    // The iterator does not guarantee sorted neighbors; collect, sort and deduplicate
    // locally before intersecting with two pointers, using common_sum / (deg*(deg-1)).
    std::vector<std::vector<uint64_t>> neighbors(max_vid + 1);
#pragma omp parallel
    {
      auto tx = m_pImpl->begin_read_only_transaction();
#pragma omp for schedule(dynamic, 64)
      for (uint64_t v = 1; v <= max_vid; v++) {
        auto& nbrs = neighbors[v];
        auto it = tx.get_edges(v, 1);
        while (it.valid()) {
          uint64_t u = it.dst_id();
          if (u != v) nbrs.push_back(u);
        }
        std::sort(nbrs.begin(), nbrs.end());
        nbrs.erase(std::unique(nbrs.begin(), nbrs.end()), nbrs.end());
      }
    }

    std::vector<double> lcc_scores(max_vid + 1, 0.0);
#pragma omp parallel for schedule(dynamic, 64)
    for (uint64_t v = 1; v <= max_vid; v++) {
      const auto& nv = neighbors[v];
      const uint64_t deg = nv.size();
      if (deg < 2) continue;
      uint64_t common_sum = 0;
      for (uint64_t u : nv) {
        const auto& nu = neighbors[u];
        size_t i = 0, j = 0;
        while (i < nv.size() && j < nu.size()) {
          if (nv[i] == nu[j]) { common_sum++; i++; j++; }
          else if (nv[i] < nu[j]) i++;
          else j++;
        }
      }
      lcc_scores[v] = static_cast<double>(common_sum) /
                     (static_cast<double>(deg) * (deg - 1));
    }

    last_result_.type = AlgorithmResult::kDouble;
    auto tx = m_pImpl->begin_read_only_transaction();
    for (uint64_t v = 1; v <= max_vid; v++) {
      std::string_view payload = tx.get_vertex(v);
      if (payload.empty()) continue;
      uint64_t ext = *reinterpret_cast<const uint64_t*>(payload.data());
      last_result_.double_vals.emplace_back(ext, lcc_scores[v]);
    }
  }

  void RunCDLP(int max_iters) override {
    last_result_.clear();
    const uint64_t max_vid = m_pImpl->get_max_allocated_vid();
    if (max_vid == 0) return;

    const uint64_t kMissing = std::numeric_limits<uint64_t>::max();
    std::vector<std::vector<uint64_t>> neighbors(max_vid + 1);
    std::vector<uint64_t> external_id(max_vid + 1, kMissing);

#pragma omp parallel
    {
      auto tx = m_pImpl->begin_read_only_transaction();
#pragma omp for schedule(dynamic, 64)
      for (uint64_t v = 1; v <= max_vid; v++) {
        std::string_view payload = tx.get_vertex(v);
        if (payload.empty()) continue;
        external_id[v] = *reinterpret_cast<const uint64_t*>(payload.data());
        auto& nbrs = neighbors[v];
        auto it = tx.get_edges(v, 1);
        while (it.valid()) {
          uint64_t u = it.dst_id();
          if (u != v) nbrs.push_back(u);
        }
      }
    }

    // Initialize labels with external IDs: tie-breaking compares numeric labels,
    // whereas internal allocation order generally differs from external ID order.
    std::vector<uint64_t> labels(max_vid + 1);
    for (uint64_t v = 1; v <= max_vid; v++) labels[v] = external_id[v];
    std::vector<uint64_t> next_labels(max_vid + 1);

    for (int iter = 0; iter < max_iters; iter++) {
      std::atomic<bool> changed{false};
#pragma omp parallel
      {
        std::vector<uint64_t> nbr_labels;
#pragma omp for schedule(dynamic, 64)
        for (uint64_t v = 1; v <= max_vid; v++) {
          if (external_id[v] == kMissing) { next_labels[v] = labels[v]; continue; }
          const auto& nbrs = neighbors[v];
          if (nbrs.empty()) { next_labels[v] = labels[v]; continue; }

          // Sort collected labels explicitly before counting equal-label runs.
          nbr_labels.clear();
          nbr_labels.reserve(nbrs.size());
          for (uint64_t u : nbrs) nbr_labels.push_back(labels[u]);
          std::sort(nbr_labels.begin(), nbr_labels.end());

          uint64_t best_label = nbr_labels[0];
          uint64_t best_count = 0;
          size_t i = 0;
          while (i < nbr_labels.size()) {
            size_t j = i;
            while (j < nbr_labels.size() && nbr_labels[j] == nbr_labels[i]) j++;
            uint64_t count = j - i;
            if (count > best_count) { best_count = count; best_label = nbr_labels[i]; }
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
      if (!changed.load()) break;
    }

    last_result_.type = AlgorithmResult::kLong;
    for (uint64_t v = 1; v <= max_vid; v++) {
      if (external_id[v] == kMissing) continue;
      last_result_.long_vals.emplace_back(external_id[v],
                                          static_cast<int64_t>(labels[v]));
    }
  }

  bool SupportsAlgorithm(const std::string& algorithm) const override {
    return algorithm == "pr" || algorithm == "bfs" || algorithm == "sssp" ||
           algorithm == "wcc" || algorithm == "lcc" || algorithm == "cdlp";
  }
};
