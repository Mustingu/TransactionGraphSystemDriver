//
// Created by Ghufran on 30/01/2023.
//

#ifndef MST_IDEA_HASHTABLMEMORY_H
#define MST_IDEA_HASHTABLMEMORY_H

#include <string.h>

#include <vector>

// #include "../SharedLib.h"
#include <mutex>

#include "../third-party/RWSpinLock.h"
#include "MemoryPoolLib.h"

class TwoLevelQueue;

class TwoLevelVector;

/* forward declaration block allocator.h*/
class FixedSizeVector;

class PerThreadMemoryAllocator;

class GlobalAllocator;

class MemoryAllocator;

const int defaultSize = 907;
struct AllocatorBlock {
  TwoLevelVector *memBlock;
  TwoLevelQueue *freeList;
  //   spinlock lock{0};
};

class MemoryBucket {
 public:
  void setBlockSize(std::size_t block_size);

  void setFreeBlockList(AllocatorBlock *free_list);

  void setNextBucket(MemoryBucket *b);

  int64_t getBlockSize();

  AllocatorBlock *getFreeBlockList();

  MemoryBucket *getNextBucket();

  bool isAvailable = false;

 private:
  std::size_t blockSize;
  AllocatorBlock *freeBlock;
  MemoryBucket *nextBucket;
};

class HashTableMemoryIndex {
 public:
  // RWSpinLock perHashIndexLock[defaultSize];
  // std::mutex hashTableMutex;
  HashTableMemoryIndex(PerThreadMemoryAllocator *ptr);

  void insert(std::size_t key, AllocatorBlock *val,
              PerThreadMemoryAllocator *ptr);

  MemoryBucket *getIndexVal(std::size_t key);

  void resizeHashTable(PerThreadMemoryAllocator *ptr);

  int getDefaultSize();

  MemoryBucket **getHashIndexArr();

  int getCurSize();

 private:
  MemoryBucket **hashIndexArr;
  int curSize1;
  int entryCounter1;
  int64_t multiplier1;
};

// two level queue
class FixedSizeBlock {
 public:
  void initVal();

  void push(void *val);

  void *pop();

  void initBlockArr(PerThreadMemoryAllocator *me);

  void **getBlockArr();

  int getCurIndex() const;

  bool IscurIndexFull() const;

 private:
  void **blockArr;
  int cur;
};

// Two level vector implementation
class TwoLevelQueue {
 public:
  void initVal();

  void insert(void *ga, PerThreadMemoryAllocator *ptr);

  void *get_block();

  bool empty();

  bool emptyMultipleBlock(int required_block);

  void addBlock(PerThreadMemoryAllocator *ptr);

  void setIndrArray(PerThreadMemoryAllocator *ptr);

 private:
  FixedSizeBlock **IndrArray;
  int curIndex;
  int curSize;
  int blockSize;
};

#endif  // OUR_IDEA_HASHTABLMEMORY_H
