#include "EdgeReader.h"
#include "WorkloadGen.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
using Edge = std::tuple<unsigned, unsigned, uint64_t>;

void usage(const char* program) {
  std::cerr << "Usage: " << program
            << " --input-file PATH --vertex-fraction F --output PATH"
               " [--seed N] [--rounds N]\n";
}

uint64_t parse_u64(const std::string& value, const char* name) {
  size_t end = 0;
  unsigned long long parsed = std::stoull(value, &end);
  if (end != value.size()) throw std::invalid_argument(std::string("invalid ") + name);
  return static_cast<uint64_t>(parsed);
}

double parse_fraction(const std::string& value) {
  size_t end = 0;
  double parsed = std::stod(value, &end);
  if (end != value.size() || !(parsed > 0.0 && parsed <= 1.0))
    throw std::invalid_argument("vertex fraction must be in (0,1]");
  return parsed;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    std::string input, output;
    double fraction = 0.0;
    uint64_t seed = 42;
    uint64_t rounds = 0;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      auto next = [&](const char* name) -> std::string {
        if (i + 1 >= argc) throw std::invalid_argument(std::string("missing value for ") + name);
        return argv[++i];
      };
      if (arg == "--input-file") input = next("--input-file");
      else if (arg == "--vertex-fraction") fraction = parse_fraction(next("--vertex-fraction"));
      else if (arg == "--output") output = next("--output");
      else if (arg == "--seed") seed = parse_u64(next("--seed"), "--seed");
      else if (arg == "--rounds") rounds = parse_u64(next("--rounds"), "--rounds");
      else { usage(argv[0]); return 2; }
    }
    if (input.empty() || output.empty() || fraction == 0.0) {
      usage(argv[0]);
      return 2;
    }

    std::vector<Edge> edges;
    bench::UpdateDataConcurrent(input, edges);
    if (edges.empty()) throw std::runtime_error("input graph has no edges");

    std::unordered_map<unsigned, uint64_t> degree;
    degree.reserve(edges.size());
    for (const auto& [src, dst, weight] : edges) {
      ++degree[src];
      ++degree[dst];
    }
    const size_t hot_count = std::max<size_t>(1, static_cast<size_t>(
        std::ceil(static_cast<long double>(degree.size()) * fraction)));

    struct Candidate { double key; unsigned vertex; };
    std::mt19937_64 rng(seed);
    std::vector<Candidate> ranked;
    ranked.reserve(degree.size());
    for (const auto& [vertex, d] : degree) {
      const double u = (static_cast<double>(rng()) + 1.0) /
                       (static_cast<double>(rng.max()) + 2.0);
      ranked.push_back({-std::log(u) / static_cast<double>(d), vertex});
    }
    const size_t selected_count = std::min(hot_count, ranked.size());
    std::nth_element(ranked.begin(), ranked.begin() + selected_count, ranked.end(),
                     [](const Candidate& a, const Candidate& b) {
                       if (a.key != b.key) return a.key < b.key;
                       return a.vertex < b.vertex;
                     });
    std::unordered_set<unsigned> hot_vertices;
    hot_vertices.reserve(selected_count * 2 + 1);
    for (size_t i = 0; i < selected_count; ++i) hot_vertices.insert(ranked[i].vertex);

    std::vector<Edge> candidates;
    candidates.reserve(edges.size());
    for (const auto& edge : edges) {
      if (hot_vertices.count(std::get<0>(edge)) ||
          hot_vertices.count(std::get<1>(edge))) candidates.push_back(edge);
    }
    if (candidates.empty()) throw std::runtime_error("hot vertices produced no candidate edges");

    uint64_t checksum = bench::MixHash64(seed ^ selected_count ^ candidates.size());
    for (const auto& [src, dst, weight] : candidates)
      checksum = bench::MixHash64(checksum ^ src ^ (static_cast<uint64_t>(dst) << 32));

    std::ofstream out(output);
    if (!out) throw std::runtime_error("cannot open output: " + output);
    out << "# VERTEX-DEGREE-HOTSET v1\n"
        << "mode = vertex-degree\n"
        << "vertex_fraction = " << std::setprecision(17) << fraction << "\n"
        << "graph_edges = " << edges.size() << "\n"
        << "graph_vertices = " << degree.size() << "\n"
        << "hot_vertices = " << selected_count << "\n"
        << "candidate_edges = " << candidates.size() << "\n"
        << "seed = " << seed << "\n"
        << "rounds = " << rounds << "\n"
        << "checksum = " << checksum << "\n"
        << "# hot edge set (src dst weight)\n";
    for (const auto& [src, dst, weight] : candidates)
      out << src << ' ' << dst << ' ' << weight << '\n';
    if (!out) throw std::runtime_error("write failed: " + output);
    std::cout << "generated output=" << output << " graph_edges=" << edges.size()
              << " graph_vertices=" << degree.size() << " hot_vertices=" << selected_count
              << " candidate_edges=" << candidates.size() << " checksum=" << checksum << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "hotset_gen: " << error.what() << '\n';
    return 1;
  }
}
