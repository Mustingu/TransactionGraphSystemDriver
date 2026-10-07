#pragma once
#include <chrono>
#include <limits>
#include <thread>

#include "../GraphDriver.h"
#include "../common/algorithms/GraphKernels.h"
#include "GraphAlgorithm.h"  // PageRank/BFS/SSSP/WCC/LCC/CDLP
#include "GraphStore.h"
#include "VersionBlock/AllVBManager.h"
#include "tbb/concurrent_hash_map.h"

class AVBDriver : public GraphDriver {
 private:
  AllVBManager* VBM;
  GraphStore* MEA;

  // External ID -> internal ID (TBB hash map).
  using vertex_dictionary_t = tbb::concurrent_hash_map<uint64_t, uint64_t>;
  vertex_dictionary_t* m_pHashMap;
  std::mutex vertex_api_mutex;
  AlgorithmResult last_result_;

  uint64_t GetInternalID(uint64_t src) {
    vertex_dictionary_t::accessor w;
    if (m_pHashMap->insert(w, src)) {
      // Construct the block and grow both vectors before releasing the ID-map
      // accessor, so other writers cannot resolve a partially initialized vertex.
      auto* eb = new VertexEdges();
      eb->build(0, nullptr, nullptr);
      const uint64_t p_id = MEA->node_num.fetch_add(1);

      if (MEA->blocks.size() <= p_id) {
        std::scoped_lock<std::mutex> lock(MEA->growing_vector_mutex);
        if (MEA->blocks.size() <= p_id) {
          const size_t grown_size = std::max<size_t>(
              p_id + 1, std::max<size_t>(MEA->blocks.size() * 2, 1));
          MEA->blocks.grow_to_at_least(grown_size, VertexEntry());
        }
      }

      if (MEA->p_mHashMap.size() <= p_id) {
        std::scoped_lock<std::mutex> lock(MEA->growing_vector_mutex);
        if (MEA->p_mHashMap.size() <= p_id) {
          const size_t grown_size =
              std::max<size_t>(p_id + 1,
                               std::max<size_t>(MEA->p_mHashMap.size() * 2, 1));
          MEA->p_mHashMap.grow_to_at_least(grown_size);
        }
      }

      MEA->p_mHashMap[p_id] = src;
      MEA->blocks[p_id].eb = eb;
      eb->setSrc(p_id);
      w->second = p_id;
      w.release();
      return p_id;
    } else {
      uint64_t p_id = w->second;
      w.release();
      return p_id;
    }
  }

