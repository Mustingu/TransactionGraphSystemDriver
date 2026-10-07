#ifndef GRAPHSTORE_H
#define GRAPHSTORE_H

#include <mutex>

#include "data-structure/EdgeBlock/VertexEdges.h"

#define INITIAL_VECTOR_SIZE 256

class VertexEntry {
 public:
  VertexEntry();
  VertexEntry(VertexEdges* eb);
  VertexEdges* eb;
};

class GraphStore : public VersionedTopologyInterface {
 public:
  tbb::concurrent_vector<VertexEntry> blocks;
  tbb::concurrent_vector<dst_t> p_mHashMap;
  // std::vector<HashTable2> hashtables;
  std::vector<std::atomic<uint8_t>>* locks;
  std::mutex growing_vector_mutex;
  // Vertex IDs, vector construction, and publication of blocks[v].eb must be
  // one operation. Existing-edge writes do not take this lock.
  std::mutex vertex_creation_mutex;
  std::atomic<uint8_t> locks2;
  size_t block_count;
  uint8_t unlocked_m = 0;

  std::atomic<int> node_num{0};

  GraphStore(size_t block_count);

  int get_node_num() { return node_num; }

  void SetBlockByIndex(unsigned index, unsigned _num, dst_t* _edges,
                       dst_t* _properties) {
    blocks[index] = new VertexEdges(index);
    blocks[index].eb->build(_num, _edges, _properties);
  }

  void SetBlockByIndex(unsigned index, VertexEdges* eb) {
    blocks[index].eb = eb;
  }
  VertexEdges* GetBlockByIndex(unsigned index) const {
    return blocks[index].eb;
  }

  bool insert_edge_block(dst_t* edge, epoch_t epoch, Transaction* txn);
  bool delete_edge_block(dst_t* edge, epoch_t epoch, Transaction* txn);

  template <typename T>
  void grow_vector_if_smaller(tbb::concurrent_vector<T>& v, size_t s,
                              T init_value);
  vertex_id_t new_vertex();
  unsigned check_degree(unsigned index);
  unsigned check_degree(unsigned index, epoch_t epoch);

  void Print(unsigned index) { blocks[index].eb->my_print_block(); }
};

#endif  // GRAPHSTORE_H
