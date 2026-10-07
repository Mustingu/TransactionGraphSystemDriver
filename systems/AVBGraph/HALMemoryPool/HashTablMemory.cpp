//
// Created by Ghufran on 30/01/2023.
//

#include "HashTablMemory.h"

#include <iostream>
#include <queue>
/* bucket defination */
#include "BlockAllocator.hpp"

void MemoryBucket::setBlockSize(std::size_t block_size) {
  blockSize = block_size;
}

void MemoryBucket::setFreeBlockList(AllocatorBlock *free_list) {
  freeBlock = free_list;
}

void MemoryBucket::setNextBucket(MemoryBucket *b) { nextBucket = b; }

int64_t MemoryBucket::getBlockSize() { return blockSize; }

AllocatorBlock *MemoryBucket::getFreeBlockList() { return freeBlock; }

MemoryBucket *MemoryBucket::getNextBucket() { return nextBucket; }

/* HashTableIndex function defination */

HashTableMemoryIndex::HashTableMemoryIndex(PerThreadMemoryAllocator *ptr) {
  curSize1 = 0;
  entryCounter1 = 0;
  multiplier1 = 10;
  resizeHashTable(ptr);
}

void HashTableMemoryIndex::insert(std::size_t key, AllocatorBlock *val,
                                  PerThreadMemoryAllocator *ptr) {
  //   std::cout << "inserting " << key << std::endl;
  entryCounter1++;
  if (entryCounter1 > curSize1) {
    // todo here start
    if (entryCounter1 > curSize1) {
      resizeHashTable(ptr);
    }
  }

  MemoryBucket *bucketResutn = getIndexVal(key);
  if (bucketResutn == NULL) {
    int index = key % curSize1;
    // perHashIndexLock[index].lock();
    if (hashIndexArr[index] == NULL) {
      MemoryBucket *bPtr =
          (MemoryBucket *)std::aligned_alloc(8, sizeof(MemoryBucket));
      bPtr->setFreeBlockList(val);
      bPtr->setBlockSize(key);
      bPtr->setNextBucket(NULL);
      hashIndexArr[index] = bPtr;
    } else {
      MemoryBucket *newB =
          (MemoryBucket *)std::aligned_alloc(8, sizeof(MemoryBucket));
      newB->setBlockSize(key);
      newB->setFreeBlockList(val);
      newB->setNextBucket(hashIndexArr[index]);
      hashIndexArr[index] = newB;
    }
    // perHashIndexLock[index].unlock();
  } else {
    // bucketResutn->setFreeBlockList(val);
    //  std::cout<<"duplicate";
  }
}

MemoryBucket *HashTableMemoryIndex::getIndexVal(std::size_t key) {
  int index = key % curSize1;
  MemoryBucket *nextPtr = hashIndexArr[index];
  while (nextPtr != NULL) {
    if (nextPtr->getBlockSize() == key) {
      return nextPtr;
    }
    nextPtr = nextPtr->getNextBucket();
  }
  return NULL;
}

