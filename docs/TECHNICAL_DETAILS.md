# Technical Design Note

This document is the code-level deep dive for the current implementation in [csim.c](../csim.c) and [trans.c](../trans.c). It is intentionally more detailed than the main [README](../README.md) and is written to match the repository as it exists now, not a hypothetical or generic Cache Lab solution.

## Reader Guide

The main [README](../README.md) is optimized for quick comprehension. This document is optimized for technical review. It is written in the style of a design note:

- first define the machine model
- then explain the simulator architecture
- then walk through the transpose strategy with the same tile structure the code uses
- finally tie the implementation to the measured results and current driver thresholds

If someone reads only one part of this file, the most valuable section is the `64x64` transpose discussion because that is where the implementation demonstrates the most deliberate cache-aware reasoning.

## Scope and Source of Truth

This writeup describes:

- the cache simulator currently implemented in [csim.c](../csim.c)
- the transpose implementation currently implemented in [trans.c](../trans.c)
- the grading thresholds currently defined in [driver.py](../driver.py)

Where possible, the explanations below use excerpts from the actual code so the document stays aligned with the implementation.

## Problem Model

The transpose component is evaluated against the standard Cache Lab configuration:

- cache size: `1 KB`
- associativity: direct-mapped for transpose evaluation (`E = 1`)
- block size: `32 bytes`
- element size: `4-byte int`

That means each cache block holds exactly `8` integers. This drives several design choices in [trans.c](../trans.c):

- loading `8` contiguous integers from a row is ideal because it fills exactly one block
- transposed writes are inherently more hostile to locality than source reads
- `64x64` is the hardest case because source and destination regions repeatedly map onto the same cache sets in a direct-mapped cache

### Memory Geometry Snapshot

```text
Cache size          = 1024 bytes
Block size          =   32 bytes
Cache lines         = 1024 / 32 = 32
Bytes per int       =    4
Ints per cache line =   32 / 4  = 8
```

That simple `8 ints per line` fact is the foundation of the transpose code:

- `transpose_32` loads exactly `8` values from one source row at a time
- `transpose_64` still works on `8x8` tiles, but reorders writes to avoid set conflicts
- `transpose_generic` uses larger blocked traversal because the irregular case is less about exact tile choreography and more about preserving locality while clipping edges safely

### Tile Vocabulary Used Below

For the `64x64` explanation, it helps to name the four `4x4` quadrants inside one `8x8` tile:

```text
A tile covering rows i..i+7 and cols j..j+7

                j..j+3       j+4..j+7
              +-----------+-----------+
i..i+3        |    UL     |    UR     |
              |   4 x 4   |   4 x 4   |
              +-----------+-----------+
i+4..i+7      |    LL     |    LR     |
              |   4 x 4   |   4 x 4   |
              +-----------+-----------+
```

That quadrant naming matches the conceptual decomposition used in the code, even though the code itself is written directly in terms of scalar loads and stores.

## Part A: Cache Simulator

File: [csim.c](../csim.c)

### Design Goals

The simulator is built around a few practical goals:

- match the reference simulator's hit, miss, and eviction behavior
- keep the implementation simple enough to audit
- avoid storing block contents that are irrelevant to grading
- handle 64-bit traces safely
- implement LRU without pointer-heavy bookkeeping

### Cache Representation

The simulator models the cache explicitly as sets containing lines:

```c
typedef struct {
    int valid;
    unsigned long long tag;
    unsigned long long last_used;
} CacheLine;

typedef struct {
    CacheLine *lines;
} CacheSet;

typedef struct {
    CacheSet *sets;
    int s;
    int E;
    int b;
    unsigned long long set_mask;
    unsigned long long timestamp;
    int hits;
    int misses;
    int evictions;
} Cache;
```

Only metadata is stored:

- `valid`
- `tag`
- `last_used`

No block payload is allocated, because the grading logic depends only on cache metadata and replacement behavior.

### Command-Line Parsing and Validation

The simulator uses `getopt` and validates inputs before allocating the cache:

