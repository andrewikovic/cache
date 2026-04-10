# Cache-Aware Systems Programming in C

This repository showcases a low-level performance engineering project in C99 centered on two problems that matter in real systems work:

- simulating cache behavior accurately
- reshaping memory access patterns to reduce cache misses

The implementation includes a configurable cache simulator and a cache-optimized matrix transpose, both written under strict correctness and implementation constraints. The project highlights practical skills in systems programming, memory hierarchy reasoning, and performance-focused algorithm design.

> Detailed design note: [Technical Design Note](/Users/ikovic/Documents/cache/docs/TECHNICAL_DETAILS.md)

## Project Summary

The codebase is organized around two components:

### 1. Cache Simulator

[`csim.c`](/Users/ikovic/Documents/cache/csim.c) implements a command-line cache simulator that models:

- configurable set count, associativity, and block size
- `L`, `S`, and `M` memory access operations from trace files
- least-recently-used eviction with a monotonic timestamp
- verbose trace reporting compatible with the reference workflow

The simulator uses dynamically allocated cache sets and line metadata only, which keeps the model compact and focused on the behavior that determines hits, misses, and evictions.

### 2. Cache-Optimized Matrix Transpose

[`trans.c`](/Users/ikovic/Documents/cache/trans.c) implements size-specific transpose strategies designed for a direct-mapped cache:

- `32x32`: `8x8` blocking with scalar register staging
- `64x64`: quadrant-aware blocking to reduce conflict misses
- `61x67`: bounded blocked traversal for irregular dimensions

This part of the project is about controlling locality explicitly. The implementation is tuned to reduce cache thrashing rather than just produce a correct transpose.

## Technical Highlights

- Wrote portable C99 with explicit heap management and no unnecessary abstractions
- Implemented LRU replacement cleanly with per-access timestamp updates
- Parsed 64-bit memory traces safely and matched reference simulator behavior
- Optimized matrix access patterns for spatial locality and reduced conflict misses
- Used specialized logic for the hardest cache-sensitive case instead of relying on one generic approach

## Results

The project was validated on Linux with the official tooling provided for the assignment baseline.

### Cache Simulator

- Official correctness score: `27/27`

This is a perfect result. The simulator matches the reference behavior across the provided trace configurations, which means the implementation is not just functional but correct under the grading model.

### Matrix Transpose

- `32x32`: `287` misses
- `64x64`: `1227` misses
- `61x67`: `1992` misses

These results fall within the standard full-credit or near-optimal range typically associated with this benchmark.

#### Interpreting the transpose results

- `32x32` at `287` misses is an excellent result and is consistent with a strong blocked solution. Typical full-credit performance is around `300` misses, and well-known optimized solutions usually land in the `256-300` range.
- `64x64` at `1227` misses is the strongest signal in the project. This is the hardest case because naive blocking performs poorly on a direct-mapped cache. A result in this range indicates effective handling of conflict misses, sub-blocking inside larger tiles, and careful movement of values through registers and cache-friendly staging.
- `61x67` at `1992` misses is also strong. Irregular dimensions are less about perfect symmetry and more about maintaining good locality while handling block edges efficiently. This score lands within the typical full-credit range for the standard driver.

Taken together, the test results show more than correctness:

- deliberate use of cache-aware blocking
- practical understanding of spatial locality
- ability to reduce direct-mapped conflict misses, especially in the `64x64` case
- performance tuning based on the cache model rather than brute force

## Why This Is a Strong Systems Project

This work demonstrates more than basic C implementation:

- translating a hardware model into a correct software simulator
- reasoning about locality, cache lines, and eviction behavior
- identifying where naive access patterns break down
- improving performance with targeted changes backed by measurable results

That combination maps well to systems, infrastructure, performance, and low-level backend work.

A concise way to describe the outcome is:

> Implemented a cache simulator and optimized matrix transpose under a direct-mapped cache model, achieving full simulator correctness and strong miss counts across multiple matrix sizes, including `1227` misses on the hardest `64x64` case.

## Build and Run

The validation workflow in this repository assumes:

- Linux `x86_64`
- `gcc`
- `make`
- `valgrind`
- `python2` for the original course driver

```bash
make
./test-csim
./test-trans -M 32 -N 32
./test-trans -M 64 -N 64
./test-trans -M 61 -N 67
```

To run the simulator directly:

```bash
./csim -s 4 -E 1 -b 4 -t traces/yi.trace
./csim -v -s 4 -E 1 -b 4 -t traces/yi.trace
```

To run the full original driver:

```bash
python2 ./driver.py
```

## Repository Layout

- [`csim.c`](/Users/ikovic/Documents/cache/csim.c): cache simulator implementation
- [`trans.c`](/Users/ikovic/Documents/cache/trans.c): cache-aware transpose implementation
- [`Makefile`](/Users/ikovic/Documents/cache/Makefile): build targets and test commands
- [`cachelab.h`](/Users/ikovic/Documents/cache/cachelab.h): helper interfaces used by the project
- [`docs/TECHNICAL_DETAILS.md`](/Users/ikovic/Documents/cache/docs/TECHNICAL_DETAILS.md): detailed design and performance writeup
- [`README.handout.md`](/Users/ikovic/Documents/cache/README.handout.md): original handout preserved for reference

## Background

This project originated from the CS:APP Cache Lab starter repository, but the implementation, optimization work, and documentation here are presented as a standalone systems programming project. The original handout remains available in [`README.handout.md`](/Users/ikovic/Documents/cache/README.handout.md) for historical context.
