#include "GraphlogReplay.h"

#include <cstring>
#include <fstream>
#include <limits>
#include <regex>
#include <stdexcept>
#include <unordered_map>

#include <zlib.h>

namespace bench {
namespace {

using Properties = std::unordered_map<std::string, std::string>;

Properties ParseProperties(std::ifstream& input) {
  std::string line;
  if (!std::getline(input, line) || line != "# GRAPHLOG") {
    throw std::runtime_error("missing graphlog magic header");
  }

  Properties properties;
  const std::regex property_pattern(
      R"(^\s*([A-Za-z0-9_.-]+?)\s*=\s*([^#\n]+?)\s*$)");
  while (std::getline(input, line)) {
    if (line == "__BINARY_SECTION_FOLLOWS") break;
    std::smatch match;
    if (std::regex_match(line, match, property_pattern)) {
      properties.emplace(match[1].str(), match[2].str());
    }
  }
  return properties;
}

uint64_t RequiredProperty(const Properties& properties,
                          const std::string& name) {
  auto property = properties.find(name);
  if (property == properties.end()) {
    throw std::runtime_error("missing graphlog property: " + name);
  }
  return std::stoull(property->second);
}

}  // namespace

GraphlogWorkload LoadGraphlog(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open graphlog: " + path);

  Properties properties = ParseProperties(input);
  const uint64_t edge_offset =
      RequiredProperty(properties, "internal.edges.begin");
  const uint64_t block_size =
      RequiredProperty(properties, "internal.edges.block_size");
  const uint64_t operation_count =
      RequiredProperty(properties, "internal.edges.cardinality");

  input.seekg(0, std::ios::end);
  const uint64_t file_size = static_cast<uint64_t>(input.tellg());
  if (edge_offset >= file_size) {
    throw std::runtime_error("invalid graphlog edge offset");
  }
  const uint64_t compressed_size = file_size - edge_offset;
  if (compressed_size > std::numeric_limits<uInt>::max()) {
    throw std::runtime_error("graphlog compressed edge section is too large");
  }

  std::vector<unsigned char> compressed(compressed_size);
  input.seekg(edge_offset, std::ios::beg);
  input.read(reinterpret_cast<char*>(compressed.data()), compressed.size());
  if (static_cast<uint64_t>(input.gcount()) != compressed_size) {
    throw std::runtime_error("cannot read graphlog edge section");
  }

  GraphlogWorkload workload;
  auto final_edges = properties.find("internal.edges.final");
  if (final_edges != properties.end()) {
    workload.final_edges = std::stoull(final_edges->second);
  }
  workload.operations.reserve(operation_count);

  uint64_t compressed_offset = 0;
  while (workload.operations.size() < operation_count) {
    const uint64_t remaining = operation_count - workload.operations.size();
    const uint64_t output_size = std::min<uint64_t>(block_size, remaining * 24);
    if (output_size > std::numeric_limits<uInt>::max()) {
      throw std::runtime_error("graphlog edge block is too large");
    }
    std::vector<uint64_t> block((output_size + 7) / 8);

    z_stream stream{};
    stream.next_in = compressed.data() + compressed_offset;
    stream.avail_in = static_cast<uInt>(compressed_size - compressed_offset);
    stream.next_out = reinterpret_cast<unsigned char*>(block.data());
    stream.avail_out = static_cast<uInt>(output_size);
    if (inflateInit2(&stream, -15) != Z_OK) {
      throw std::runtime_error("cannot initialize graphlog decompressor");
    }
    int result = inflate(&stream, Z_FINISH);
    const uint64_t consumed =
        compressed_size - compressed_offset - stream.avail_in;
    const uint64_t produced = output_size - stream.avail_out;
    inflateEnd(&stream);
    if (result != Z_STREAM_END || produced % 24 != 0 || consumed == 0) {
      throw std::runtime_error("cannot decompress graphlog edge block");
    }

    const uint64_t block_operations = produced / 24;
    const uint64_t* sources = block.data();
    const uint64_t* destinations = sources + block_operations;
    const uint64_t* weight_bits = destinations + block_operations;
    for (uint64_t index = 0; index < block_operations; ++index) {
      double weight;
      std::memcpy(&weight, &weight_bits[index], sizeof(weight));
      workload.operations.push_back(
          {sources[index], destinations[index], weight});
    }
    compressed_offset += consumed;
  }

  if (workload.operations.size() != operation_count) {
    throw std::runtime_error("graphlog operation count mismatch");
  }
  return workload;
}

}  // namespace bench
