# AVBGraph

In-memory transactional graph storage with epoch-based MVCC. Supports coarse-grained (epoch) and fine-grained (intra-epoch) snapshot isolation for consistent analytical queries on evolving graphs.

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

**Prerequisites**: C++17, CMake ≥ 3.17, Intel TBB, OpenMP.

The build provides the `AVB_Core` and `AVB_Core_coarse` libraries.
In this repository, benchmarks use the root driver's `bench_run` and
`bench_run_avb_coarse` executables; see the [driver README](../../README.md)
for build instructions and workload options.

## Core API

### AllVBManager — Epoch & Transaction Manager

The central coordinator. Manages epoch assignment, publication, and transaction lifecycle.

```cpp
#include "data-structure/VersionBlock/AllVBManager.h"

// on=false: manual epoch management (no background thread)
// on=true:  background thread auto-publishes epochs
auto* vbm = new AllVBManager(/*max_threads=*/32, /*on=*/false);
```

| Method | Description |
|--------|-------------|
| `registerTransaction(txn)` | Assign epoch + VBData to a write transaction. Blocks if too many in-flight epochs. |
| `registerROTransaction(txn)` | Set read epoch for a read-only transaction. Returns latest committed epoch. |
| `deregisterTransaction()` | Mark write transaction complete (thread-local). |
| `deregisterROTransaction()` | Mark read transaction complete (thread-local). |
| `commitVersionBlock(epoch)` | Publish all epochs up to (but not including) `epoch`. Advances `read_epoch_`. |
| `NoMoreTxn()` | Signal no more writes; background thread can finalize remaining epochs. |
| `getCurrentEpoch()` | Return current write epoch (`cur_`). |
| `getCurrentReadEpoch()` | Return latest committed read epoch (thread-safe). |

### GraphStore — Vertex Container

Holds all vertices. Each vertex is a `VertexEdges` block.

```cpp
#include "data-structure/EdgeBlock/GraphStore.h"

auto* graph = new GraphStore(/*num_vertices=*/N + 16);
graph->node_num.store(N);         // set actual vertex count

// Pre-allocate a vertex
auto* eb = new VertexEdges(v);    // v = vertex ID
eb->build(0, nullptr, nullptr);  // empty adjacency
graph->blocks[v].eb = eb;
```

| Method | Description |
|--------|-------------|
| `GetBlockByIndex(v)` | Return `VertexEdges*` for vertex `v`. |
| `check_degree(v, epoch)` | Return out-degree of vertex `v` at given epoch (thread-safe). |
| `get_node_num()` | Return number of vertices. |
| `new_vertex()` | Allocate a new vertex, return its ID. |

### VertexEdges — Per-Vertex Adjacency

Holds the Primary Adjacency (PA), Write Buffer (tmp_item), and VersionBlock chain for one vertex.

```cpp
auto* ds = graph->GetBlockByIndex(v);
ds->getReadLock();               // acquire read lock

// ... iterate edges ...

ds->unleashReadLock();           // release lock
```

| Method | Description |
|--------|-------------|
| `getReadLock()` / `unleashReadLock()` | Acquire/release read lock (tbb::spin_rw_mutex). |
| `build(n, edges, props)` | Initialize with `n` edges and properties. |
| `for_each_edge_sorted(epoch, cb)` | Iterate visible edges in sorted dst order (merges PA + tmp). |

### Transaction — Read/Write Handle

```cpp
#include "data-structure/Transaction/Transaction.h"

// Write transaction
Transaction txn(/*version=*/1, /*read_only=*/false, /*write_only=*/true, graph);
vbm->registerTransaction(&txn);

dst_t e[] = {(dst_t)src, (dst_t)dst, (dst_t)weight};
txn.insertedge(e);       // insert or update edge
txn.deletedge(e);        // logical-delete edge (src, dst only)
txn.commit();            // commit (FG: releases deferred locks)
vbm->deregisterTransaction();

// Read-only transaction
Transaction ro(1, true, false, graph);
vbm->registerROTransaction(&ro);
epoch_t snap = ro.get_read_epoch();    // coarse snapshot epoch
// In fine-grained mode:
// Composite fg = ro.get_read_ts();   // (read_epoch_+1, intra)
vbm->deregisterROTransaction();
```

| Method | Description |
|--------|-------------|
| `insertedge(edge)` | Insert or update edge `{src, dst, weight}`. |
| `deletedge(edge)` | Logical-delete edge `{src, dst}` (DELETION_MASK). |
| `commit()` | Commit transaction. FG mode: fetch intra_c, release deferred write locks. |
| `get_read_epoch()` | Return coarse read snapshot epoch. |
| `get_read_ts()` | Return fine-grained composite `(coarse, intra)` (FG only). |
| `get_intra_c()` | Lazy-fetch intra-epoch timestamp from VBData (FG only). |

## Edge Iteration

All iteration requires holding the read lock on the `VertexEdges` block.

```cpp
auto* ds = graph->GetBlockByIndex(v);
ds->getReadLock();
Composite snap(epoch, 0);   // or fg_read_ts in FG mode

// 1. Basic: iterate all visible out-edges
GraphAlgorithms::for_each_edge(ds, snap,
    [](EdgeWithIndex* e) {
        dst_t neighbor = e->e & ~DELETION_MASK;  // strip deletion bit
    }, nullptr);

// 2. With edge weights / properties
GraphAlgorithms::for_each_edge_with_property(ds, snap,
    [](EdgeWithIndex* e, EdgeWithIndex* p) {
        dst_t dst = e->e & ~DELETION_MASK;
        double w  = *reinterpret_cast<double*>(&p->properties);
    });

// 3. Early-exit: stops when callback returns true
GraphAlgorithms::for_each_edge_condition(ds, snap,
    [](EdgeWithIndex* e) -> bool {
        return /* found target */ false;
    });

// 4. Sorted by dst (merges sorted PA with ≤64 tmp entries)
//    Used by CDLP and LCC for O(deg) ordered scans
ds->for_each_edge_sorted(epoch,
    [](EdgeWithIndex* e) { /* visited in ascending dst order */ });

ds->unleashReadLock();
```