```c
while ((opt = getopt(argc, argv, "hvs:E:b:t:")) != -1) {
    switch (opt) {
    case 'h':
        print_usage(argv[0]);
        return 0;
    case 'v':
        verbose = 1;
        break;
    case 's':
        if (!parse_int_arg(optarg, 1, &s)) {
            print_usage(argv[0]);
            return 1;
        }
        have_s = 1;
        break;
    case 'E':
        if (!parse_int_arg(optarg, 0, &E)) {
            print_usage(argv[0]);
            return 1;
        }
        have_E = 1;
        break;
    case 'b':
        if (!parse_int_arg(optarg, 1, &b)) {
            print_usage(argv[0]);
            return 1;
        }
        have_b = 1;
        break;
    case 't':
        trace_path = optarg;
        break;
    default:
        print_usage(argv[0]);
        return 1;
    }
}
```

Two details are worth noting because they reflect the actual code:

- `E` must be nonzero
- `s` and `b` are parsed with `allow_zero = 1`, so `0` is accepted for those fields

The implementation also guards against unsafe shifts:

```c
if (!have_s || !have_E || !have_b || trace_path == NULL) {
    print_usage(argv[0]);
    return 1;
}
if (s >= addr_bits || b >= addr_bits || s + b >= addr_bits) {
    print_usage(argv[0]);
    return 1;
}
```

That check matters because the cache uses bit shifts to extract set index and tag, and the code should not rely on undefined behavior for oversized shifts.

### Allocation Strategy

The cache allocates:

- one array of `CacheSet`
- one `CacheLine` array per set

The allocation is sized directly from `s` and `E`:

```c
set_count = 1ULL << s;
cache->sets = (CacheSet *)calloc((size_t)set_count, sizeof(CacheSet));
if (cache->sets == NULL) {
    return 0;
}

for (set_index = 0; set_index < set_count; ++set_index) {
    cache->sets[set_index].lines = (CacheLine *)calloc((size_t)E, sizeof(CacheLine));
    if (cache->sets[set_index].lines == NULL) {
        unsigned long long cleanup_index;
        for (cleanup_index = 0; cleanup_index < set_index; ++cleanup_index) {
            free(cache->sets[cleanup_index].lines);
        }
        free(cache->sets);
        cache->sets = NULL;
        return 0;
    }
}
```

This is more robust than assuming a single allocation will always succeed. If allocation fails partway through, the code frees everything already allocated before returning failure.

### Address Decomposition

The implementation stores a precomputed `set_mask` in the cache object:

```c
cache->set_mask = (s == 0) ? 0ULL : ((1ULL << s) - 1ULL);
```

Then each access computes:

```c
set_index = (address >> cache->b) & cache->set_mask;
tag = address >> (cache->s + cache->b);
```

Using `unsigned long long` for `address`, `tag`, `set_mask`, and `timestamp` keeps the simulator safe for 64-bit traces and high-bit values.

### LRU Implementation

LRU is implemented with a single monotonic timestamp stored in the `Cache` object. Every data access increments it:

```c
cache->timestamp += 1ULL;
```

This avoids:

- per-set linked lists
- line reordering after every hit
- global aging passes

The timestamp approach is a good fit here because:

- it is simple to reason about
- it keeps each access local to one set
- the trace sizes in the lab are small enough that timestamp overflow is not a practical concern

### Access Path

The core behavior lives in `access_cache`, which performs a single pass over one set:

```c
for (line_index = 0; line_index < cache->E; ++line_index) {
    CacheLine *line = &set->lines[line_index];

    if (line->valid) {
        if (line->tag == tag) {
            line->last_used = cache->timestamp;
            cache->hits += 1;
            return ACCESS_HIT;
        }
        if (lru_line == NULL || line->last_used < lru_line->last_used) {
            lru_line = line;
        }
    } else if (empty_line == NULL) {
        empty_line = line;
    }
}
```

During that one pass, the code tracks:

- a hit candidate
- the first invalid line
- the least recently used valid line

If no hit occurs, the miss path is:

```c
cache->misses += 1;
if (empty_line != NULL) {
    empty_line->valid = 1;
    empty_line->tag = tag;
    empty_line->last_used = cache->timestamp;
    return ACCESS_MISS;
}

lru_line->tag = tag;
lru_line->last_used = cache->timestamp;
cache->evictions += 1;
return ACCESS_MISS | ACCESS_EVICTION;
```

This matches the intended behavior exactly:

- fill invalid lines before evicting
- evict the least recently used valid line only when the set is full