 private:
  struct CommonView {
    const AVBDriver& driver;
    // One graph algorithm must read from a single stable snapshot. Resolve the
    // read epoch once at construction and reuse it for the whole traversal;
    // re-querying getCurrentReadEpoch() per access would take a global lock
    // ~(vertices × iterations) times and serialize all reader threads (this is
    // what made the common PR ~15x slower than legacy, which reuses one
    // read_ts). Streaming block reads are kept, so no full graph is materialized.
    const epoch_t read_epoch_;
    const Composite snap_;
    std::vector<uint64_t> active_;
    explicit CommonView(const AVBDriver& d)
        : driver(d),
          read_epoch_(d.VBM->getCurrentReadEpoch()),
          snap_(read_epoch_, INTRA_MAX) {
      const uint64_t n = driver.MEA->get_node_num();
      active_.reserve(n);
      for (uint64_t v = 0; v < n; ++v) {
        auto* eb = driver.MEA->GetBlockByIndex(v);
        if (eb && eb->get_degree(read_epoch_) != 0) active_.push_back(v);
      }
    }
    uint64_t vertex_count() const { return driver.MEA->get_node_num(); }
    const std::vector<uint64_t>& active_vertices() const { return active_; }
    uint64_t external_id(uint64_t v) const { return driver.MEA->p_mHashMap[v]; }
    uint64_t degree(uint64_t v) const {
      return driver.MEA->check_degree(v, read_epoch_);
    }
    template <typename Fn>
    void TraverseEdges(uint64_t v, Fn&& fn) const {
      auto* eb = driver.MEA->GetBlockByIndex(v);
      if (!eb) return;
      eb->getReadLock();
      if (eb->has_primary_only_state()) {
        eb->for_each_primary_edge([&](EdgeWithIndex* edge) {
          fn(static_cast<uint64_t>(edge->e & ~DELETION_MASK));
        });
      } else {
        GraphAlgorithms::for_each_edge(
            eb, snap_,
            [&](EdgeWithIndex* edge) {
              fn(static_cast<uint64_t>(edge->e & ~DELETION_MASK));
            },
            static_cast<Satistical*>(nullptr));
      }
      eb->unleashReadLock();
    }
    template <typename Fn>
    bool TraverseEdgesUntil(uint64_t v, Fn&& fn) const {
      auto* eb = driver.MEA->GetBlockByIndex(v);
      if (!eb) return false;
      bool stopped = false;
      eb->getReadLock();
      if (eb->has_primary_only_state()) {
        eb->for_each_primary_edge([&](EdgeWithIndex* edge) {
          if (!stopped)
            stopped = fn(static_cast<uint64_t>(edge->e & ~DELETION_MASK));
        });
      } else {
        GraphAlgorithms::for_each_edge_condition(
            eb, snap_, [&](EdgeWithIndex* edge) {
              return fn(static_cast<uint64_t>(edge->e & ~DELETION_MASK));
            });
      }
      eb->unleashReadLock();
      return stopped;
    }
    template <typename Fn>
    bool TraverseEdgesSorted(uint64_t v, Fn&& fn) const {
      auto* eb = driver.MEA->GetBlockByIndex(v);
      if (!eb) return true;
      eb->getReadLock();
      // VertexEdges merges the sorted primary array with visible temporary
      // updates, so this remains valid in both FINEGRAIN and coarse AVB.
      eb->for_each_edge_sorted(read_epoch_, [&](EdgeWithIndex* edge) {
        fn(static_cast<uint64_t>(edge->e & ~DELETION_MASK));
      });
      eb->unleashReadLock();
      return true;
    }
    template <typename Fn>
    void TraverseEdgesWithProperty(uint64_t v, Fn&& fn) const {
      auto* eb = driver.MEA->GetBlockByIndex(v);
      if (!eb) return;
      eb->getReadLock();
      GraphAlgorithms::for_each_edge_with_property(
          eb, snap_,
          [&](EdgeWithIndex* edge, EdgeWithIndex* property) {
            double w;
            std::memcpy(&w, &property->properties, sizeof(w));
            fn(static_cast<uint64_t>(edge->e & ~DELETION_MASK), w);
          });
      eb->unleashReadLock();
    }
  };

 public:
  std::string Name() const override { return "AVB"; }

  const AlgorithmResult& GetLastResult() const override { return last_result_; }

  void Init(int thread_num) override {
    VBM = new AllVBManager(thread_num + 1, true);
    MEA = new GraphStore(200000);
    la = new MemoryAllocator();
    la->init(50, 65);  // Use the same allocator parameters as the bundled AVB benchmark.
    m_pHashMap = new vertex_dictionary_t();
    std::cout << "[AVB] System Initialized." << std::endl;
  }

  void RegisterThread(int thread_id) override {
    VBM->register_thread(thread_id, true);
  }

  // Release registrations so subsequent writer rounds can reuse thread IDs.
  void DeregisterThread(int thread_id) override {
    VBM->deregister_thread(thread_id);
  }

  // Look up existing vertices without creating them on the deletion path.
  uint64_t FindInternalID(uint64_t external) {
    vertex_dictionary_t::const_accessor a;
    if (m_pHashMap->find(a, external)) return a->second;
    return std::numeric_limits<uint64_t>::max();
  }

  bool UpsertEdge(uint64_t src, uint64_t dst, uint64_t property,
                  int thread_id) override {
    uint64_t internal_src = GetInternalID(src);
    uint64_t internal_dst = GetInternalID(dst);

    dst_t f[3] = {(dst_t)internal_src, (dst_t)internal_dst, (dst_t)property};
    dst_t g[3] = {(dst_t)internal_dst, (dst_t)internal_src, (dst_t)property};
    // One transaction per edge, inserting both directions.
    Transaction txn = Transaction(0, false, true, MEA);
    VBM->registerTransaction(&txn);

    txn.insertedge(f);
    // A self-loop has identical endpoints, so inserting the reverse direction
    // would reacquire the same VertexEdges lock in this transaction.
    if (internal_src != internal_dst) txn.insertedge(g);
    txn.commit();

    VBM->deregisterTransaction();
    return true;
  }

  bool InsertVertex(uint64_t external_id) override {
    std::lock_guard<std::mutex> api_lock(vertex_api_mutex);
    vertex_dictionary_t::const_accessor a;
    if (m_pHashMap->find(a, external_id)) return false;
    GetInternalID(external_id);
    return true;
  }

