# TransactionalGraphSystemDriver

TransactionalGraphSystemDriver is a unified benchmark driver for evaluating transactional graph systems. It provides common workload generation, execution, and measurement paths for write workloads, concurrent read/write workloads, and static graph analytics.

The repository contains the driver and selected system integrations. Some systems used in the paper are optional because their source code is not redistributed here. Experimental batch scripts, generated results, and plots are intentionally not part of the public source package.

AVBGraph is also maintained as a clean standalone repository: [Mustingu/AVBGraph](https://github.com/Mustingu/AVBGraph).

## Driver overview

`GraphDriver.h` defines the interface implemented by each system adapter in `Drivers/`. It covers system initialization, edge operations, thread registration, read snapshots, and graph algorithm calls. The adapters translate this interface to each system's transaction and storage APIs.

Static graph analytics can use the shared kernels in `common/algorithms/GraphKernels.h` for PageRank (PR), breadth-first search (BFS), single-source shortest paths (SSSP), weakly connected components (WCC), local clustering coefficient (LCC), and community label propagation (CDLP). Each backend provides a `CommonView` that streams edges from its own data structures and read transaction/snapshot, without copying the full graph into a shared adjacency structure. System-specific algorithm implementations remain available.

The common kernels use the same traversal operations on every backend:

| Interface | Operation |
|---|---|
| `TraverseEdges` | Visit outgoing neighbors. |
| `TraverseEdgesWithProperty` | Visit outgoing neighbors and edge values. |
| `TraverseEdgesUntil` | Stop invoking the callback when it requests early termination. |
| `TraverseEdgesSorted` | Visit neighbors in destination-ID order when supported. |

This common path is opt-in through `--graphalytics --common-algorithms`.

## Experiments

The driver can be used for the following experiment types. The examples below show the relevant options; replace paths, systems, roots, and thread counts with those required by the dataset and experiment.

### Insert-only

Load a graph and measure edge-insertion throughput without concurrent readers or a mixed-update workload:

```bash
./build/bench_run --system avb --file /path/to/graph.e \
  --write-thread 64 -p --quiet
```

### Write-only mixed workloads

Run the supported mixed write workloads (`id`, `idm`, or `m100`) without concurrent analytics:

```bash
./build/bench_run --system avb --file /path/to/graph.e \
  --write-thread 64 --mix idm --rounds 2 --wseed 42 --quiet
```

The workload and transaction-count semantics are implemented in `utils/WorkloadGen.h`.

### Graphalytics-style static analytics

Load a graph once and run one algorithm or the supported algorithm set:

```bash
./build/bench_run --system avb --file /path/to/graph.e \
  --graphalytics --algorithm all --read-thread 32 --root 123
```

To select the shared kernels, add `--common-algorithms`. Dataset-specific roots and algorithm parameters should be supplied explicitly when needed.

### Concurrent read/write workloads

Run one or more analytics tasks while writers update the graph. Use `--repeat` for the regular repeated-edge write workload, or select a HotSet workload with `--hotset`:

```bash
./build/bench_run --system avb --file /path/to/graph.e \
  --con --tasks pr --write-thread 32 --read-thread 32 \
  --hotset 0.1 --rounds 2 --wseed 42 --quiet
```

The `--tasks` list controls the concurrent reader tasks; for example, it can contain repeated `pr` or `bfs` entries, or a combination of both. Omitting `--hotset` selects the regular non-HotSet write distribution.

### Memory monitoring

On Linux, run a benchmark through `mem_monitor.py` to sample the process's RSS, peak RSS (VmHWM), and virtual memory size from `/proc/<pid>/status` while preserving its output and phase markers:

```bash
python3 mem_monitor.py \
  --interval 0.2 \
  --out-prefix results/memory/avb_graph24_pr \
  --label "AVB Graph24 PR" \
  -- ./build/bench_run --system avb --file /path/to/graph.e \
       --con --tasks pr --write-thread 32 --read-thread 32 \
       --hotset 0.1 --rounds 2 --quiet
```

The output prefix produces `.mem.csv` (memory samples), `.events.csv` (recognized phase markers), `.stdout.log` (benchmark output), `.png` (memory curve), and `.summary.json` (peak usage, duration, and exit status). Use a distinct prefix for each run. For phase-aligned comparisons, `phase_merge.py` can combine runs that contain the required markers; select `--mode concurrent` or `--mode hotset` and pass each run as `--run NAME=PREFIX`.

### HotSet generation and update-ratio experiments

`hotset_gen` creates a candidate hot-edge file that can be passed to the driver with `--hotset-file`:

```bash
./build/hotset_gen --input-file /path/to/graph.e \
  --vertex-fraction 0.001 --output /path/to/hotset.e --seed 42
```

Use `--hotset-file` instead of `--hotset` to run with that candidate set. The concurrent HotSet workload also supports `--hotset-update-pct` values `0`, `20`, `40`, `60`, `80`, and `100`; the non-update portion is split evenly between deletes and inserts.

### Write-thread scalability

Use the same workload and dataset while varying `--write-thread` to measure scaling. The thread count is an ordinary driver option, so experiment points can be run independently without a batch runner:

```bash
./build/bench_run --system avb --file /path/to/graph.e \
  --write-thread 16 --mix id --rounds 2 --wseed 42 --quiet
```

Repeat with the desired writer counts and workload options.

### Parameter quick reference

| Parameter | Purpose |
|---|---|
| `--system NAME` | Select a system backend; available names depend on the build. |
| `--file PATH` / `--prefile KEY` | Select the input graph directly or by a locally configured dataset key. |
| `--write-thread N` / `--read-thread N` | Set writer/reader thread counts; both default to 64. |
| `--graphalytics --algorithm ALG` | Run static analytics (`pr`, `bfs`, `sssp`, `wcc`, `lcc`, `cdlp`; static mode also accepts `all` or a comma-separated list). |
| `--common-algorithms` | Use shared graph kernels with Graphalytics mode, when supported by the backend. |
| `--con --tasks ALG...` | Run reader tasks concurrently with writers; repeated task names request multiple tasks. |
| `--repeat N` | Set edge amplification for the regular concurrent-write workload. |
| `--mix id\|idm\|m100 --rounds N` | Select a write-only mixed workload and its number of rounds. |
| `--hotset F` / `--hotset-file PATH` | Select a HotSet edge pool directly or provide a candidate edge file; use with `--con --tasks`. |
| `--hotset-update-pct P` | Set the HotSet update share (0–100% in 20% steps); remaining operations are split between delete and insert. |
| `--root ID` | Set the BFS/SSSP source vertex. |

`--repeat`, `--rounds`, and static `--repetitions` control different things: edge amplification, update rounds, and repeated algorithm executions, respectively. See `./build/bench_run --help` for additional options.

## Build

The project is intended for Linux and uses CMake with a C++17 compiler. The build also requires OpenMP, TBB, Zlib, and Threads. The included system integrations have additional dependencies; for example, Teseo is built through `build_teseo.sh` and requires its native build dependencies.

Configure and build the default targets:

```bash
cmake -S . -B build
cmake --build build --parallel
```

The default build includes the source integrations present in this repository, including AVBGraph, RapidStore/NeoGraph, LiveGraph, Teseo, and the CSR baseline. Build output is written under `build/`.

### Selecting a system

Use the executable and `--system` value below. The experiment options shown
above apply to the unified runners, including `bench_run_slt`.

| System | Executable | `--system` | Build requirement |
|---|---|---|---|
| AVBGraph (coarse) | `build/bench_run_avb_coarse` | `avb` | Default build |
| AVBGraph-dual | `build/bench_run` | `avb` | Default build |
| GTX | `build/bench_run` | `gtx` | `ENABLE_GTX=ON` and GTX sources |
| Sortledton (SLT) | `build/bench_run_slt` | `slt` | `ENABLE_SORTLEDTON=ON` and Sortledton sources |
| RapidStore-Opt | `build/bench_run` | `rs` | Default build |
| LiveGraph | `build/bench_run` | `lg` | Default build |
| Teseo | `build/bench_run` | `teseo` | Default build |
| CSR | `build/bench_run` | `csr` | Static analytics only |

`bench_run_slt` is compiled from the same `main.cpp` as the other unified
runners, with `USE_SLT`. It remains separate because AVBGraph and Sortledton
declare conflicting global types, including `Transaction` and
`VersionedTopologyInterface`.

For example, after enabling Sortledton:

```bash
./build/bench_run_slt --system slt --file /path/to/graph.e \
  --graphalytics --common-algorithms --algorithm all --read-thread 32 --root 123
```

## Optional backends

GTX and Sortledton source trees are not included in this repository. To build with either backend, clone its upstream repository into the corresponding directory below, then enable the CMake option. Follow the upstream build instructions and record the exact revision used.

```bash
# GTX upstream source
git clone https://github.com/Jiboxiake/GTX-SIGMOD2025.git systems/GTX

# Sortledton upstream source (access may depend on the upstream project)
git clone https://gitlab.db.in.tum.de/per.fuchs/sortledton.git systems/Sortledton

cmake -S . -B build -DENABLE_GTX=ON -DENABLE_SORTLEDTON=ON
cmake --build build --parallel
```

GTX's upstream build documentation lists CMake, TBB, and a C++17/20-capable toolchain. Sortledton's upstream repository provides its own setup instructions. These CMake options are off by default, and requesting a backend that was not built results in an explicit error.

The GTX version used in our experiments includes local changes to reader-snapshot tracking and OpenMP worker-slot management. Those source changes are not distributed here, so an unmodified upstream checkout may not reproduce the reported GTX behavior or results.

## Datasets and input files

Datasets are not included. Supply a Graphalytics-compatible edge file with `--file /path/to/graph.e`, or update the local prefile paths in `utils/Parse.h` for your environment. Some algorithms require a dataset-specific root or parameters; pass these explicitly when running experiments.

## License and third-party components

This repository integrates components with different licensing terms. Refer to the license and notice files shipped with each included component before redistributing or reusing it. No single project-wide license should be inferred from the presence of those component licenses.