void HashTableMemoryIndex::resizeHashTable(PerThreadMemoryAllocator *ptr) {
  int prevCurSize = curSize1;
  if (curSize1 != 0) {
    curSize1 = getCurSizeTable(curSize1);
    multiplier1 = 10;
    MemoryBucket **tempArray = static_cast<MemoryBucket **>(std::aligned_alloc(
        8, curSize1 * 8));  // reinterpret_cast<Bucket **>((Bucket *)
                            // malloc(curSize * 8));
    //    memset(tempArray, 0, curSize * 8);

    for (int i = 0; i < (curSize1); i++) {
      tempArray[i] = 0;
    }
    //
    for (int i = 0; i < prevCurSize; i++) {
      MemoryBucket *nextPtr = hashIndexArr[i];
      while (nextPtr != NULL) {
        int newIndex = nextPtr->getBlockSize() % curSize1;
        if (tempArray[newIndex] == NULL) {
          MemoryBucket *newBucket =
              (MemoryBucket *)std::aligned_alloc(8, sizeof(MemoryBucket));
          // Bucket *update_ptr = nextPtr;
          newBucket->setBlockSize(nextPtr->getBlockSize());
          newBucket->setFreeBlockList(nextPtr->getFreeBlockList());
          newBucket->setNextBucket(NULL);
          tempArray[newIndex] = newBucket;
        } else {
          // nextPtr->setNextBucket(tempArray[newIndex]);
          MemoryBucket *b =
              (MemoryBucket *)std::aligned_alloc(8, sizeof(MemoryBucket));
          b->setBlockSize(nextPtr->getBlockSize());
          b->setNextBucket(tempArray[newIndex]);
          b->setFreeBlockList(nextPtr->getFreeBlockList());
          tempArray[newIndex] = b;
        }
        //  Bucket *prevPtr = nextPtr;
        nextPtr = nextPtr->getNextBucket();
        // delete prevPtr;
      }
    }
    // curIndVertexSize decremented becuase of neutralizing the increament
    // effect few lines before std::copy(hashIndexArr, hashIndexArr +
    // prevCurSize, tempArray); delete[] hashIndexArr;
    hashIndexArr = tempArray;
  } else {
    hashIndexArr =
        static_cast<MemoryBucket **>(std::aligned_alloc(8, defaultSize * 8));
    //  memset(hashIndexArr, 0, defaultSize * 8);
    for (int j = 0; j < defaultSize; j++) {
      hashIndexArr[j] = NULL;
      //  perHashIndexLock[j].init();
    }
    curSize1 = defaultSize;
    multiplier1 = 10;
  }
}

MemoryBucket **HashTableMemoryIndex::getHashIndexArr() { return hashIndexArr; }

int HashTableMemoryIndex::getCurSize() { return curSize1; }

int HashTableMemoryIndex::getDefaultSize() { return defaultSize; }

/* FixedSizeBlock class defination */
void FixedSizeBlock::initVal() { cur = -1; }

void FixedSizeBlock::initBlockArr(PerThreadMemoryAllocator *me) {
  blockArr = reinterpret_cast<void **>(me->Allocate(
      8 *
      MEMORY_QUEUE_SIZE));  // reinterpret_cast<void **>(std::aligned_alloc(8,8
                            // * MEMORY_QUEUE_SIZE));
}

void FixedSizeBlock::push(void *val) { blockArr[++cur] = val; }

void *FixedSizeBlock::pop() {
  void *pt = blockArr[cur];
  cur = cur - 1;
  return pt;
}

void **FixedSizeBlock::getBlockArr() { return blockArr; }

int FixedSizeBlock::getCurIndex() const { return cur; }

bool FixedSizeBlock::IscurIndexFull() const {
  if (cur == (MEMORY_QUEUE_SIZE - 1)) {
    return true;
  } else {
    return false;
  }
}

/* TwoLevelQueue class defination */
void TwoLevelQueue::initVal() {
  curIndex = -1;
  curSize = 1;
  blockSize = 0;
  IndrArray = NULL;
}

//  template<class T>
void TwoLevelQueue::insert(void *ga, PerThreadMemoryAllocator *ptr) {
  if (curIndex == (blockSize - 1)) {
    if (IndrArray[curIndex]->IscurIndexFull()) {
      setIndrArray(ptr);
      addBlock(ptr);
      IndrArray[curIndex]->push(ga);
    } else {
      IndrArray[curIndex]->push(ga);
    }
  } else {
    // start from here
    if (IndrArray[curIndex]->IscurIndexFull()) {
      addBlock(ptr);
      IndrArray[curIndex]->push(ga);
    } else {
      IndrArray[curIndex]->push(ga);
    }
  }
}