  template <typename Callback>
  bool TraverseEdges(uint64_t external_src, Callback callback) const {
    const uint64_t src = const_cast<AVBDriver*>(this)->FindInternalID(external_src);
    if (src == std::numeric_limits<uint64_t>::max() ||
        src >= static_cast<uint64_t>(MEA->get_node_num()) ||
        src >= MEA->blocks.size()) return false;
    auto* eb = MEA->GetBlockByIndex(src);
    if (eb == nullptr) return false;
    const Composite snap(VBM->getCurrentReadEpoch(), INTRA_MAX);
    eb->getReadLock();
    GraphAlgorithms::for_each_edge(eb, snap, [&](EdgeWithIndex* edge) {
      callback(static_cast<uint64_t>(edge->e & ~DELETION_MASK));
    });
    eb->unleashReadLock();
    return true;
  }

  template <typename Callback>
  bool TraverseEdgesWithProperty(uint64_t external_src, Callback callback) const {
    const uint64_t src = const_cast<AVBDriver*>(this)->FindInternalID(external_src);
    if (src == std::numeric_limits<uint64_t>::max() ||
        src >= static_cast<uint64_t>(MEA->get_node_num()) ||
        src >= MEA->blocks.size()) return false;
    auto* eb = MEA->GetBlockByIndex(src);
    if (eb == nullptr) return false;
    const Composite snap(VBM->getCurrentReadEpoch(), INTRA_MAX);
    eb->getReadLock();
    GraphAlgorithms::for_each_edge_with_property(
        eb, snap, [&](EdgeWithIndex* edge, EdgeWithIndex* property) {
          callback(static_cast<uint64_t>(edge->e & ~DELETION_MASK),
                   static_cast<uint64_t>(property->properties));
        });
    eb->unleashReadLock();
    return true;
  }

  template <typename Callback>
  bool TraverseEdgesUntil(uint64_t external_src, Callback callback) const {
    const uint64_t src = const_cast<AVBDriver*>(this)->FindInternalID(external_src);
    if (src == std::numeric_limits<uint64_t>::max() ||
        src >= static_cast<uint64_t>(MEA->get_node_num()) ||
        src >= MEA->blocks.size()) return false;
    auto* eb = MEA->GetBlockByIndex(src);
    if (eb == nullptr) return false;
    const Composite snap(VBM->getCurrentReadEpoch(), INTRA_MAX);
    eb->getReadLock();
    GraphAlgorithms::for_each_edge_condition(
        eb, snap, [&](EdgeWithIndex* edge) -> bool {
          return callback(static_cast<uint64_t>(edge->e & ~DELETION_MASK));
        });
    eb->unleashReadLock();
    return true;
  }

  template <typename Callback>
  bool TraverseEdgesSorted(uint64_t external_src, Callback callback) const {
    const uint64_t src = const_cast<AVBDriver*>(this)->FindInternalID(external_src);
    if (src == std::numeric_limits<uint64_t>::max() ||
        src >= static_cast<uint64_t>(MEA->get_node_num()) ||
        src >= MEA->blocks.size()) return false;
    auto* eb = MEA->GetBlockByIndex(src);
    if (eb == nullptr) return false;
    const epoch_t snap = VBM->getCurrentReadEpoch();
    eb->getReadLock();
    eb->for_each_edge_sorted(snap, [&](EdgeWithIndex* edge) {
      callback(static_cast<uint64_t>(edge->e & ~DELETION_MASK));
    });
    eb->unleashReadLock();
    return true;
  }

  bool HasEdge(uint64_t src, uint64_t dst) const override {
    const uint64_t s = const_cast<AVBDriver*>(this)->FindInternalID(src);
    const uint64_t d = const_cast<AVBDriver*>(this)->FindInternalID(dst);
    if (s == std::numeric_limits<uint64_t>::max() ||
        d == std::numeric_limits<uint64_t>::max() ||
        s >= static_cast<uint64_t>(MEA->get_node_num())) return false;
    auto* eb = MEA->GetBlockByIndex(s);
    if (eb == nullptr) return false;
    const epoch_t read_epoch = VBM->getCurrentReadEpoch();
    bool found = false;
    eb->getReadLock();
    eb->for_each_edge_sorted(read_epoch, [&](EdgeWithIndex* edge) {
      if ((edge->e & ~DELETION_MASK) == static_cast<dst_t>(d)) found = true;
    });
    eb->unleashReadLock();
    return found;
  }

