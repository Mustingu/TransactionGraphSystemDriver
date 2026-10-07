#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <omp.h>

#include "../GraphDriver.h"
#include "../common/algorithms/GraphKernels.h"
#ifdef USE_SLT
#include "../systems/Sortledton/third-party/gapbs.h"
#else
#include "../systems/AVBGraph/third-party/gapbs.h"
#endif
#include "teseo.hpp"  // teseo namespace declarations (systems/Teseo/include).
#include "tbb/concurrent_hash_map.h"

// Teseo (cwida/teseo): built with autotools and linked as systems/Teseo/build/libteseo.a.
// Adapter requirements:
// - All threads except the creator must register_thread; thread_local state prevents duplicates.
// - insert_edge throws LogicalError for an existing edge; no update primitive is available.
//   UpsertEdge therefore removes and reinserts an edge within one transaction.
// - The adapter assigns dense vertex IDs using an atomic counter. Algorithms convert
//   IDs to ranks via logical_id/vertex_id; logical=true uses rank-valued callbacks.
// - Weights are doubles decoded from the stored bit pattern.
class TeseoDriver : public GraphDriver {
 private:
  static constexpr size_t kMaxDiagnosticWriters = 64;
  teseo::Teseo* m_pImpl;

  // Map external IDs to adapter-assigned Teseo vertex IDs.
  using vertex_dictionary_t = tbb::concurrent_hash_map<uint64_t, uint64_t>;
  vertex_dictionary_t* m_pHashMap;
  std::atomic<uint64_t> m_next_vertex_id{0};
  std::atomic<int> m_num_vertices{0};
  AlgorithmResult last_result_;
  mutable std::mutex reverse_map_mutex_;
  std::vector<uint64_t> internal_to_external_;
  std::mutex aux_view_mutex_;
  std::array<std::atomic<uint64_t>, kMaxDiagnosticWriters>
      delete_conflicts_{};
  std::array<std::atomic<uint64_t>, kMaxDiagnosticWriters>
      insert_conflicts_{};

  static inline thread_local bool tls_registered = false;
  void EnsureRegistered() {
    if (!tls_registered) {
      m_pImpl->register_thread();
      tls_registered = true;
    }
  }
  void EnsureUnregistered() {
    if (tls_registered) {
      m_pImpl->unregister_thread();
      tls_registered = false;
    }
  }

  static void BackoffAfterConflict(uint32_t retry_count) {
    if (retry_count < 16) {
      std::this_thread::yield();
      return;
    }
    const uint32_t shift = std::min<uint32_t>(retry_count - 16, 10);
    std::this_thread::sleep_for(std::chrono::microseconds(1u << shift));
  }

  void RecordDeleteConflict(int thread_id) {
    if (thread_id >= 0 &&
        static_cast<size_t>(thread_id) < kMaxDiagnosticWriters) {
      delete_conflicts_[thread_id].fetch_add(1, std::memory_order_relaxed);
    }
  }

  void RecordInsertConflict(int thread_id) {
    if (thread_id >= 0 &&
        static_cast<size_t>(thread_id) < kMaxDiagnosticWriters) {
      insert_conflicts_[thread_id].fetch_add(1, std::memory_order_relaxed);
    }
  }

  class WorkerRegistration {
   private:
    TeseoDriver* driver_;
    bool unregister_on_destroy_;

   public:
    WorkerRegistration(TeseoDriver* driver, bool register_worker)
        : driver_(driver), unregister_on_destroy_(false) {
      if (register_worker && !tls_registered) {
        driver_->EnsureRegistered();
        unregister_on_destroy_ = true;
      }
    }

    WorkerRegistration(const WorkerRegistration&) = delete;
    WorkerRegistration& operator=(const WorkerRegistration&) = delete;

    ~WorkerRegistration() {
      if (unregister_on_destroy_) driver_->EnsureUnregistered();
    }
  };

  class SharedReadTransaction {
   private:
    TeseoDriver* driver_;
    WorkerRegistration registration_;
    teseo::Transaction transaction_;

   public:
    explicit SharedReadTransaction(TeseoDriver* driver)
        : driver_(driver),
          registration_(driver, false),
          transaction_(driver->m_pImpl->start_transaction(true)) {}

