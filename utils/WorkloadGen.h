#ifndef BENCH_WORKLOADGEN_H
#define BENCH_WORKLOADGEN_H

// Mixed-write and HotSet workload generation.
// A REINSERT job deletes then reinserts the same edge on one writer, using two
// independent transactions and preserving the original weight. MODIFY assigns
// a new weight encoded as double bits. Mixed rounds choose disjoint edge pools
// from a seeded shuffle: ID uses |E|/2 reinsert jobs; IDM uses |E|/4 reinsert
// jobs and |E|/2 modifications; M100 modifies all edges (counts round down).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bench {

enum class OpType : uint8_t { MODIFY, REINSERT };

struct WOp {
  OpType type;
  unsigned src, dst;
  uint64_t weight;
};

struct ChurnOp {
  unsigned remove_src, remove_dst;
  unsigned insert_src, insert_dst;
  uint64_t weight;
};

struct RoundStats {
  size_t reinserts = 0;  // Each reinsert is one delete transaction followed by one insert transaction.
  size_t modifies = 0;
  size_t txns() const { return reinserts * 2 + modifies; }
};

struct HotSetInfo {
  size_t edge_count = 0;
  uint64_t checksum = 0;
};

inline uint64_t MixHash64(uint64_t value) {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

// One mixed HotSet round contains exactly hot_edge_count physical transactions.
// Structural jobs use distinct edges and a separate edge pool; each paired
// delete/insert is consecutive on one writer. Updates sample with replacement
// from the non-structural pool, matching the existing HotSet access pattern.
struct HotSetMixedPlan {
  size_t hot_edges = 0;
  size_t warmup_updates = 0;
  size_t reinserts = 0;
  size_t updates = 0;
  size_t jobs = 0;
  size_t offset = 0;
  size_t stride = 1;
  uint64_t round_seed = 0;

  struct Job {
    size_t edge_index;
    bool reinsert;
  };

  Job JobAt(size_t job) const {
    const size_t available = hot_edges - warmup_updates;
    const uint64_t scaled_job = static_cast<uint64_t>(job) * reinserts;
    const size_t before = static_cast<size_t>(scaled_job / jobs);
    const size_t remainder = static_cast<size_t>(scaled_job % jobs);
    const bool reinsert = remainder >= jobs - reinserts;
    const size_t rank = reinsert
                            ? before
                            : reinserts + MixHash64(round_seed ^ job) %
                                              (available - reinserts);
    const size_t index = warmup_updates +
        (offset + stride * rank) % available;
    return Job{index, reinsert};
  }
};

inline HotSetMixedPlan MakeHotSetMixedPlan(size_t hot_edges, size_t writers,
                                           int update_pct, uint64_t seed,
                                           uint64_t round) {
  if (update_pct < 0 || update_pct > 80 || update_pct % 20 != 0 ||
      writers == 0 || hot_edges <= writers) {
    throw std::invalid_argument("invalid mixed HotSet ratio or hot edge count");
  }
  HotSetMixedPlan plan;
  plan.hot_edges = hot_edges;
  plan.warmup_updates = round == 1 && update_pct > 0 ? writers : 0;
  if (update_pct == 0) {
    // Keep delete/insert paired and do not invent an Update to absorb an odd
    // edge count; at most one physical operation is omitted in that case.
    plan.reinserts = hot_edges / 2;
    plan.updates = 0;
  } else {
    plan.reinserts = static_cast<size_t>(
        (static_cast<unsigned __int128>(hot_edges) * (100 - update_pct) + 100) /
        200);
    plan.updates = hot_edges - 2 * plan.reinserts;
  }
  if (plan.updates < plan.warmup_updates) {
    throw std::invalid_argument(
        "mixed HotSet needs enough update transactions for writer warmup");
  }
  plan.jobs = plan.reinserts + plan.updates - plan.warmup_updates;
  const size_t available = hot_edges - plan.warmup_updates;
  plan.round_seed = MixHash64(seed ^ (round * 0x9e3779b97f4a7c15ULL));
  plan.offset = plan.round_seed % available;
  plan.stride = 1 + MixHash64(seed ^ round ^ 0xd6e8feb86659fd93ULL) % available;
  while (std::gcd(plan.stride, available) != 1) {
    plan.stride = plan.stride == available ? 1 : plan.stride + 1;
  }
  return plan;
}

inline uint64_t HotEdgeRank(
    const std::tuple<unsigned, unsigned, uint64_t>& edge, uint64_t seed) {
  uint64_t source = std::get<0>(edge);
  uint64_t destination = std::get<1>(edge);
  uint64_t source_hash = MixHash64(source ^ seed);
  return MixHash64(destination ^ source_hash ^ (seed << 1));
}

inline HotSetInfo SelectHotSet(
    std::vector<std::tuple<unsigned, unsigned, uint64_t>>& edges,
    double fraction, uint64_t seed) {
  HotSetInfo info;
  if (edges.empty()) return info;

  info.edge_count = std::max<size_t>(
      1, static_cast<size_t>(static_cast<long double>(edges.size()) *
                             static_cast<long double>(fraction)));
  info.edge_count = std::min(info.edge_count, edges.size());

  auto comparator = [seed](const auto& left, const auto& right) {
    uint64_t left_rank = HotEdgeRank(left, seed);
    uint64_t right_rank = HotEdgeRank(right, seed);
    if (left_rank != right_rank) return left_rank < right_rank;
    if (std::get<0>(left) != std::get<0>(right))
      return std::get<0>(left) < std::get<0>(right);
    if (std::get<1>(left) != std::get<1>(right))
      return std::get<1>(left) < std::get<1>(right);
    return std::get<2>(left) < std::get<2>(right);
  };

  if (info.edge_count < edges.size()) {
    std::nth_element(edges.begin(), edges.begin() + info.edge_count,
                     edges.end(), comparator);
  }

  uint64_t sum = MixHash64(info.edge_count ^ seed);
  uint64_t xors = 0;
  for (size_t i = 0; i < info.edge_count; ++i) {
    uint64_t value = HotEdgeRank(edges[i], seed ^ 0xd6e8feb86659fd93ULL);
    sum += value;
    xors ^= value;
  }
  info.checksum = MixHash64(sum ^ xors);
  return info;
}

inline uint64_t DefaultHotSetRounds(double fraction) {
  if (!(fraction > 0.0)) {
    throw std::invalid_argument("hotset fraction must be positive");
  }
  long double rounds =
      std::ceil(5.0L / static_cast<long double>(fraction));
  if (rounds > static_cast<long double>(
                   std::numeric_limits<uint64_t>::max())) {
    throw std::invalid_argument("hotset fraction produces too many rounds");
  }
  return static_cast<uint64_t>(rounds);
}

inline HotSetInfo LoadHotSetFile(
    std::vector<std::tuple<unsigned, unsigned, uint64_t>>& edges,
    const std::string& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open hotset file: " + path);
  std::vector<std::tuple<unsigned, unsigned, uint64_t>> loaded;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#' || line.find('=') != std::string::npos)
      continue;
    std::istringstream row(line);
    unsigned source, destination;
    uint64_t weight;
    if (!(row >> source >> destination >> weight))
      throw std::runtime_error("invalid hotset row in: " + path);
    std::string extra;
    if (row >> extra)
      throw std::runtime_error("extra data in hotset row in: " + path);
    loaded.emplace_back(source, destination, weight);
  }
  if (loaded.empty()) throw std::runtime_error("hotset file has no edges: " + path);
  uint64_t sum = MixHash64(loaded.size());
  uint64_t xors = 0;
  for (const auto& edge : loaded) {
    uint64_t value = HotEdgeRank(edge, 0xd6e8feb86659fd93ULL);
    sum += value;
    xors ^= value;
  }
  edges = std::move(loaded);
  return HotSetInfo{edges.size(), MixHash64(sum ^ xors)};
}

