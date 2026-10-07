#pragma once

#include <cstdint>
#include <limits>

// For NeoGraph
#define VERTEX_GROUP_BITS 6
constexpr uint64_t VERTEX_GROUP_SIZE = 1 << VERTEX_GROUP_BITS;
constexpr uint64_t VERTEX_GROUP_MASK = (1 << VERTEX_GROUP_BITS) - 1;
constexpr uint64_t INDEPENDENT_MAP_BLOCK_NUM = (VERTEX_GROUP_SIZE + 63) / 64;
#define RANGE_LEAF_SIZE 512         // 512 by default
#define ART_EXTRACT_THRESHOLD 8192  // 8192 by default
#define ART_LEAF_SIZE 256           // 16 * 16
#define SEQUENTIAL_SCAN_THRESHOLD 16
#define EDGE_INSERT_VEC_THRESHOLD 0.8
#define BATCH_UPDATE_THRESHOLD (1 << 2)
#define INIT_READER_NUM 64
// ActiveWriterTracer::writer_register() spins on `blocks[INIT_WRITER_NUM]`
// until it can try_lock a free block; it has no backoff and no fallback, so
// running out of blocks is a permanent livelock, not a slow path.
//
// Each writer thread needs TWO blocks at once:
//   1. one held for the thread's whole lifetime by RSDriver::RegisterThread()
//      (Drivers/RSDriver.h), because DeleteEdge() feeds it to
//      LightWriteTransaction::remove_edge();
//   2. one held transiently per UpsertEdge(), created and destroyed by
//      WriteTransaction (NeoGraph/src/neo_transaction.cpp:146).
// So N writer threads require >= 2N blocks. RapidStore's own POSIX writers
// (neo_transaction.cpp) also register through the same pool, and the RS
// algorithm phases register one block per OMP thread for their duration.
//
// The benchmark runs --write-thread 64, so 2*64 = 128 is the floor; 192 also
// leaves room for the 96-thread OMP phases (nproc here) and stays well clear
// of the 64 that silently saturated (and livelocked) at --write-thread 64.
#define INIT_WRITER_NUM 192
// For Property
#define VERTEX_PROPERTY_NUM 0
#define EDGE_PROPERTY_NUM 1

#define COMPRESSION_ENABLE 1
#define FROM_CLUSTERED_TO_SMALL_VEC_ENABLE 0
#define SIMULATE_PER_EDGE_VERSIONING_ENABLE 0
// For segment pool
#define SEGMENT_POOL_INIT_SIZE 256
#define BATCH_UPDATE_THREAD_NUM 31
#define BATCH_UPDATE_ENABLE_THRESHOLD 32

#define VERSION_HEAD_MASK 0x8000000000000000