void *TwoLevelQueue::get_block() {
  if (curIndex == 0) {
    return IndrArray[curIndex]->pop();
  } else {
    if (IndrArray[curIndex]->getCurIndex() == -1) {
      curIndex = curIndex - 1;
      return IndrArray[curIndex]->pop();
    } else {
      return IndrArray[curIndex]->pop();
    }
  }
}

bool TwoLevelQueue::empty() {
  if (curIndex == 0 && IndrArray[curIndex]->getCurIndex() == -1) {
    return true;
  } else {
    return false;
  }
}

void TwoLevelQueue::addBlock(PerThreadMemoryAllocator *ptr) {
  FixedSizeBlock *new_block = static_cast<
      FixedSizeBlock *>(ptr->Allocate(sizeof(
      FixedSizeBlock)));  // static_cast<FixedSizeBlock
                          // *>(std::aligned_alloc(8,sizeof(FixedSizeBlock)));//new
                          // FixedSizeBlock();
  new_block->initVal();
  new_block->initBlockArr(ptr);
  IndrArray[++curIndex] = new_block;
}

void TwoLevelQueue::setIndrArray(PerThreadMemoryAllocator *ptr) {
  if (IndrArray == NULL) {
    // blockLen = 1;

    IndrArray =
        reinterpret_cast<FixedSizeBlock **>(static_cast<FixedSizeBlock **>(
            ptr->Allocate((8 * (curSize * fixedSize_queue)))));
    ;  // reinterpret_cast<FixedSizeBlock **>(static_cast<FixedSizeBlock
       // **>(std::aligned_alloc(8,( 8 *(curSize *
       // fixedSize_queue)))));//reinterpret_cast<FixedSizeBlock
       // **>(static_cast<FixedSizeBlock **>(ptr->Allocate(( 8 *(curSize *
       // fixedSize_queue)))));;//reinterpret_cast<FixedSizeBlock
       // **>(static_cast<FixedSizeBlock **>(std::aligned_alloc(8,( 8 *(curSize
       // * fixedSize_queue)))));//static_cast<EdgeBlock **>(malloc(8 *
       // (blockLen * fixedSize)));

    memset(IndrArray, 0, (curSize * fixedSize_queue));
    blockSize = fixedSize_queue;
    FixedSizeBlock *new_block = static_cast<
        FixedSizeBlock *>(ptr->Allocate(sizeof(
        FixedSizeBlock)));  // static_cast<FixedSizeBlock
                            // *>(std::aligned_alloc(8,sizeof(FixedSizeBlock)));//new
                            // FixedSizeBlock();
    new_block->initVal();
    new_block->initBlockArr(ptr);
    IndrArray[++curIndex] = new_block;

    /*

for(int i=0;i<(blockLen * fixedSize);i++)
{
blockArr[i] = NULL;
}

*/
  } else {
    int blenprev = curSize;
    int b_len_new = 2 * curSize;

    FixedSizeBlock **temp = static_cast<FixedSizeBlock **>(ptr->Allocate(
        8 *
        ((b_len_new)*fixedSize_queue)));  // static_cast<FixedSizeBlock
                                          // **>(std::aligned_alloc(8,8 *
                                          // ((b_len_new) * fixedSize_queue)));
    memset(temp, 0, ((b_len_new)*fixedSize_queue));

    //  FixedTimeIndexBlock **temp2;
    // todo deletion mechanism ()create node in globle linklist and add the
    // address of deleted index array and with time to replace the garbage
    // collector will deallocate the memory than int k=0; int tempC = (blenprev)
    // * fixedSize;
    int bprev = (blenprev)*fixedSize_queue;
    for (int i = 0; i < bprev; i++) {
      temp[i] = IndrArray[i];
    }

    curIndex = blenprev - 1;

    void *ptr1 = IndrArray;

    IndrArray = temp;

    ptr->free(ptr1, bprev * 8);
    // std::free(ptr1);
    blockSize = b_len_new * fixedSize_queue;

    curSize = b_len_new;
  }
}