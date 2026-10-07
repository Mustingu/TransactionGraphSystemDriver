#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

#include <omp.h>

#include "../GraphDriver.h"
#include "../common/algorithms/GraphKernels.h"

// Static CSR baseline for the six Graphalytics algorithms. FinalizeStaticLoad()
// builds the adjacency array once; subsequent algorithms read it immutably.
class CSRDriver : public GraphDriver {
 private:
  struct StagedEdge {
    uint64_t src;
    uint64_t dst;
    uint64_t weight_bits;  // raw bit pattern of a double, see EdgeReader.cpp
  };
  struct CSREdge {
    uint32_t nbr;
    double weight;
  };

  std::vector<std::vector<StagedEdge>> staging_;

  std::vector<uint64_t> internal_to_external_;  // internal id -> external id
  std::vector<uint64_t> offsets_;               // size V+1
  std::vector<CSREdge> adj_;                    // size E, sorted per vertex
  uint64_t num_vertices_ = 0;

  AlgorithmResult last_result_;

  uint64_t Degree(uint64_t v) const { return offsets_[v + 1] - offsets_[v]; }

  struct CommonView {
    const CSRDriver& driver;
    uint64_t vertex_count() const { return driver.num_vertices_; }
    uint64_t external_id(uint64_t v) const {
      return driver.internal_to_external_[v];
    }
    uint64_t degree(uint64_t v) const { return driver.Degree(v); }
    // Returns the dense id for an external root id, or UINT64_MAX if absent.
    uint64_t resolve_root(uint64_t external) const {
      auto it = std::lower_bound(driver.internal_to_external_.begin(),
                                 driver.internal_to_external_.end(), external);
      if (it == driver.internal_to_external_.end() || *it != external)
        return std::numeric_limits<uint64_t>::max();
      return static_cast<uint64_t>(it - driver.internal_to_external_.begin());
    }
    template <typename Fn>
    void TraverseEdges(uint64_t v, Fn&& fn) const {
      for (uint64_t k = driver.offsets_[v]; k < driver.offsets_[v + 1]; k++)
        fn(static_cast<uint64_t>(driver.adj_[k].nbr));
    }
    template <typename Fn>
    bool TraverseEdgesUntil(uint64_t v, Fn&& fn) const {
      for (uint64_t k = driver.offsets_[v]; k < driver.offsets_[v + 1]; k++)
        if (fn(static_cast<uint64_t>(driver.adj_[k].nbr))) return true;
      return false;
    }
    template <typename Fn>
    bool TraverseEdgesSorted(uint64_t v, Fn&& fn) const {
      // FinalizeStaticLoad() sorts and compacts every CSR row, so expose the
      // storage order directly instead of making common LCC collect and sort
      // the row again.
      for (uint64_t k = driver.offsets_[v]; k < driver.offsets_[v + 1]; k++)
        fn(static_cast<uint64_t>(driver.adj_[k].nbr));
      return true;
    }
    template <typename Fn>
    void TraverseEdgesWithProperty(uint64_t v, Fn&& fn) const {
      for (uint64_t k = driver.offsets_[v]; k < driver.offsets_[v + 1]; k++)
        fn(static_cast<uint64_t>(driver.adj_[k].nbr), driver.adj_[k].weight);
    }
  };

 public:
  std::string Name() const override { return "CSR"; }

  const AlgorithmResult& GetLastResult() const override { return last_result_; }

  bool SupportsAlgorithm(const std::string& algorithm) const override {
    return algorithm == "pr" || algorithm == "bfs" || algorithm == "sssp" ||
           algorithm == "wcc" || algorithm == "lcc" || algorithm == "cdlp";
  }

  void Init(int thread_num) override {
    staging_.clear();
    staging_.resize(std::max(thread_num, 1));
    internal_to_external_.clear();
    offsets_.clear();
    adj_.clear();
    num_vertices_ = 0;
    last_result_.clear();
    printf("[CSR] Initialized.\n");
  }

  bool UpsertEdge(uint64_t src, uint64_t dst, uint64_t property,
                  int thread_id) override {
    if (thread_id < 0) thread_id = 0;
    auto& buf = staging_[thread_id];
    buf.push_back({src, dst, property});
    buf.push_back({dst, src, property});  // symmetrize, same convention as
                                           // every other driver's UpsertEdge
    return true;
  }