## Algorithms

All algorithms follow the pattern: create RO transaction → iterate edges → produce result. Constructor signatures are `(GraphStore*, AllVBManager*, thread_num)`.

### PageRank snapshot semantics

Each `compute_pagerank()` call holds one read-only transaction through initialization, all iterations, and result materialization. Coarse `read_ts` and fine-grained `fg_read_ts` are captured once, keeping the snapshot stable and its versions protected until the call finishes.

```cpp
// PageRank
PageRank pr(graph, vbm, 64);
pr.compute_pagerank(/*iterations=*/10, /*damping=*/0.85);
auto* scores = pr.get_raw_result();    // vector<double>

// BFS (direction-optimizing: top-down + bottom-up)
BFS bfs(graph, vbm, 64);
bfs.bfs(source_vertex);
auto* dists = bfs.get_raw_result();    // vector<int64_t>

// SSSP (Δ-stepping)
SSSP sssp(graph, vbm, 64);
sssp.compute_sssp(source_vertex, /*delta=*/2.0);
auto* sssp_dist = sssp.get_raw_result();  // vector<double>

// LCC (binary-search triangle counting)
LCC lcc(graph, vbm, 64);
lcc.compute_lcc();
auto* lcc_scores = lcc.get_raw_result();  // vector<double>

// WCC (Afforest label propagation)
WCC wcc(graph, vbm, 64);
wcc.compute_wcc();
auto* components = wcc.get_raw_result();  // vector<uint64_t>

// CDLP (label propagation, run-length counting on sorted edges)
CDLP cdlp(graph, vbm, 64);
cdlp.compute_cdlp(/*max_iterations=*/10);
auto* communities = cdlp.get_raw_result();  // vector<uint64_t>
```

Each algorithm provides `get_result()` returning `vector<pair<uint64_t, T>>` for logical-to-physical ID mapping (when `MEA != nullptr`).

## Full Example: Build Graph + Run PageRank

```cpp
#include "data-structure/EdgeBlock/GraphStore.h"
#include "data-structure/VersionBlock/AllVBManager.h"
#include "data-structure/Transaction/Transaction.h"
#include "algorithm/PageRank.h"

int main() {
    int N = 1000;
    auto* graph = new GraphStore(N + 16);
    auto* vbm   = new AllVBManager(32, false);
    graph->node_num.store(N);

    // Init vertices
    for (int v = 0; v < N; v++) {
        auto* eb = new VertexEdges(v);
        eb->build(0, nullptr, nullptr);
        graph->blocks[v].eb = eb;
    }

    // Insert edges at epoch 1
    for (auto [src, dst, w] : edges) {
        Transaction txn(1, false, true, graph);
        vbm->registerTransaction(&txn);
        dst_t e[] = {(dst_t)src, (dst_t)dst, (dst_t)w};
        txn.insertedge(e);
        txn.commit();
        vbm->deregisterTransaction();
    }

    // Publish: make edges visible to readers
    vbm->commitVersionBlock(2);

    // Run PageRank
    PageRank pr(graph, vbm, 64);
    pr.compute_pagerank(10, 0.85);
    auto* scores = pr.get_raw_result();

    delete graph;
    delete vbm;
}
```


## Configuration

Edit `utils/utils.h`:

```cpp
#define FINEGRAIN     // Fine-grained timestamps (default: on)
// #define TVB_STATS  // TVB scan statistics per iteration
// #define PR_DEBUG   // PageRank per-iteration timestamp output
// #define AVB_LEGACY_RESULT_OUTPUT  // write legacy output_pr/bfs/sssp files
```

## Driver static-evaluation integration

When AVB is used through the repository's `--graphalytics` driver mode, the
load phase ends with `AllVBManager::FinalizeNoMoreTxn()`. It first closes and
joins the background epoch updater, then synchronously transforms/publishes
the last write epoch. This gives static readers a complete snapshot without
racing the finalizer against the updater.

The driver obtains algorithm results from the in-memory result vectors and
writes its requested `--output` file after `processing_ms` has been measured.
The historical AVB algorithm files (`output_pr.result`, `bfs.output`, and
`output_sssp.result`) are disabled by default because their full sort and
per-line I/O distort benchmark timing. Define `AVB_LEGACY_RESULT_OUTPUT` only
for standalone debugging that explicitly needs them.

## Large-graph vertex growth

`GraphStore::blocks` and the driver-maintained `p_mHashMap` use
`tbb::concurrent_vector`. Their growth boundary is `size()`, not `capacity()`:
the latter may reserve unconstructed slots that cannot safely be indexed.
For driver loading, follow the upstream AVB benchmark path: construct a
`VertexEdges`, grow both vectors to include its logical ID, install
`blocks[id].eb`, and only then release the external-ID map accessor. This
ordering keeps other writers from resolving an ID before its block is ready
and supports inputs such as `graph24` that exceed the initial 200K table.

`GraphStore::new_vertex()` retains a defensive internal creation lock for
direct callers, but the benchmark driver deliberately uses the upstream
publication sequence above rather than substituting that helper.
