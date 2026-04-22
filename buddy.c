#include "buddy.h"
#include <stddef.h>

#define MAXR 16
#define PAGE_SIZE 4096
#define MAXPAGES (1 << (MAXR - 1)) /* 32768 */

static char *base_ptr = NULL;
static int total_pages = 0;
static int max_rank = 0;

/* free_map[r][i] indicates whether the i-th block of rank r is free */
static unsigned char free_map[MAXR + 1][MAXPAGES];
static int free_count[MAXR + 1];
/* alloc_rank[idx] stores the rank of allocated block starting at base page idx, 0 if free */
static unsigned char alloc_rank_map[MAXPAGES];

static inline int is_power_of_two(int x) { return x > 0 && (x & (x - 1)) == 0; }

static inline int calc_max_rank_from_pages(int pages) {
    /* find r such that pages == 1 << (r-1). If not exact power of two, use floor */
    int r = 1;
    int p = 1; /* pages per block at rank 1 */
    while (r < MAXR && (p << 1) <= pages) {
        p <<= 1;
        r++;
    }
    return r;
}

int init_page(void *p, int pgcount){
    if (!p || pgcount <= 0) return -EINVAL;
    base_ptr = (char *)p;
    total_pages = pgcount;
    max_rank = calc_max_rank_from_pages(pgcount);

    /* clear structures */
    for (int r = 0; r <= MAXR; ++r) {
        free_count[r] = 0;
    }
    for (int r = 0; r <= MAXR; ++r) {
        for (int i = 0; i < MAXPAGES; ++i) free_map[r][i] = 0;
    }
    for (int i = 0; i < MAXPAGES; ++i) alloc_rank_map[i] = 0;

    /* Decompose the pgcount consecutive pages into aligned buddy blocks. */
    int remaining = total_pages;
    int cur_idx = 0; /* in pages */
    while (remaining > 0) {
        int chosen_r = 1;
        for (int r = max_rank; r >= 1; --r) {
            int block_pages = 1 << (r - 1);
            if (block_pages <= remaining && (cur_idx % block_pages == 0)) {
                chosen_r = r;
                break;
            }
        }
        int bp = 1 << (chosen_r - 1);
        int block_idx = cur_idx / bp;
        free_map[chosen_r][block_idx] = 1;
        free_count[chosen_r]++;
        cur_idx += bp;
        remaining -= bp;
    }
    return OK;
}

/* helper to find first free block index at rank r; returns -1 if none */
static int find_first_free(int r) {
    int blocks = total_pages >> (r - 1);
    if (blocks <= 0) return -1;
    for (int i = 0; i < blocks; ++i) {
        if (free_map[r][i]) return i;
    }
    return -1;
}

void *alloc_pages(int rank){
    if (rank < 1 || rank > max_rank) return ERR_PTR(-EINVAL);
    /* Find smallest r' >= rank with free blocks */
    int r = rank;
    while (r <= max_rank && free_count[r] == 0) r++;
    if (r > max_rank) return ERR_PTR(-ENOSPC);

    /* split down from r to rank */
    int cur_idx = find_first_free(r);
    if (cur_idx < 0) return ERR_PTR(-ENOSPC);
    /* remove this block from free */
    free_map[r][cur_idx] = 0;
    free_count[r]--;

    while (r > rank) {
        /* split into two children at rank r-1 */
        int left = cur_idx * 2;
        int right = left + 1;
        free_map[r - 1][left] = 1;
        free_map[r - 1][right] = 1;
        free_count[r - 1] += 2;
        r = r - 1;
        cur_idx = left; /* pick left to continue splitting for deterministic order */
        /* remove the chosen one for further splitting */
        free_map[r][cur_idx] = 0;
        free_count[r]--;
    }

    /* allocate block at rank r (== rank) with index cur_idx */
    int block_pages = 1 << (r - 1);
    int base_idx = cur_idx * block_pages;
    if (base_idx < 0 || base_idx >= total_pages) return ERR_PTR(-ENOSPC);
    alloc_rank_map[base_idx] = (unsigned char)r;

    return (void *)(base_ptr + (size_t)base_idx * PAGE_SIZE);
}

int return_pages(void *p){
    if (p == NULL) return -EINVAL;
    if (base_ptr == NULL) return -EINVAL;

    long diff = (char *)p - base_ptr;
    if (diff < 0 || (size_t)diff >= (size_t)total_pages * PAGE_SIZE) return -EINVAL;
    if (diff % PAGE_SIZE != 0) return -EINVAL;
    int idx = (int)(diff / PAGE_SIZE);
    if (idx < 0 || idx >= total_pages) return -EINVAL;
    int r = alloc_rank_map[idx];
    if (r <= 0 || r > max_rank) return -EINVAL;

    /* clear allocation mark */
    alloc_rank_map[idx] = 0;

    int block_pages = 1 << (r - 1);
    int block_idx = idx / block_pages;

    /* merge with buddy as much as possible */
    while (r < MAXR) {
        int buddy_idx = block_idx ^ 1;
        int blocks_at_r = total_pages >> (r - 1);
        if (buddy_idx >= blocks_at_r) break;
        if (free_map[r][buddy_idx]) {
            /* remove buddy */
            free_map[r][buddy_idx] = 0;
            if (free_count[r] > 0) free_count[r]--;
            /* move up */
            block_idx = block_idx / 2;
            r = r + 1;
        } else {
            break;
        }
    }

    free_map[r][block_idx] = 1;
    free_count[r]++;

    return OK;
}

int query_ranks(void *p){
    if (p == NULL) return -EINVAL;
    if (base_ptr == NULL) return -EINVAL;
    long diff = (char *)p - base_ptr;
    if (diff < 0 || (size_t)diff >= (size_t)total_pages * PAGE_SIZE) return -EINVAL;
    if (diff % PAGE_SIZE != 0) return -EINVAL;
    int idx = (int)(diff / PAGE_SIZE);
    if (idx < 0 || idx >= total_pages) return -EINVAL;

    int ar = alloc_rank_map[idx];
    if (ar >= 1 && ar <= max_rank) return ar;

    /* find maximum rank of free block containing this page */
    for (int r = max_rank; r >= 1; --r) {
        int block_pages = 1 << (r - 1);
        int block_idx = idx / block_pages;
        int blocks = total_pages >> (r - 1);
        if (block_idx < blocks && free_map[r][block_idx]) return r;
    }
    return -EINVAL; /* if neither allocated nor found in free blocks, invalid */
}

int query_page_counts(int rank){
    if (rank < 1 || rank > max_rank) return -EINVAL;
    return free_count[rank];
}