### Control Flow Summary

At a high level, the simulator's per-access decision tree looks like this:

```text
parse trace op
    |
    +--> I : ignore
    |
    +--> L / S : one access_cache() call
    |
    +--> M : two access_cache() calls

access_cache(address)
    |
    +--> scan one set
           |
           +--> tag match      -> hit
           +--> invalid line   -> miss, fill
           +--> otherwise      -> miss, evict LRU
```

That summary is intentionally simple because the actual code is intentionally simple. There is no hidden state machine beyond the line scan and timestamp updates shown above.

### Trace Processing

The simulator reads the trace file line by line and applies the lab semantics:

```c
if (sscanf(buffer, " %c %llx,%d", &op, &address, &size) != 3) {
    continue;
}
if (op == 'I') {
    continue;
}
```

It then handles each operation type:

```c
if (op == 'L' || op == 'S') {
    int result = access_cache(&cache, address);
    if (verbose) {
        print_result_tokens(result);
        printf("\n");
    }
} else if (op == 'M') {
    int first = access_cache(&cache, address);
    int second = access_cache(&cache, address);
    if (verbose) {
        print_result_tokens(first);
        print_result_tokens(second);
        printf("\n");
    }
}
```

This is an exact implementation of the lab rules:

- `I` is ignored
- `L` and `S` perform one access
- `M` performs two accesses

### Verbose Mode

The simulator prints verbose result tokens using:

```c
static void print_result_tokens(int result)
{
    if ((result & ACCESS_MISS) != 0) {
        printf(" miss");
    }
    if ((result & ACCESS_EVICTION) != 0) {
        printf(" eviction");
    }
    if ((result & ACCESS_HIT) != 0) {
        printf(" hit");
    }
}
```

That ordering matters. For example:

- a simple hit becomes `hit`
- a miss with eviction becomes `miss eviction`
- an `M` that misses first and then hits becomes `miss hit`

### Cleanup and Final Reporting

The code frees all allocated memory and calls `printSummary` exactly once at the end of `main`:

```c
fclose(trace_file);
printSummary(cache.hits, cache.misses, cache.evictions);
free_cache(&cache);
return 0;
```

The file-open failure path also frees the cache before exiting:

```c
trace_file = fopen(trace_path, "r");
if (trace_file == NULL) {
    fprintf(stderr, "%s: %s\n", trace_path, strerror(errno));
    free_cache(&cache);
    return 1;
}
```

### Complexity

For `S = 2^s` sets and `E` lines per set:

- memory: `O(S * E)`
- access time per memory operation: `O(E)`

That is the expected complexity for a straightforward explicit cache simulator.

### Result

Official Linux result:

- `test-csim = 27/27`

This is a perfect correctness score and means the simulator matched the reference results across the provided evaluation traces.

## Part B: Cache-Aware Matrix Transpose

File: [trans.c](../trans.c)

### Design Constraints Reflected in the Code

The current implementation respects the project constraints in a concrete way:

- no recursion
- no `malloc`
- no local arrays in the transpose logic
- scalar temporaries only
- minimal `transpose_submit` dispatch logic

The dispatch function is intentionally small:

```c
void transpose_submit(int M, int N, int A[N][M], int B[M][N])
{
    if (M == 32 && N == 32) {
        transpose_32(M, N, A, B);
    } else if (M == 64 && N == 64) {
        transpose_64(M, N, A, B);
    } else {
        transpose_generic(M, N, A, B);
    }
}
```

One important detail: the code does not have a dedicated `transpose_61x67` helper by name. Instead, the generic helper is used for all sizes other than `32x32` and `64x64`. In graded use, that means it handles `61x67`.

## `32x32` Implementation

### Exact Strategy in the Current Code

The `32x32` helper uses `8x8` tiling and scalar register staging:

```c
for (i = 0; i < N; i += 8) {
    for (j = 0; j < M; j += 8) {
        for (k = i; k < i + 8; ++k) {
            a0 = A[k][j];
            a1 = A[k][j + 1];
            a2 = A[k][j + 2];
            a3 = A[k][j + 3];
            a4 = A[k][j + 4];
            a5 = A[k][j + 5];
            a6 = A[k][j + 6];
            a7 = A[k][j + 7];

            B[j][k] = a0;
            B[j + 1][k] = a1;
            B[j + 2][k] = a2;
            B[j + 3][k] = a3;
            B[j + 4][k] = a4;
            B[j + 5][k] = a5;
            B[j + 6][k] = a6;
            B[j + 7][k] = a7;
        }
    }
}
```

