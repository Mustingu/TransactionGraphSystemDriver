#pragma once
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "../GraphDriver.h"
#include "../common/algorithms/GraphKernels.h"
#include "tbb/concurrent_hash_map.h"
#include "tbb/concurrent_vector.h"

#include "include/neo_index.h"
#include "include/neo_snapshot.h"
#include "include/neo_transaction.h"

using namespace container;

thread_local WriterTraceBlock* tracer = nullptr;

class RSDriver : public GraphDriver {
 private:
  TransactionManager tm;
  const bool m_is_directed = true;
  const bool m_is_weighted = true;
  std::mutex growing_vector_mutex;
  int m_alpha = 15, m_beta = 18;

  tbb::concurrent_vector<uint64_t> p_mHashMap;

  std::atomic<int> node_num;

  void grow_vector_if_smaller(dst_t s) {
    if (p_mHashMap.size() <= s) {
      std::scoped_lock<std::mutex> l(growing_vector_mutex);
      if (p_mHashMap.size() <= s) {
        const size_t required = static_cast<size_t>(s) + 1;
        const size_t current = p_mHashMap.size();
        const size_t target = std::max(required, std::max<size_t>(current * 2, 1));
        std::cout << "Growing vector to " << target << std::endl;
        p_mHashMap.grow_to_at_least(target);
      }
    }
  }

  // TBB hash map for vertex ID translation.
  using vertex_dictionary_t = tbb::concurrent_hash_map<uint64_t, uint64_t>;
  vertex_dictionary_t* m_pHashMap;

  // Get or create an internal vertex ID.
  bool insert_vertex(uint64_t src) {
    auto tx = tm.get_write_transaction();
    bool inserted = true;
    try {
      tx->insert_vertex(src, nullptr);
      // std::cout << "Insert V before Commit " + std::to_string(src) + '\n';
      tx->commit();
      // std::cout << "Commit Finish " + std::to_string(src) + '\n';
    } catch (std::exception& e) {
      // print error message
      std::cerr << e.what() << std::endl;
      tx->abort();
      inserted = false;
    }
    delete tx;
    return inserted;
  }

  uint64_t GetInternalID(uint64_t src) {
    dst_t p_id;
    vertex_dictionary_t::accessor w;
    if (m_pHashMap->insert(w, src)) {
      p_id = node_num.fetch_add(1);
      grow_vector_if_smaller(p_id);
      p_mHashMap[p_id] = src;
      w->second = p_id;

      insert_vertex(p_id);
      // std::cout << "Insert V " + std::to_string(src) + " " +
      //                  std::to_string(p_id) + "\n";

      w.release();

    } else {
      p_id = w->second;
      w.release();
    }
    return p_id;
  }

  class Snapshot {
   private:
    const uint64_t m_num_vertices;
    const uint64_t m_num_edges;
    NeoSnapshot snapshot;

   public:
    explicit Snapshot(const TransactionManager& tm)
        : m_num_vertices(tm.vertex_count()),
          m_num_edges(tm.edge_count()),
          snapshot{&tm} {
      //                                                                             snapshot{tm.index_impl, tm.global_timestamp, true} {
    }

    ~Snapshot() = default;

    uint64_t vertex_count() const { return this->m_num_vertices; }
    uint64_t edge_count() const { return this->m_num_edges; }

    auto clone() const {
      //            std::cout << "Snapshot cloned with timestamp" << std::endl;
      return std::make_shared<Snapshot>(*this);
    }

    bool has_vertex(uint64_t vertex) const {
      auto non_const_this = const_cast<Snapshot*>(this);
      return non_const_this->snapshot.has_vertex(vertex);
    }

    uint64_t degree(uint64_t vertex) {
      if (!has_vertex(vertex)) {
        // std::cout << "Vertex does not exist " << vertex << " \n";
        return 0;
      }
      return snapshot.get_degree(vertex);
    }

    double edge_weight(uint64_t src, uint64_t dst) const {
      const uint64_t bits = snapshot.get_edge_property(src, dst, 0);
      double weight = 0.0;
      std::memcpy(&weight, &bits, sizeof(weight));
      return weight;
    }

    template <typename F>
    void edges(uint64_t src, F&& callback) {
      snapshot.edges(src, std::forward<F>(callback));
    }
  };

  std::shared_ptr<Snapshot> get_shared_snapshot() const {
    return std::make_shared<Snapshot>(tm);
  }

