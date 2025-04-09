#include <stdio.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/errno.h>
#else
#include <errno.h>
#endif

#include "lab.h"

#define handle_error_and_die(msg) \
    do                            \
    {                             \
        perror(msg);              \
        raise(SIGKILL);          \
    } while (0)

size_t btok(size_t bytes)
{
    size_t kval = 0;
    size_t bytes_ceil = 1U;

    // Gets the power of 2 ceiling of bytes while increasing kval
    while (bytes_ceil < bytes)
    {
        // Bit shift for bytes_ciel *= 2
        bytes_ceil <<= 1;
        kval++;
    }
    return kval;
}

struct avail *buddy_calc(struct buddy_pool *pool, struct avail *buddy)
{
    uintptr_t base = (uintptr_t)pool->base;
    uintptr_t offset = (uintptr_t)buddy - base;
    // Flip the k-th bit to find the buddy
    uintptr_t buddy_offset = offset ^ (1ULL << buddy->kval);
    return (struct avail*)(base + buddy_offset);
}

void *buddy_malloc(struct buddy_pool *pool, size_t size)
{
    if (!pool || size == 0) return NULL;

    // Add room for an availability block and get the k value
    size_t required_size = size + sizeof(struct avail);
    size_t kval = btok(required_size);
    if (kval < SMALLEST_K) kval = SMALLEST_K;

    // Search for available block
    size_t block_k = kval;
    while (block_k <= pool->kval_m && pool->avail[block_k].next == &pool->avail[block_k]) block_k++;

    // No memory block found, set no memory error
    if (block_k > pool->kval_m)
    {
        errno = ENOMEM;
        return NULL;
    }

    // Remove block from availability list
    struct avail* block = pool->avail[block_k].next;
    block->prev->next = block->next;
    block->next->prev = block->prev;

    // Split until block is correct size
    while (block_k > kval)
    {
        block_k--;
        block->kval = block_k;

        // Create buddy
        struct avail* buddy = buddy_calc(pool, block);
        buddy->tag = BLOCK_AVAIL;
        buddy->kval = block_k;
        buddy->next = pool->avail[block_k].next;
        buddy->prev = &pool->avail[block_k];

        // Add buddy to available pool
        pool->avail[block_k].next->prev = buddy;
        pool->avail[block_k].next = buddy;
    }

    // Reserve block
    block->tag = BLOCK_RESERVED;

    return (void *)(block + 1);
}

void buddy_free(struct buddy_pool *pool, void *ptr)
{
    if (ptr == NULL) return;

    // Find availibility block and set it to available
    struct avail* block = ((struct avail*)ptr) - 1;
    block->tag = BLOCK_AVAIL;

    // Combine buddies
    size_t kval = block->kval;
    while (kval < pool->kval_m)
    {
        // Find buddy
        struct avail* buddy = buddy_calc(pool, block);

        // No available buddy found
        if (buddy->tag != BLOCK_AVAIL || buddy->kval != kval) break;

        // Remove buddy from list
        buddy->prev->next = buddy->next;
        buddy->next->prev = buddy->prev;

        // Keep lower address of buddies
        if (buddy < block) block = buddy;

        kval++;
        block->kval = kval;
    }

    // Add block to the available list
    block->tag = BLOCK_AVAIL;
    block->next = pool->avail[kval].next;
    block->prev = &pool->avail[kval];
    pool->avail[kval].next->prev = block;
    pool->avail[kval].next = block;
}

// Unimplemented method
// void *buddy_realloc(struct buddy_pool *pool, void *ptr, size_t size) {}

void buddy_init(struct buddy_pool *pool, size_t size)
{
    size_t kval = 0;
    if (size == 0)
        kval = DEFAULT_K;
    else
        kval = btok(size);

    if (kval < MIN_K)
        kval = MIN_K;
    if (kval > MAX_K)
        kval = MAX_K - 1;

    //make sure pool struct is cleared out
    memset(pool,0,sizeof(struct buddy_pool));
    pool->kval_m = kval;
    pool->numbytes = (UINT64_C(1) << pool->kval_m);
    //Memory map a block of raw memory to manage
    pool->base = mmap(
        NULL,                               /*addr to map to*/
        pool->numbytes,                     /*length*/
        PROT_READ | PROT_WRITE,             /*prot*/
        MAP_PRIVATE | MAP_ANONYMOUS,        /*flags*/
        -1,                                 /*fd -1 when using MAP_ANONYMOUS*/
        0                                   /* offset 0 when using MAP_ANONYMOUS*/
    );
    if (MAP_FAILED == pool->base)
    {
        handle_error_and_die("buddy_init avail array mmap failed");
    }

    //Set all blocks to empty. We are using circular lists so the first elements just point
    //to an available block. Thus the tag, and kval feild are unused burning a small bit of
    //memory but making the code more readable. We mark these blocks as UNUSED to aid in debugging.
    for (size_t i = 0; i <= kval; i++)
    {
        pool->avail[i].next = pool->avail[i].prev = &pool->avail[i];
        pool->avail[i].kval = i;
        pool->avail[i].tag = BLOCK_UNUSED;
    }

    //Add in the first block
    pool->avail[kval].next = pool->avail[kval].prev = (struct avail *)pool->base;
    struct avail *m = pool->avail[kval].next;
    m->tag = BLOCK_AVAIL;
    m->kval = kval;
    m->next = m->prev = &pool->avail[kval];
}

void buddy_destroy(struct buddy_pool *pool)
{
    int rval = munmap(pool->base, pool->numbytes);
    if (-1 == rval)
    {
        handle_error_and_die("buddy_destroy avail array");
    }
    //Zero out the array so it can be reused it needed
    memset(pool,0,sizeof(struct buddy_pool));
}

#define UNUSED(x) (void)x

// /**
//  * This function can be useful to visualize the bits in a block. This can
//  * help when figuring out the buddy_calc function!
//  */
// static void printb(unsigned long int b)
// {
//      size_t bits = sizeof(b) * 8;
//      unsigned long int curr = UINT64_C(1) << (bits - 1);
//      for (size_t i = 0; i < bits; i++)
//      {
//           if (b & curr)
//           {
//                printf("1");
//           }
//           else
//           {
//                printf("0");
//           }
//           curr >>= 1L;
//      }
// }