  // Builds the CSR adjacency structure from all staged edges. This is the
  // single, one-shot construction point; the graph is read-only afterward.
  bool FinalizeStaticLoad() override {
    size_t total = 0;
    for (auto& buf : staging_) total += buf.size();

    // 1. Discover distinct external vertex ids and assign dense internal ids
    //    in ascending external-id order (a reasonable, low-risk choice for
    //    the WCC "min vertex id" convention used by the reference fixtures).
    std::vector<uint64_t> ids;
    ids.reserve(total);
    for (auto& buf : staging_)
      for (auto& e : buf) ids.push_back(e.src);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    internal_to_external_ = ids;
    num_vertices_ = ids.size();

    std::unordered_map<uint64_t, uint32_t> external_to_internal;
    external_to_internal.reserve(num_vertices_ * 2);
    for (uint32_t i = 0; i < num_vertices_; i++)
      external_to_internal[ids[i]] = i;

    // 2. Convert staged edges to internal ids and count degrees.
    std::vector<uint32_t> degree(num_vertices_, 0);
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> internal_edges(
        staging_.size());
    std::vector<std::vector<double>> internal_weights(staging_.size());
    for (size_t t = 0; t < staging_.size(); t++) {
      auto& buf = staging_[t];
      internal_edges[t].reserve(buf.size());
      internal_weights[t].reserve(buf.size());
      for (auto& e : buf) {
        uint32_t u = external_to_internal[e.src];
        uint32_t v = external_to_internal[e.dst];
        double w;
        std::memcpy(&w, &e.weight_bits, sizeof(w));
        internal_edges[t].emplace_back(u, v);
        internal_weights[t].push_back(w);
      }
      std::vector<StagedEdge>().swap(buf);  // free staged memory eagerly
    }
    for (auto& vec : internal_edges)
      for (auto& [u, v] : vec) degree[u]++;

    // 3. Prefix sum -> offsets, then bucket-fill the flat adjacency array.
    offsets_.assign(num_vertices_ + 1, 0);
    for (uint64_t v = 0; v < num_vertices_; v++)
      offsets_[v + 1] = offsets_[v] + degree[v];
    adj_.resize(offsets_[num_vertices_]);
    std::vector<uint64_t> cursor(offsets_.begin(), offsets_.end() - 1);
    for (size_t t = 0; t < internal_edges.size(); t++) {
      auto& vec = internal_edges[t];
      auto& wvec = internal_weights[t];
      for (size_t i = 0; i < vec.size(); i++) {
        auto [u, v] = vec[i];
        uint64_t pos = cursor[u]++;
        adj_[pos].nbr = v;
        adj_[pos].weight = wvec[i];
      }
    }
    internal_edges.clear();
    internal_weights.clear();

    // 4. Sort each vertex's adjacency by neighbor id, then collapse parallel
    //    edges keeping the last-staged weight (matches the upsert semantics
    //    every other driver's UpsertEdge already applies to repeated pairs).
#pragma omp parallel for schedule(dynamic, 256)
    for (uint64_t v = 0; v < num_vertices_; v++) {
      std::stable_sort(adj_.begin() + offsets_[v], adj_.begin() + offsets_[v + 1],
                        [](const CSREdge& a, const CSREdge& b) {
                          return a.nbr < b.nbr;
                        });
    }

    std::vector<uint64_t> new_offsets(num_vertices_ + 1, 0);
    std::vector<CSREdge> compact;
    compact.reserve(adj_.size());
    for (uint64_t v = 0; v < num_vertices_; v++) {
      new_offsets[v] = compact.size();
      uint64_t begin = offsets_[v], end = offsets_[v + 1];
      for (uint64_t i = begin; i < end;) {
        uint64_t j = i;
        while (j + 1 < end && adj_[j + 1].nbr == adj_[i].nbr) j++;
        compact.push_back(adj_[j]);  // stable_sort keeps last-staged order
        i = j + 1;
      }
    }
    new_offsets[num_vertices_] = compact.size();
    offsets_.swap(new_offsets);
    adj_.swap(compact);

    return true;
  }

