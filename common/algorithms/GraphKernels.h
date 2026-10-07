#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <utility>
#include <vector>

#include <omp.h>

namespace common::algorithms {

namespace detail {

// Optional sorted-traversal hook. A view returns true only when it can stream
// the neighbors in the promised order; otherwise LCC collects and sorts.
template <typename View, typename Fn>
auto traverse_edges_sorted_if_supported(const View& view, uint64_t v, Fn&& fn, int)
    -> decltype(view.TraverseEdgesSorted(v, std::forward<Fn>(fn)), bool()) {
  return view.TraverseEdgesSorted(v, std::forward<Fn>(fn));
}

template <typename View, typename Fn>
bool traverse_edges_sorted_if_supported(const View&, uint64_t, Fn&&, ...) {
  return false;
}

inline bool lcc_env_enabled(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

inline bool lcc_timing_enabled() {
  static const bool enabled = lcc_env_enabled("COMMON_LCC_TIMING");
  return enabled;
}

inline bool lcc_force_sort() {
  static const bool enabled = lcc_env_enabled("COMMON_LCC_FORCE_SORT");
  return enabled;
}

inline bool lcc_local_counts() {
  static const bool enabled = lcc_env_enabled("COMMON_LCC_LOCAL_COUNTS");
  return enabled;
}

}  // namespace detail

// Collect one adjacency list in sorted dense-id order.  Backends with a
// native ordered scan can stream directly into the vector; all other views
// retain the old collect-and-sort behavior.  unique() is deliberately kept
// in both paths because versioned backends may expose duplicate historical
// records before their iterator filters them.
template <typename View>
std::vector<uint64_t> collect_sorted_neighbors(const View& view, uint64_t v,
                                               bool* used_native_sorted = nullptr) {
  std::vector<uint64_t> neighbors;
  bool native_sorted = false;
  if (!detail::lcc_force_sort()) {
    native_sorted = detail::traverse_edges_sorted_if_supported(
        view, v, [&](uint64_t u) { neighbors.push_back(u); }, 0);
  }
  if (!native_sorted) {
    neighbors.clear();
    view.TraverseEdges(v, [&](uint64_t u) { neighbors.push_back(u); });
    std::sort(neighbors.begin(), neighbors.end());
  }
  neighbors.erase(std::unique(neighbors.begin(), neighbors.end()),
                  neighbors.end());
  if (used_native_sorted != nullptr) *used_native_sorted = native_sorted;
  return neighbors;
}

// Read-only graph view contract used by the portable kernels. Each backend
// keeps its storage-specific read transaction/snapshot and ID mapping in the
// view, and streams edges through templated callbacks (no type-erased callback
// on the per-edge hot path). TraverseEdges* take a dense vertex ID. The view
// must not materialize a graph-wide adjacency copy. TraverseEdgesSorted
// returns true only when native ordered traversal was used; false requests the
// kernel's collect-and-sort fallback. TraverseEdgesUntil invokes the callback
// until it returns true, then suppresses further callbacks and returns true.
// Weighted traversal supplies a dense destination ID and its double weight.
template <typename View>
std::vector<uint64_t> cdlp(const View& view, int max_iterations) {
  const uint64_t n = view.vertex_count();
  std::vector<uint64_t> labels(n), next(n);
#pragma omp parallel for schedule(static)
  for (uint64_t v = 0; v < n; v++) labels[v] = view.external_id(v);

  for (int iteration = 0; iteration < max_iterations; iteration++) {
    int changed = 0;
#pragma omp parallel reduction(| : changed)
    {
      std::vector<uint64_t> neighbors;
#pragma omp for schedule(dynamic, 256)
      for (uint64_t v = 0; v < n; v++) {
        neighbors.clear();
        view.TraverseEdges(v, [&](uint64_t u) { neighbors.push_back(labels[u]); });
        if (neighbors.empty()) {
          next[v] = labels[v];
          continue;
        }
        std::sort(neighbors.begin(), neighbors.end());
        uint64_t best = neighbors.front();
        uint64_t best_count = 1;
        for (size_t i = 0; i < neighbors.size();) {
          size_t j = i + 1;
          while (j < neighbors.size() && neighbors[j] == neighbors[i]) j++;
          const uint64_t count = j - i;
          if (count > best_count || (count == best_count && neighbors[i] < best)) {
            best = neighbors[i];
            best_count = count;
          }
          i = j;
        }
        next[v] = best;
        changed |= (next[v] != labels[v]);
      }
    }
    labels.swap(next);
    if (!changed) break;
  }
  return labels;
}

// Triangle-counting LCC kernel. The degree/id orientation ensures every
// triangle is enumerated once while the result uses the standard undirected
// clustering coefficient definition. Adjacency is intentionally streamed:
// only the two forward lists currently being intersected are retained.
template <typename View>
std::vector<double> lcc(const View& view) {
  using clock = std::chrono::steady_clock;
  const bool timing = detail::lcc_timing_enabled();
  const auto total_start = clock::now();
  const uint64_t n = view.vertex_count();
  std::vector<uint64_t> degrees(n, 0);
#pragma omp parallel for schedule(dynamic, 256)
  for (uint64_t v = 0; v < n; v++) {
    degrees[v] = view.degree(v);
  }
  const auto degree_end = clock::now();

  const bool local_counts = detail::lcc_local_counts();
  std::vector<uint64_t> triangles(n, 0);
  std::vector<std::vector<uint64_t>> local_triangles;
  if (local_counts) {
    local_triangles.resize(static_cast<size_t>(omp_get_max_threads()));
    for (auto& counts : local_triangles) counts.assign(n, 0);
  }
  std::atomic<uint64_t> native_scans{0};
  std::atomic<uint64_t> fallback_scans{0};
#pragma omp parallel for schedule(dynamic, 64)
  for (uint64_t v = 0; v < n; v++) {
    const int tid = omp_get_thread_num();
    bool native_sorted = false;
    auto forward_v = collect_sorted_neighbors(view, v, &native_sorted);
    if (native_sorted)
      native_scans.fetch_add(1, std::memory_order_relaxed);
    else
      fallback_scans.fetch_add(1, std::memory_order_relaxed);
    forward_v.erase(
        std::remove_if(forward_v.begin(), forward_v.end(), [&](uint64_t u) {
          return !(degrees[v] < degrees[u] ||
                   (degrees[v] == degrees[u] && v < u));
        }),
        forward_v.end());

    for (uint64_t u : forward_v) {
      bool u_native_sorted = false;
      auto forward_u = collect_sorted_neighbors(view, u, &u_native_sorted);
      if (u_native_sorted)
        native_scans.fetch_add(1, std::memory_order_relaxed);
      else
        fallback_scans.fetch_add(1, std::memory_order_relaxed);
      forward_u.erase(
          std::remove_if(forward_u.begin(), forward_u.end(), [&](uint64_t w) {
            return !(degrees[u] < degrees[w] ||
                     (degrees[u] == degrees[w] && u < w));
          }),
          forward_u.end());

      // Forward adjacency is oriented by (degree, id), not by numeric id.
      // Therefore a common forward neighbor may have an ID smaller than u;
      // start the intersection at the beginning rather than using upper_bound.
      auto i = forward_v.begin();
      auto j = forward_u.begin();
      while (i != forward_v.end() && j != forward_u.end()) {
        if (*i == *j) {
          if (local_counts) {
            auto& counts = local_triangles[static_cast<size_t>(tid)];
            ++counts[v];
            ++counts[u];
            ++counts[*i];
          } else {
#pragma omp atomic update
            ++triangles[v];
#pragma omp atomic update
            ++triangles[u];
#pragma omp atomic update
            ++triangles[*i];
          }
          ++i;
          ++j;
        } else if (*i < *j) {
          ++i;
        } else {
          ++j;
        }
      }
    }
  }
  if (local_counts) {
#pragma omp parallel for schedule(static)
    for (uint64_t v = 0; v < n; ++v) {
      for (const auto& counts : local_triangles) triangles[v] += counts[v];
    }
  }
  const auto intersection_end = clock::now();

  std::vector<double> result(n, 0.0);
#pragma omp parallel for schedule(dynamic, 256)
  for (uint64_t v = 0; v < n; v++) {
    if (degrees[v] >= 2)
      result[v] = 2.0 * triangles[v] /
                  (static_cast<double>(degrees[v]) * (degrees[v] - 1));
  }
  const auto result_end = clock::now();
  if (timing) {
    const auto ms = [](clock::time_point begin, clock::time_point end) {
      return std::chrono::duration<double, std::milli>(end - begin).count();
    };
    std::cerr << "LCC_PHASE_TIMING"
              << " native_sorted_scans=" << native_scans.load()
              << " fallback_sorted_scans=" << fallback_scans.load()
              << " degree_ms=" << ms(total_start, degree_end)
              << " streaming_neighbor_intersection_ms="
              << ms(degree_end, intersection_end)
              << " result_materialization_ms=" << ms(intersection_end, result_end)
              << " total_ms=" << ms(total_start, result_end)
              << " force_sort=" << (detail::lcc_force_sort() ? 1 : 0)
              << " local_counts=" << (local_counts ? 1 : 0) << '\n';
  }
  return result;
}

// Power-iteration PageRank. Init score 1/n; dangling scores summed and divided
// by n; score = base + d * (incoming_contrib + dangling/n). Matches the AVB and
// CSR reference conventions (fixed iteration count).
template <typename View>
std::vector<double> pagerank(const View& view, int iterations,
                             double damping_factor) {
  const uint64_t n = view.vertex_count();
  std::vector<double> scores(n, 1.0 / n), next(n, 0.0), contrib(n, 0.0);
  std::vector<uint64_t> deg(n, 0);
#pragma omp parallel for schedule(static)
  for (uint64_t v = 0; v < n; v++) deg[v] = view.degree(v);
  const double base = (1.0 - damping_factor) / n;

  for (int iter = 0; iter < iterations; iter++) {
    double dangling = 0.0;
#pragma omp parallel for reduction(+ : dangling) schedule(dynamic, 1024)
    for (uint64_t v = 0; v < n; v++) {
      if (deg[v] == 0) {
        contrib[v] = 0.0;
        dangling += scores[v];
      } else {
        contrib[v] = scores[v] / deg[v];
      }
    }
    dangling /= n;
#pragma omp parallel for schedule(dynamic, 1024)
    for (uint64_t v = 0; v < n; v++) {
      double incoming = 0.0;
      view.TraverseEdges(v, [&](uint64_t u) { incoming += contrib[u]; });
      next[v] = base + damping_factor * (incoming + dangling);
    }
    scores.swap(next);
  }
  return scores;
}

// Level-synchronous BFS over the undirected view. Root distance 0, unreached
// vertices use -1. Returns a dense distance vector.
template <typename View>
std::vector<int64_t> bfs(const View& view, uint64_t dense_root) {
  const uint64_t n = view.vertex_count();
  const int64_t kUnreached = -1;
  std::vector<std::atomic<int64_t>> dist(n);
  for (uint64_t v = 0; v < n; v++) dist[v].store(kUnreached);
  if (dense_root < n) dist[dense_root].store(0);

  std::vector<uint64_t> frontier{dense_root};
  int64_t level = 0;
  while (!frontier.empty()) {
    level++;
    std::vector<uint64_t> next;
    const int num_threads = omp_get_max_threads();
    std::vector<std::vector<uint64_t>> local(num_threads);
#pragma omp parallel
    {
      int tid = omp_get_thread_num();
#pragma omp for schedule(dynamic, 256)
      for (size_t i = 0; i < frontier.size(); i++) {
        const uint64_t u = frontier[i];
        view.TraverseEdges(u, [&](uint64_t v) {
          int64_t expected = kUnreached;
          if (dist[v].compare_exchange_strong(expected, level))
            local[tid].push_back(v);
        });
      }
    }
    for (auto& bucket : local)
      next.insert(next.end(), bucket.begin(), bucket.end());
    frontier.swap(next);
  }
  std::vector<int64_t> out(n);
  for (uint64_t v = 0; v < n; v++) out[v] = dist[v].load();
  return out;
}

// Union-find WCC. Component label = minimum external vertex id in the
// component, matching the official reference convention. Returns, per dense
// vertex, the component label in external-id space.
template <typename View>
std::vector<uint64_t> wcc(const View& view) {
  const uint64_t n = view.vertex_count();
  std::vector<uint64_t> parent(n);
  for (uint64_t v = 0; v < n; v++) parent[v] = v;

  auto find = [&](uint64_t x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };

  for (uint64_t v = 0; v < n; v++) {
    view.TraverseEdges(v, [&](uint64_t u) {
      uint64_t rv = find(v), ru = find(u);
      if (rv != ru) parent[std::max(rv, ru)] = std::min(rv, ru);
    });
  }

  std::vector<uint64_t> external(n);
  for (uint64_t v = 0; v < n; v++) external[v] = view.external_id(v);

  std::vector<uint64_t> min_external(n, std::numeric_limits<uint64_t>::max());
  for (uint64_t v = 0; v < n; v++) {
    uint64_t r = find(v);
    if (external[v] < min_external[r]) min_external[r] = external[v];
  }
  for (uint64_t v = 0; v < n; v++) external[v] = min_external[find(v)];
  return external;
}

// Parallel Bellman-Ford SSSP with unit edge weights.
template <typename View>
std::vector<double> sssp(const View& view, uint64_t dense_root) {
  const uint64_t n = view.vertex_count();
  const double kInf = std::numeric_limits<double>::infinity();
  std::vector<std::atomic<double>> dist(n);
  for (uint64_t v = 0; v < n; v++) dist[v].store(kInf);
  if (dense_root < n) dist[dense_root].store(0.0);
  bool changed = true;
  while (changed) {
    changed = false;
#pragma omp parallel for schedule(dynamic, 1024)
    for (uint64_t u = 0; u < n; u++) {
      const double du = dist[u].load();
      if (du == kInf) continue;
      view.TraverseEdges(u, [&](uint64_t v) {
        const double desired = du + 1.0;
        double cur = dist[v].load();
        while (cur > desired) {
          if (dist[v].compare_exchange_weak(cur, desired)) {
            changed = true;
            break;
          }
        }
      });
    }
  }
  std::vector<double> out(n);
  for (uint64_t v = 0; v < n; v++) out[v] = dist[v].load();
  return out;
}

// Weighted Bellman-Ford variant. The view must provide
// TraverseEdgesWithProperty(v, callback(dst, weight)).
template <typename View>
std::vector<double> sssp_weighted(const View& view, uint64_t dense_root) {
  const uint64_t n = view.vertex_count();
  const double kInf = std::numeric_limits<double>::infinity();
  std::vector<std::atomic<double>> dist(n);
  for (uint64_t v = 0; v < n; v++) dist[v].store(kInf);
  if (dense_root < n) dist[dense_root].store(0.0);
  // Bellman-Ford needs at most |V|-1 relaxation rounds for graphs without a
  // reachable negative cycle.  The old implementation used an unbounded
  // loop and wrote a plain bool from OpenMP workers, which could both spin
  // forever and race even on valid non-negative Graphalytics inputs.
  std::atomic<bool> changed{true};
  for (uint64_t round = 0; round + 1 < n; ++round) {
    changed.store(false, std::memory_order_relaxed);
#pragma omp parallel for schedule(dynamic, 1024)
    for (uint64_t u = 0; u < n; u++) {
      const double du = dist[u].load();
      if (du == kInf) continue;
      view.TraverseEdgesWithProperty(u, [&](uint64_t v, double w) {
        if (v >= n || !std::isfinite(w)) return;
        const double desired = du + w;
        double cur = dist[v].load();
        while (cur > desired) {
          if (dist[v].compare_exchange_weak(cur, desired)) {
            changed.store(true, std::memory_order_relaxed);
            break;
          }
        }
      });
    }
    if (!changed.load(std::memory_order_relaxed)) break;
  }
  std::vector<double> out(n);
  for (uint64_t v = 0; v < n; v++) out[v] = dist[v].load();
  return out;
}

}  // namespace common::algorithms
