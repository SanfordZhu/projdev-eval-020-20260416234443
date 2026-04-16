#include "buddy.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAXRANK 16
#define NULL ((void *)0)

// Page size is 4K
#define PAGE_SIZE (4096)

// Global variables
static void *base_addr = NULL;       // Base address of memory pool
static int total_pages = 0;          // Total number of 4K pages
static int max_rank = 0;             // Maximum rank for this pool

// Free lists for each rank
// Each entry stores the starting address of a free block
static void *free_list[MAXRANK + 1];

// Track allocated blocks for query_ranks
// We'll use a simple approach: mark each page's allocation status
static char *alloc_map = NULL;       // Track which pages are allocated (1 = allocated)
static int *rank_map = NULL;         // Track the rank of each allocated block

// Helper function to check if address is valid
static int is_valid_addr(void *p) {
    if (p == NULL) return 0;
    uintptr_t addr = (uintptr_t)p;
    uintptr_t base = (uintptr_t)base_addr;
    if (addr < base || addr >= base + total_pages * PAGE_SIZE) return 0;
    if ((addr - base) % PAGE_SIZE != 0) return 0;
    return 1;
}

// Helper function to get the buddy of a block
static void *get_buddy(void *p, int rank) {
    uintptr_t addr = (uintptr_t)p;
    uintptr_t base = (uintptr_t)base_addr;
    uintptr_t offset = addr - base;
    uintptr_t block_size = PAGE_SIZE * (1 << (rank - 1));
    uintptr_t buddy_offset = offset ^ block_size;
    return (void *)(base + buddy_offset);
}

// Check if a block is in the free list at a given rank
static int is_in_free_list(void *p, int rank) {
    void *curr = free_list[rank];
    while (curr != NULL) {
        if (curr == p) return 1;
        curr = *(void **)curr;  // Next pointer stored at the beginning of the block
    }
    return 0;
}

// Remove a block from the free list at a given rank
static void remove_from_free_list(void *p, int rank) {
    void **curr = &free_list[rank];
    while (*curr != NULL) {
        if (*curr == p) {
            *curr = *(void **)(*curr);  // Skip this block
            return;
        }
        curr = (void **)(*curr);  // Move to next
    }
}

// Add a block to the free list at a given rank
static void add_to_free_list(void *p, int rank) {
    *(void **)p = free_list[rank];
    free_list[rank] = p;
}

int init_page(void *p, int pgcount) {
    if (p == NULL || pgcount <= 0) return -EINVAL;

    base_addr = p;
    total_pages = pgcount;

    // Find the maximum rank that can fit in this pool
    max_rank = 1;
    while ((1 << (max_rank - 1)) < total_pages && max_rank < MAXRANK) {
        max_rank++;
    }

    // Initialize free lists
    for (int i = 1; i <= MAXRANK; i++) {
        free_list[i] = NULL;
    }

    // Initialize allocation tracking
    alloc_map = (char *)calloc(pgcount, sizeof(char));
    rank_map = (int *)calloc(pgcount, sizeof(int));

    // Add the entire pool to the free list at the maximum rank
    add_to_free_list(p, max_rank);

    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAXRANK) return ERR_PTR(-EINVAL);

    // Find a free block of the required rank or larger
    int current_rank = rank;
    while (current_rank <= MAXRANK && free_list[current_rank] == NULL) {
        current_rank++;
    }

    if (current_rank > MAXRANK) {
        return ERR_PTR(-ENOSPC);
    }

    // Get the block from the free list
    void *block = free_list[current_rank];
    remove_from_free_list(block, current_rank);

    // Split the block if necessary
    while (current_rank > rank) {
        current_rank--;
        uintptr_t addr = (uintptr_t)block;
        uintptr_t block_size = PAGE_SIZE * (1 << (current_rank - 1));
        void *buddy = (void *)(addr + block_size);
        add_to_free_list(buddy, current_rank);
    }

    // Mark the block as allocated
    int page_idx = ((uintptr_t)block - (uintptr_t)base_addr) / PAGE_SIZE;
    int block_pages = 1 << (rank - 1);
    for (int i = 0; i < block_pages; i++) {
        alloc_map[page_idx + i] = 1;
        rank_map[page_idx + i] = rank;
    }

    return block;
}

int return_pages(void *p) {
    if (!is_valid_addr(p)) return -EINVAL;

    int page_idx = ((uintptr_t)p - (uintptr_t)base_addr) / PAGE_SIZE;

    if (!alloc_map[page_idx]) return -EINVAL;  // Already free or invalid

    int rank = rank_map[page_idx];

    if (rank < 1 || rank > MAXRANK) return -EINVAL;

    // Mark the block as free
    int block_pages = 1 << (rank - 1);
    for (int i = 0; i < block_pages; i++) {
        alloc_map[page_idx + i] = 0;
        rank_map[page_idx + i] = 0;
    }

    // Add the block to the free list and try to merge with buddy
    void *block = p;
    int current_rank = rank;

    while (current_rank < MAXRANK) {
        void *buddy = get_buddy(block, current_rank);

        // Check if buddy is free and in the free list
        if (!is_in_free_list(buddy, current_rank)) {
            break;
        }

        // Remove buddy from free list
        remove_from_free_list(buddy, current_rank);

        // Merge: the new block starts at the lower address
        if ((uintptr_t)buddy < (uintptr_t)block) {
            block = buddy;
        }

        current_rank++;
    }

    // Add the merged block to the free list
    add_to_free_list(block, current_rank);

    return OK;
}

int query_ranks(void *p) {
    if (!is_valid_addr(p)) return -EINVAL;

    int page_idx = ((uintptr_t)p - (uintptr_t)base_addr) / PAGE_SIZE;

    if (alloc_map[page_idx]) {
        return rank_map[page_idx];
    }

    // For unallocated pages, return the maximum rank that can fit
    // starting from this page
    int max_possible_rank = 1;
    while (max_possible_rank <= MAXRANK) {
        int pages_needed = 1 << (max_possible_rank - 1);
        if (page_idx + pages_needed > total_pages) {
            break;
        }

        int all_free = 1;
        for (int i = 0; i < pages_needed; i++) {
            if (alloc_map[page_idx + i]) {
                all_free = 0;
                break;
            }
        }

        if (!all_free) {
            break;
        }

        max_possible_rank++;
    }

    return max_possible_rank - 1;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAXRANK) return -EINVAL;

    // Count blocks in the free list at this rank
    int count = 0;
    void *curr = free_list[rank];
    while (curr != NULL) {
        count++;
        curr = *(void **)curr;  // Next pointer stored at the beginning of the block
    }

    return count;
}