inline void PrepareHotSetRound(
    std::vector<std::tuple<unsigned, unsigned, uint64_t>>& edges,
    size_t hot_edge_count, uint64_t round_seed) {
  auto hot_end = edges.begin() + hot_edge_count;
  std::mt19937_64 rng(round_seed);
  std::shuffle(edges.begin(), hot_end, rng);

  std::uniform_real_distribution<double> weight_dist(1.0, 100.0);
  for (auto it = edges.begin(); it != hot_end; ++it) {
    double weight = weight_dist(rng);
    uint64_t encoded_weight;
    std::memcpy(&encoded_weight, &weight, sizeof(encoded_weight));
    std::get<2>(*it) = encoded_weight;
  }
}

inline uint64_t UndirectedEdgeKey(unsigned source, unsigned destination) {
  uint64_t low = std::min(source, destination);
  uint64_t high = std::max(source, destination);
  return (low << 32) | high;
}

inline std::vector<ChurnOp> GenerateGraphlogLikeChurn(
    const std::vector<std::tuple<unsigned, unsigned, uint64_t>>& edges,
    uint64_t seed) {
  std::vector<ChurnOp> operations;
  if (edges.empty()) return operations;

  std::vector<unsigned> vertices;
  vertices.reserve(edges.size() * 2);
  std::unordered_set<unsigned> vertex_set;
  vertex_set.reserve(edges.size() * 2);
  std::unordered_set<uint64_t> active_or_planned;
  active_or_planned.reserve(edges.size() * 3);
  for (const auto& [source, destination, weight] : edges) {
    if (!active_or_planned.insert(UndirectedEdgeKey(source, destination))
             .second) {
      throw std::invalid_argument(
          "churn workload requires unique undirected input edges");
    }
    if (vertex_set.insert(source).second) vertices.push_back(source);
    if (vertex_set.insert(destination).second) vertices.push_back(destination);
  }
  if (vertices.size() < 2) {
    throw std::invalid_argument("churn workload requires at least two vertices");
  }

  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<size_t> vertex_dist(0, vertices.size() - 1);
  operations.reserve(edges.size());
  for (const auto& [source, destination, weight] : edges) {
    bool found = false;
    for (size_t attempt = 0; attempt < 4096; ++attempt) {
      unsigned replacement_destination = vertices[vertex_dist(rng)];
      if (replacement_destination == source) continue;
      uint64_t key = UndirectedEdgeKey(source, replacement_destination);
      if (!active_or_planned.insert(key).second) continue;
      operations.push_back({source, destination, source,
                            replacement_destination, weight});
      found = true;
      break;
    }
    if (!found) {
      throw std::invalid_argument(
          "cannot generate a missing replacement edge for churn workload");
    }
  }
  return operations;
}

