#pragma once
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <omp.h>

#include "../GraphDriver.h"
#include "../common/algorithms/GraphKernels.h"
#include "livegraph.hpp"  // lg namespace declarations (systems/LiveGraph/bind).
#include "tbb/concurrent_hash_map.h"

// LiveGraph adapter: put_edge inserts or updates an edge; rolled-back
// transactions are retried with a new transaction.
class LiveGraphDriver : public GraphDriver {
 private:
  lg::Graph* m_pImpl;

  // External ID -> LiveGraph vertex_t
  using vertex_dictionary_t = tbb::concurrent_hash_map<uint64_t, uint64_t>;
  vertex_dictionary_t* m_pHashMap;

  std::atomic<int> m_num_vertices{0};
  mutable std::mutex reverse_map_mutex_;
  std::vector<uint64_t> internal_to_external_;
  // Transaction rollbacks can consume an ID before the retry succeeds. Keep
  // the actual live IDs instead of assuming they are 0..m_num_vertices-1.
  std::vector<uint64_t> internal_vertex_ids_;
  AlgorithmResult last_result_;

  // Batch loading is enabled during the --mix loading phase. Batch transactions
  // write directly, bypass conflict detection and commit processing (commit is a no-op).
  // The upstream test uses one batch transaction per worker; keep it thread_local.
  std::atomic<bool> bulk_{false};

