#ifndef MEMORYPOOLLIB_H
#define MEMORYPOOLLIB_H

#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#define PAGE_SIZE 8
#define MEMORY_VECTOR_SIZE 16
#define MEMORY_QUEUE_SIZE 1024
#define fixedSize_queue 16
#define START_BLOCK_UPDATE_SIZE 1
#define END_BLOCK_UPDATE_SIZE 1024
#define StartFixedBlockThreshold 11
#define EndFixedBlockThreshold 11
#define PREFETCH_AHEAD 1
#define BLOCKSIZEFIXED 4096

#define FOURMB 4L * (1024L) * (1024L)
#define EIGHTMB 8L * (1024L) * (1024L)
#define SIXTEENMB 16L * (1024L) * (1024L)
#define ONEGB (1024L) * (1024L) * (1024L)

#define fixedSize 1

const uint64_t PRIMETABLE[9] = {4ul,    16ul,    128ul,    512ul,   1024ul,
                                8192ul, 16384ul, 131072ul, 524288ul};
const u_int64_t FLAG_EMPTY_SLOT = 0xFFFFFFFFFFFFFFFF;
const u_int64_t FLAG_EMPTY_SLOT_HT = 0xFFFFFFFFFFFFFFFF;
const u_int64_t FLAG_TOMB_STONE = 0xFFFFFFFFFFFFFFFE;

int getCurSizeTable(int curSize2);

#endif