  void init_thread(int thread_id) { tracer = writer_register(); }
  void end_thread(int thread_id) { writer_unregister(tracer); }

  gapbs::pvector<std::atomic<int64_t>> init_distances(
      std::shared_ptr<Snapshot> m_snapshot) {
    const uint64_t N = m_snapshot->vertex_count();
    gapbs::pvector<std::atomic<int64_t>> distances(N);

    std::atomic<uint64_t> vertex_checked = 0;
    std::vector<std::thread> threads;
    uint64_t size = N;
    int m_num_threads = omp_get_max_threads();
    uint64_t chunk_size = (size + m_num_threads - 1) / m_num_threads;
    try {
      for (int i = 0; i < m_num_threads; i++) {
        threads.emplace_back(std::thread(
            [this, &vertex_checked, &distances, size, chunk_size,
             &m_snapshot](int thread_id) {
              init_thread(thread_id);
              auto snapshot_local = m_snapshot->clone();
              uint64_t start = thread_id * chunk_size;
              uint64_t end = start + chunk_size;
              if (end > size) end = size;

              for (uint64_t i = start; i < end; i++) {
                uint64_t out_degree = snapshot_local->degree(i);
                // uint64_t out_degree = 0;
                distances[i] = out_degree != 0 ? -out_degree : -1;
              }

              // delete snapshot_local;
              end_thread(thread_id);
            },
            i));
      }
    } catch (const std::exception& e) {
      std::cerr << "Exception in init_distances: " << e.what() << std::endl;
      throw;
    }

    for (auto& thread : threads) {
      thread.join();
    }

    return distances;
  }

  void QueueToBitmap(const gapbs::SlidingQueue<int64_t>& queue,
                     gapbs::Bitmap& bm, std::shared_ptr<Snapshot> m_snapshot) {
#pragma omp parallel for
    for (auto q_iter = queue.begin(); q_iter < queue.end(); q_iter++) {
      int64_t u = *q_iter;
      bm.set_bit_atomic(u);
    }
  }

  void BitmapToQueue(const int64_t size, const gapbs::Bitmap& bm,
                     gapbs::SlidingQueue<int64_t>& queue,
                     std::shared_ptr<Snapshot> m_snapshot) {
    const int64_t N = size;

#pragma omp parallel
    {
      gapbs::QueueBuffer<int64_t> lqueue(queue);
#pragma omp for
      for (int64_t n = 0; n < N; n++) {
        if (bm.get_bit(n)) lqueue.push_back(n);
      }
      lqueue.flush();
    }
    queue.slide_window();
  }

  int64_t TDStep(gapbs::pvector<std::atomic<int64_t>>& distances,
                 int64_t distance, gapbs::SlidingQueue<int64_t>& queue,
                 std::shared_ptr<Snapshot> m_snapshot) {
    int m_num_threads = omp_get_max_threads();
    std::vector<int64_t> results(m_num_threads, 0);
    uint64_t scout_count = 0;
    std::vector<std::thread> threads;

    uint64_t chunk_size = (queue.size() + m_num_threads - 1) / m_num_threads;

    try {
      for (uint64_t i = 0; i < m_num_threads; i++) {
        threads.emplace_back(std::thread(
            [this, &distances, distance, &scout_count, &queue, chunk_size,
             &results, &m_snapshot](int thread_id) {
              init_thread(thread_id);
              uint64_t start = thread_id * chunk_size;
              uint64_t end = start + chunk_size;

              if (end > queue.size()) end = queue.size();

              if (start < end) {
                gapbs::QueueBuffer<int64_t> lqueue(queue);

                auto snapshot_local = m_snapshot->clone();

                for (auto q_iter = queue.begin() + start; q_iter != queue.end();
                     q_iter++) {
                  int64_t u = *q_iter;

                  snapshot_local->edges(
                      u, [&distances, distance, &lqueue, &results, thread_id](
                             uint64_t destination, double w) {
                        int64_t curr_val = distances[destination];

                        if (curr_val < 0 &&
                            distances[destination].compare_exchange_strong(
                                curr_val, distance)) {
                          lqueue.push_back(destination);
                          results[thread_id] += -curr_val;
                        }
                      });
                }

                // std::unique_lock<std::mutex> lock(m_mutex);
                lqueue.flush();
              }
              // std::unique_lock<std::mutex> unlock(m_mutex);

              end_thread(thread_id);
            },
            i));
      }

      for (auto& thread : threads) {
        thread.join();
      }

      for (auto& result : results) {
        scout_count += result;
      }

    } catch (const std::exception& e) {
      std::cerr << "Exception in TDStep: " << e.what() << std::endl;
      throw e;
    }

    return scout_count;
  }