  void RunPageRank(int iterations, double damping_factor,
                   S_Driver* global_counter = nullptr) override {
    const uint64_t n = num_vertices_;
    if (n == 0) return;
    std::vector<double> scores(n, 1.0 / n), new_scores(n, 0.0);
    std::vector<double> contrib(n, 0.0);

    for (int iter = 0; iter < iterations; iter++) {
      double dangling = 0.0;
#pragma omp parallel for reduction(+ : dangling) schedule(dynamic, 1024)
      for (uint64_t v = 0; v < n; v++) {
        uint64_t deg = Degree(v);
        if (deg == 0) {
          contrib[v] = 0.0;
          dangling += scores[v];
        } else {
          contrib[v] = scores[v] / deg;
        }
      }
      dangling /= n;
      const double base = (1.0 - damping_factor) / n;

#pragma omp parallel for schedule(dynamic, 1024)
      for (uint64_t v = 0; v < n; v++) {
        double incoming = 0.0;
        for (uint64_t k = offsets_[v]; k < offsets_[v + 1]; k++)
          incoming += contrib[adj_[k].nbr];
        new_scores[v] = base + damping_factor * (incoming + dangling);
      }
      scores.swap(new_scores);
    }

    last_result_.clear();
    last_result_.type = AlgorithmResult::kDouble;
    for (uint64_t v = 0; v < n; v++)
      last_result_.double_vals.emplace_back(internal_to_external_[v], scores[v]);
  }

  void RunBFS(uint64_t root, S_Driver* global_counter = nullptr) override {
    const uint64_t n = num_vertices_;
    if (n == 0) return;
    auto it = std::lower_bound(internal_to_external_.begin(),
                                internal_to_external_.end(), root);
    if (it == internal_to_external_.end() || *it != root) {
      printf("BFS Root %lu not found!\n", (unsigned long)root);
      return;
    }
    uint64_t r = it - internal_to_external_.begin();

    constexpr int64_t kUnreached = std::numeric_limits<int64_t>::max();
    std::vector<std::atomic<int64_t>> dist(n);
    for (uint64_t v = 0; v < n; v++) dist[v].store(kUnreached);
    dist[r].store(0);

    std::vector<uint64_t> frontier{r};
    int64_t level = 0;
    while (!frontier.empty()) {
      level++;
      std::vector<uint64_t> next;
      int num_threads = omp_get_max_threads();
      std::vector<std::vector<uint64_t>> local(num_threads);
#pragma omp parallel
      {
        int tid = omp_get_thread_num();
#pragma omp for schedule(dynamic, 256)
        for (size_t i = 0; i < frontier.size(); i++) {
          uint64_t u = frontier[i];
          for (uint64_t k = offsets_[u]; k < offsets_[u + 1]; k++) {
            uint64_t v = adj_[k].nbr;
            int64_t expected = kUnreached;
            if (dist[v].compare_exchange_strong(expected, level))
              local[tid].push_back(v);
          }
        }
      }
      for (auto& bucket : local)
        next.insert(next.end(), bucket.begin(), bucket.end());
      frontier.swap(next);
    }

    last_result_.clear();
    last_result_.type = AlgorithmResult::kLong;
    for (uint64_t v = 0; v < n; v++)
      last_result_.long_vals.emplace_back(internal_to_external_[v], dist[v].load());
  }

  void RunSSSP(uint64_t root, S_Driver* global_counter = nullptr) override {
    const uint64_t n = num_vertices_;
    if (n == 0) return;
    auto it = std::lower_bound(internal_to_external_.begin(),
                                internal_to_external_.end(), root);
    if (it == internal_to_external_.end() || *it != root) {
      printf("SSSP Root %lu not found!\n", (unsigned long)root);
      return;
    }
    uint64_t r = it - internal_to_external_.begin();

    const double kInf = std::numeric_limits<double>::infinity();
    std::vector<std::atomic<double>> dist(n);
    for (uint64_t v = 0; v < n; v++) dist[v].store(kInf);
    dist[r].store(0.0);

    // Parallel Bellman-Ford, same style as LiveGraph/Teseo's self-implemented
    // SSSP: relax every edge each round until no distance changes.
    bool changed = true;
    while (changed) {
      changed = false;
#pragma omp parallel for schedule(dynamic, 1024)
      for (uint64_t u = 0; u < n; u++) {
        double du = dist[u].load();
        if (du == kInf) continue;
        for (uint64_t k = offsets_[u]; k < offsets_[u + 1]; k++) {
          uint64_t v = adj_[k].nbr;
          double desired = du + adj_[k].weight;
          double cur = dist[v].load();
          while (cur > desired) {
            if (dist[v].compare_exchange_weak(cur, desired)) {
              changed = true;
              break;
            }
          }
        }
      }
    }

    last_result_.clear();
    last_result_.type = AlgorithmResult::kDouble;
    for (uint64_t v = 0; v < n; v++)
      last_result_.double_vals.emplace_back(internal_to_external_[v], dist[v].load());
  }

