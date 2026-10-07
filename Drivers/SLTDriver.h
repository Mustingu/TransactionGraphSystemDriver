#pragma once
#include <atomic>
#include <limits>
#include <optional>
#include <thread>
#include <vector>

#include "../GraphDriver.h"
#include "../common/algorithms/GraphKernels.h"
#include "data-structure/VersionedBlockedEdgeIterator.h"
#include "data-structure/VersionedBlockedPropertyEdgeIterator.h"

// SLT headers.
#include "data-structure/EdgeDoesNotExistsPrecondition.h"
#include "data-structure/TransactionManager.h"
#include "data-structure/VersioningBlockedSkipListAdjacencyList.h"

// Backend algorithm implementations.
#include "algorithms/CDLP.h"
#include "algorithms/GAPBSAlgorithms.h"
#include "algorithms/LCC.h"
#include "algorithms/PageRank.h"
#include "algorithms/SSSP.h"
#include "algorithms/WCC.h"

class SLTDriver : public GraphDriver {
 private:
  static constexpr uint64_t kNoFallbackRoot =
      std::numeric_limits<uint64_t>::max();
  std::atomic<uint64_t> fallback_root_{kNoFallbackRoot};
  std::atomic<bool> fallback_root_notified_{false};
  AlgorithmResult last_result_;

  std::optional<uint64_t> ResolveReadRoot(SnapshotTransaction& tx,
                                          uint64_t requested_root) {
    if (tx.has_vertex(requested_root)) return requested_root;

    uint64_t fallback_root = fallback_root_.load(std::memory_order_acquire);
    if (fallback_root != kNoFallbackRoot && tx.has_vertex(fallback_root)) {
      bool expected = false;
      if (fallback_root_notified_.compare_exchange_strong(
              expected, true, std::memory_order_relaxed,
              std::memory_order_relaxed)) {
        std::cout << "[SLT] Requested root " << requested_root
                  << " is absent; using loaded root " << fallback_root << '\n';
      }
      return fallback_root;
    }

    std::cerr << "[SLT] No valid BFS/SSSP root is available. Requested root "
              << requested_root << '\n';
    return std::nullopt;
  }

