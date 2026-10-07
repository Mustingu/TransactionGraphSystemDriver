#ifndef LINEARALLOCATOR_H
#define LINEARALLOCATOR_H

#include <sys/mman.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <ctime>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <unordered_map>
#include <utility>  //declarations of unique_ptr
#include <vector>
// #include "../SharedLib.h"
#include "../third-party/RWSpinLock.h"
#include "MemoryPoolLib.h"

class TwoLevelVector;

class FixedSizeVector;

class PerThreadMemoryAllocator;

class HashTableMemoryIndex;

class GlobalAllocator {
 public:
  std::mutex globalAllocator;

  void init(int size, bool isInit);

  void *allocate(int size);

  uint64_t getOffset();

  uint64_t getTotalSpace();

  bool isFreeSpace(uint64_t block_size);

 private:
  void *mem;
  uint64_t offset;
  uint64_t totalSpace;
  // todo mutex lock on allocation
};

// fixed size vector

class FixedSizeVector {
 public:
  void initVal();

  void setBlockArr(GlobalAllocator *val);

  void initBlockArray(PerThreadMemoryAllocator *me);

  GlobalAllocator **getBlockArr();

  int getCurIndex() const;

  bool IscurIndexFull() const;

 private:
  GlobalAllocator **blockArr;
  int curIndex;
};

// Two level vector implementation
class TwoLevelVector {
 public:
  void initVal();

  void insert(GlobalAllocator *ga);

  void addBlock();

  void *getBlock(int requested_block);

  void setIndrArray();

  FixedSizeVector **IndrArray;
  int curIndex;
  int curSize;
  int blockSize;
};

struct PerThreadMemoryAllocator {
 public:
  PerThreadMemoryAllocator();

  void init(int space, int total_thread);

  void *Allocate(std::size_t requestedBlock, bool IsHash = false);

  // void* getBlock(int index, int requestedBlock);
  void free(void *ptr, std::size_t deletedBlock, bool isHash = false);

  int curIndex;
  int totalThread;
  int spacePerThread;
  bool freeBlockHash = false;
  bool freeBlockBlock = false;
  // own two level vector implementation
  int thread_id;
  // own hash table and queue implementation
  HashTableMemoryIndex *block;
  //   spinlock lock{0};
  // GlobalAllocator* memAllocation[10];
  //  AllocatorSize **allocatorBlockArr;

  // todo  mutex lock array 14 size
};

class MemoryAllocator {
 public:
  void init(int totalSpace, int totalThread);

  void *Allocate(std::size_t requestedBlock, int thread_id,
                 bool IsHash = false);

  void free(void *ptr, std::size_t deletedBlock, int thread_id,
            bool isHash = false);

  PerThreadMemoryAllocator *getPerThreadMemoryAllocator(int thread_id);

 private:
  PerThreadMemoryAllocator **perThreadArr;
};

#endif /* LINEARALLOCATOR_H */