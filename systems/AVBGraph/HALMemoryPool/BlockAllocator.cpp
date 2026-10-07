//
// Created by Ghufran on 25/01/2023.
//
#include "BlockAllocator.hpp"

#include <iostream>

#include "HashTablMemory.h"

/* GlobalAllocator function defination */

void GlobalAllocator::init(int size, bool isInit) {
  if (isInit) {
    totalSpace = size * (1024ul * 1024ul);
    mem = mmap(nullptr, totalSpace, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    // touch each page to insert int to allocate memory
    if (mem == MAP_FAILED && totalSpace == 0) {
      mem = mmap(nullptr, 16 * (1024ul * 1024ul), PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    }
    std::size_t currentAddress1 = 0;

    size_t y;
    for (size_t i = 0; i < totalSpace; i += 0x1000) {
      currentAddress1 = (std::size_t)mem + i;
      size_t* y = reinterpret_cast<size_t*>((void*)currentAddress1);
      *y = 0;
    }

    offset = 0;
  } else {
    if (size > SIXTEENMB)
      totalSpace = size;
    else
      totalSpace = size * (1024ul * 1024ul);
    mem = mmap(nullptr, totalSpace, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED) {
      std::cout << "error memory" << std::endl;
    }
    // touch each page to insert int to allocate memory
    offset = 0;
  }
}

void* GlobalAllocator::allocate(int size) {
  const std::size_t cur_addr = (std::size_t)mem + offset;
  offset += size;
  return (void*)cur_addr;
}

uint64_t GlobalAllocator::getOffset() { return offset; }

uint64_t GlobalAllocator::getTotalSpace() { return totalSpace; }

bool GlobalAllocator::isFreeSpace(uint64_t block_size) {
  if ((offset + block_size) > totalSpace) {
    return false;
  } else {
    return true;
  }
}

/* FixedSizeVector class defination */
void FixedSizeVector::initVal() { curIndex = -1; }

void FixedSizeVector::setBlockArr(GlobalAllocator* val) {
  blockArr[++curIndex] = val;
}

GlobalAllocator** FixedSizeVector::getBlockArr() { return blockArr; }

int FixedSizeVector::getCurIndex() const { return curIndex; }

bool FixedSizeVector::IscurIndexFull() const {
  if (curIndex == (MEMORY_VECTOR_SIZE - 1)) {
    return true;
  } else {
    return false;
  }
}

void FixedSizeVector::initBlockArray(PerThreadMemoryAllocator* me) {
  blockArr = reinterpret_cast<GlobalAllocator**>(
      std::aligned_alloc(8, 8 * MEMORY_VECTOR_SIZE));
}

/* Two level vector */

void TwoLevelVector::initVal() {
  IndrArray = NULL;
  curIndex = -1;
  curSize = 1;
  blockSize = 0;
}

void TwoLevelVector::insert(GlobalAllocator* ga) {
  if (curIndex == (blockSize - 1)) {
    if (IndrArray[curIndex]->IscurIndexFull()) {
      setIndrArray();
      addBlock();
      IndrArray[curIndex]->setBlockArr(ga);
    } else {
      IndrArray[curIndex]->setBlockArr(ga);
    }
  } else {
    // start from here
    if (IndrArray[curIndex]->IscurIndexFull()) {
      addBlock();
      IndrArray[curIndex]->setBlockArr(ga);
    } else {
      IndrArray[curIndex]->setBlockArr(ga);
    }
  }
}

void* TwoLevelVector::getBlock(int requested_block) {
  if (IndrArray[curIndex]
          ->getBlockArr()[IndrArray[curIndex]->getCurIndex()]
          ->isFreeSpace(requested_block)) {
    return IndrArray[curIndex]
        ->getBlockArr()[IndrArray[curIndex]->getCurIndex()]
        ->allocate(requested_block);
  } else {
    GlobalAllocator* ptr = static_cast<GlobalAllocator*>(
        std::aligned_alloc(8, sizeof(GlobalAllocator)));
    if (requested_block == 40 || requested_block == 32) {
      ptr->init(512, false);
    } else if (requested_block <= FOURMB) {
      ptr->init(512, false);
    } else if (requested_block > FOURMB && requested_block <= EIGHTMB) {
      ptr->init(512, false);
    } else if (requested_block > EIGHTMB && requested_block <= SIXTEENMB) {
      ptr->init(512, false);
    } else {
      ptr->init(requested_block, false);
    }
    insert(ptr);
    return IndrArray[curIndex]
        ->getBlockArr()[IndrArray[curIndex]->getCurIndex()]
        ->allocate(requested_block);
  }
}

void TwoLevelVector::addBlock() {
  FixedSizeVector* new_block =
      static_cast<FixedSizeVector*>(aligned_alloc(8, sizeof(FixedSizeVector)));
  new_block->initVal();
  new_block->initBlockArray(NULL);
  IndrArray[++curIndex] = new_block;
}

void TwoLevelVector::setIndrArray() {
  if (IndrArray == NULL) {
    // blockLen = 1;

    IndrArray = reinterpret_cast<FixedSizeVector**>(
        static_cast<FixedSizeVector**>(aligned_alloc(
            8,
            8 * (curSize * fixedSize))));  // static_cast<EdgeBlock **>(malloc(8
                                           // * (blockLen * fixedSize)));
    blockSize = fixedSize;

    FixedSizeVector* new_block = static_cast<FixedSizeVector*>(
        aligned_alloc(8, sizeof(FixedSizeVector)));
    new_block->initVal();
    new_block->initBlockArray(NULL);

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

    FixedSizeVector** temp = static_cast<FixedSizeVector**>(
        aligned_alloc(8, 8 * ((b_len_new)*fixedSize)));

    //  FixedTimeIndexBlock **temp2;
    // todo deletion mechanism ()create node in globle linklist and add the
    // address of deleted index array and with time to replace the garbage
    // collector will deallocate the memory than int k=0; int tempC = (blenprev)
    // * fixedSize;

    int bprev = (blenprev)*fixedSize;

    memcpy(temp, IndrArray, bprev * 8);

    curIndex = blenprev - 1;

    void* ptr = IndrArray;

    IndrArray = temp;
    free(ptr);
    blockSize = b_len_new * fixedSize;

    curSize = b_len_new;
  }
}

/* PerThreadMemoryAllocator class defination */
void PerThreadMemoryAllocator::init(int space, int total_thread) {
  /*
   Keep only the 32 * 64 (2048-byte) pool.
   */

  // Supported allocation block size.
  const int TARGET_BLOCK_SIZE = 32 * 64;

  block = static_cast<HashTableMemoryIndex*>(
      std::aligned_alloc(8, sizeof(HashTableMemoryIndex)));
  new (block) HashTableMemoryIndex(NULL);

  // Initialize only the matching entry in the block-size array.
  // The first entry is 32*64; the remaining entries are zero-initialized.
  // Match by value rather than relying on a particular array index.
  int BlockSize[16] = {32 * 64};
  // Preallocation size for the supported pool.
  int spaceNeeded[16] = {4};

  for (int i = 0; i < 16; i++) {
    // Skip unsupported block sizes.
    if (BlockSize[i] != TARGET_BLOCK_SIZE) {
      continue;
    }

    AllocatorBlock* ptr = new AllocatorBlock();

    TwoLevelVector* vec_two = static_cast<TwoLevelVector*>(
        std::aligned_alloc(8, sizeof(TwoLevelVector)));
    vec_two->initVal();
    vec_two->setIndrArray();

    GlobalAllocator* gl = static_cast<GlobalAllocator*>(
        std::aligned_alloc(8, sizeof(GlobalAllocator)));

    // Initialize the global allocator with this pool's preallocation size.
    gl->init((spaceNeeded[i]), true);

    vec_two->insert(gl);

    ptr->memBlock = vec_two;

    // Register the allocator by block size.
    block->insert(BlockSize[i], ptr, NULL);

    // Allocate through memBlock rather than taking directly from the free list.
    // The availability flag is initialized before use.
    block->getIndexVal(BlockSize[i])->isAvailable = false;
  }

  // No separate pools for the other power-of-two block sizes are initialized.
  // No prime-sized pools are initialized.
}

void* PerThreadMemoryAllocator::Allocate(std::size_t requestedBlock,
                                         bool IsHash) {
  if (requestedBlock <= SIXTEENMB) {
    MemoryBucket* m = block->getIndexVal(requestedBlock);

    /*  if(m == NULL)
      {
          std::cout<<"the m is null"<<" "<<requestedBlock<<std::endl;
      }
        if(m->getBlockSize() != requestedBlock)
        {
            std::cout<<"the block size does not match"<<std::endl;
        }*/
    if (!m->isAvailable) {
      /////

      void* ptr = m->getFreeBlockList()->memBlock->getBlock(requestedBlock);

      return ptr;
    } else {
      ///// if size is 24 byte then lock

      TwoLevelQueue* q = m->getFreeBlockList()->freeList;

      if (q->empty()) {
        void* ptr = m->getFreeBlockList()->memBlock->getBlock(requestedBlock);

        return ptr;
      } else {
        void* ptr = q->get_block();

        return ptr;
      }
    }

  } else {
    //   std::cout<<"hello"<<std::endl;

    return mmap(
        nullptr, requestedBlock, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1,
        0);  // aligned_alloc(8,requestedBlock);//mmap(nullptr, requestedBlock,
             // PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS |
             // MAP_NORESERVE, -1, 0);//aligned_alloc(8,requestedBlock);
  }

  /*
   if(!IsHash)
    {
        if(!freeBlockBlock)
        {
            return memAlocation->getBlock(requestedBlock);
        }
        else
        {
            // if two level queue is empty
            MemoryBucket *m = FreeList->getIndexVal(requestedBlock);
            if(m == NULL)
            {
                return memAlocation->getBlock(requestedBlock);
            }
            else
            {
                if(m->getFreeBlockList()->empty())
                {
                    return memAlocation->getBlock(requestedBlock);
                }
                else
                {
                    return m->getFreeBlockList()->get_block();
                }
            }
        }
    }

    else
    {
        if(!freeBlockHash)
        {
            return memAlocation->getBlock(requestedBlock);
        }
        else
        {
            // if two level queue is empty
            MemoryBucket *m = FreeList->getIndexVal(requestedBlock);
            if(m == NULL)
            {
                return memAlocation->getBlock(requestedBlock);
            }
            else
            {
                if(m->getFreeBlockList()->empty())
                {
                    return memAlocation->getBlock(requestedBlock);
                }
                else
                {
                    return m->getFreeBlockList()->get_block();
                }
            }
        }
    }
    */
}

void PerThreadMemoryAllocator::free(void* ptr, std::size_t deletedBlock,
                                    bool isHash) {
  // std::cout<<this->thread_id<<std::endl;

  if (deletedBlock <= SIXTEENMB) {
    // block->getIndexVal(deletedBlock)->getFreeBlockList()->lock.lock();

    TwoLevelQueue* two =
        block->getIndexVal(deletedBlock)->getFreeBlockList()->freeList;
    if (two == NULL) {
      ///
      TwoLevelQueue* newB =
          static_cast<TwoLevelQueue*>(this->Allocate((sizeof(TwoLevelQueue))));
      newB->initVal();
      newB->setIndrArray(this);
      newB->insert(ptr, this);
      block->getIndexVal(deletedBlock)->getFreeBlockList()->freeList = newB;
      block->getIndexVal(deletedBlock)->isAvailable = true;
      ////
    } else {
      two->insert(ptr, this);
    }

    // block->getIndexVal(deletedBlock)->getFreeBlockList()->lock.unlock();

  } else {
    munmap(ptr, deletedBlock);

    /* int rblock = (deletedBlock/4096);

     TwoLevelQueue *two = block->getIndexVal(65)->getFreeBlockList()->freeList;

     if(two == NULL)
     {
         TwoLevelQueue *newB = static_cast<TwoLevelQueue
     *>(this->Allocate((sizeof(TwoLevelQueue)))); newB->initVal();
         newB->setIndrArray(this);

         std::size_t currentAddress1 = 0;
         size_t y;
         for (size_t i = 0; i < rblock; i++)
         {
             currentAddress1 = (std::size_t) ptr + (i*0x1000);
             newB->insert(((void *) currentAddress1), this);
         }
         block->getIndexVal(65)->getFreeBlockList()->freeList = newB;
         block->getIndexVal(65)->isAvailable = true;
     }
     else
     {
         std::size_t currentAddress1 = 0;
         size_t y;
         for (size_t i = 0; i < rblock; i++)
         {
             currentAddress1 = (std::size_t) ptr + (i*0x1000);
             two->insert(((void *) currentAddress1), this);
         }
     }*/
  }

  /*
if(!isHash)
{
   if (FreeList->getIndexVal(deletedBlock) == NULL)
   {
       TwoLevelQueue *queue = static_cast<TwoLevelQueue
*>(this->Allocate(sizeof(TwoLevelQueue))); queue->initVal();
       queue->setIndrArray(this);
       queue->insert(ptr, this);
       FreeList->insert(deletedBlock, queue, this);
       freeBlockBlock = true;
   }
   else
   {
       FreeList->getIndexVal(deletedBlock)->getFreeBlockList()->insert(ptr,
this);
       //FreeList->getIndexVal(deletedBlock)->
   }

}
else
{
   if (FreeList->getIndexVal(deletedBlock) == NULL)
   {
       TwoLevelQueue *queue = static_cast<TwoLevelQueue
*>(this->Allocate(sizeof(TwoLevelQueue))); queue->initVal();
       queue->setIndrArray(this);
       queue->insert(ptr, this);
       FreeList->insert(deletedBlock, queue, this);
       freeBlockHash = true;
   }
   else
   {
       FreeList->getIndexVal(deletedBlock)->getFreeBlockList()->insert(ptr,
this);
       //FreeList->getIndexVal(deletedBlock)->
   }
}
*/
}

/* MemoryAllocator class defination */
void MemoryAllocator::init(int totalSpace, int totalThread) {
  int per_thread_space = totalSpace / totalThread;
  // std::cout<<
  // todo perthreadalloc
  perThreadArr =
      static_cast<PerThreadMemoryAllocator**>(malloc(totalThread * 8));

  for (int i = 0; i < totalThread; i++) {
    PerThreadMemoryAllocator* ptr = static_cast<PerThreadMemoryAllocator*>(
        aligned_alloc(8, sizeof(PerThreadMemoryAllocator)));
    ptr->init(per_thread_space, totalThread);
    ptr->thread_id = i;
    perThreadArr[i] = ptr;
  }
}

PerThreadMemoryAllocator* MemoryAllocator::getPerThreadMemoryAllocator(
    int thread_id) {
  return perThreadArr[thread_id];
}

void* MemoryAllocator::Allocate(std::size_t requestedBlock, int thread_id,
                                bool IsHash) {
  // std::cout<<thread_id<<std::endl;

  // perThreadArr[thread_id]->lock.lock();
  void* ptr;

  ptr = perThreadArr[thread_id]->Allocate(requestedBlock, IsHash);

  // perThreadArr[thread_id]->lock.unlock();

  //  perThreadArr[thread_id]->lock.unlock();

  return ptr;
}

void MemoryAllocator::free(void* ptr, std::size_t deletedBlock, int thread_id,
                           bool isHash) {
  /*  if(deletedBlock == 32)
    {
        counter++;
    }
    */

  perThreadArr[thread_id]->free(ptr, deletedBlock, isHash);

  // perThreadArr[thread_id]->lock.unlock();

  // perThreadArr[thread_id]->free(ptr, deletedBlock, isHash);
}
/* AllocateMemory function defination*/
// AllocateMemory::AllocateMemory()
//{
//   curIndex=-1;
// }
// void AllocateMemory::init()
//{

/*

 Allocator big memory array
   0 indrArray 4 GB
   1 EdgeBlock 8 GB
   2 HashMap 10 GB
 */
/*
   8 bytes:   ArtPerIndex 30 MB  0
   16 bytes:  EdgeBlock 100 MB metadata 1, HashTableIndex
   24 bytes:  AemAdress (300 MB),  Bucket (300MB) 700 MB   2
   40 bytes:  EdgeInfoHal 10 GB , PerSourceVertexIndr (300 MB)
   56 bytes:   N4 70MB 5
   64 bytes:   EdgeBlock Indr array (if size is 8, 16, 32 ..) 2 GB
   160 bytes:  N16 70MB 7
   512 bytes:  EdgeUpdate array if size is 64 neighbour edges per block (512 can
   be passed as bit flag) 4096 8 656 bytes:  N48 70MB 9 2048 bytes: Source
   vertex array indirection size it double the size next time 2KB 10 2049 bytes:
   this flag bit handle the vertex array size which is 4194304 (280 MB) bytes 11
   2064 bytes: N256 70MB 12
   20 GB extra page allocation: any block can use that space and request for
   allocation 13
   */
// const int totalBlock = 3;
// vertex_t spaceNeeded[totalBlock] = {1,1,1};

/*  for(int i=0;i<totalBlock;i++)
  {
      Block *b = new Block();
      GlobalAllocator *memnew = new GlobalAllocator();
      memnew->init(spaceNeeded[i]);
      b->freeBlock.push_back(memnew);
      b->Isfree = false;
      HashTableMemoryIndex *ht = new HashTableMemoryIndex();
      b->freeList = ht;
      b->blockLock.init();
      allocArr[i] = b;
  }*/
// allocArr[i].freeBlock = NULL;

/*
     GlobalAllocator *ga = new GlobalAllocator();
     ga->init(space);
    // memAllocation = new std::vector<GlobalAllocator*>();
     memAllocation[++curIndex] = ga;
  */
//   freeList = new HashTableMemoryIndex();

/*
    vertex_t blockSize[14]   =
   {8,16,24,32,40,56,160,656,2064,blockIndr,blocksize,2048,4194304,80}; vertex_t
   spaceNeeded[14] =
   {100,300,2100,900,16240,200,200,200,200,6144,8096,3,680,1400}; int maxBlock =
   2065; GlobalAllocator *mem_alloc = memAllocation[curIndex]; allocatorBlockArr
   = reinterpret_cast<AllocatorSize **>((AllocatorSize *) (malloc(
            sizeof(AllocatorSize) * maxBlock)));
    for(int i=0;i<maxBlock;i++)
    {
        allocatorBlockArr[i] = new AllocatorSize();
    }*/
/*
 init_space = spaceNeeded[0] * 1024 * 1024
 loop_end = init_space/blockSize[0]
 loop_start = 0;
 loop_start+=blockSize[0];
 loop_start < init_space
 */

/* fixed size block allocation

 for(int i=0;i<9;i++) {
     vertex_t  init_space = spaceNeeded[i] * 1024ul * 1024ul; // MB
     vertex_t  loop_end = init_space/blockSize[i];
     if(allocatorBlockArr[blockSize[i]]->freelist.empty())
     {
         //std::queue<void *> freelist;// =  new std::vector<void *>();
         for (vertex_t loop_start = 0; loop_start < init_space; loop_start +=
 blockSize[i]) {
             allocatorBlockArr[blockSize[i]]->freelist.push(mem_alloc->allocate(blockSize[i]));
         }
         // (std::queue<void *> &&)
        // allocatorBlockArr[65]->freeList->insert(blockSize[i],  freelist);
     }
     else
     {
         for (vertex_t loop_start = 0; loop_start < init_space; loop_start +=
 blockSize[i]) {
             allocatorBlockArr[blockSize[i]]->freelist.push(mem_alloc->allocate(blockSize[i]));
         }
     }
 }
*/

/* variable size block allocation
allocatorBlockArr[65]->freeList = new HashTableMemoryIndex();

for(int i=9;i<14;i++)
{
    vertex_t  init_space = spaceNeeded[i] * 1024ul * 1024ul; // MB
    vertex_t  loop_end = init_space/blockSize[i];
    if(allocatorBlockArr[65]->freeList->getIndexVal(blockSize[i]) == NULL)
    {
        std::queue<void *> freelist;// =  new std::vector<void *>();
        for (vertex_t loop_start = 0; loop_start < init_space; loop_start +=
blockSize[i]) { freelist.push(mem_alloc->allocate(blockSize[i]));
        }
        // (std::queue<void *> &&)
        allocatorBlockArr[65]->freeList->insert(blockSize[i],  freelist);
    }
    else
    {
        for (vertex_t loop_start = 0; loop_start < init_space; loop_start +=
blockSize[i]) {
            allocatorBlockArr[65]->freeList->getIndexVal(blockSize[i])->getFreeBlockList().push(mem_alloc->allocate(blockSize[i]));
        }
    }
}
*/

//}
