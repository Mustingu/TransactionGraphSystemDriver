#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

class S_Driver {
 public:
  long long record_cnt, visible_record_cnt;
  S_Driver() {
    record_cnt = 0;
    visible_record_cnt = 0;
  }

  void reset() {
    record_cnt = 0;
    visible_record_cnt = 0;
  }

  S_Driver& operator+=(const S_Driver& other) {
    this->record_cnt += other.record_cnt;
    this->visible_record_cnt += other.visible_record_cnt;
    return *this;
  }
};

// Lightweight result type for Graphalytics-like static evaluation.
// Each driver stores the last algorithm result here for export.
struct AlgorithmResult {
  enum Type { kNone, kDouble, kLong };
  Type type = kNone;
  std::vector<std::pair<uint64_t, double>> double_vals;
  std::vector<std::pair<uint64_t, int64_t>> long_vals;

  void clear() { type = kNone; double_vals.clear(); long_vals.clear(); }
};

// Shared interface implemented by graph-system adapters.
class GraphDriver {
 protected:
  // Hot-set readers can execute several algorithms concurrently. Their
  // full Graphalytics result vectors are neither consumed nor safe to publish
  // into a single per-driver slot, so that phase can opt out explicitly.
  std::atomic<bool> collect_algorithm_results_{true};

 public:
  virtual ~GraphDriver() = default;

  // Initialize memory, transaction managers and other backend state.
  // thread_num: maximum number of concurrent threads.
  virtual void Init(int thread_num) = 0;

  // Register a driver worker thread.
  virtual void RegisterThread(int thread_id) {};
  virtual void DeregisterThread(int thread_id) {};
  virtual void SetWorkThread(int thread_num) {};
  virtual void SetWRThread(int wt, int rt) {};
  // Reset worker ID mappings before each loading or mixed phase; default is a no-op.
  // Backends such as GTX have 64 fixed worker slots and monotonically allocated IDs;
  // separate loading and mixed workers require a reset between phases.
  virtual void ResetWorkerThreads() {};

  // Core write operation: insert or update an edge.
  // src, dst: external vertex IDs.
  // property: edge weight or property.
  // thread_id: worker ID used for transaction management.
  virtual bool UpsertEdge(uint64_t src, uint64_t dst, uint64_t property,
                          int thread_id) = 0;

  // Public graph mutation/query API. Drivers that do not support a
  // capability return false without changing state.
  virtual bool HasEdge(uint64_t src, uint64_t dst) const { return false; }

  virtual bool DeleteVertex(uint64_t vertex) { return false; }

  // Backend-specific read traversal capabilities. Implementations return
  // false when the capability is unavailable; callbacks receive external IDs.
  using EdgeCallback = std::function<void(uint64_t)>;
  using EdgePropertyCallback = std::function<void(uint64_t, uint64_t)>;
  using EdgeConditionCallback = std::function<bool(uint64_t)>;
  using SortedEdgeCallback = std::function<void(uint64_t)>;

  virtual bool TraverseEdges(uint64_t, EdgeCallback) const { return false; }
  virtual bool TraverseEdgesWithProperty(uint64_t,
                                         EdgePropertyCallback) const {
    return false;
  }
  virtual bool TraverseEdgesUntil(uint64_t, EdgeConditionCallback) const {
    return false;
  }
  virtual bool TraverseEdgesSorted(uint64_t, SortedEdgeCallback) const {
    return false;
  }

  // Delete an edge for mixed-update workloads; the default implementation returns false.
  // Like UpsertEdge, delete both directions and tolerate missing edges or vertices.
  virtual bool DeleteEdge(uint64_t src, uint64_t dst, int thread_id) {
    return false;
  };

  // Insert only if the edge is absent; return false if it exists or a conflict occurs.
  // Teseo churn uses this to retry candidates without falling back to an upsert.
  virtual bool InsertEdgeIfAbsent(uint64_t src, uint64_t dst,
                                  uint64_t property, int thread_id) {
    return false;
  };

  // Batch loading accelerates the --mix initialization phase (used by LiveGraph).
  // Other backends default to a no-op; ordinary insert-only loading is unchanged.
  virtual void BeginBulkLoad() {};
  virtual void EndBulkLoad() {};

  // Static evaluation hook: signal that the initial load is complete and allow
  // systems with unpublished write epochs to finalize them before readers run.
  // Returns false when the system cannot publish a complete read snapshot.
  virtual bool FinalizeStaticLoad() {
    FinishWrites();
    return true;
  }

  virtual bool InsertVertex(uint64_t src) { return false; }

  // Finalize writes, including AVB NoMoreTxn and GTX commit processing.
  virtual void FinishWrites() {};

  // Graph algorithm interface.
  virtual void RunPageRank(int iterations, double damping_factor,
                           S_Driver* global_counter = nullptr) = 0;
  virtual void RunBFS(uint64_t root, S_Driver* global_counter = nullptr) = 0;
  virtual void RunSSSP(uint64_t root, S_Driver* global_counter = nullptr) = 0;
  virtual void RunWCC() {};
  virtual void RunLCC() {};
  virtual void RunCDLP(int max_iters) {};

  // Opt-in common-kernel entry points. Legacy algorithm methods above remain
  // unchanged so existing experiments retain their historical implementation.
  virtual bool SupportsCommonAlgorithms() const { return false; }
  virtual bool SupportsCommonAlgorithm(const std::string&) const { return false; }
  virtual bool RunCDLPCommon(int) { return false; }
  virtual bool RunLCCCommon() { return false; }
  virtual bool RunPageRankCommon(int, double) { return false; }
  virtual bool RunBFSCommon(uint64_t) { return false; }
  virtual bool RunSSSPCommon(uint64_t) { return false; }
  virtual bool RunWCCCommon() { return false; }

  // Static Graphalytics-like mode uses this capability check so a default
  // no-op algorithm cannot be reported as a successful benchmark run.
  virtual bool SupportsAlgorithm(const std::string& algorithm) const {
    return algorithm == "pr" || algorithm == "bfs" || algorithm == "sssp";
  }

  // Existing drivers load edges with their historical direction semantics.
  // Directed static jobs remain opt-in until a driver explicitly supports them.
  virtual bool SupportsDirected() const { return false; }

  virtual const AlgorithmResult& GetLastResult() const {
    static const AlgorithmResult empty;
    return empty;
  }

  // Returns the prior setting. Static evaluation keeps the default (true);
  // concurrent throughput workloads disable collection around their reader
  // phase to avoid a race on each driver's single last-result buffer.
  bool SetAlgorithmResultCollectionEnabled(bool enabled) {
    return collect_algorithm_results_.exchange(enabled,
                                               std::memory_order_acq_rel);
  }
  bool ShouldCollectAlgorithmResults() const {
    return collect_algorithm_results_.load(std::memory_order_acquire);
  }

  // Return the system name.
  virtual std::string Name() const = 0;
};