  int64_t BUStep(gapbs::pvector<std::atomic<int64_t>>& distances,
                 int64_t distance, gapbs::Bitmap& front, gapbs::Bitmap& next,
                 std::shared_ptr<Snapshot> m_snapshot) {
    const uint64_t N = m_snapshot->vertex_count();
    int64_t awake_count = 0;
    int m_num_threads = omp_get_max_threads();
    std::vector<uint64_t> results(m_num_threads, 0);
    std::vector<std::thread> threads;
    next.reset();

    uint64_t chunk_size = (N + m_num_threads - 1) / m_num_threads;

    try {
      for (int i = 0; i < m_num_threads; i++) {
        threads.emplace_back(std::thread(
            [this, &distances, distance, &front, &next, chunk_size,
             &awake_count, &results, N, &m_snapshot](int thread_id) {
              init_thread(thread_id);
              uint64_t start = thread_id * chunk_size;
              uint64_t end = std::min(start + chunk_size, N);
              auto snapshot_local = m_snapshot->clone();
              for (uint64_t u = start; u < end; u++) {
                if (distances[u] < 0) {
                  bool done = false;
                  snapshot_local->edges(u, [u, &distances, distance, &front,
                                            &next, &done, &results, &thread_id](
                                               uint64_t destination, double w) {
                    if (destination > distances.size()) return;
                    if (front.get_bit(destination)) {
                      distances[u] = distance;
                      results[thread_id]++;
                      next.set_bit(u);
                      done = true;
                    }
                  });
                }
              }
              end_thread(thread_id);
            },
            i));
      }

      for (auto& thread : threads) {
        thread.join();
      }

      for (auto& result : results) {
        awake_count += result;
      }

    } catch (const std::exception& e) {
      std::cerr << "Exception in BUStep: " << e.what() << std::endl;
      throw e;
    }
    return awake_count;
  }

 public:
  RSDriver() : tm(true, true), p_mHashMap(1024) {}
  std::string Name() const override { return "RapidStore"; }

  bool SupportsAlgorithm(const std::string& algorithm) const override {
    return algorithm == "pr" || algorithm == "bfs" || algorithm == "sssp" ||
           algorithm == "wcc" || algorithm == "lcc" || algorithm == "cdlp";
  }

  const AlgorithmResult& GetLastResult() const override { return last_result; }

  void RegisterThread(int thread_id) override {
    tracer = writer_register();
    std::cout << "Register Thread " + std::to_string(thread_id) + " " +
                     std::to_string((uint64_t)tracer) + "\n";
  }

  void DeregisterThread(int thread_id) override { writer_unregister(tracer); }
  void Init(int thread_num) override {
    node_num = 0;
    m_pHashMap = new vertex_dictionary_t();
    printf("[RapidStore] Initialized.\n");
  }

  bool UpsertEdge(uint64_t src, uint64_t dst, uint64_t property,
                  int thread_id) override {
    auto src_p = GetInternalID(src);
    auto dst_p = GetInternalID(dst);
    double weight = 0.0;
    std::memcpy(&weight, &property, sizeof(weight));
    Property_t stored_property = 0;
    std::memcpy(&stored_property, &weight, sizeof(stored_property));
    auto tx = tm.get_write_transaction();
    tx->insert_edge(src_p, dst_p, &stored_property);
    tx->insert_edge(dst_p, src_p, &stored_property);
    tx->commit();
    delete tx;
    return true;
  }

  // Look up existing vertices without creating them on the deletion path.
  uint64_t FindInternalID(uint64_t src) {
    vertex_dictionary_t::const_accessor a;
    if (m_pHashMap->find(a, src)) return a->second;
    return std::numeric_limits<uint64_t>::max();
  }

