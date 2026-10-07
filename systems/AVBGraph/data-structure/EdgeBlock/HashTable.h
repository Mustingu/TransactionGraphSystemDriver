
#ifndef EDGE_BLOCK_HASHTABLE_H
#define EDGE_BLOCK_HASHTABLE_H

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <forward_list>
#include <mutex>
#include <vector>

class HashTable {
  struct node {
    uint64_t key;
    uint64_t value;
  };
  unsigned entryCounter;
  unsigned hashBlockSize;
  node *hashtable;

  uint64_t hash(uint64_t key) { return key & (hashBlockSize - 1); }

 public:
  HashTable() {
    hashBlockSize = 2;
    hashtable = static_cast<node *>(malloc(hashBlockSize * 16));
    // ptr = static_cast<uint64_t *>(malloc(16));
    memset(hashtable, 0xff, sizeof(node) * hashBlockSize);

    entryCounter = 0;
  }

  bool getDestHashTableVal(uint64_t dest_node, uint64_t &link) {
    // get the dest node
    size_t index = hash(dest_node);
    int count = 0;
    while (hashtable[index].key != FLAG_EMPTY_SLOT) {
      count++;
      if (hashtable[index].key == dest_node) {
        link = hashtable[index].value;
        return true;
      }
      if (count == hashBlockSize) {
        return false;
      }
      index = index + 1;
      if (index >= hashBlockSize) {
        index = 0;
      }
    }
    return false;
  }

  void resizeHashTable(int requested_block) {
    // HashTablePerSource* oldArr = hashPtr;
    // std::cout << "resizeHashTable " << requested_block << std::endl;
    // for (int i = 0; i < hashBlockSize; i++) {
    //   std::cout << hashtable[i].key << " " << hashtable[i].value <<
    //   std::endl;
    // }
    // std::cout << "entryCounter " << entryCounter << std::endl;

    // assert(false);
    u_int32_t oldCap = hashBlockSize;

    // capacity =requested_block;
    // u_int64_t* newHashTable = static_cast<u_int64_t
    // *>(la->Allocate(sizeof(HashTablePerSource), thread_id));

    node *newHashTable =
        static_cast<node *>(malloc(sizeof(node) * requested_block));
    // u_int64_t *newPtr =
    //     static_cast<u_int64_t *>(malloc(sizeof(u_int64_t) *
    //     requested_block));

    memset(newHashTable, 0xff,
           requested_block * sizeof(node));  // reset new array
    hashBlockSize = requested_block;

    // newHashTable->hashBlockSize = requested_block;

    for (u_int32_t i = 0; i < oldCap; i++) {
      u_int64_t adj_p = hashtable[i].key;
      if (adj_p != FLAG_EMPTY_SLOT) {
        u_int64_t index = hash(adj_p);
        while (newHashTable[index].key != FLAG_EMPTY_SLOT) {
          index++;
          if (index >= requested_block) {
            index = 0;
          }
        }
        newHashTable[index] = hashtable[i];
        // newPtr[index] = ptr[i];
      }
    }

    // node *oldArr = hashtable;
    std::swap(hashtable, newHashTable);
    // hashtable = newHashTable;

    free(newHashTable);
    // free(newPtr);
    // for resize everytime you need to move to the STAL to get dest node and
    // store it in the array
  }

  void setDestHashTableVal(uint64_t dest_node, uint64_t link) {
    if (entryCounter > ((uint32_t)hashBlockSize >> 1)) {
      resizeHashTable(hashBlockSize * 2);
    }

    entryCounter++;

    size_t index = hash(dest_node);
    // int count =0;
    while (hashtable[index].key != FLAG_EMPTY_SLOT) {
      // count++;
      index++;
      if (index >= hashBlockSize) {
        index = 0;
      }
    }
    hashtable[index] = (node){dest_node, link};
  }
  // ~HashTable() {}
};

class HashTable2 {
  uint64_t *hashtable;
  unsigned entryCounter;
  unsigned hashBlockSize;