    SharedReadTransaction(const SharedReadTransaction& other)
        : driver_(other.driver_),
          registration_(driver_, true),
          transaction_(other.transaction_) {}

    SharedReadTransaction& operator=(const SharedReadTransaction&) = delete;

    teseo::Transaction& transaction() { return transaction_; }
  };

  template <typename F>
  void RunThreads(int num_threads, F&& body) {
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++)
      threads.emplace_back([&, i]() {
        EnsureRegistered();
        body(i);
        EnsureUnregistered();
      });
    for (auto& t : threads) t.join();
  }

  // Upsert a directed edge inside a transaction.
  void UpsertEdge(teseo::Transaction& tx, uint64_t src, uint64_t dst,
                  double w) {
    if (tx.has_edge(src, dst)) tx.remove_edge(src, dst);
    tx.insert_edge(src, dst, w);
  }

  static int64_t BFSBottomUp(SharedReadTransaction& read_tx,
                             gapbs::pvector<int64_t>& distances,
                             int64_t distance, gapbs::Bitmap& frontier,
                             gapbs::Bitmap& next) {
    const int64_t n = read_tx.transaction().num_vertices();
    int64_t awake_count = 0;
    next.reset();
#pragma omp parallel firstprivate(read_tx) reduction(+ : awake_count)
    {
      auto iterator = read_tx.transaction().iterator();
#pragma omp for schedule(dynamic, 1024)
      for (int64_t vertex = 0; vertex < n; vertex++) {
        if (distances[vertex] < 0) {
          bool done = false;
          iterator.edges(vertex, true, [&](uint64_t neighbor, double weight) {
            if (frontier.get_bit(neighbor)) {
              distances[vertex] = distance;
              awake_count++;
              next.set_bit(vertex);
              done = true;
            }
            return !done;
          });
        }
      }
    }
    return awake_count;
  }

  static int64_t BFSTopDown(SharedReadTransaction& read_tx,
                            gapbs::pvector<int64_t>& distances,
                            int64_t distance,
                            gapbs::SlidingQueue<int64_t>& queue) {
    int64_t scout_count = 0;
#pragma omp parallel firstprivate(read_tx)
    {
      gapbs::QueueBuffer<int64_t> local_queue(queue);
      auto iterator = read_tx.transaction().iterator();
#pragma omp for reduction(+ : scout_count)
      for (auto queue_it = queue.begin(); queue_it < queue.end(); queue_it++) {
        int64_t vertex = *queue_it;
        iterator.edges(vertex, true, [&](uint64_t destination, double weight) {
          int64_t current = distances[destination];
          if (current < 0 &&
              gapbs::compare_and_swap(distances[destination], current, distance)) {
            local_queue.push_back(destination);
            scout_count += -current;
          }
        });
      }
      local_queue.flush();
    }
    return scout_count;
  }

  static void BFSQueueToBitmap(const gapbs::SlidingQueue<int64_t>& queue,
                               gapbs::Bitmap& bitmap) {
#pragma omp parallel for
    for (auto queue_it = queue.begin(); queue_it < queue.end(); queue_it++)
      bitmap.set_bit_atomic(*queue_it);
  }

  static void BFSBitmapToQueue(SharedReadTransaction& read_tx,
                               const gapbs::Bitmap& bitmap,
                               gapbs::SlidingQueue<int64_t>& queue) {
    const int64_t n = read_tx.transaction().num_vertices();
#pragma omp parallel
    {
      gapbs::QueueBuffer<int64_t> local_queue(queue);
#pragma omp for
      for (int64_t vertex = 0; vertex < n; vertex++)
        if (bitmap.get_bit(vertex)) local_queue.push_back(vertex);
      local_queue.flush();
    }
    queue.slide_window();
  }

  static gapbs::pvector<int64_t> BFSInitDistances(
      SharedReadTransaction& read_tx) {
    const int64_t n = read_tx.transaction().num_vertices();
    gapbs::pvector<int64_t> distances(n);
#pragma omp parallel for firstprivate(read_tx)
    for (int64_t vertex = 0; vertex < n; vertex++) {
      uint64_t degree = read_tx.transaction().degree(vertex, true);
      distances[vertex] = degree != 0 ? -static_cast<int64_t>(degree) : -1;
    }
    return distances;
  }

  static gapbs::pvector<int64_t> RunDirectionOptimizingBFS(
      SharedReadTransaction& read_tx, int64_t source) {
    gapbs::pvector<int64_t> distances = BFSInitDistances(read_tx);
    distances[source] = 0;
    const int64_t n = read_tx.transaction().num_vertices();
    gapbs::SlidingQueue<int64_t> queue(n);
    queue.push_back(source);
    queue.slide_window();
    gapbs::Bitmap current(n), frontier(n);
    current.reset();
    frontier.reset();
    int64_t edges_to_check = read_tx.transaction().num_edges();
    int64_t scout_count = read_tx.transaction().degree(source, true);
    int64_t distance = 1;

    while (!queue.empty()) {
      if (scout_count > edges_to_check / 15) {
        int64_t awake_count = queue.size();
        int64_t old_awake_count;
        BFSQueueToBitmap(queue, frontier);
        queue.slide_window();
        do {
          old_awake_count = awake_count;
          awake_count = BFSBottomUp(read_tx, distances, distance, frontier,
                                    current);
          frontier.swap(current);
          distance++;
        } while ((awake_count >= old_awake_count) ||
                 (awake_count > n / 18));
        BFSBitmapToQueue(read_tx, frontier, queue);
        scout_count = 1;
      } else {
        edges_to_check -= scout_count;
        scout_count = BFSTopDown(read_tx, distances, distance, queue);
        queue.slide_window();
        distance++;
      }
    }
    return distances;
  }

 public:
  std::string Name() const override { return "Teseo"; }

  const AlgorithmResult& GetLastResult() const override { return last_result_; }

  void ResetChurnConflictStats(int writer_count) {
    size_t count = std::min<size_t>(std::max(writer_count, 0),
                                    kMaxDiagnosticWriters);
    for (size_t index = 0; index < count; ++index) {
      delete_conflicts_[index].store(0, std::memory_order_release);
      insert_conflicts_[index].store(0, std::memory_order_release);
    }
  }

  uint64_t DeleteConflictCount(int thread_id) const {
    if (thread_id < 0 ||
        static_cast<size_t>(thread_id) >= kMaxDiagnosticWriters) {
      return 0;
    }
    return delete_conflicts_[thread_id].load(std::memory_order_acquire);
  }

  uint64_t InsertConflictCount(int thread_id) const {
    if (thread_id < 0 ||
        static_cast<size_t>(thread_id) >= kMaxDiagnosticWriters) {
      return 0;
    }
    return insert_conflicts_[thread_id].load(std::memory_order_acquire);
  }

  void Init(int thread_num) override {
    m_pImpl = new teseo::Teseo();  // The creating thread (main) is registered automatically.
    m_pHashMap = new vertex_dictionary_t();
    printf("[Teseo] Initialized.\n");
  }

  void RegisterThread(int thread_id) override { EnsureRegistered(); }
  void DeregisterThread(int thread_id) override { EnsureUnregistered(); }

  bool UpsertEdge(uint64_t src, uint64_t dst, uint64_t property,
                  int thread_id) override {
    EnsureRegistered();
    // As in GTX/LiveGraph, retain the exclusive mapping lock until commit to deduplicate vertices.
    uint64_t internal_source_id = 0;
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

    // Claim IDs once outside the retry loop; only committed IDs form the dense sequence.
    if (insert_source)
      internal_source_id = m_next_vertex_id.fetch_add(1);
    if (insert_destination)
      internal_destination_id = m_next_vertex_id.fetch_add(1);
    double w = *reinterpret_cast<const double*>(&property);

    // Teseo requires a fresh transaction after TransactionConflict; discard the old
    // transaction and call start_transaction() again (see the upstream README and
    // DeleteEdge/InsertEdgeIfAbsent). Retrying within a conflicted transaction
    // can spin indefinitely under contention and prevent writer progress,
    // triggering heartbeat STALL/SIGABRT or exhausting lock/GC resources.
    // Retry the entire upsert in a fresh transaction with backoff.
    // Claim the internal IDs once and reuse them across retries.
    // Teseo's memstore is undirected (is_directed=false in global_context.cpp);
    // insert_edge(s,d) inserts both directions. Initial loading inserts the edge;
    // subsequent upserts remove and reinsert it within one transaction.
    for (uint32_t retry_count = 0;; ++retry_count) {
      try {
        auto tx = m_pImpl->start_transaction();
        if (insert_source && !tx.has_vertex(internal_source_id))
          tx.insert_vertex(internal_source_id);
        if (insert_destination && !tx.has_vertex(internal_destination_id))
          tx.insert_vertex(internal_destination_id);
        UpsertEdge(tx, internal_source_id, internal_destination_id, w);
        tx.commit();

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
      } catch (teseo::TransactionConflict&) {
        RecordInsertConflict(thread_id);
        BackoffAfterConflict(retry_count);
      } catch (teseo::LogicalError&) {
        // A concurrent remove/insert can leave the edge or vertex already present.
        // Treat that LogicalError as satisfying the upsert instead of retrying indefinitely.
        return true;
      }
    }
  }

  // Modify an existing edge with a same-transaction remove+insert in both directions.
  // Only look up vertices: modifications target edges loaded into the graph.
  bool ModifyEdge(uint64_t src, uint64_t dst, uint64_t property,
                  int thread_id) {
    EnsureRegistered();
    uint64_t s = GetInternalID(src), d = GetInternalID(dst);
    if (s == std::numeric_limits<uint64_t>::max() ||
        d == std::numeric_limits<uint64_t>::max())
      return false;
    double w = *reinterpret_cast<const double*>(&property);
    auto tx = m_pImpl->start_transaction();
    // One remove+insert call handles both directions in Teseo's undirected graph.
    while (true) {
      try {
        tx.remove_edge(s, d);
        break;
      } catch (teseo::TransactionConflict&) {
      } catch (teseo::LogicalError&) {
        break;  // Defensive handling for a missing edge.
      }
    }
    while (true) {
      try {
        tx.insert_edge(s, d, w);
        break;
      } catch (teseo::TransactionConflict&) {
      } catch (teseo::LogicalError&) {
        break;
      }
    }
    tx.commit();
    return true;
  }

  // Discard and reopen the transaction after TransactionConflict rather than
  // spinning on the same operation within a conflicted transaction. remove_edge
  // throws LogicalError for a missing edge; prevalidated graphlog treats it as a failure.
  bool DeleteEdge(uint64_t src, uint64_t dst, int thread_id) override {
    EnsureRegistered();
    uint64_t s = GetInternalID(src), d = GetInternalID(dst);
    if (s == std::numeric_limits<uint64_t>::max() ||
        d == std::numeric_limits<uint64_t>::max())
      return false;
    for (uint32_t retry_count = 0;; ++retry_count) {
      try {
        auto tx = m_pImpl->start_transaction();
        // One remove_edge call handles both directions in Teseo's undirected graph.
        tx.remove_edge(s, d);
        tx.commit();
        return true;
      } catch (teseo::TransactionConflict&) {
        RecordDeleteConflict(thread_id);
        BackoffAfterConflict(retry_count);
      } catch (teseo::LogicalError&) {
        return false;
      }
    }
  }

  bool InsertEdgeIfAbsent(uint64_t src, uint64_t dst, uint64_t property,
                          int thread_id) override {
    EnsureRegistered();
    uint64_t s = GetInternalID(src), d = GetInternalID(dst);
    if (s == std::numeric_limits<uint64_t>::max() ||
        d == std::numeric_limits<uint64_t>::max()) {
      return false;
    }
    double weight = *reinterpret_cast<const double*>(&property);
    for (uint32_t retry_count = 0;; ++retry_count) {
      try {
        auto tx = m_pImpl->start_transaction();
        tx.insert_edge(s, d, weight);
        tx.commit();
        return true;
      } catch (teseo::TransactionConflict&) {
        RecordInsertConflict(thread_id);
        BackoffAfterConflict(retry_count);
      } catch (teseo::LogicalError&) {
        return false;
      }
    }
  }

  uint64_t GetInternalID(uint64_t external) {
    vertex_dictionary_t::const_accessor a;
    if (m_pHashMap->find(a, external)) return a->second;
    return std::numeric_limits<uint64_t>::max();
  }

  void RecordExternalID(uint64_t internal, uint64_t external) {
    std::lock_guard<std::mutex> lock(reverse_map_mutex_);
    if (internal_to_external_.size() <= internal)
      internal_to_external_.resize(internal + 1,
                                   std::numeric_limits<uint64_t>::max());
    internal_to_external_[internal] = external;
  }

  uint64_t GetExternalID(uint64_t internal) const {
    std::lock_guard<std::mutex> lock(reverse_map_mutex_);
    if (internal >= internal_to_external_.size())
      return std::numeric_limits<uint64_t>::max();
    return internal_to_external_[internal];
  }

  void RunPageRank(int iterations, double damping_factor,
                   S_Driver* global_counter) override {
    EnsureRegistered();
    uint64_t n = 0;
    std::vector<double> scores;
    {
      SharedReadTransaction read_tx(this);
      {
        std::lock_guard<std::mutex> lock(aux_view_mutex_);
        n = read_tx.transaction().num_vertices();
      }
      if (n != 0) {
        scores.assign(n, 1.0 / n);
        std::vector<double> new_scores(n, 0.0);
        const double base = (1.0 - damping_factor) / n;

        for (int iteration = 0; iteration < iterations; iteration++) {
          std::vector<double> contrib(n, 0.0);
          double dangling = 0.0;

#pragma omp parallel for reduction(+ : dangling) firstprivate(read_tx)
          for (uint64_t v = 0; v < n; v++) {
            uint64_t degree = read_tx.transaction().degree(v, true);
            if (degree == 0)
              dangling += scores[v];
            else
              contrib[v] = scores[v] / degree;
          }

          dangling /= n;
#pragma omp parallel firstprivate(read_tx)
          {
            auto iterator = read_tx.transaction().iterator();
#pragma omp for schedule(dynamic, 64)
            for (uint64_t v = 0; v < n; v++) {
              double incoming = 0.0;
              iterator.edges(v, true, [&](uint64_t dst, double w) {
                incoming += contrib[dst];
              });
              new_scores[v] = base + damping_factor * (incoming + dangling);
            }
          }
          scores.swap(new_scores);
        }
      }
    }
    EnsureUnregistered();

    if (!ShouldCollectAlgorithmResults()) return;
    last_result_.clear();
    if (n > 0) {
      last_result_.type = AlgorithmResult::kDouble;
      for (uint64_t v = 0; v < n; v++)
        last_result_.double_vals.emplace_back(GetExternalID(v), scores[v]);
    }
  }

  void RunBFS(uint64_t root, S_Driver* global_counter) override {
    EnsureRegistered();
    uint64_t r = GetInternalID(root);
    if (r == std::numeric_limits<uint64_t>::max()) {
      printf("BFS Root %lu not found!\n", (unsigned long)root);
      EnsureUnregistered();
      return;
    }
    uint64_t n = 0;
    gapbs::pvector<int64_t> distances;
    std::vector<uint64_t> bfs_external_ids;
    {
      SharedReadTransaction read_tx(this);
      uint64_t source_rank;
      {
        std::lock_guard<std::mutex> lock(aux_view_mutex_);
        n = read_tx.transaction().num_vertices();
        source_rank = read_tx.transaction().logical_id(r);
      }
      if (n != 0) {
        distances = RunDirectionOptimizingBFS(read_tx, source_rank);
        bfs_external_ids.resize(n);
        for (uint64_t v = 0; v < n; v++)
          bfs_external_ids[v] = GetExternalID(read_tx.transaction().vertex_id(v));
      }
    }
    EnsureUnregistered();

    if (!ShouldCollectAlgorithmResults()) return;
    last_result_.clear();
    if (n > 0) {
      last_result_.type = AlgorithmResult::kLong;
      for (uint64_t v = 0; v < n; v++)
        last_result_.long_vals.emplace_back(bfs_external_ids[v], distances[v]);
    }
  }

  void RunSSSP(uint64_t root, S_Driver* global_counter) override {
    EnsureRegistered();
    uint64_t r = GetInternalID(root);
    if (r == std::numeric_limits<uint64_t>::max()) {
      printf("SSSP Root %lu not found!\n", (unsigned long)root);
      EnsureUnregistered();
      return;
    }
    uint64_t n;
    std::vector<double> final_dist;
    std::vector<uint64_t> sssp_external_ids;
    {
      std::vector<uint64_t> rank_of, id_of;
      {
        std::lock_guard<std::mutex> lock(aux_view_mutex_);
        auto init_tx = m_pImpl->start_transaction(true);
        n = init_tx.num_vertices();
        rank_of.resize(n);
        id_of.resize(n);
        sssp_external_ids.resize(n);
        for (uint64_t v = 0; v < n; v++) {
          uint64_t rk = init_tx.logical_id(v);
          rank_of[v] = rk;
          id_of[rk] = v;
          sssp_external_ids[v] = GetExternalID(v);
        }
        init_tx.rollback();
      }
      EnsureUnregistered();

      const int num_threads = omp_get_max_threads();
      std::vector<std::atomic<double>> dist(n);
      for (uint64_t v = 0; v < n; v++)
        dist[v].store(std::numeric_limits<double>::max());
      dist[r].store(0.0);

      // Parallel Bellman-Ford until convergence.
      std::atomic<bool> changed{true};
      while (changed.load()) {
        changed.store(false);
        RunThreads(num_threads, [&](int tid) {
          auto tx = m_pImpl->start_transaction(true);
          auto iter = tx.iterator();
          bool local_changed = false;
          for (uint64_t u = tid; u < n; u += num_threads) {
            double du = dist[u].load();
            if (du == std::numeric_limits<double>::max()) continue;
            iter.edges(rank_of[u], true, [&](uint64_t dst_rank, double w) {
              double desired = du + w;
              double cur = dist[id_of[dst_rank]].load();
              do {
                if (cur <= desired) break;
              } while (
                  !dist[id_of[dst_rank]].compare_exchange_weak(cur, desired));
              if (cur > desired) local_changed = true;
            });
        }
        if (local_changed) changed.store(true);
      });
      }

      final_dist.resize(n);
      for (uint64_t v = 0; v < n; v++) final_dist[v] = dist[v].load();
    }

    if (!ShouldCollectAlgorithmResults()) return;
    last_result_.clear();
    last_result_.type = AlgorithmResult::kDouble;
    for (uint64_t v = 0; v < n; v++)
      last_result_.double_vals.emplace_back(sssp_external_ids[v], final_dist[v]);
  }

  // Teseo has no native WCC/LCC/CDLP implementation. Reuse the SSSP
  // rank_of/id_of conversion layer instead of assuming real IDs equal ranks.
  // Use logical_id()/vertex_id() explicitly; identity mappings are not guaranteed.
  // iterator().edges() pushes neighbors through a callback without guaranteeing
  // label or ID order. LCC/CDLP collect and sort local lists before processing.

  void RunWCC() override {
    last_result_.clear();
    EnsureRegistered();
    uint64_t n;
    std::vector<uint64_t> rank_of, id_of, external_ids;
    {
      std::lock_guard<std::mutex> lock(aux_view_mutex_);
      auto init_tx = m_pImpl->start_transaction(true);
      n = init_tx.num_vertices();
      rank_of.resize(n);
      id_of.resize(n);
      external_ids.resize(n);
      for (uint64_t v = 0; v < n; v++) {
        uint64_t rk = init_tx.logical_id(v);
        rank_of[v] = rk;
        id_of[rk] = v;
        external_ids[v] = GetExternalID(v);
      }
      init_tx.rollback();
    }
    if (n == 0) { EnsureUnregistered(); return; }

    std::vector<uint64_t> parent(n);
    for (uint64_t v = 0; v < n; v++) parent[v] = v;
    auto find = [&](uint64_t x) {
      while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
      return x;
    };

    {
      auto tx = m_pImpl->start_transaction(true);
      auto iterator = tx.iterator();
      for (uint64_t u = 0; u < n; u++) {
        iterator.edges(rank_of[u], true, [&](uint64_t dst_rank, double w) {
          uint64_t v = id_of[dst_rank];
          uint64_t ru = find(u), rv = find(v);
          if (ru != rv) parent[std::max(ru, rv)] = std::min(ru, rv);
        });
      }
    }
    EnsureUnregistered();

    last_result_.type = AlgorithmResult::kLong;
    for (uint64_t v = 0; v < n; v++)
      last_result_.long_vals.emplace_back(external_ids[v],
                                          static_cast<int64_t>(find(v)));
  }

  void RunLCC() override {
    last_result_.clear();
    EnsureRegistered();
    uint64_t n;
    std::vector<uint64_t> rank_of, id_of, external_ids;
    {
      std::lock_guard<std::mutex> lock(aux_view_mutex_);
      auto init_tx = m_pImpl->start_transaction(true);
      n = init_tx.num_vertices();
      rank_of.resize(n);
      id_of.resize(n);
      external_ids.resize(n);
      for (uint64_t v = 0; v < n; v++) {
        uint64_t rk = init_tx.logical_id(v);
        rank_of[v] = rk;
        id_of[rk] = v;
        external_ids[v] = GetExternalID(v);
      }
      init_tx.rollback();
    }
    if (n == 0) { EnsureUnregistered(); return; }

    std::vector<std::vector<uint64_t>> neighbors(n);
    {
      SharedReadTransaction read_tx(this);
#pragma omp parallel firstprivate(read_tx)
      {
        auto iterator = read_tx.transaction().iterator();
#pragma omp for schedule(dynamic, 64)
        for (uint64_t u = 0; u < n; u++) {
          auto& nbrs = neighbors[u];
          iterator.edges(rank_of[u], true, [&](uint64_t dst_rank, double w) {
            uint64_t v = id_of[dst_rank];
            if (v != u) nbrs.push_back(v);
          });
          std::sort(nbrs.begin(), nbrs.end());
          nbrs.erase(std::unique(nbrs.begin(), nbrs.end()), nbrs.end());
        }
      }
    }
    EnsureUnregistered();

    std::vector<double> lcc_scores(n, 0.0);
#pragma omp parallel for schedule(dynamic, 64)
    for (uint64_t v = 0; v < n; v++) {
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
    for (uint64_t v = 0; v < n; v++)
      last_result_.double_vals.emplace_back(external_ids[v], lcc_scores[v]);
  }

  void RunCDLP(int max_iters) override {
    last_result_.clear();
    EnsureRegistered();
    uint64_t n;
    std::vector<uint64_t> rank_of, id_of, external_ids;
    {
      std::lock_guard<std::mutex> lock(aux_view_mutex_);
      auto init_tx = m_pImpl->start_transaction(true);
      n = init_tx.num_vertices();
      rank_of.resize(n);
      id_of.resize(n);
      external_ids.resize(n);
      for (uint64_t v = 0; v < n; v++) {
        uint64_t rk = init_tx.logical_id(v);
        rank_of[v] = rk;
        id_of[rk] = v;
        external_ids[v] = GetExternalID(v);
      }
      init_tx.rollback();
    }
    if (n == 0) { EnsureUnregistered(); return; }

    std::vector<std::vector<uint64_t>> neighbors(n);
    {
      SharedReadTransaction read_tx(this);
#pragma omp parallel firstprivate(read_tx)
      {
        auto iterator = read_tx.transaction().iterator();
#pragma omp for schedule(dynamic, 64)
        for (uint64_t u = 0; u < n; u++) {
          auto& nbrs = neighbors[u];
          iterator.edges(rank_of[u], true, [&](uint64_t dst_rank, double w) {
            uint64_t v = id_of[dst_rank];
            if (v != u) nbrs.push_back(v);
          });
        }
      }
    }
    EnsureUnregistered();

    // Initialize labels with external IDs: tie-breaking compares numeric labels,
    // whereas rank/real-ID allocation order differs from external ID order.
    std::vector<uint64_t> labels(n);
    for (uint64_t v = 0; v < n; v++) labels[v] = external_ids[v];
    std::vector<uint64_t> next_labels(n);

    for (int iter = 0; iter < max_iters; iter++) {
      std::atomic<bool> changed{false};
#pragma omp parallel
      {
        std::vector<uint64_t> nbr_labels;
#pragma omp for schedule(dynamic, 64)
        for (uint64_t v = 0; v < n; v++) {
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
    for (uint64_t v = 0; v < n; v++)
      last_result_.long_vals.emplace_back(external_ids[v],
                                          static_cast<int64_t>(labels[v]));
  }

  bool SupportsAlgorithm(const std::string& algorithm) const override {
    return algorithm == "pr" || algorithm == "bfs" || algorithm == "sssp" ||
           algorithm == "wcc" || algorithm == "lcc" || algorithm == "cdlp";
  }

  struct CommonView {
    TeseoDriver* driver = nullptr;
    std::shared_ptr<SharedReadTransaction> read_tx;
    std::vector<uint64_t> external, rank_of, id_of;
    uint64_t vertex_count() const { return external.size(); }
    uint64_t external_id(uint64_t v) const { return external[v]; }
    uint64_t degree(uint64_t v) const {
      ensure_thread_registered();
      return read_tx->transaction().degree(v, true);
    }

   private:
    static auto& thread_state() {
      struct ThreadState {
        const SharedReadTransaction* tx_identity = nullptr;
        std::shared_ptr<SharedReadTransaction> tx;
        std::unique_ptr<WorkerRegistration> registration;
        std::unique_ptr<teseo::Iterator> iterator;
      };
      static thread_local ThreadState state;
      return state;
    }

    void ensure_thread_registered() const {
      auto& state = thread_state();
      if (state.tx_identity != read_tx.get()) {
        // Destroy the iterator before unregistering the worker and release the
        // previous transaction only after both handles are gone. Teseo allows
        // multiple iterators from one read-only transaction, but an iterator
        // itself must remain private to its worker thread.
        state.iterator.reset();
        state.registration.reset();
        state.tx.reset();

        state.tx_identity = read_tx.get();
        state.tx = read_tx;
        state.registration =
            std::make_unique<WorkerRegistration>(driver, true);
      }
    }

    teseo::Iterator& thread_iterator() const {
      ensure_thread_registered();
      auto& state = thread_state();
      if (!state.iterator) {
        state.iterator = std::make_unique<teseo::Iterator>(
            state.tx->transaction().iterator());
      }
      return *state.iterator;
    }

   public:
    template <typename Fn> void TraverseEdges(uint64_t v, Fn&& fn) const {
      auto& it = thread_iterator();
      it.edges(v, true, [&](uint64_t r, double) { if (r < id_of.size()) fn(r); });
    }
    template <typename Fn> bool TraverseEdgesUntil(uint64_t v, Fn&& fn) const {
      bool stopped = false;
      auto& it = thread_iterator();
      // Teseo exposes a void callback here, so traversal cannot be cancelled
      // in the storage layer; no callbacks are dispatched after stop.
      it.edges(v, true, [&](uint64_t r, double) {
        if (!stopped && r < id_of.size()) stopped = fn(r);
      });
      return stopped;
    }
    template <typename Fn> bool TraverseEdgesSorted(uint64_t v, Fn&& fn) const {
      // Teseo documents iterator.edges() as sorted.  Dense IDs in this view
      // are deliberately logical ranks, so the backend order is preserved.
      auto& it = thread_iterator();
      it.edges(v, true, [&](uint64_t r, double) { if (r < id_of.size()) fn(r); });
      return true;
    }
    template <typename Fn> void TraverseEdgesWithProperty(uint64_t v, Fn&& fn) const {
      auto& it = thread_iterator();
      it.edges(v, true, [&](uint64_t r, double w) { if (r < id_of.size()) fn(r, w); });
    }
    uint64_t resolve_root(uint64_t id) const { for (uint64_t v=0; v<external.size(); ++v) if (external[v]==id) return v; return std::numeric_limits<uint64_t>::max(); }
  };

  CommonView BuildCommonView() {
    CommonView view; view.driver = this; EnsureRegistered();
    auto init_tx = m_pImpl->start_transaction(true); const uint64_t n = init_tx.num_vertices();
    view.rank_of.resize(n); view.id_of.resize(n);
    view.external.resize(n);
    for (uint64_t v=0; v<n; ++v) {
      const auto r = init_tx.logical_id(v);
      view.rank_of[v] = r;
      if (r >= view.id_of.size()) view.id_of.resize(r + 1);
      view.id_of[r] = v;
      view.external[r] = GetExternalID(v);
    }
    init_tx.rollback();
    view.read_tx = std::make_shared<SharedReadTransaction>(this);
    EnsureUnregistered(); return view;
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