  // The undirected remove_edge branch calls NeoTreeVersion::remove_edge
  // for both directions and returns early for missing edges.
  // The upstream directed branch calls insert_edge instead; do not use it for deletion.
  bool DeleteEdge(uint64_t src, uint64_t dst, int thread_id) override {
    uint64_t s = FindInternalID(src), d = FindInternalID(dst);
    if (s == std::numeric_limits<uint64_t>::max() ||
        d == std::numeric_limits<uint64_t>::max())
      return false;
    LightWriteTransaction::remove_edge(s, d, false, &tm, tracer);
    return true;
  }

  bool InsertVertex(uint64_t src) override {
    auto src_p = GetInternalID(src);
    return true;
  }

  void FinishWrites() override {}

  void RunPageRank(int iterations, double damping_factor,
                   S_Driver* global_counter) override {
    // std::cout << "[RapidStore] Running PageRank\n";

    auto m_snapshot = get_shared_snapshot();
    // auto m_snapshot = new NeoSnapshot(&tm);

    // std::cout << "[RapidStore] Running PageRank2\n";

    const uint64_t num_vertices = m_snapshot->vertex_count();
    // delete m_snapshot;

    // std::cout << "[RapidStore] Running PageRank3\n";

    const double init_score = 1.0 / num_vertices;
    const double base_score = (1.0 - damping_factor) / num_vertices;

    std::unique_ptr<double[]> ptr_scores{new double[num_vertices]()};
    double* scores = ptr_scores.get();
#pragma omp parallel for
    for (uint64_t v = 0; v < num_vertices; v++) {
      scores[v] = init_score;
    }
    gapbs::pvector<double> outgoing_contrib(num_vertices, 0.0);

    auto m_num_threads = omp_get_max_threads();
    std::cout << "[RapidStore] Running PageRank with " +
                     std::to_string(m_num_threads) + " threads\n";

    for (uint64_t iter = 0; iter < iterations; iter++) {
      std::vector<double> dangling_sums(m_num_threads, 0.0);
      double dangling_sum = 0.0;

      uint64_t chunk_size = (num_vertices + m_num_threads - 1) / m_num_threads;
      std::vector<std::thread> threads;
      try {
        // m_interface->set_max_threads(m_num_threads);
        for (int i = 0; i < m_num_threads; i++) {
          threads.emplace_back(std::thread(
              [this, &dangling_sums, &outgoing_contrib, chunk_size,
               num_vertices, &scores, &m_snapshot](int thread_id) {
                init_thread(thread_id);
                auto snapshot_local = m_snapshot->clone();
                uint64_t start = thread_id * chunk_size;
                uint64_t end = std::min(start + chunk_size, num_vertices);

                for (uint64_t v = start; v < end; v++) {
                  uint64_t out_degree = snapshot_local->degree(v);
                  if (out_degree == 0) {
                    dangling_sums[thread_id] += scores[v];
                  } else {
                    outgoing_contrib[v] = scores[v] / out_degree;
                  }
                }

                // delete snapshot_local;
                end_thread(thread_id);
              },
              i));
        }

        for (auto& t : threads) {
          t.join();
        }

        threads.clear();

        for (int i = 0; i < m_num_threads; i++) {
          dangling_sum += dangling_sums[i];
        }

        dangling_sum /= num_vertices;

        for (int i = 0; i < m_num_threads; i++) {
          threads.emplace_back(std::thread(
              [this, &dangling_sum, &outgoing_contrib, chunk_size, num_vertices,
               &scores, &base_score, &damping_factor,
               &m_snapshot](int thread_id) {
                init_thread(thread_id);
                auto snapshot_local = m_snapshot->clone();
                uint64_t start = thread_id * chunk_size;
                uint64_t end = std::min(start + chunk_size, num_vertices);

                for (uint64_t v = start; v < end; v++) {
                  double incoming_totol = 0.0;
                  snapshot_local->edges(v, [&](uint64_t src, double w) {
                    incoming_totol += outgoing_contrib[src];
                  });

                  scores[v] = base_score +
                              damping_factor * (incoming_totol + dangling_sum);
                }

                // delete snapshot_local;
                end_thread(thread_id);
              },
              i));
        }

        for (auto& t : threads) {
          t.join();
        }
      } catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        throw e;
      }
    }