  bool DeleteVertex(uint64_t external_id) override {
    std::lock_guard<std::mutex> api_lock(vertex_api_mutex);
    vertex_dictionary_t::const_accessor a;
    if (!m_pHashMap->find(a, external_id)) return false;
    const uint64_t internal = a->second;
    a.release();

    const uint64_t node_count = static_cast<uint64_t>(MEA->get_node_num());
    if (internal >= node_count || internal + 1 != node_count) {
      return false;
    }
    auto* eb = MEA->GetBlockByIndex(internal);
    if (eb == nullptr) return false;

    const epoch_t read_epoch = VBM->getCurrentReadEpoch();
    eb->getReadLock();
    const bool has_edges = eb->get_degree(read_epoch) != 0;
    eb->unleashReadLock();
    if (has_edges) return false;

    // Algorithms address [0, node_num), so only the current tail can be
    // removed without exposing a hole. Callers must quiesce readers/writers
    // before vertex deletion; the API lock only serializes vertex operations.
    MEA->blocks[internal].eb = nullptr;
    MEA->p_mHashMap[internal] = 0;
    MEA->node_num.fetch_sub(1);
    delete eb;
    return m_pHashMap->erase(external_id);
  }

  // Delete both directions using tombstone version chains; missing edges or vertices return false.
  bool DeleteEdge(uint64_t src, uint64_t dst, int thread_id) override {
    uint64_t s = FindInternalID(src), d = FindInternalID(dst);
    if (s == std::numeric_limits<uint64_t>::max() ||
        d == std::numeric_limits<uint64_t>::max())
      return false;
    dst_t f[3] = {(dst_t)s, (dst_t)d, 0};
    dst_t g[3] = {(dst_t)d, (dst_t)s, 0};
    Transaction txn = Transaction(0, false, true, MEA);
    VBM->registerTransaction(&txn);
    txn.deletedge(f);
    txn.deletedge(g);
    txn.commit();
    VBM->deregisterTransaction();
    return true;
  }

  void FinishWrites() override { VBM->NoMoreTxn(); }

  bool FinalizeStaticLoad() override {
    // Publish the last write epoch synchronously. A large single-threaded load
    // may need longer than the updater's polling interval to transform its
    // VBData, so static finalization must not treat that normal work as a
    // visibility timeout.
    VBM->FinalizeNoMoreTxn();
    return VBM->getCurrentReadEpoch() >= VBM->getCurrentEpoch();
  }

  void RunPageRank(int iterations, double damping_factor,
                   S_Driver* global_counter) override {
    PageRank pr(MEA, VBM, 1);
    pr.compute_pagerank(iterations, damping_factor, MEA);
    if (!ShouldCollectAlgorithmResults()) return;
    last_result_.clear();
    auto* res = pr.get_result();
    if (res) {
      last_result_.type = AlgorithmResult::kDouble;
      last_result_.double_vals = *res;
    }
  }

  void RunBFS(uint64_t root, S_Driver* global_counter) override {
    const bool collect_result = ShouldCollectAlgorithmResults();
    if (collect_result) last_result_.clear();
    const uint64_t internal_root = FindInternalID(root);
    // Read algorithms must not turn an invalid source ID into a new vertex
    // after static-load finalization. The empty result is reported by the
    // common static runner as result-unavailable.
    if (internal_root == std::numeric_limits<uint64_t>::max()) return;
    BFS bfs(MEA, VBM, 1);
    bfs.bfs(internal_root, 15, 18, MEA);
    if (!collect_result) return;
    auto* res = bfs.get_result();
    if (res) {
      last_result_.type = AlgorithmResult::kLong;
      for (const auto& p : *res) {
        if (p.first != std::numeric_limits<uint64_t>::max())
          last_result_.long_vals.emplace_back(p.first, p.second);
      }
    }
  }

  void RunSSSP(uint64_t root, S_Driver* global_counter) override {
    const bool collect_result = ShouldCollectAlgorithmResults();
    if (collect_result) last_result_.clear();
    const uint64_t internal_root = FindInternalID(root);
    // See RunBFS(): lookup-only is required once static loading is closed.
    if (internal_root == std::numeric_limits<uint64_t>::max()) return;
    SSSP sssp(MEA, VBM, 1);
    sssp.compute_sssp(internal_root, 2, MEA);
    if (!collect_result) return;
    auto* res = sssp.get_result();
    if (res) {
      last_result_.type = AlgorithmResult::kDouble;
      last_result_.double_vals = *res;
    }
  }

