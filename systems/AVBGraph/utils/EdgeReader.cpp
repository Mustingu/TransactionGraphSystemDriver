#include "EdgeReader.h"
ConcurrentEdgeReader::ConcurrentEdgeReader(const std::string& fname,
                                           size_t c_size)
    : filename(fname), chunk_size(c_size) {
  file.open(filename);
  if (!file.is_open()) {
    throw std::runtime_error("Cannot open file: " + filename);
  }
}

// Reader-thread entry point.
void ConcurrentEdgeReader::reader_thread() {
  int EdgeChunkNum = 0;
  while (!reading_finished) {
    // Wait for queue space.
    std::unique_lock<std::mutex> lock(queue_mutex);
    space_available.wait(lock, [this] {
      return buffer_queue.size() < MAX_BUFFER_SIZE || reading_finished;
    });

    if (reading_finished) break;

    // Create the next data chunk.
    auto chunk = std::make_shared<EdgeChunk>(EdgeChunkNum++, chunk_size);

    // Read a batch of edges.
    std::string line;
    size_t count = 0;
    while (count < chunk_size && std::getline(file, line)) {
      chunk->edges.emplace_back(line);
      count++;
      // uint64_t src, dst;
      // double property;
      // if (sscanf(line.c_str(), "%lu %lu %lf", &src, &dst, &property) == 3)
      // {
      //   chunk->edges.emplace_back(src, dst, property);
      //   count++;
      // }
    }

    chunk->is_last = count < chunk_size || file.eof();

    // Enqueue the chunk.
    buffer_queue.push(chunk);

    if (chunk->is_last) {
      reading_finished = true;
      lock.unlock();
      break;
    }
    lock.unlock();

    // Notify workers that new data is available.
    data_available.notify_one();

    if (chunk->is_last) {
      reading_finished = true;
      break;
    }
  }

  // Wake all waiting workers.
  data_available.notify_all();
}

// Get the next data chunk.
std::shared_ptr<EdgeChunk> ConcurrentEdgeReader::get_next_chunk() {
  std::unique_lock<std::mutex> lock(queue_mutex);
  data_available.wait(
      lock, [this] { return !buffer_queue.empty() || reading_finished; });

  if (buffer_queue.empty() && reading_finished) {
    return nullptr;
  }

  auto chunk = buffer_queue.front();
  buffer_queue.pop();
  lock.unlock();

  // Notify the reader that queue space is available.
  space_available.notify_one();

  return chunk;
}

void UpdateDataConcurrent(
    std::string input_file,
    std::vector<std::tuple<unsigned, unsigned, uint64_t>>& edges) {
  const size_t CHUNK_SIZE = 1 << 20;  // Edges per chunk.
  // const int PROCESSOR_THREADS = 4; // Number of processing workers.

  ConcurrentEdgeReader reader(input_file, CHUNK_SIZE);

  // Start the reader thread.
  std::thread reader_t([&reader]() { reader.reader_thread(); });

  // Start processing workers.
  auto chunk = reader.get_next_chunk();
  while (chunk) {
    int old_size = edges.size(), n = chunk->edges.size();
    // std::cout << old_size << " " << n << std::endl;
    edges.resize(edges.size() + chunk->edges.size());
#pragma omp parallel for
    for (int i = 0; i < n; i++) {
      uint64_t src, dst, property;
      sscanf(chunk->edges[i].c_str(), "%lu %lu %lf", &src, &dst,
             (double*)&property);
      edges[old_size + i] = std::make_tuple(src, dst, property);
    }
    chunk = reader.get_next_chunk();
  }

  // Wait for all workers.
  reader_t.join();
  std::cout << "Total edges processed: " << edges.size() << std::endl;
}