  template <typename T>
  void WriteResultToFile(const std::vector<std::pair<uint64_t, T>>& result,
                         const std::string& filename) {
    // Sort by ID.
    std::vector<std::pair<uint64_t, T>> sorted_result = result;
    std::sort(sorted_result.begin(), sorted_result.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Write results to a file.
    std::ofstream ofs(filename);
    if (!ofs.is_open()) {
      std::cerr << "[SLT] Failed to open output file: " << filename
                << std::endl;
      return;
    }

    for (const auto& [id, value] : sorted_result) {
      ofs << id << "\t" << value << "\n";
    }
    ofs.close();
    std::cout << "[SLT] Result written to " << filename << " ("
              << sorted_result.size() << " vertices)" << std::endl;
  }

  // Per-thread registration state.
  // SLT requires register_thread before a thread accesses the system.
  // Use thread_local state to register each thread only once.
  static inline thread_local bool is_thread_registered = false;
  static vector<pair<uint64_t, uint>> translate_bfs(SnapshotTransaction& tx,
                                                    pvector<int64_t>& values) {
    auto N = values.size();

    vector<pair<vertex_id_t, uint>> logical_result(N);

#pragma omp parallel for
    for (uint v = 0; v < N; v++) {
      if (tx.has_vertex_p(v)) {
        if (values[v] >= 0) {
          logical_result[v] = make_pair(tx.logical_id(v), values[v]);
        } else {
          logical_result[v] =
              make_pair(tx.logical_id(v), numeric_limits<uint>::max());
        }
      } else {
        logical_result[v] = make_pair(v, numeric_limits<uint>::max());
      }
    }
    return logical_result;
  }

  struct CommonView {
    SnapshotTransaction* tx;
    uint64_t vertex_count() const { return tx->max_physical_vertex(); }
    uint64_t external_id(uint64_t v) const { return tx->logical_id(v); }
    uint64_t degree(uint64_t v) const { return tx->neighbourhood_size_p(v); }
    template <typename Fn>
    void TraverseEdges(uint64_t v, Fn&& fn) const {
      // The iterator is read-only and bound to this snapshot version. The
      // enclosing algorithm owns the transaction until all traversal ends.
      SORTLEDTON_ITERATE((*tx), v, { fn(static_cast<uint64_t>(e)); });
    }
    template <typename Fn>
    bool TraverseEdgesUntil(uint64_t v, Fn&& fn) const {
      bool stopped = false;
      SORTLEDTON_ITERATE_NAMED((*tx), v, e, end_traverse_until, {
        if (fn(static_cast<uint64_t>(e))) {
          stopped = true;
          goto end_traverse_until;
        }
      });
      return stopped;
    }
    template <typename Fn>
    bool TraverseEdgesSorted(uint64_t v, Fn&& fn) const {
      // EdgeBlock inserts with upper_bound and the skip-list iterator visits
      // blocks in key order.  The snapshot iterator therefore exposes the
      // visible physical neighbor IDs in sorted order.
      SORTLEDTON_ITERATE((*tx), v, { fn(static_cast<uint64_t>(e)); });
      return true;
    }
    template <typename Fn>
    void TraverseEdgesWithProperty(uint64_t v, Fn&& fn) const {
      SORTLEDTON_ITERATE_WITH_PROPERTIES_NAMED((*tx), v, e, w, end_iteration, {
        fn(static_cast<uint64_t>(e), static_cast<double>(w));
      });
    }
    uint64_t resolve_root(uint64_t external) const {
      const uint64_t n = tx->max_physical_vertex();
      for (uint64_t v = 0; v < n; v++)
        if (tx->has_vertex_p(v) && tx->logical_id(v) == external) return v;
      return std::numeric_limits<uint64_t>::max();
    }
  };

  template <typename T>
  std::vector<pair<uint64_t, T>> translate(SnapshotTransaction& tx,
                                           vector<T>& values) {
    int N = values.size();

    std::vector<pair<uint64_t, T>> logical_result(N);

#pragma omp parallel for
    for (uint v = 0; v < N; v++) {
      if (tx.has_vertex_p(v)) {
        logical_result[v] = make_pair(tx.logical_id(v), values[v]);
      } else {
        logical_result[v] = make_pair(v, numeric_limits<T>::max());
      }
    }
    return logical_result;
  }

 public:
  TransactionManager* tm = nullptr;
  VersioningBlockedSkipListAdjacencyList* ds = nullptr;
  std::string Name() const override { return "Sortledton"; }

  const AlgorithmResult& GetLastResult() const override { return last_result_; }

  void RegisterThread(int thread_id) override {
    // SLT requires register_thread before a thread accesses the system.
    // Use thread_local state to register each thread only once.
    if (!is_thread_registered) {
      tm->register_thread(thread_id);
      is_thread_registered = true;
    }
  }

  // Loading and mixed phases spawn separate workers that reuse integer thread IDs
  // (0..wt-1). TransactionManager keeps a persistent thread_id_in_use registry.
  // Without deregistration at the end of loading, registering those IDs again
  // throws IllegalOperation("Trying to reuse a thread id."). Deregistration requires
  // no active transaction; transactionCompleted leaves active_snapshots[id] == NO_TRANSACTION.
  void DeregisterThread(int thread_id) override {
    if (is_thread_registered) {
      tm->deregister_thread(thread_id);
      is_thread_registered = false;
    }
  }
  void Init(int thread_num) override {
    // Keep one extra transaction-manager slot beyond the 64 worker slots.
    tm = new TransactionManager(65);

    ds = new VersioningBlockedSkipListAdjacencyList(512, 8, *tm);

    printf("[SLT] Initialized.\n");
  }

  bool UpsertEdge(uint64_t src, uint64_t dst, uint64_t property,
                  int thread_id) override {
    thread_local std::optional<SnapshotTransaction> tls_tx = std::nullopt;
    if (tls_tx.has_value()) {
      tm->getSnapshotTransaction(ds, true, *tls_tx);
    } else {
      tls_tx = tm->getSnapshotTransaction(ds, true);
    }

    auto tx = *tls_tx;

    // Configure transaction semantics.
    tx.use_vertex_does_not_exists_semantics();

    // Insert vertices and edges.
    tx.insert_vertex(src);
    tx.insert_vertex(dst);

    edge_t edge{static_cast<dst_t>(src), static_cast<dst_t>(dst)};
    tx.insert_or_update_edge(edge, (char*)&property, sizeof(property));
    // Store both directions for the driver's undirected graph semantics.
    tx.insert_or_update_edge({edge.dst, edge.src}, (char*)&property,
                             sizeof(property));

    tx.execute();
    tm->transactionCompleted(tx);
    uint64_t expected_root = kNoFallbackRoot;
    fallback_root_.compare_exchange_strong(expected_root, src,
                                           std::memory_order_release,
                                           std::memory_order_relaxed);

    return true;
  }


  // delete_edge enqueues a bidirectional deletion for execute(). Missing edges
  // normally cause EdgeDoesNotExistsException at execution, so enable the
  // ignore-missing-edge semantics and retain the defensive exception handler.
  bool DeleteEdge(uint64_t src, uint64_t dst, int thread_id) override {
    thread_local std::optional<SnapshotTransaction> tls_tx = std::nullopt;
    if (tls_tx.has_value()) {
      tm->getSnapshotTransaction(ds, true, *tls_tx);
    } else {
      tls_tx = tm->getSnapshotTransaction(ds, true);
    }
    auto tx = *tls_tx;

    tx.use_edge_does_not_exists_semantics();

    tx.delete_edge(edge_t{static_cast<dst_t>(src), static_cast<dst_t>(dst)});
    tx.delete_edge(edge_t{static_cast<dst_t>(dst), static_cast<dst_t>(src)});

    try {
      tx.execute();
    } catch (const std::exception& e) {
      std::cout << "[SLT] Exception in DeleteEdge: " << e.what() << std::endl;
    }
    tm->transactionCompleted(tx);
    return true;
  }

  void FinishWrites() override {
    // Transactions are completed per operation; no extra finalization is needed.
  }

  void RunPageRank(int iterations, double damping_factor,
                   S_Driver* global_counter) override {
    // Use a read-only snapshot transaction for the algorithm.
    // The invoking thread must be registered, including concurrent reader threads.
    // RegisterThread(65);
    SnapshotTransaction tx = tm->getSnapshotTransaction(ds, false);

    try {
      // The native PageRank implementation handles its OpenMP parallel regions.
      // The caller configures the number of threads before invoking this method.
      // Workers access the shared snapshot through the native algorithm;
      // the invoking thread owns transaction registration and completion.
      // No additional thread registration is performed in this method.

      // PageRank returns one score per physical vertex. Translate those
      // positions back to the logical/external IDs before exposing them via
      // the common driver result interface.
      auto scores = PageRank::page_rank_bs(tx, iterations, damping_factor);
      if (ShouldCollectAlgorithmResults()) {
        auto external_ids = translate<double>(tx, scores);
        last_result_.clear();
        last_result_.type = AlgorithmResult::kDouble;
        last_result_.double_vals = std::move(external_ids);
      }
    } catch (const std::exception& e) {
      std::cout << "[SLT] Exception in PageRank: " << e.what() << std::endl;
      if (ShouldCollectAlgorithmResults()) last_result_.clear();
    }

    tm->transactionCompleted(tx);
  }

  void RunBFS(uint64_t root, S_Driver* global_counter) override {
    SnapshotTransaction tx = tm->getSnapshotTransaction(ds, false);

    try {
      auto logical_root = ResolveReadRoot(tx, root);
      if (logical_root.has_value()) {
        auto physical_src = tx.physical_id(*logical_root);
        auto distances = GAPBSAlgorithms::bfs(tx, physical_src, false);
        if (ShouldCollectAlgorithmResults()) {
          auto external_ids = translate_bfs(tx, distances);
          last_result_.clear();
          last_result_.type = AlgorithmResult::kLong;
          for (const auto& p : external_ids)
            last_result_.long_vals.emplace_back(p.first, p.second);
        }
      }
    } catch (const std::exception& e) {
      std::cerr << "[SLT] Exception in BFS: " << e.what() << std::endl;
      if (ShouldCollectAlgorithmResults()) last_result_.clear();
    }

    tm->transactionCompleted(tx);
  }

  void RunSSSP(uint64_t root, S_Driver* global_counter) override {
    SnapshotTransaction tx = tm->getSnapshotTransaction(ds, false);

    try {
      auto logical_root = ResolveReadRoot(tx, root);
      if (logical_root.has_value()) {
        auto physical_src = tx.physical_id(*logical_root);
        auto distances = SSSP::gabbs_sssp(tx, physical_src, 2.0);
        if (ShouldCollectAlgorithmResults()) {
          auto external_ids = translate<double>(tx, distances);
          last_result_.clear();
          last_result_.type = AlgorithmResult::kDouble;
          last_result_.double_vals = std::move(external_ids);
        }
      }
    } catch (const std::exception& e) {
      std::cerr << "[SLT] Exception in SSSP: " << e.what() << std::endl;
      if (ShouldCollectAlgorithmResults()) last_result_.clear();
    }

    tm->transactionCompleted(tx);
  }

  void RunWCC() override {
    SnapshotTransaction tx = tm->getSnapshotTransaction(ds, false);
    try {
      auto components = WCC::gapbs_wcc(tx);
      auto external_ids = translate<vertex_id_t>(tx, components);
      last_result_.clear();
      last_result_.type = AlgorithmResult::kLong;
      for (const auto& p : external_ids)
        last_result_.long_vals.emplace_back(p.first, static_cast<int64_t>(p.second));
    } catch (const std::exception& e) {
      std::cerr << "[SLT] Exception in WCC: " << e.what() << std::endl;
      last_result_.clear();
    }
    tm->transactionCompleted(tx);
  }

  void RunLCC() override {
    SnapshotTransaction tx = tm->getSnapshotTransaction(ds, false);
    try {
      auto lcc_values = LCC::lcc_merge_sort(tx);
      auto external_ids = translate<double>(tx, lcc_values);
      last_result_.clear();
      last_result_.type = AlgorithmResult::kDouble;
      last_result_.double_vals = std::move(external_ids);
    } catch (const std::exception& e) {
      std::cerr << "[SLT] Exception in LCC: " << e.what() << std::endl;
      last_result_.clear();
    }
    tm->transactionCompleted(tx);
  }

  bool SupportsCommonAlgorithms() const override { return true; }
  bool SupportsCommonAlgorithm(const std::string& algorithm) const override {
    return algorithm == "cdlp" || algorithm == "lcc" || algorithm == "pr" ||
           algorithm == "bfs" || algorithm == "sssp" || algorithm == "wcc";
  }

  bool RunLCCCommon() override {
    SnapshotTransaction tx = tm->getSnapshotTransaction(ds, false);
    try {
      auto values = common::algorithms::lcc(CommonView{&tx});
      if (ShouldCollectAlgorithmResults()) {
        auto external_ids = translate<double>(tx, values);
        last_result_.clear();
        last_result_.type = AlgorithmResult::kDouble;
        last_result_.double_vals = std::move(external_ids);
      }
      tm->transactionCompleted(tx);
      return true;
    } catch (const std::exception& e) {
      std::cerr << "[SLT] Exception in common LCC: " << e.what() << std::endl;
      tm->transactionCompleted(tx);
      if (ShouldCollectAlgorithmResults()) last_result_.clear();
      return false;
    }
  }

  bool RunCDLPCommon(int max_iters) override {
    SnapshotTransaction tx = tm->getSnapshotTransaction(ds, false);
    try {
      auto labels = common::algorithms::cdlp(CommonView{&tx}, max_iters);
      if (ShouldCollectAlgorithmResults()) {
        last_result_.clear();
        last_result_.type = AlgorithmResult::kLong;
        for (uint64_t v = 0; v < labels.size(); v++)
          last_result_.long_vals.emplace_back(tx.logical_id(v),
                                              static_cast<int64_t>(labels[v]));
      }
      tm->transactionCompleted(tx);
      return true;
    } catch (const std::exception& e) {
      std::cerr << "[SLT] Exception in common CDLP: " << e.what() << std::endl;
      tm->transactionCompleted(tx);
      if (ShouldCollectAlgorithmResults()) last_result_.clear();
      return false;
    }
  }

  bool RunPageRankCommon(int iterations, double damping) override {
    SnapshotTransaction tx = tm->getSnapshotTransaction(ds, false);
    try {
      auto scores = common::algorithms::pagerank(CommonView{&tx}, iterations, damping);
      if (ShouldCollectAlgorithmResults()) {
        auto external_ids = translate<double>(tx, scores);
        last_result_.clear();
        last_result_.type = AlgorithmResult::kDouble;
        last_result_.double_vals = std::move(external_ids);
      }
      tm->transactionCompleted(tx);
      return true;
    } catch (const std::exception& e) {
      std::cerr << "[SLT] Exception in common PR: " << e.what() << std::endl;
      tm->transactionCompleted(tx);
      if (ShouldCollectAlgorithmResults()) last_result_.clear();
      return false;
    }
  }

  bool RunBFSCommon(uint64_t root) override {
    SnapshotTransaction tx = tm->getSnapshotTransaction(ds, false);
    try {
      CommonView view{&tx};
      const uint64_t r = view.resolve_root(root);
      if (r == std::numeric_limits<uint64_t>::max()) {
        tm->transactionCompleted(tx);
        return false;
      }
      auto dist = common::algorithms::bfs(view, r);
      if (ShouldCollectAlgorithmResults()) {
        last_result_.clear();
        last_result_.type = AlgorithmResult::kLong;
        for (uint64_t v = 0; v < dist.size(); v++)
          last_result_.long_vals.emplace_back(tx.logical_id(v), dist[v]);
      }
      tm->transactionCompleted(tx);
      return true;
    } catch (const std::exception& e) {
      std::cerr << "[SLT] Exception in common BFS: " << e.what() << std::endl;
      tm->transactionCompleted(tx);
      if (ShouldCollectAlgorithmResults()) last_result_.clear();
      return false;
    }
  }

  bool RunSSSPCommon(uint64_t root) override {
    SnapshotTransaction tx = tm->getSnapshotTransaction(ds, false);
    try {
      CommonView view{&tx};
      const uint64_t r = view.resolve_root(root);
      if (r == std::numeric_limits<uint64_t>::max()) {
        tm->transactionCompleted(tx);
        return false;
      }
      auto dist = common::algorithms::sssp_weighted(view, r);
      if (ShouldCollectAlgorithmResults()) {
        last_result_.clear();
        last_result_.type = AlgorithmResult::kDouble;
        for (uint64_t v = 0; v < dist.size(); v++)
          last_result_.double_vals.emplace_back(tx.logical_id(v), dist[v]);
      }
      tm->transactionCompleted(tx);
      return true;
    } catch (const std::exception& e) {
      std::cerr << "[SLT] Exception in common SSSP: " << e.what() << std::endl;
      tm->transactionCompleted(tx);
      if (ShouldCollectAlgorithmResults()) last_result_.clear();
      return false;
    }
  }

  bool RunWCCCommon() override {
    SnapshotTransaction tx = tm->getSnapshotTransaction(ds, false);
    try {
      auto comp = common::algorithms::wcc(CommonView{&tx});
      if (ShouldCollectAlgorithmResults()) {
        last_result_.clear();
        last_result_.type = AlgorithmResult::kLong;
        for (uint64_t v = 0; v < comp.size(); v++)
          last_result_.long_vals.emplace_back(tx.logical_id(v),
                                              static_cast<int64_t>(comp[v]));
      }
      tm->transactionCompleted(tx);
      return true;
    } catch (const std::exception& e) {
      std::cerr << "[SLT] Exception in common WCC: " << e.what() << std::endl;
      tm->transactionCompleted(tx);
      if (ShouldCollectAlgorithmResults()) last_result_.clear();
      return false;
    }
  }

  void RunCDLP(int max_iters) override {
    SnapshotTransaction tx = tm->getSnapshotTransaction(ds, false);
    try {
      auto labels = CDLP::teseo_cdlp(tx, max_iters);
      auto external_ids = translate<vertex_id_t>(tx, labels);
      last_result_.clear();
      last_result_.type = AlgorithmResult::kLong;
      for (const auto& p : external_ids)
        last_result_.long_vals.emplace_back(p.first, static_cast<int64_t>(p.second));
    } catch (const std::exception& e) {
      std::cerr << "[SLT] Exception in CDLP: " << e.what() << std::endl;
      last_result_.clear();
    }
    tm->transactionCompleted(tx);
  }

  bool SupportsAlgorithm(const std::string& algorithm) const override {
    return algorithm == "pr" || algorithm == "bfs" || algorithm == "sssp" ||
           algorithm == "wcc" || algorithm == "lcc" || algorithm == "cdlp";
  }
};