    last_result.clear();
    last_result.type = AlgorithmResult::kDouble;
    last_result.double_vals.reserve(num_vertices);
    for (uint64_t logical_id = 0; logical_id < num_vertices; logical_id++) {
      last_result.double_vals.emplace_back(p_mHashMap[logical_id],
                                           scores[logical_id]);
    }
  }

  void RunBFS(uint64_t root, S_Driver* global_counter) override {
    auto m_snapshot = get_shared_snapshot();
    uint64_t internal_root = GetInternalID(root);
    gapbs::pvector<std::atomic<int64_t>> distances = init_distances(m_snapshot);

    std::cout << "[RapidStore] Running BFS with " << omp_get_max_threads()
              << " threads\n";

    distances[internal_root] = 0;

    gapbs::SlidingQueue<int64_t> queue(m_snapshot->vertex_count());
    queue.push_back(internal_root);
    queue.slide_window();

    gapbs::Bitmap curr(m_snapshot->vertex_count());
    curr.reset();
    gapbs::Bitmap front(m_snapshot->vertex_count());
    front.reset();

    int64_t edges_to_check = m_snapshot->edge_count();
    int64_t scout_count = m_snapshot->degree(internal_root);
    int64_t distance = 1;

    while (!queue.empty()) {
      if (scout_count > edges_to_check / m_alpha) {
        int64_t awake_count, old_awake_count;
        QueueToBitmap(queue, front, m_snapshot);
        awake_count = queue.size();
        queue.slide_window();

        do {
          old_awake_count = awake_count;
          awake_count = BUStep(distances, distance, front, curr, m_snapshot);
          front.swap(curr);
          distance++;
        } while ((awake_count >= old_awake_count) ||
                 (awake_count > m_snapshot->vertex_count() / m_beta));
        BitmapToQueue(m_snapshot->vertex_count(), front, queue, m_snapshot);
        scout_count = 1;

      } else {
        edges_to_check -= scout_count;
        scout_count = TDStep(distances, distance, queue, m_snapshot);
        queue.slide_window();
        distance++;
      }
    }

    last_result.clear();
    last_result.type = AlgorithmResult::kLong;
    auto N = m_snapshot->vertex_count();
    last_result.long_vals.reserve(N);
    for (uint64_t logical_id = 0; logical_id < N; logical_id++) {
      last_result.long_vals.emplace_back(
          p_mHashMap[logical_id], distances[logical_id].load());
    }
  }

  void RunSSSP(uint64_t root, S_Driver* global_counter) override {
    auto snapshot = get_shared_snapshot();
    const uint64_t n = snapshot->vertex_count();
    last_result.clear();
    last_result.type = AlgorithmResult::kDouble;
    if (n == 0) return;
    const uint64_t source = FindInternalID(root);
    const double infinity_value = std::numeric_limits<double>::infinity();
    if (source == std::numeric_limits<uint64_t>::max() || source >= n) return;

    std::vector<std::atomic<double>> distance(n);
    for (auto& d : distance) d.store(infinity_value, std::memory_order_relaxed);
    distance[source].store(0.0, std::memory_order_relaxed);
    bool changed = true;
    while (changed) {
      std::atomic<bool> round_changed{false};
#pragma omp parallel for schedule(dynamic, 256)
      for (uint64_t u = 0; u < n; ++u) {
        const double du = distance[u].load(std::memory_order_relaxed);
        if (!std::isfinite(du)) continue;
        snapshot->edges(u, [&](uint64_t v, double edge_weight) {
          if (v >= n) return;
          const double candidate = du + edge_weight;
          double old = distance[v].load(std::memory_order_relaxed);
          bool relaxed = false;
          while (candidate < old) {
            if (distance[v].compare_exchange_weak(
                    old, candidate, std::memory_order_relaxed)) {
              relaxed = true;
              break;
            }
          }
          if (relaxed) round_changed.store(true, std::memory_order_relaxed);
        });
      }
      changed = round_changed.load(std::memory_order_relaxed);
    }
    last_result.double_vals.reserve(n);
    for (uint64_t v = 0; v < n; ++v)
      last_result.double_vals.emplace_back(
          p_mHashMap[v], distance[v].load(std::memory_order_relaxed));
  }

  void RunWCC() override {
    auto snapshot = get_shared_snapshot();
    const uint64_t n = snapshot->vertex_count();
    last_result.clear();
    last_result.type = AlgorithmResult::kLong;
    if (n == 0) return;
    std::vector<uint64_t> parent(n);
    for (uint64_t v = 0; v < n; ++v) parent[v] = v;
    auto find = [&](uint64_t x) {
      while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
      return x;
    };
    for (uint64_t v = 0; v < n; ++v)
      snapshot->edges(v, [&](uint64_t u, double) {
        if (u >= n) return;
        uint64_t a = find(v), b = find(u);
        while (a != b) {
          if (a < b) { if (parent[b] == b) { parent[b] = a; break; } b = parent[b]; }
          else { if (parent[a] == a) { parent[a] = b; break; } a = parent[a]; }
        }
      });
    std::vector<uint64_t> minimum(n, std::numeric_limits<uint64_t>::max());
    for (uint64_t v = 0; v < n; ++v) minimum[find(v)] = std::min(minimum[find(v)], p_mHashMap[v]);
    last_result.long_vals.reserve(n);
    for (uint64_t v = 0; v < n; ++v) last_result.long_vals.emplace_back(p_mHashMap[v], minimum[find(v)]);
  }

  void RunLCC() override {
    auto snapshot = get_shared_snapshot();
    const uint64_t n = snapshot->vertex_count();
    last_result.clear(); last_result.type = AlgorithmResult::kDouble;
    std::vector<std::vector<uint64_t>> adjacency(n);
#pragma omp parallel for schedule(dynamic, 128)
    for (uint64_t v = 0; v < n; ++v) snapshot->edges(v, [&](uint64_t u, double) { if (u < n && u != v) adjacency[v].push_back(u); });
#pragma omp parallel for schedule(dynamic, 128)
    for (uint64_t v = 0; v < n; ++v) { auto& a = adjacency[v]; std::sort(a.begin(), a.end()); a.erase(std::unique(a.begin(), a.end()), a.end()); }
    std::vector<double> score(n, 0.0);
#pragma omp parallel for schedule(dynamic, 64)
    for (uint64_t v = 0; v < n; ++v) {
      const auto& a = adjacency[v]; if (a.size() < 2) continue;
      uint64_t common = 0;
      for (uint64_t u : a) { const auto& b = adjacency[u]; size_t i=0,j=0; while(i<a.size() && j<b.size()) { if(a[i]==b[j]) ++common,++i,++j; else if(a[i]<b[j]) ++i; else ++j; } }
      score[v] = static_cast<double>(common) / (a.size() * (a.size()-1));
    }
    last_result.double_vals.reserve(n); for (uint64_t v=0; v<n; ++v) last_result.double_vals.emplace_back(p_mHashMap[v], score[v]);
  }

  struct CommonView {
    mutable std::shared_ptr<Snapshot> snapshot;
    std::vector<uint64_t> external;
    uint64_t vertex_count() const { return external.size(); }
    uint64_t external_id(uint64_t v) const { return external[v]; }
    uint64_t degree(uint64_t v) const { return snapshot->degree(v); }
    template <typename Fn> void TraverseEdges(uint64_t v, Fn&& fn) const {
      snapshot->edges(v, [&](uint64_t u, double) { if (u < external.size() && u != v) fn(u); });
    }
    template <typename Fn> bool TraverseEdgesUntil(uint64_t v, Fn&& fn) const {
      bool stopped = false;
      // NeoGraph's snapshot callback is not cancellable; suppress callbacks
      // after the requested stop while allowing its storage scan to finish.
      snapshot->edges(v, [&](uint64_t u, double) {
        if (!stopped && u < external.size() && u != v) stopped = fn(u);
      });
      return stopped;
    }
    template <typename Fn> bool TraverseEdgesSorted(uint64_t, Fn&&) const {
      // NeoGraph's snapshot callback has no documented sorted-order contract.
      return false;
    }
    template <typename Fn> void TraverseEdgesWithProperty(uint64_t v, Fn&& fn) const {
      snapshot->edges(v, [&](uint64_t u, double w) { if (u < external.size() && u != v) fn(u, w); });
    }
    uint64_t resolve_root(uint64_t id) const { for (uint64_t v=0; v<external.size(); ++v) if (external[v]==id) return v; return std::numeric_limits<uint64_t>::max(); }
  };

  CommonView BuildCommonView() const {
    CommonView view; const uint64_t n=tm.vertex_count(); view.external.reserve(n);
    for (uint64_t v=0; v<n; ++v) view.external.push_back(p_mHashMap[v]);
    view.snapshot=get_shared_snapshot(); return view;
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
      last_result.clear(); last_result.type = AlgorithmResult::kLong;
      for (uint64_t v = 0; v < labels.size(); ++v)
        last_result.long_vals.emplace_back(view.external_id(v), static_cast<int64_t>(labels[v]));
    }
    return true;
  }
  bool RunLCCCommon() override {
    auto view = BuildCommonView();
    auto values = common::algorithms::lcc(view);
    if (ShouldCollectAlgorithmResults()) {
      last_result.clear(); last_result.type = AlgorithmResult::kDouble;
      for (uint64_t v = 0; v < values.size(); ++v)
        last_result.double_vals.emplace_back(view.external_id(v), values[v]);
    }
    return true;
  }
  bool RunPageRankCommon(int iterations, double damping) override {
    auto view = BuildCommonView();
    auto scores = common::algorithms::pagerank(view, iterations, damping);
    if (ShouldCollectAlgorithmResults()) {
      last_result.clear(); last_result.type = AlgorithmResult::kDouble;
      for (uint64_t v = 0; v < scores.size(); ++v)
        last_result.double_vals.emplace_back(view.external_id(v), scores[v]);
    }
    return true;
  }
  bool RunBFSCommon(uint64_t root) override {
    auto view = BuildCommonView();
    const uint64_t r = view.resolve_root(root);
    if (r == std::numeric_limits<uint64_t>::max()) return false;
    auto dist = common::algorithms::bfs(view, r);
    if (ShouldCollectAlgorithmResults()) {
      last_result.clear(); last_result.type = AlgorithmResult::kLong;
      for (uint64_t v = 0; v < dist.size(); ++v)
        last_result.long_vals.emplace_back(view.external_id(v), dist[v]);
    }
    return true;
  }
  bool RunSSSPCommon(uint64_t root) override {
    auto view = BuildCommonView();
    const uint64_t r = view.resolve_root(root);
    if (r == std::numeric_limits<uint64_t>::max()) return false;
    auto dist = common::algorithms::sssp_weighted(view, r);
    if (ShouldCollectAlgorithmResults()) {
      last_result.clear(); last_result.type = AlgorithmResult::kDouble;
      for (uint64_t v = 0; v < dist.size(); ++v)
        last_result.double_vals.emplace_back(view.external_id(v), dist[v]);
    }
    return true;
  }
  bool RunWCCCommon() override {
    auto view = BuildCommonView();
    auto comp = common::algorithms::wcc(view);
    if (ShouldCollectAlgorithmResults()) {
      last_result.clear(); last_result.type = AlgorithmResult::kLong;
      for (uint64_t v = 0; v < comp.size(); ++v)
        last_result.long_vals.emplace_back(view.external_id(v), static_cast<int64_t>(comp[v]));
    }
    return true;
  }

  void RunCDLP(int max_iters) override {
    auto snapshot = get_shared_snapshot(); const uint64_t n = snapshot->vertex_count();
    last_result.clear(); last_result.type = AlgorithmResult::kLong; if (n == 0) return;
    std::vector<std::vector<uint64_t>> adjacency(n);
#pragma omp parallel for schedule(dynamic, 128)
    for (uint64_t v=0; v<n; ++v) snapshot->edges(v, [&](uint64_t u,double){ if(u<n) adjacency[v].push_back(u); });
    std::vector<uint64_t> label(n), next(n); for(uint64_t v=0;v<n;++v) label[v]=p_mHashMap[v];
    for(int iter=0; iter<max_iters; ++iter) {
#pragma omp parallel for schedule(dynamic, 128)
      for(uint64_t v=0;v<n;++v) { if(adjacency[v].empty()){next[v]=label[v];continue;} std::vector<std::pair<uint64_t,uint64_t>> counts; for(uint64_t u:adjacency[v]) { auto it=std::find_if(counts.begin(),counts.end(),[&](auto& x){return x.first==label[u];}); if(it==counts.end()) counts.emplace_back(label[u],1); else ++it->second; } next[v]=std::max_element(counts.begin(),counts.end(),[](auto& a,auto& b){return a.second<b.second || (a.second==b.second && a.first>b.first); })->first; }
      label.swap(next);
    }
    last_result.long_vals.reserve(n); for(uint64_t v=0;v<n;++v) last_result.long_vals.emplace_back(p_mHashMap[v], static_cast<int64_t>(label[v]));
  }

  AlgorithmResult last_result;
};