### Tile Sketch

The access pattern for one `8x8` tile is conceptually:

```text
Read one full row segment from A:

A[k][j..j+7] -> a0 a1 a2 a3 a4 a5 a6 a7

Write one full column segment into B:

B[j][k]     = a0
B[j + 1][k] = a1
B[j + 2][k] = a2
B[j + 3][k] = a3
B[j + 4][k] = a4
B[j + 5][k] = a5
B[j + 6][k] = a6
B[j + 7][k] = a7
```

So the code is effectively doing:

```text
8x8 source tile in A
    row by row load
        ->
8x8 destination tile in B
    column by column fill
```

### Why This Matches the Cache Geometry

Because each cache block holds `8` integers:

- `A[k][j]` through `A[k][j + 7]` sit in one source block
- scalar temporaries keep those values out of the way while the code writes to `B`
- the tile is small enough that locality remains good without extra diagonal handling logic

This implementation does not use a separate diagonal-special-case path. The code relies on simple blocked traversal plus scalar staging.

### Result

- `32x32`: `287` misses

That is well within the full-score threshold defined in [driver.py](../driver.py).

## `64x64` Implementation

### Why This Case Requires a Different Strategy

`64x64` is the difficult case because naive `8x8` blocking interacts poorly with a direct-mapped cache:

- source rows and destination columns repeatedly map to the same sets
- simple row-wise transpose inside each `8x8` tile causes heavy line thrashing

The current implementation uses a three-phase tile strategy.

### Tile Layout

The best way to read the code is as movement between quadrants of one `8x8` tile:

```text
Source tile in A

                j..j+3       j+4..j+7
              +-----------+-----------+
i..i+3        |    UL     |    UR     |
              |   4 x 4   |   4 x 4   |
              +-----------+-----------+
i+4..i+7      |    LL     |    LR     |
              |   4 x 4   |   4 x 4   |
              +-----------+-----------+
```

The final destination in `B` should be:

```text
Destination tile in B, indexed as B[col][row]

                i..i+3       i+4..i+7
              +-----------+-----------+
j..j+3        |   UL^T    |   LL^T    |
              |   4 x 4   |   4 x 4   |
              +-----------+-----------+
j+4..j+7      |   UR^T    |   LR^T    |
              |   4 x 4   |   4 x 4   |
              +-----------+-----------+
```

The implementation does not write that final layout in one straight pass. It gets there through a staging step because that staging step avoids the worst direct-mapped conflicts.

### Phase 1: Process the Upper Four Rows

The helper first reads the top half of an `8x8` tile and writes:

- the left half to its final destination
- the right half into staged positions in `B`

```c
for (k = 0; k < 4; ++k) {
    a0 = A[i + k][j];
    a1 = A[i + k][j + 1];
    a2 = A[i + k][j + 2];
    a3 = A[i + k][j + 3];
    a4 = A[i + k][j + 4];
    a5 = A[i + k][j + 5];
    a6 = A[i + k][j + 6];
    a7 = A[i + k][j + 7];

    B[j][i + k] = a0;
    B[j + 1][i + k] = a1;
    B[j + 2][i + k] = a2;
    B[j + 3][i + k] = a3;
    B[j][i + k + 4] = a4;
    B[j + 1][i + k + 4] = a5;
    B[j + 2][i + k + 4] = a6;
    B[j + 3][i + k + 4] = a7;
}
```

The four staged writes:

- `B[j][i + k + 4]`
- `B[j + 1][i + k + 4]`
- `B[j + 2][i + k + 4]`
- `B[j + 3][i + k + 4]`

are not the final resting place for the upper-right quadrant. They are temporary staging locations used to avoid the worst conflict pattern.

After phase 1, the relevant region of `B` looks like this:

```text
Intermediate B state after phase 1

                i..i+3         i+4..i+7
              +-------------+-------------+
j..j+3        |    UL^T     |  UR^T temp  |
              |    final    |   staged    |
              +-------------+-------------+
j+4..j+7      |   unused    |   unused    |
              |             |             |
              +-------------+-------------+
```