  void RunWCC() override {
    const uint64_t n = num_vertices_;
    if (n == 0) return;
    std::vector<uint32_t> parent(n);
    for (uint64_t v = 0; v < n; v++) parent[v] = v;

    auto find = [&](uint32_t x) {
      while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
      }
      return x;
    };

    for (uint64_t v = 0; v < n; v++) {
      for (uint64_t k = offsets_[v]; k < offsets_[v + 1]; k++) {
        uint64_t u = adj_[k].nbr;
        if (u <= v) continue;  // each undirected edge is staged both ways
        uint32_t ru = find(v), rv = find(u);
        if (ru != rv) parent[ru] = rv;
      }
    }

    // Label = minimum external vertex id within the component, matching the
    // convention used by the official reference fixtures.
    std::vector<uint64_t> min_external(n, std::numeric_limits<uint64_t>::max());
    for (uint64_t v = 0; v < n; v++) {
      uint32_t root = find(v);
      uint64_t ext = internal_to_external_[v];
      if (ext < min_external[root]) min_external[root] = ext;
    }

    last_result_.clear();
    last_result_.type = AlgorithmResult::kLong;
    for (uint64_t v = 0; v < n; v++)
      last_result_.long_vals.emplace_back(
          internal_to_external_[v],
          static_cast<int64_t>(min_external[find(v)]));
  }

  void RunLCC() override {
    const uint64_t n = num_vertices_;
    if (n == 0) return;
    std::vector<double> lcc(n, 0.0);

    // Orient each edge from the lower (degree, id) endpoint to the higher
    // endpoint.  Triangle enumeration over these forward lists visits every
    // triangle exactly once, instead of intersecting N(v) with N(u) for every
    // original neighbor as the legacy kernel did.
    std::vector<uint64_t> forward_degree(n, 0);
#pragma omp parallel for schedule(dynamic, 256)
    for (uint64_t v = 0; v < n; v++) {
      const uint64_t dv = Degree(v);
      for (uint64_t k = offsets_[v]; k < offsets_[v + 1]; k++) {
        const uint32_t u = adj_[k].nbr;
        const uint64_t du = Degree(u);
        if (dv < du || (dv == du && v < u)) forward_degree[v]++;
      }
    }

    std::vector<uint64_t> forward_offsets(n + 1, 0);
    for (uint64_t v = 0; v < n; v++)
      forward_offsets[v + 1] = forward_offsets[v] + forward_degree[v];
    std::vector<uint32_t> forward_adj(forward_offsets[n]);
#pragma omp parallel for schedule(dynamic, 256)
    for (uint64_t v = 0; v < n; v++) {
      uint64_t out = forward_offsets[v];
      const uint64_t dv = Degree(v);
      for (uint64_t k = offsets_[v]; k < offsets_[v + 1]; k++) {
        const uint32_t u = adj_[k].nbr;
        const uint64_t du = Degree(u);
        if (dv < du || (dv == du && v < u)) forward_adj[out++] = u;
      }
    }

    std::vector<std::atomic<uint64_t>> triangles(n);
    for (auto& count : triangles) count.store(0, std::memory_order_relaxed);
#pragma omp parallel for schedule(dynamic, 64)
    for (uint64_t v = 0; v < n; v++) {
      const uint64_t v_begin = forward_offsets[v];
      const uint64_t v_end = forward_offsets[v + 1];
      for (uint64_t p = v_begin; p < v_end; p++) {
        const uint32_t u = forward_adj[p];
        uint64_t i = p + 1;
        uint64_t j = forward_offsets[u];
        const uint64_t j_end = forward_offsets[u + 1];
        while (i < v_end && j < j_end) {
          const uint32_t a = forward_adj[i], b = forward_adj[j];
          if (a == b) {
            triangles[v].fetch_add(1, std::memory_order_relaxed);
            triangles[u].fetch_add(1, std::memory_order_relaxed);
            triangles[a].fetch_add(1, std::memory_order_relaxed);
            i++;
            j++;
          } else if (a < b) {
            i++;
          } else {
            j++;
          }
        }
      }
    }

#pragma omp parallel for schedule(dynamic, 256)
    for (uint64_t v = 0; v < n; v++) {
      const uint64_t deg = Degree(v);
      if (deg >= 2) {
        lcc[v] = 2.0 * static_cast<double>(triangles[v].load()) /
                 (static_cast<double>(deg) * (deg - 1));
      }
    }

    last_result_.clear();
    last_result_.type = AlgorithmResult::kDouble;
    for (uint64_t v = 0; v < n; v++)
      last_result_.double_vals.emplace_back(internal_to_external_[v], lcc[v]);
  }

  bool SupportsCommonAlgorithms() const override { return true; }
  bool SupportsCommonAlgorithm(const std::string& algorithm) const override {
    return algorithm == "cdlp" || algorithm == "lcc" || algorithm == "pr" ||
           algorithm == "bfs" || algorithm == "sssp" || algorithm == "wcc";
  }

  bool RunCDLPCommon(int max_iters) override {
    const auto labels = common::algorithms::cdlp(CommonView{*this}, max_iters);
    last_result_.clear();
    last_result_.type = AlgorithmResult::kLong;
    for (uint64_t v = 0; v < num_vertices_; v++)
      last_result_.long_vals.emplace_back(internal_to_external_[v],
                                          static_cast<int64_t>(labels[v]));
    return true;
  }

  bool RunLCCCommon() override {
    const auto values = common::algorithms::lcc(CommonView{*this});
    last_result_.clear();
    last_result_.type = AlgorithmResult::kDouble;
    for (uint64_t v = 0; v < num_vertices_; v++)
      last_result_.double_vals.emplace_back(internal_to_external_[v], values[v]);
    return true;
  }

  bool RunPageRankCommon(int iterations, double damping) override {
    const auto scores = common::algorithms::pagerank(CommonView{*this}, iterations, damping);
    last_result_.clear();
    last_result_.type = AlgorithmResult::kDouble;
    for (uint64_t v = 0; v < num_vertices_; v++)
      last_result_.double_vals.emplace_back(internal_to_external_[v], scores[v]);
    return true;
  }

  bool RunBFSCommon(uint64_t root) override {
    const uint64_t r = CommonView{*this}.resolve_root(root);
    if (r == std::numeric_limits<uint64_t>::max()) return false;
    const auto dist = common::algorithms::bfs(CommonView{*this}, r);
    last_result_.clear();
    last_result_.type = AlgorithmResult::kLong;
    for (uint64_t v = 0; v < num_vertices_; v++)
      last_result_.long_vals.emplace_back(internal_to_external_[v], dist[v]);
    return true;
  }

  bool RunSSSPCommon(uint64_t root) override {
    const uint64_t r = CommonView{*this}.resolve_root(root);
    if (r == std::numeric_limits<uint64_t>::max()) return false;
    const auto dist = common::algorithms::sssp_weighted(CommonView{*this}, r);
    last_result_.clear();
    last_result_.type = AlgorithmResult::kDouble;
    for (uint64_t v = 0; v < num_vertices_; v++)
      last_result_.double_vals.emplace_back(internal_to_external_[v], dist[v]);
    return true;
  }

  bool RunWCCCommon() override {
    const auto comp = common::algorithms::wcc(CommonView{*this});
    last_result_.clear();
    last_result_.type = AlgorithmResult::kLong;
    for (uint64_t v = 0; v < num_vertices_; v++)
      last_result_.long_vals.emplace_back(internal_to_external_[v],
                                          static_cast<int64_t>(comp[v]));
    return true;
  }

  void RunCDLP(int max_iters) override {
    const uint64_t n = num_vertices_;
    if (n == 0) return;
    std::vector<uint64_t> label(n), new_label(n);
    for (uint64_t v = 0; v < n; v++) label[v] = internal_to_external_[v];

    for (int iter = 0; iter < max_iters; iter++) {
#pragma omp parallel for schedule(dynamic, 256)
      for (uint64_t v = 0; v < n; v++) {
        uint64_t deg = Degree(v);
        if (deg == 0) {
          new_label[v] = label[v];
          continue;
        }
        std::vector<uint64_t> nbr_labels;
        nbr_labels.reserve(deg);
        for (uint64_t k = offsets_[v]; k < offsets_[v + 1]; k++)
          nbr_labels.push_back(label[adj_[k].nbr]);
        std::sort(nbr_labels.begin(), nbr_labels.end());

        uint64_t best_label = nbr_labels[0], best_count = 0;
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
        new_label[v] = best_label;
      }
      label.swap(new_label);
    }

    last_result_.clear();
    last_result_.type = AlgorithmResult::kLong;
    for (uint64_t v = 0; v < n; v++)
      last_result_.long_vals.emplace_back(internal_to_external_[v],
                                          static_cast<int64_t>(label[v]));
  }
};