// Generate a shuffled round for id/idm/m100; return empty for unknown modes validated by main.
inline std::vector<WOp> GenerateMixedRound(
    const std::vector<std::tuple<unsigned, unsigned, uint64_t>>& edges,
    const std::string& mix, uint64_t round_seed, RoundStats* stats = nullptr) {
  const size_t n = edges.size();
  std::vector<WOp> ops;
  if (n == 0) return ops;

  size_t reinsert_cnt = 0, modify_cnt = 0;
  if (mix == "id") {
    reinsert_cnt = n / 2;
  } else if (mix == "idm") {
    // Strict insert:delete:modify = 1:1:2.
    reinsert_cnt = n / 4;
    modify_cnt = n / 2;
  } else if (mix == "m100") {
    // Pure-modification baseline follows the existing --repeat semantics through
    // the mixed executor, isolating deletion overhead from mixed-framework overhead.
    modify_cnt = n;
  } else {
    return ops;
  }

  std::mt19937_64 rng(round_seed);

  // Shuffle all indices, then use the first reinsert_cnt for structural jobs
  // and the next modify_cnt for modifications; the sets are disjoint and contain no duplicates.
  std::vector<uint32_t> idx(n);
  std::iota(idx.begin(), idx.end(), 0u);
  std::shuffle(idx.begin(), idx.end(), rng);

  ops.reserve(reinsert_cnt + modify_cnt);
  size_t p = 0;
  for (size_t i = 0; i < reinsert_cnt; i++, p++) {
    const auto& [s, d, w] = edges[idx[p]];
    ops.push_back(WOp{OpType::REINSERT, s, d, w});
  }
  std::uniform_real_distribution<double> weight_dist(1.0, 100.0);
  for (size_t i = 0; i < modify_cnt; i++, p++) {
    const auto& [s, d, w] = edges[idx[p]];
    double nw = weight_dist(rng);
    uint64_t wi;
    std::memcpy(&wi, &nw, sizeof(wi));  // Preserve the weight bit pattern, matching EdgeReader.
    ops.push_back(WOp{OpType::MODIFY, s, d, wi});
  }

  // Randomly interleave REINSERT and MODIFY jobs.
  std::shuffle(ops.begin(), ops.end(), rng);

  if (stats) {
    stats->reinserts = reinsert_cnt;
    stats->modifies = modify_cnt;
  }
  return ops;
}

}  // namespace bench

#endif  // BENCH_WORKLOADGEN_H