That is why phase 1 looks slightly odd in code: it is intentionally writing the upper-right values into a temporary holding area inside the left half of the destination tile.

### Phase 2: Swap the Staged Values with the Lower-Left Quadrant

The second phase reads the staged values back out of `B`, reads the lower-left values from `A`, and swaps them into their final positions:

```c
for (k = 0; k < 4; ++k) {
    a0 = B[j + k][i + 4];
    a1 = B[j + k][i + 5];
    a2 = B[j + k][i + 6];
    a3 = B[j + k][i + 7];

    a4 = A[i + 4][j + k];
    a5 = A[i + 5][j + k];
    a6 = A[i + 6][j + k];
    a7 = A[i + 7][j + k];

    B[j + k][i + 4] = a4;
    B[j + k][i + 5] = a5;
    B[j + k][i + 6] = a6;
    B[j + k][i + 7] = a7;

    B[j + 4 + k][i] = a0;
    B[j + 4 + k][i + 1] = a1;
    B[j + 4 + k][i + 2] = a2;
    B[j + 4 + k][i + 3] = a3;
}
```

Conceptually, phase 2 performs this exchange:

```text
before phase 2:
    B[j..j+3][i+4..i+7] contains staged UR^T

phase 2 writes:
    LL^T -> B[j..j+3][i+4..i+7]
    UR^T -> B[j+4..j+7][i..i+3]
```

So the tile transitions like this:

```text
Intermediate B state after phase 2

                i..i+3         i+4..i+7
              +-------------+-------------+
j..j+3        |    UL^T     |    LL^T     |
              |    final    |    final    |
              +-------------+-------------+
j+4..j+7      |    UR^T     |   pending   |
              |    final    |             |
              +-------------+-------------+
```

This is the key step in the implementation. The code uses `B` as a controlled staging area so that upper-right and lower-left data can be reordered without forcing the worst-case direct-mapped collisions at the wrong time.

### Phase 3: Write the Lower-Right Quadrant

After the swap, the bottom-right `4x4` quadrant can be written directly:

```c
for (k = 0; k < 4; ++k) {
    a0 = A[i + 4 + k][j + 4];
    a1 = A[i + 4 + k][j + 5];
    a2 = A[i + 4 + k][j + 6];
    a3 = A[i + 4 + k][j + 7];

    B[j + 4][i + 4 + k] = a0;
    B[j + 5][i + 4 + k] = a1;
    B[j + 6][i + 4 + k] = a2;
    B[j + 7][i + 4 + k] = a3;
}
```

After phase 3, the tile is complete:

```text
Final B tile

                i..i+3         i+4..i+7
              +-------------+-------------+
j..j+3        |    UL^T     |    LL^T     |
              |    final    |    final    |
              +-------------+-------------+
j+4..j+7      |    UR^T     |    LR^T     |
              |    final    |    final    |
              +-------------+-------------+
```

### Why This Works

This is not just "blocking." It is conflict-aware reordering.

The important idea is:

- read one half of the tile
- stage part of it in `B`
- move the staged values later, after the lower-left source data has been handled

An ASCII summary of the data movement is:

```text
UL -> final top-left
UR -> temporary left-bottom staging area
LL -> final left-bottom, replacing staged UR
UR -> final top-right after being pulled back out of staging
LR -> final bottom-right
```

That is what allows the implementation to avoid the large miss penalties that simpler `64x64` transposes incur on a direct-mapped cache.

### Result

- `64x64`: `1227` misses

This is the strongest result in the project because `64x64` is the case that most clearly distinguishes truly cache-aware logic from ordinary blocked transpose code.

## Generic Helper for `61x67` and Other Sizes

### Exact Strategy in the Current Code

The generic helper uses `16x16` tiles with boundary checks:

```c
for (i = 0; i < N; i += 16) {
    for (j = 0; j < M; j += 16) {
        for (k = i; k < N && k < i + 16; ++k) {
            for (l = j; l < M && l < j + 16; ++l) {
                B[l][k] = A[k][l];
            }
        }
    }
}
```

This helper is used for:

- `61x67` in the graded workload
- any non-`32x32` and non-`64x64` dimensions if the function is called with other sizes

### Why the Generic Helper Is Reasonable

