#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bench {

struct GraphlogOperation {
  uint64_t source;
  uint64_t destination;
  double weight;
};

struct GraphlogWorkload {
  uint64_t final_edges = 0;
  std::vector<GraphlogOperation> operations;
};

GraphlogWorkload LoadGraphlog(const std::string& path);

}  // namespace bench
