#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cachelab.h"

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

enum {
    ACCESS_HIT = 1,
    ACCESS_MISS = 2,
    ACCESS_EVICTION = 4
};

static void print_usage(const char *progname)
{
    printf("Usage: %s [-hv] -s <num> -E <num> -b <num> -t <file>\n", progname);
    printf("Options:\n");
    printf("  -h         Print this help message.\n");
    printf("  -v         Optional verbose flag.\n");
    printf("  -s <num>   Number of set index bits.\n");
    printf("  -E <num>   Number of lines per set.\n");
    printf("  -b <num>   Number of block offset bits.\n");
    printf("  -t <file>  Trace file.\n");
    printf("Example:\n");
    printf("  %s -s 4 -E 1 -b 4 -t traces/yi.trace\n", progname);
}

static int parse_int_arg(const char *text, int allow_zero, int *value)
{
    long parsed;
    char *endptr;

    if (text == NULL || *text == '\0') {
        return 0;
    }

    errno = 0;
    parsed = strtol(text, &endptr, 10);
    if (errno != 0 || *endptr != '\0') {
        return 0;
    }
    if (parsed < 0 || parsed > INT_MAX) {
        return 0;
    }
    if (!allow_zero && parsed == 0) {
        return 0;
    }

    *value = (int)parsed;
    return 1;
}

static int init_cache(Cache *cache, int s, int E, int b)
{
    unsigned long long set_count;
    unsigned long long set_index;

    cache->sets = NULL;
    cache->s = s;
    cache->E = E;
    cache->b = b;
    cache->set_mask = (s == 0) ? 0ULL : ((1ULL << s) - 1ULL);
    cache->timestamp = 0ULL;
    cache->hits = 0;
    cache->misses = 0;
    cache->evictions = 0;

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

    return 1;
}

static void free_cache(Cache *cache)
{
    unsigned long long set_count;
    unsigned long long set_index;

    if (cache->sets == NULL) {
        return;
    }

    set_count = 1ULL << cache->s;
    for (set_index = 0; set_index < set_count; ++set_index) {
        free(cache->sets[set_index].lines);
    }
    free(cache->sets);
    cache->sets = NULL;
}

static int access_cache(Cache *cache, unsigned long long address)
{
    unsigned long long set_index;
    unsigned long long tag;
    CacheSet *set;
    CacheLine *empty_line;
    CacheLine *lru_line;
    int line_index;

    cache->timestamp += 1ULL;
    set_index = (address >> cache->b) & cache->set_mask;
    tag = address >> (cache->s + cache->b);
    set = &cache->sets[set_index];
    empty_line = NULL;
    lru_line = NULL;

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
}

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

int main(int argc, char **argv)
{
    Cache cache;
    char *trace_path;
    int verbose;
    int have_s;
    int have_E;
    int have_b;
    int s;
    int E;
    int b;
    int opt;
    FILE *trace_file;
    char buffer[128];
    const int addr_bits = (int)(sizeof(unsigned long long) * CHAR_BIT);

    trace_path = NULL;
    verbose = 0;
    have_s = 0;
    have_E = 0;
    have_b = 0;
    s = 0;
    E = 0;
    b = 0;

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

    if (!have_s || !have_E || !have_b || trace_path == NULL) {
        print_usage(argv[0]);
        return 1;
    }
    if (s >= addr_bits || b >= addr_bits || s + b >= addr_bits) {
        print_usage(argv[0]);
        return 1;
    }

    if (!init_cache(&cache, s, E, b)) {
        fprintf(stderr, "Failed to allocate cache\n");
        return 1;
    }

    trace_file = fopen(trace_path, "r");
    if (trace_file == NULL) {
        fprintf(stderr, "%s: %s\n", trace_path, strerror(errno));
        free_cache(&cache);
        return 1;
    }

    while (fgets(buffer, (int)sizeof(buffer), trace_file) != NULL) {
        char op;
        unsigned long long address;
        int size;

        if (sscanf(buffer, " %c %llx,%d", &op, &address, &size) != 3) {
            continue;
        }
        if (op == 'I') {
            continue;
        }

        if (verbose) {
            printf("%c %llx,%d", op, address, size);
        }

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
    }

    fclose(trace_file);
    printSummary(cache.hits, cache.misses, cache.evictions);
    free_cache(&cache);
    return 0;
}