For irregular dimensions, the main concerns are:

- preserving locality through blocking
- handling boundaries cleanly
- not overcomplicating the implementation with special-case logic that offers little benefit

The `16x16` tile size is a practical compromise:

- large enough to exploit locality
- small enough that edge handling remains straightforward

For the graded irregular input, the traversal looks like this conceptually:

```text
61x67 matrix

rows:
0..15   16..31   32..47   48..63   64..66

cols:
0..15   16..31   32..47   48..60
```

So the last tiles are clipped naturally by:

```c
for (k = i; k < N && k < i + 16; ++k) {
    for (l = j; l < M && l < j + 16; ++l) {
        B[l][k] = A[k][l];
    }
}
```

That boundary logic is the whole reason the generic helper remains robust without requiring a separate irregular-case implementation.

### Result

- `61x67`: `1992` misses

That is inside the full-score threshold in the current driver.

## Exact Scoring Thresholds in This Repository

The performance thresholds are not just folklore. The current [driver.py](../driver.py) uses:

```python
trans32_score = computeMissScore(miss32, 300, 600, maxscore['trans32']) * int(result32[0])
trans64_score = computeMissScore(miss64, 1300, 2000, maxscore['trans64']) * int(result64[0])
trans61_score = computeMissScore(miss61, 2000, 3000, maxscore['trans61']) * int(result61[0])
```

That means the full-score lower thresholds in this repository are:

- `32x32`: `300`
- `64x64`: `1300`
- `61x67`: `2000`

The current results compare as follows:

| Case | Current Result | Full-Score Threshold | Outcome |
| --- | ---: | ---: | --- |
| Cache simulator | `27/27` | `27/27` | perfect correctness |
| `32x32` | `287` misses | `300` | full-score range |
| `64x64` | `1227` misses | `1300` | full-score range |
| `61x67` | `1992` misses | `2000` | full-score range |

## What the Results Demonstrate

From a systems perspective, these results show:

- the simulator is correct, not merely plausible
- the transpose implementation uses cache-aware blocking rather than brute-force copying
- the `64x64` path specifically handles direct-mapped conflict behavior in a nontrivial way
- the implementation choices are backed by measured outcomes on the actual driver

The `64x64` result is the most meaningful performance signal because that case punishes simplistic blocked implementations.

## Design Tradeoffs and Deliberate Simplifications

### Why timestamps for LRU?

- simpler than linked structures
- easy to inspect and reason about
- naturally supports one-pass set scanning

### Why not allocate block contents in the simulator?

- the lab does not require block payload simulation
- hits, misses, and evictions depend only on metadata
- omitting payload storage reduces code and memory footprint

### Why specialize the transpose by size?

- `32x32` is solved well by direct `8x8` blocking
- `64x64` requires conflict-aware staging and reordering
- `61x67` benefits more from clean blocked traversal than from intricate tile choreography

The current code specializes where specialization pays off and stays simple where it does not.

### Why no special diagonal path in `32x32`?

Because the current implementation already achieves `287` misses, which is in the full-score range. Additional diagonal-specific logic would increase complexity without being necessary for the observed result.

### Why keep the `64x64` logic imperative rather than abstract?

The `64x64` helper is easier to verify in its current style because:

- every loaded scalar has an immediate role
- the staging writes are visible as explicit assignments
- the swap step is concrete rather than hidden behind helper abstractions

For performance-sensitive code like this, explicitness is valuable. The function is longer than the `32x32` helper, but it remains mechanically readable because each phase corresponds to a specific tile transformation.

## Validation Notes

The code was validated on Linux using the provided build system and test tooling. The workflow assumes:

- Linux `x86_64`
- `gcc`
- `make`
- `valgrind`
- `python2` for the original driver

This matters because:

- [test-csim](../test-csim) and [csim-ref](../csim-ref) are Linux ELF binaries
- the repository's original tooling is not fully portable across modern non-Linux environments

## Resume-Style Summary

One concise way to describe the current implementation is:

> Implemented a cache simulator and a cache-aware matrix transpose in C, achieving perfect simulator correctness and full-score-range miss counts across multiple matrix sizes, including `1227` misses on the hardest `64x64` direct-mapped cache case.

That phrasing accurately reflects what this code does and what the measured results show.
