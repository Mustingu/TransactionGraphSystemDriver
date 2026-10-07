#ifndef CONCURRENT_EDGE_READER_H
#define CONCURRENT_EDGE_READER_H

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <cstring>

namespace bench {
  struct EdgeChunk {
  unsigned num_;
  std::vector<std::string> edges;
  bool is_last = false;

  EdgeChunk(unsigned num, size_t reserve_size) {
    num_ = num;
    edges.reserve(reserve_size);
  }
};

class ConcurrentEdgeReader {
 private:
  std::ifstream file;
  std::string filename;
  size_t chunk_size;
  size_t max_edges;

  // Buffered chunk queue.
  std::queue<std::shared_ptr<EdgeChunk>> buffer_queue;
  std::mutex queue_mutex;
  std::condition_variable data_available;
  std::condition_variable space_available;

  // Control flags.
  std::atomic<bool> reading_finished{false};
  std::atomic<bool> processing_finished{false};

  const size_t MAX_BUFFER_SIZE = 4;  // Maximum number of queued chunks.

 public:
  ConcurrentEdgeReader(const std::string& fname, size_t c_size = (1 << 15),
                       size_t max_edges = 0);

  // Reader-thread entry point.

  void reader_thread();

  // Get the next data chunk.
  std::shared_ptr<EdgeChunk> get_next_chunk();
};
void UpdateDataConcurrent(
    std::string input_file,
    std::vector<std::tuple<unsigned, unsigned, uint64_t>>& edges,
    size_t max_edges = 0);
}
#endif  // USE_CONCURRENT_EDGE_READER