  // Run body(thread_id) on each worker, as in RSDriver.
  template <typename F>
  static void RunThreads(int num_threads, F&& body) {
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++)
      threads.emplace_back([&, i]() { body(i); });
    for (auto& t : threads) t.join();
  }

 public:
  std::string Name() const override { return "LiveGraph"; }

  const AlgorithmResult& GetLastResult() const override { return last_result_; }

  void Init(int thread_num) override {
    m_pImpl = new lg::Graph();  // An empty path selects an in-memory graph.
    m_pHashMap = new vertex_dictionary_t();
    printf("[LiveGraph] Initialized.\n");
  }

  bool UpsertEdge(uint64_t src, uint64_t dst, uint64_t property,
                  int thread_id) override {
    // As in GTXDriver, the hash map determines whether this transaction must create a vertex.
    uint64_t internal_source_id = std::numeric_limits<uint64_t>::max();
    uint64_t internal_destination_id = 0;
    bool insert_source = false;
    bool insert_destination = false;
    vertex_dictionary_t::const_accessor slock1, slock2;
    vertex_dictionary_t::accessor xlock1, xlock2;
    if (m_pHashMap->find(slock1, src)) {
      internal_source_id = slock1->second;
    } else {
      slock1.release();
      if (m_pHashMap->insert(xlock1, src))
        insert_source = true;
      else
        internal_source_id = xlock1->second;
    }

    if (m_pHashMap->find(slock2, dst)) {
      internal_destination_id = slock2->second;
    } else {
      slock2.release();
      if (m_pHashMap->insert(xlock2, dst))
        insert_destination = true;
      else
        internal_destination_id = xlock2->second;
    }

    bool done = false;
    if (bulk_.load()) {
      // Each worker writes directly through its private batch transaction, without
      // conflict detection or RollbackExcept. lg::Transaction has a user-declared
      // destructor and cannot be moved; use a pointer with C++17 copy elision.
      thread_local lg::Transaction* bulk_tx = nullptr;
      if (!bulk_tx)
        bulk_tx = new lg::Transaction(m_pImpl->begin_batch_loader());
      lg::Transaction& tx = *bulk_tx;
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
      std::string_view weight{(char*)&property, sizeof(property)};
      tx.put_edge(internal_source_id, 1, internal_destination_id, weight);
      tx.put_edge(internal_destination_id, 1, internal_source_id, weight);
      // Batch commit() is a no-op because writes are already applied.
      done = true;
    }
    if (!done) do {
      auto tx = m_pImpl->begin_transaction();
      try {
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
        // Insert both directions with label 1; store the weight as an 8-byte bit pattern.
        std::string_view weight{(char*)&property, sizeof(property)};
        tx.put_edge(internal_source_id, 1, internal_destination_id, weight);
        tx.put_edge(internal_destination_id, 1, internal_source_id, weight);
        // Wait for publication so the following static reader cannot capture a
        // stale epoch while the commit-manager server is still advancing it.
        tx.commit(true);
        done = true;
      } catch (lg::Transaction::RollbackExcept& e) {
        tx.abort();
        // retry
      }
    } while (!done);

    if (insert_source) {
      xlock1->second = internal_source_id;
      RecordExternalID(internal_source_id, src);
      m_num_vertices++;
    }
    if (insert_destination) {
      xlock2->second = internal_destination_id;
      RecordExternalID(internal_destination_id, dst);
      m_num_vertices++;
    }
    return true;
  }


  // Delete both directions; missing edges return false, while RollbackExcept is retried.
  bool DeleteEdge(uint64_t src, uint64_t dst, int thread_id) override {
    uint64_t s = GetInternalID(src), d = GetInternalID(dst);
    if (s == std::numeric_limits<uint64_t>::max() ||
        d == std::numeric_limits<uint64_t>::max())
      return false;
    bool done = false;
    do {
      auto tx = m_pImpl->begin_transaction();
      try {
        tx.del_edge(s, 1, d);
        tx.del_edge(d, 1, s);
        tx.commit(true);
        done = true;
      } catch (lg::Transaction::RollbackExcept& e) {
        tx.abort();
        // retry
      }
    } while (!done);
    return true;
  }

  void BeginBulkLoad() override { bulk_.store(true); }
  void EndBulkLoad() override { bulk_.store(false); }

  uint64_t GetInternalID(uint64_t external) {
    vertex_dictionary_t::const_accessor a;
    if (m_pHashMap->find(a, external)) return a->second;
    return std::numeric_limits<uint64_t>::max();
  }

  void RecordExternalID(uint64_t internal, uint64_t external) {
    std::lock_guard<std::mutex> lock(reverse_map_mutex_);
    if (internal_to_external_.size() <= internal) {
      internal_to_external_.resize(
          internal + 1, std::numeric_limits<uint64_t>::max());
    }
    internal_to_external_[internal] = external;
    if (std::find(internal_vertex_ids_.begin(), internal_vertex_ids_.end(),
                  internal) == internal_vertex_ids_.end()) {
      internal_vertex_ids_.push_back(internal);
    }
  }

  std::vector<uint64_t> SnapshotInternalIDs() const {
    std::lock_guard<std::mutex> lock(reverse_map_mutex_);
    auto ids = internal_vertex_ids_;
    std::sort(ids.begin(), ids.end());
    return ids;
  }

  uint64_t GetExternalID(uint64_t internal) const {
    std::lock_guard<std::mutex> lock(reverse_map_mutex_);
    if (internal >= internal_to_external_.size())
      return std::numeric_limits<uint64_t>::max();
    return internal_to_external_[internal];
  }

  void RunPageRank(int iterations, double damping_factor,
                   S_Driver* global_counter) override {
    const auto vertices = SnapshotInternalIDs();
    const uint64_t n = vertices.size();
    if (n == 0) return;
    const uint64_t max_internal_id = vertices.back();
    const size_t storage_size = static_cast<size_t>(max_internal_id) + 1;
    const int num_threads = omp_get_max_threads();
    std::vector<double> scores(storage_size, 0.0);
    std::vector<double> new_scores(storage_size, 0.0);
    for (uint64_t v : vertices) scores[v] = 1.0 / n;

    for (int iter = 0; iter < iterations; iter++) {
      std::vector<double> contrib(storage_size, 0.0);
      std::vector<double> dangling_sums(num_threads, 0.0);

      // Phase 1: degrees, contributions and dangling mass.
      RunThreads(num_threads, [&](int tid) {
        auto tx = m_pImpl->begin_read_only_transaction();
        for (uint64_t i = tid; i < n; i += num_threads) {
          uint64_t v = vertices[i];
          uint64_t degree = 0;
          auto it = tx.get_edges(v, 1);
          while (it.valid()) {
            degree++;
            it.next();
          }
          if (degree == 0)
            dangling_sums[tid] += scores[v];
          else
            contrib[v] = scores[v] / degree;
        }
      });

      double dangling = 0.0;
      for (double d : dangling_sums) dangling += d;
      dangling /= n;
      const double base = (1.0 - damping_factor) / n;

      // Phase 2: gather contributions.
      RunThreads(num_threads, [&](int tid) {
        auto tx = m_pImpl->begin_read_only_transaction();
        for (uint64_t i = tid; i < n; i += num_threads) {
          uint64_t v = vertices[i];
          double incoming = 0.0;
          auto it = tx.get_edges(v, 1);
          while (it.valid()) {
            const uint64_t dst = it.dst_id();
            if (dst < storage_size) incoming += contrib[dst];
            it.next();
          }
          new_scores[v] = base + damping_factor * (incoming + dangling);
        }
      });
      scores.swap(new_scores);
    }

    last_result_.clear();
    last_result_.type = AlgorithmResult::kDouble;
    for (uint64_t v : vertices)
      last_result_.double_vals.emplace_back(GetExternalID(v), scores[v]);
  }

  void RunBFS(uint64_t root, S_Driver* global_counter) override {
    uint64_t r = GetInternalID(root);
    if (r == std::numeric_limits<uint64_t>::max()) {
      printf("BFS Root %lu not found!\n", (unsigned long)root);
      return;
    }
    const auto vertices = SnapshotInternalIDs();
    const uint64_t n = vertices.size();
    if (n == 0) return;
    const uint64_t max_internal_id = vertices.back();
    const size_t storage_size = static_cast<size_t>(max_internal_id) + 1;
    const int num_threads = omp_get_max_threads();

    std::vector<std::atomic<int64_t>> dist(storage_size);
    for (uint64_t v : vertices) dist[v].store(-1);
    dist[r].store(0);

    std::vector<uint64_t> frontier{r};
    int64_t level = 0;
    while (!frontier.empty()) {
      level++;
      std::vector<std::vector<uint64_t>> next_buckets(num_threads);
      RunThreads(num_threads, [&](int tid) {
        auto tx = m_pImpl->begin_read_only_transaction();
        for (uint64_t i = tid; i < frontier.size(); i += num_threads) {
          uint64_t u = frontier[i];
          auto it = tx.get_edges(u, 1);
          while (it.valid()) {
            uint64_t v = it.dst_id();
            if (v < storage_size) {
              int64_t expected = -1;
              if (dist[v].compare_exchange_strong(expected, level))
                next_buckets[tid].push_back(v);
            }
            it.next();
          }
        }
      });
      frontier.clear();
      for (auto& bucket : next_buckets)
        for (uint64_t v : bucket) frontier.push_back(v);
    }

    last_result_.clear();
    last_result_.type = AlgorithmResult::kLong;
    for (uint64_t v : vertices)
      last_result_.long_vals.emplace_back(GetExternalID(v), dist[v].load());
  }

  void RunSSSP(uint64_t root, S_Driver* global_counter) override {
    const auto vertices = SnapshotInternalIDs();
    const uint64_t n = vertices.size();
    if (n == 0) return;
    const uint64_t max_internal_id = vertices.back();
    const size_t storage_size = static_cast<size_t>(max_internal_id) + 1;
    uint64_t r = GetInternalID(root);
    if (r == std::numeric_limits<uint64_t>::max()) {
      printf("SSSP Root %lu not found!\n", (unsigned long)root);
      return;
    }
    const int num_threads = omp_get_max_threads();

    std::vector<std::atomic<double>> dist(storage_size);
    for (uint64_t v : vertices)
      dist[v].store(std::numeric_limits<double>::max());
    dist[r].store(0.0);

    // Bellman-Ford: each round uses a fresh read transaction so every edge
    // is read from the same committed graph snapshot.
    std::atomic<bool> changed{true};
    while (changed.load()) {
      changed.store(false);
      RunThreads(num_threads, [&](int tid) {
        auto tx = m_pImpl->begin_read_only_transaction();
        bool local_changed = false;
        for (uint64_t i = tid; i < n; i += num_threads) {
          uint64_t u = vertices[i];
          double du = dist[u].load();
          if (du == std::numeric_limits<double>::max()) continue;
          auto it = tx.get_edges(u, 1);
          while (it.valid()) {
            uint64_t v = it.dst_id();
            if (v < storage_size) {
              double w;
              std::memcpy(&w, it.edge_data().data(), sizeof(w));
              double desired = du + w;
              double cur = dist[v].load();
              bool relaxed = false;
              while (cur > desired) {
                if (dist[v].compare_exchange_weak(cur, desired)) {
                  relaxed = true;
                  break;
                }
                // On failure compare_exchange_weak refreshes cur. Recheck
                // the current distance before retrying instead of issuing a
                // second CAS after a successful relaxation.
              }
              if (relaxed) local_changed = true;
            }
            it.next();
          }
        }
        if (local_changed) changed = true;
      });
    }

    last_result_.clear();
    last_result_.type = AlgorithmResult::kDouble;
    for (uint64_t v : vertices)
      last_result_.double_vals.emplace_back(GetExternalID(v), dist[v].load());
  }

  // LiveGraph has no native WCC/LCC/CDLP implementation. As with PR/BFS/SSSP,
  // snapshot the vertex IDs, partition work with RunThreads, and create a separate
  // read-only transaction for each thread. lg::EdgeIterator::valid() is
  // a const peek, unlike GTX's advancing valid(); the loop must
  // explicitly call next() to avoid infinite iteration.
  // get_edges does not guarantee sorted neighbors. Collect and sort local
  // neighbor lists for LCC instead of assuming ordered traversal.

  void RunWCC() override {
    const auto vertices = SnapshotInternalIDs();
    const uint64_t n = vertices.size();
    last_result_.clear();
    if (n == 0) return;
    const uint64_t max_internal_id = vertices.back();
    const size_t storage_size = static_cast<size_t>(max_internal_id) + 1;

    std::vector<uint64_t> parent(storage_size);
    for (uint64_t v : vertices) parent[v] = v;

    auto find = [&](uint64_t x) {
      while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
      }
      return x;
    };

    auto tx = m_pImpl->begin_read_only_transaction();
    for (uint64_t v : vertices) {
      auto it = tx.get_edges(v, 1);
      while (it.valid()) {
        uint64_t u = it.dst_id();
        if (u < storage_size) {
          uint64_t rv = find(v);
          uint64_t ru = find(u);
          if (ru != rv) parent[std::max(ru, rv)] = std::min(ru, rv);
        }
        it.next();
      }
    }

    last_result_.type = AlgorithmResult::kLong;
    for (uint64_t v : vertices)
      last_result_.long_vals.emplace_back(GetExternalID(v),
                                          static_cast<int64_t>(find(v)));
  }

  void RunLCC() override {
    const auto vertices = SnapshotInternalIDs();
    const uint64_t n = vertices.size();
    last_result_.clear();
    if (n == 0) return;
    const uint64_t max_internal_id = vertices.back();
    const size_t storage_size = static_cast<size_t>(max_internal_id) + 1;
    const int num_threads = omp_get_max_threads();

    std::vector<std::vector<uint64_t>> neighbors(storage_size);
    RunThreads(num_threads, [&](int tid) {
      auto tx = m_pImpl->begin_read_only_transaction();
      for (uint64_t i = tid; i < n; i += num_threads) {
        uint64_t v = vertices[i];
        auto& nbrs = neighbors[v];
        auto it = tx.get_edges(v, 1);
        while (it.valid()) {
          uint64_t u = it.dst_id();
          if (u != v && u < storage_size) nbrs.push_back(u);
          it.next();
        }
        std::sort(nbrs.begin(), nbrs.end());
        nbrs.erase(std::unique(nbrs.begin(), nbrs.end()), nbrs.end());
      }
    });

    std::vector<double> lcc_scores(storage_size, 0.0);
    RunThreads(num_threads, [&](int tid) {
      for (uint64_t i = tid; i < n; i += num_threads) {
        uint64_t v = vertices[i];
        const auto& nv = neighbors[v];
        const uint64_t deg = nv.size();
        if (deg < 2) continue;
        uint64_t common_sum = 0;
        for (uint64_t u : nv) {
          const auto& nu = neighbors[u];
          size_t a = 0, b = 0;
          while (a < nv.size() && b < nu.size()) {
            if (nv[a] == nu[b]) { common_sum++; a++; b++; }
            else if (nv[a] < nu[b]) a++;
            else b++;
          }
        }
        lcc_scores[v] = static_cast<double>(common_sum) /
                       (static_cast<double>(deg) * (deg - 1));
      }
    });

    last_result_.type = AlgorithmResult::kDouble;
    for (uint64_t v : vertices)
      last_result_.double_vals.emplace_back(GetExternalID(v), lcc_scores[v]);
  }

  void RunCDLP(int max_iters) override {
    const auto vertices = SnapshotInternalIDs();
    const uint64_t n = vertices.size();
    last_result_.clear();
    if (n == 0) return;
    const uint64_t max_internal_id = vertices.back();
    const size_t storage_size = static_cast<size_t>(max_internal_id) + 1;
    const int num_threads = omp_get_max_threads();

    std::vector<std::vector<uint64_t>> neighbors(storage_size);
    RunThreads(num_threads, [&](int tid) {
      auto tx = m_pImpl->begin_read_only_transaction();
      for (uint64_t i = tid; i < n; i += num_threads) {
        uint64_t v = vertices[i];
        auto& nbrs = neighbors[v];
        auto it = tx.get_edges(v, 1);
        while (it.valid()) {
          uint64_t u = it.dst_id();
          if (u != v && u < storage_size) nbrs.push_back(u);
          it.next();
        }
      }
    });

    // Initialize labels with external IDs: tie-breaking compares numeric labels,
    // whereas internal allocation order differs from external ID order.
    std::vector<uint64_t> labels(storage_size);
    for (uint64_t v : vertices) labels[v] = GetExternalID(v);
    std::vector<uint64_t> next_labels(storage_size);

    for (int iter = 0; iter < max_iters; iter++) {
      std::atomic<bool> changed{false};
      RunThreads(num_threads, [&](int tid) {
        std::vector<uint64_t> nbr_labels;
        for (uint64_t i = tid; i < n; i += num_threads) {
          uint64_t v = vertices[i];
          const auto& nbrs = neighbors[v];
          if (nbrs.empty()) { next_labels[v] = labels[v]; continue; }

          // Sort collected labels explicitly before counting equal-label runs.
          nbr_labels.clear();
          nbr_labels.reserve(nbrs.size());
          for (uint64_t u : nbrs) nbr_labels.push_back(labels[u]);
          std::sort(nbr_labels.begin(), nbr_labels.end());

          uint64_t best_label = nbr_labels[0];
          uint64_t best_count = 0;
          size_t a = 0;
          while (a < nbr_labels.size()) {
            size_t b = a;
            while (b < nbr_labels.size() && nbr_labels[b] == nbr_labels[a]) b++;
            uint64_t count = b - a;
            if (count > best_count) { best_count = count; best_label = nbr_labels[a]; }
            a = b;
          }

          if (best_label != labels[v]) {
            changed.store(true);
            next_labels[v] = best_label;
          } else {
            next_labels[v] = labels[v];
          }
        }
      });
      std::swap(labels, next_labels);
      if (!changed.load()) break;
    }

    last_result_.type = AlgorithmResult::kLong;
    for (uint64_t v : vertices)
      last_result_.long_vals.emplace_back(GetExternalID(v),
                                          static_cast<int64_t>(labels[v]));
  }

  bool SupportsAlgorithm(const std::string& algorithm) const override {
    return algorithm == "pr" || algorithm == "bfs" || algorithm == "sssp" ||
           algorithm == "wcc" || algorithm == "lcc" || algorithm == "cdlp";
  }

  struct CommonView {
    std::unique_ptr<lg::Transaction> tx;
    std::vector<uint64_t> external;
    std::vector<uint64_t> internal;
    std::vector<uint64_t> dense_of;
    uint64_t vertex_count() const { return external.size(); }
    uint64_t external_id(uint64_t v) const { return external[v]; }
    uint64_t degree(uint64_t v) const {
      uint64_t n = 0;
      auto it = tx->get_edges(internal[v], 1);
      while (it.valid()) { ++n; it.next(); }
      return n;
    }
    template <typename Fn> void TraverseEdges(uint64_t v, Fn&& fn) const {
      auto it = tx->get_edges(internal[v], 1);
      while (it.valid()) {
        const auto dst = it.dst_id();
        if (dst < dense_of.size() && dense_of[dst] != std::numeric_limits<uint64_t>::max()) fn(dense_of[dst]);
        it.next();
      }
    }
    template <typename Fn> bool TraverseEdgesUntil(uint64_t v, Fn&& fn) const {
      auto it = tx->get_edges(internal[v], 1);
      while (it.valid()) {
        const auto dst = it.dst_id();
        if (dst < dense_of.size() &&
            dense_of[dst] != std::numeric_limits<uint64_t>::max() &&
            fn(dense_of[dst])) return true;
        it.next();
      }
      return false;
    }
    template <typename Fn> bool TraverseEdgesSorted(uint64_t, Fn&&) const {
      // LiveGraph get_edges() does not guarantee destination ordering.
      return false;
    }
    template <typename Fn> void TraverseEdgesWithProperty(uint64_t v, Fn&& fn) const {
      auto it = tx->get_edges(internal[v], 1);
      while (it.valid()) {
        const auto dst = it.dst_id();
        if (dst < dense_of.size() && dense_of[dst] != std::numeric_limits<uint64_t>::max()) {
          double weight = 0.0; auto data = it.edge_data();
          if (data.size() >= sizeof(weight)) std::memcpy(&weight, data.data(), sizeof(weight));
          fn(dense_of[dst], weight);
        }
        it.next();
      }
    }
    uint64_t resolve_root(uint64_t id) const { for (uint64_t v=0; v<external.size(); ++v) if (external[v]==id) return v; return std::numeric_limits<uint64_t>::max(); }
  };

  CommonView BuildCommonView() {
    CommonView view;
    const auto ids = SnapshotInternalIDs();
    const uint64_t max_id = ids.empty() ? 0 : ids.back();
    view.dense_of.assign(max_id + 1, std::numeric_limits<uint64_t>::max());
    for (auto id : ids) { view.dense_of[id] = view.external.size(); view.internal.push_back(id); view.external.push_back(GetExternalID(id)); }
    view.tx = std::make_unique<lg::Transaction>(m_pImpl->begin_read_only_transaction());
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
};