  void RunWCC() override {
    last_result_.clear();
    WCC wcc(MEA, VBM, 1);
    wcc.compute_wcc(MEA);
    auto* res = wcc.get_result();
    if (res) {
      last_result_.type = AlgorithmResult::kLong;
      for (const auto& p : *res)
        last_result_.long_vals.emplace_back(p.first, static_cast<int64_t>(p.second));
    }
  }

  bool SupportsCommonAlgorithms() const override { return true; }
  bool SupportsCommonAlgorithm(const std::string& algorithm) const override {
    return algorithm == "cdlp" || algorithm == "lcc" || algorithm == "pr" ||
           algorithm == "bfs" || algorithm == "sssp" || algorithm == "wcc";
  }

  bool RunCDLPCommon(int max_iters) override {
    const auto labels = common::algorithms::cdlp(CommonView(*this), max_iters);
    last_result_.clear();
    last_result_.type = AlgorithmResult::kLong;
    for (uint64_t v = 0; v < labels.size(); v++)
      last_result_.long_vals.emplace_back(MEA->p_mHashMap[v],
                                          static_cast<int64_t>(labels[v]));
    return true;
  }

  bool RunLCCCommon() override {
    const auto values = common::algorithms::lcc(CommonView(*this));
    last_result_.clear();
    last_result_.type = AlgorithmResult::kDouble;
    for (uint64_t v = 0; v < values.size(); v++)
      last_result_.double_vals.emplace_back(MEA->p_mHashMap[v], values[v]);
    return true;
  }

  bool RunPageRankCommon(int iterations, double damping) override {
    const auto scores = common::algorithms::pagerank(CommonView(*this), iterations, damping);
    last_result_.clear();
    last_result_.type = AlgorithmResult::kDouble;
    for (uint64_t v = 0; v < scores.size(); v++)
      last_result_.double_vals.emplace_back(MEA->p_mHashMap[v], scores[v]);
    return true;
  }

  bool RunBFSCommon(uint64_t root) override {
    const uint64_t r = ResolveRoot(root);
    if (r == std::numeric_limits<uint64_t>::max()) return false;
    const auto dist = common::algorithms::bfs(CommonView(*this), r);
    last_result_.clear();
    last_result_.type = AlgorithmResult::kLong;
    for (uint64_t v = 0; v < dist.size(); v++)
      last_result_.long_vals.emplace_back(MEA->p_mHashMap[v], dist[v]);
    return true;
  }

  bool RunSSSPCommon(uint64_t root) override {
    const uint64_t r = ResolveRoot(root);
    if (r == std::numeric_limits<uint64_t>::max()) return false;
    const auto dist = common::algorithms::sssp_weighted(CommonView(*this), r);
    last_result_.clear();
    last_result_.type = AlgorithmResult::kDouble;
    for (uint64_t v = 0; v < dist.size(); v++)
      last_result_.double_vals.emplace_back(MEA->p_mHashMap[v], dist[v]);
    return true;
  }

  bool RunWCCCommon() override {
    const auto comp = common::algorithms::wcc(CommonView(*this));
    last_result_.clear();
    last_result_.type = AlgorithmResult::kLong;
    for (uint64_t v = 0; v < comp.size(); v++)
      last_result_.long_vals.emplace_back(MEA->p_mHashMap[v],
                                          static_cast<int64_t>(comp[v]));
    return true;
  }

  // Resolve an external root id to the dense internal id, or UINT64_MAX.
  uint64_t ResolveRoot(uint64_t external) const {
    for (uint64_t v = 0; v < MEA->get_node_num(); v++)
      if (MEA->p_mHashMap[v] == external) return v;
    return std::numeric_limits<uint64_t>::max();
  }

  void RunLCC() override {
    last_result_.clear();
    LCC lcc(MEA, VBM, 1);
    lcc.compute_lcc(MEA);
    auto* res = lcc.get_result();
    if (res) {
      last_result_.type = AlgorithmResult::kDouble;
      last_result_.double_vals = *res;
    }
  }

  void RunCDLP(int max_iters) override {
    last_result_.clear();
    CDLP cdlp(MEA, VBM, 1);
    cdlp.compute_cdlp(max_iters, MEA);
    auto* res = cdlp.get_result();
    if (res) {
      last_result_.type = AlgorithmResult::kLong;
      for (const auto& p : *res)
        last_result_.long_vals.emplace_back(p.first, static_cast<int64_t>(p.second));
    }
  }

  bool SupportsAlgorithm(const std::string& algorithm) const override {
    return algorithm == "pr" || algorithm == "bfs" || algorithm == "sssp" ||
           algorithm == "wcc" || algorithm == "lcc" || algorithm == "cdlp";
  }
};