  uint64_t hash(uint64_t key) { return key & (hashBlockSize - 1); }

 public:
  HashTable2() {
    hashtable = static_cast<uint64_t *>(malloc(16));
    hashBlockSize = 2;
    memset(hashtable, 0xff, sizeof(uint64_t) * hashBlockSize);

    entryCounter = 0;
  }

  bool getDestHashTableVal(uint64_t dest_node, uint64_t link) {
    // get the dest node
    size_t index = hash(dest_node);
    //  int count = 0;
    while (hashtable[index] != FLAG_EMPTY_SLOT) {
      //   count++;
      if (hashtable[index] == dest_node) {
        return true;
      }
      index = index + 1;
      if (index >= hashBlockSize) {
        index = 0;
      }
    }
    return false;
  }

  void resizeHashTable(int requested_block) {
    // HashTablePerSource* oldArr = hashPtr;

    u_int32_t oldCap = hashBlockSize;

    // capacity =requested_block;
    // u_int64_t* newHashTable = static_cast<u_int64_t
    // *>(la->Allocate(sizeof(HashTablePerSource), thread_id));

    u_int64_t *newHashTable =
        static_cast<u_int64_t *>(malloc(sizeof(u_int64_t) * requested_block));

    memset(newHashTable, 0xff,
           requested_block * sizeof(u_int64_t));  // reset new array
    hashBlockSize = requested_block;

    // newHashTable->hashBlockSize = requested_block;

    for (u_int32_t i = 0; i < oldCap; i++) {
      u_int64_t adj_p = hashtable[i];
      if (adj_p != FLAG_EMPTY_SLOT) {
        u_int64_t index = hash(adj_p);
        while (newHashTable[index] != FLAG_EMPTY_SLOT) {
          index++;
          if (index >= requested_block) {
            index = 0;
          }
        }
        newHashTable[index] = adj_p;
      }
    }

    u_int64_t *oldArr = hashtable;
    hashtable = newHashTable;

    free(oldArr);
    // for resize everytime you need to move to the STAL to get dest node and
    // store it in the array
  }

  void setDestHashTableVal(uint64_t dest_node, uint64_t link) {
    if (entryCounter >= ((uint32_t)hashBlockSize >> 1)) {
      resizeHashTable(hashBlockSize * 2);
    }

    entryCounter++;

    size_t index = hash(dest_node);
    // int count =0;
    while (hashtable[index] != FLAG_EMPTY_SLOT) {
      // count++;
      index++;
      if (index >= hashBlockSize) {
        index = 0;
      }
    }
    hashtable[index] = dest_node;
  }

  void removeDestHashTableVal(uint64_t dest_node) {
    size_t index = hash(dest_node);
    while (hashtable[index] != FLAG_EMPTY_SLOT && hashtable[index] != dest_node) {
      index = (index + 1) & (hashBlockSize - 1);
    }
    if (hashtable[index] != dest_node) return;  // not present

    // Backward-shift deletion (Knuth) keeps the linear-probe chain valid.
    hashtable[index] = FLAG_EMPTY_SLOT;
    size_t i = index;
    size_t j = index;
    for (;;) {
      j = (j + 1) & (hashBlockSize - 1);
      if (hashtable[j] == FLAG_EMPTY_SLOT) break;  // end of cluster
      size_t home = hash(hashtable[j]);
      // Move key at j into gap i iff i lies on j's probe path [home, j).
      bool on_path = (home <= j) ? (home <= i && i < j)
                                 : (i >= home || i < j);
      if (on_path) {
        hashtable[i] = hashtable[j];
        hashtable[j] = FLAG_EMPTY_SLOT;
        i = j;
      }
    }
    entryCounter--;
  }
  // ~HashTable() {}
};
#endif  // EDGE_BLOCK_HASHTABLE_H
        /*******  61b699f2-6b13-4d32-8c9c-4d1c803653f5  *******/