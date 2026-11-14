/*
 * USync server shared state
 * WineForge-Internal: usync-v6.
 *
 * Copyright (C) 2026 Radim Vesely
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "config.h"

#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"

#include "request.h"
#include "usync.h"

int do_usync(void)
{
    static int enabled = -1;

    if (enabled == -1) enabled = getenv( "WINEUSYNC" ) && atoi( getenv( "WINEUSYNC" ) );
    return enabled;
}

#ifdef __APPLE__

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <AvailabilityMacros.h>
#ifdef MAC_OS_VERSION_14_4
# include <os/os_sync_wait_on_address.h>
#endif

/* One fixed backing object on macOS, with pages mapped only as needed.
 * Exhaustion falls back to the server until a retired slot becomes reusable. */
#define USYNC_SHM_SIZE (16 * 1024 * 1024)
/* monotonic_counter() uses 100ns ticks. */
#define USYNC_RETRY_MIN (100 * 10000)
#define USYNC_RETRY_MAX (5000 * 10000)

#define UL_COMPARE_AND_WAIT_SHARED 3
#define ULF_WAKE_ALL               0x00000100
#define USYNC_TYPE_MASK          0x003f
#define USYNC_EVENT_DIRECT_PROTOCOL 0x0040
#define USYNC_DIRECT_WAITING       1u
#define USYNC_DIRECT_COMPLETED        2u
#define USYNC_DIRECT_MAX_GENERATION (INT_MAX >> 2)
#define USYNC_DIRECT_MAX_SLOT      (USHRT_MAX >> 1)
#define USYNC_EVENT_PROTOCOL     0x0080
#define USYNC_MUTEX_PROTOCOL     0x0100
#define USYNC_MUTEX_UNWAITED     0x0200
#define USYNC_SEMAPHORE_UNWAITED  0x0400
#define USYNC_SEMAPHORE_PROTOCOL  0x0800
#define USYNC_EVENT_UNWAITED     0x1000
#define USYNC_STATE_LOCKED       0x2000
#define USYNC_MULTI_WAITERS      0x4000
#define USYNC_SERVER_WAITERS     0x8000

extern int __ulock_wake( uint32_t operation, void *addr, uint64_t wake_value );

struct usync_state
{
    int            low;
    int            high;
    unsigned int   generation;
    unsigned short type;
    unsigned short refcount; /* owner bit 0, direct Event slot in bits 1..15 */
};
C_ASSERT( sizeof(struct usync_state) == 16 );

struct usync
{
    unsigned int shm_idx;
    unsigned int server_waiters;
};

struct usync_waiter
{
    struct thread *thread;
    unsigned int slot_idx;
    unsigned int count;
    unsigned int *shm_idxs;
    struct usync_waiter *next;
};

static void **usync_pages;
static unsigned int usync_pages_count;
static unsigned int next_shm_idx = 1;
static unsigned int free_shm_idx;
static unsigned long long recycled_states, retired_states;
static int shared_fd = -1;
static long page_size;
static struct usync_waiter *usync_waiters;
static unsigned int *usync_multi_waiter_counts;
static unsigned int usync_multi_waiter_counts_count;

enum allocation_error
{
    ALLOC_PAGE_SIZE, ALLOC_OPEN, ALLOC_UNLINK, ALLOC_SIZE, ALLOC_CLOEXEC,
    ALLOC_CACHE, ALLOC_MAP, ALLOC_OBJECT, ALLOC_CAPACITY, ALLOC_ERROR_COUNT
};

static const char * const allocation_error_names[] =
{
    "page-size", "shm_open", "shm_unlink", "ftruncate", "fcntl",
    "page-cache", "mmap", "object", "capacity"
};
static struct
{
    unsigned long long count;
    int first_errno, last_errno;
} allocation_errors[ALLOC_ERROR_COUNT];
static unsigned long long allocated_states, allocation_fallbacks, allocation_suppressed;
static timeout_t allocation_retry_after;
static timeout_t allocation_retry_delay = USYNC_RETRY_MIN;
static int allocation_disabled, allocation_last_errno, allocation_stats_registered;
static enum allocation_error allocation_last_op;

static void report_allocation_stats( const char *reason )
{
    unsigned int i;

    fprintf( stderr, "wineserver: USync allocation %s: states=%llu fallbacks=%llu suppressed=%llu disabled=%d\n",
             reason, allocated_states, allocation_fallbacks, allocation_suppressed, allocation_disabled );
    fprintf( stderr, "wineserver: USync slots: high-water=%u reused=%llu retired=%llu\n",
             next_shm_idx - 1, recycled_states, retired_states );
    for (i = 0; i < ALLOC_ERROR_COUNT; i++)
        if (allocation_errors[i].count)
            fprintf( stderr, "wineserver: USync allocation errors: op=%s count=%llu first_errno=%d last_errno=%d\n",
                     allocation_error_names[i], allocation_errors[i].count,
                     allocation_errors[i].first_errno, allocation_errors[i].last_errno );
}

static void report_allocation_exit(void)
{
    if (allocation_last_errno || debug_level) report_allocation_stats( "shutdown" );
}

static int allocation_allowed(void)
{
    if (!allocation_stats_registered)
    {
        allocation_stats_registered = 1;
        if (atexit( report_allocation_exit ))
            fprintf( stderr, "wineserver: USync could not register allocation summary\n" );
    }
    if (!allocation_disabled &&
        (!allocation_retry_after || monotonic_counter() >= allocation_retry_after)) return 1;
    allocation_suppressed++;
    errno = allocation_last_errno;
    return 0;
}

static void allocation_failed( enum allocation_error op, int error, int permanent )
{
    int report = !allocation_retry_after || !allocation_errors[op].count;

    if (!allocation_errors[op].count++) allocation_errors[op].first_errno = error;
    allocation_errors[op].last_errno = allocation_last_errno = error;
    allocation_disabled = permanent;
    allocation_last_op = op;
    allocation_retry_after = monotonic_counter() + allocation_retry_delay;
    if (report)
        fprintf( stderr, "wineserver: USync allocation failed: op=%s errno=%d (%s), %s\n",
                 allocation_error_names[op], error, strerror( error ),
                 permanent ? (op == ALLOC_CAPACITY ? "new states use server fallback until a slot is released" :
                               "new states disabled until server restart") : "new states use server fallback during retry delay" );
    if (allocation_retry_delay < USYNC_RETRY_MAX)
    {
        allocation_retry_delay *= 2;
        if (allocation_retry_delay > USYNC_RETRY_MAX) allocation_retry_delay = USYNC_RETRY_MAX;
    }
    errno = error;
}

static void allocation_succeeded(void)
{
    allocated_states++;
    if (allocation_retry_after) report_allocation_stats( "recovered" );
    allocation_retry_after = 0;
    allocation_retry_delay = USYNC_RETRY_MIN;
}

static long get_page_size(void)
{
    if (!page_size)
    {
        long size;

        errno = 0;
        size = sysconf( _SC_PAGESIZE );
        if (size <= 0 || size > USYNC_SHM_SIZE ||
            size % sizeof(struct usync_state) || USYNC_SHM_SIZE % size)
        {
            allocation_failed( ALLOC_PAGE_SIZE, size < 0 && errno ? errno : EINVAL, 1 );
            return 0;
        }
        page_size = size;
    }
    return page_size;
}

static void wake_address( int *addr, int wake_all )
{
#ifdef MAC_OS_VERSION_14_4
    if (__builtin_available( macOS 14.4, * ))
    {
        if (wake_all) os_sync_wake_by_address_all( addr, sizeof(*addr), OS_SYNC_WAKE_BY_ADDRESS_SHARED );
        else os_sync_wake_by_address_any( addr, sizeof(*addr), OS_SYNC_WAKE_BY_ADDRESS_SHARED );
        return;
    }
#endif
    __ulock_wake( UL_COMPARE_AND_WAIT_SHARED | (wake_all ? ULF_WAKE_ALL : 0), addr, 0 );
}

static struct usync_state *get_state( unsigned int idx )
{
    long size = get_page_size();
    size_t byte_offset;
    unsigned int entry, offset;

    if (!size || !idx || idx >= USYNC_SHM_SIZE / sizeof(struct usync_state)) return NULL;
    byte_offset = (size_t)idx * sizeof(struct usync_state);
    entry = byte_offset / size;
    offset = byte_offset % size;
    if (entry >= usync_pages_count || !usync_pages[entry]) return NULL;
    return (struct usync_state *)((char *)usync_pages[entry] + offset);
}

static unsigned short get_state_type( struct usync_state *state )
{
    return __atomic_load_n( &state->type, __ATOMIC_SEQ_CST ) & USYNC_TYPE_MASK;
}

static int lock_state( struct usync_state *state, unsigned short expected_type )
{
    unsigned int attempts = 0;
    unsigned short type;

    for (;;)
    {
        type = __atomic_load_n( &state->type, __ATOMIC_SEQ_CST );
        if ((type & USYNC_TYPE_MASK) != expected_type) return 0;
        if (!(type & USYNC_STATE_LOCKED) &&
            __atomic_compare_exchange_n( &state->type, &type, type | USYNC_STATE_LOCKED, FALSE,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ))
            return 1;
        if (!(++attempts % 64)) sched_yield();
    }
}

static void unlock_state( struct usync_state *state )
{
    __atomic_fetch_and( &state->type, ~USYNC_STATE_LOCKED, __ATOMIC_SEQ_CST );
}

int usync_get_shared_fd(void)
{
    char name[64];
    unsigned int attempt;

    if (shared_fd != -1) return shared_fd;
    if (!allocation_allowed() || !get_page_size()) return -1;

    for (attempt = 0; attempt < 16; attempt++)
    {
        snprintf( name, sizeof(name), "/wine-usync-%x-%x", getpid(), arc4random() );
        shared_fd = shm_open( name, O_RDWR | O_CREAT | O_EXCL, 0600 );
        if (shared_fd != -1) break;
        if (errno != EEXIST) break;
    }
    if (shared_fd == -1)
    {
        allocation_failed( ALLOC_OPEN, errno, 0 );
        return -1;
    }
    if (shm_unlink( name ) == -1)
    {
        int error = errno;

        close( shared_fd );
        shared_fd = -1;
        allocation_failed( ALLOC_UNLINK, error, 0 );
        return -1;
    }

    if (ftruncate( shared_fd, USYNC_SHM_SIZE ) == -1)
    {
        int error = errno;

        close( shared_fd );
        shared_fd = -1;
        allocation_failed( ALLOC_SIZE, error, 0 );
        return -1;
    }

    if (fcntl( shared_fd, F_SETFD, FD_CLOEXEC ) == -1)
    {
        int error = errno;

        close( shared_fd );
        shared_fd = -1;
        allocation_failed( ALLOC_CLOEXEC, error, 0 );
        return -1;
    }
    return shared_fd;
}

/* Slot ownership follows the owning Wine object, whose lifetime already covers all
 * valid handles. Exporting an index does not create a client reference. Registered
 * multi-waits additionally pin the slot until unregister or thread cleanup.
 * Shared mappings stay mapped, so waking a retired address remains safe.
 */
static void advance_generation( struct usync_state *state )
{
    unsigned int generation = __atomic_load_n( &state->generation, __ATOMIC_SEQ_CST );

    if (generation != UINT_MAX)
        __atomic_store_n( &state->generation, generation + 1, __ATOMIC_SEQ_CST );
}

static void recycle_state_slot( unsigned int idx )
{
    struct usync_state *state = get_state( idx );

    if (!state || __atomic_load_n( &state->refcount, __ATOMIC_SEQ_CST ) ||
        (idx < usync_multi_waiter_counts_count && usync_multi_waiter_counts[idx])) return;
    assert( get_state_type( state ) == INPROC_SYNC_UNKNOWN );
    assert( state->high != -1 && state->high != -2 );
    if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) == UINT_MAX)
    {
        /* Never let a reused slot alias an old generation after wraparound. */
        state->high = -2;
        retired_states++;
        return;
    }
    state->low = free_shm_idx;
    state->high = -1;
    free_shm_idx = idx;
    if (allocation_disabled && allocation_last_op == ALLOC_CAPACITY)
    {
        allocation_disabled = 0;
        allocation_retry_after = monotonic_counter();
    }
}

static void release_state_slot( unsigned int idx )
{
    struct usync_state *state = get_state( idx );

    assert( state && __atomic_load_n( &state->refcount, __ATOMIC_SEQ_CST ) == 1 );
    __atomic_store_n( &state->type, INPROC_SYNC_UNKNOWN, __ATOMIC_SEQ_CST );
    advance_generation( state );
    __atomic_store_n( &state->refcount, 0, __ATOMIC_SEQ_CST );
    wake_address( &state->low, 1 );
    recycle_state_slot( idx );
}

static struct usync_state *alloc_state_slot( unsigned int *idx )
{
    struct usync_state *state;
    unsigned int entry, offset, generation;
    long size;
    size_t byte_offset;
    void *page;
    int fd;

    if (!allocation_allowed()) goto fallback;
    if (free_shm_idx)
    {
        *idx = free_shm_idx;
        state = get_state( *idx );
        assert( state && !__atomic_load_n( &state->refcount, __ATOMIC_SEQ_CST ) && state->high == -1 );
        free_shm_idx = state->low;
        recycled_states++;
        goto initialize;
    }
    if (!(size = get_page_size())) goto fallback;
    if (next_shm_idx >= USYNC_SHM_SIZE / sizeof(struct usync_state))
    {
        allocation_failed( ALLOC_CAPACITY, ENOSPC, 1 );
        goto fallback;
    }
    byte_offset = (size_t)next_shm_idx * sizeof(struct usync_state);
    entry = byte_offset / size;
    offset = byte_offset % size;

    if (entry >= usync_pages_count)
    {
        unsigned int new_count = usync_pages_count ? usync_pages_count * 2 : 16;
        void **new_pages;

        while (entry >= new_count) new_count *= 2;
        if (new_count > USYNC_SHM_SIZE / size) new_count = USYNC_SHM_SIZE / size;
        if (!(new_pages = realloc( usync_pages, new_count * sizeof(*usync_pages) )))
        {
            allocation_failed( ALLOC_CACHE, errno, 0 );
            goto fallback;
        }
        memset( new_pages + usync_pages_count, 0, (new_count - usync_pages_count) * sizeof(*usync_pages) );
        usync_pages = new_pages;
        usync_pages_count = new_count;
    }

    if (!usync_pages[entry])
    {
        if ((fd = usync_get_shared_fd()) == -1) goto fallback;
        page = mmap( NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)entry * size );
        if (page == MAP_FAILED)
        {
            allocation_failed( ALLOC_MAP, errno, 0 );
            goto fallback;
        }
        usync_pages[entry] = page;
    }

    *idx = next_shm_idx++;
    state = (struct usync_state *)((char *)usync_pages[entry] + offset);
initialize:
    generation = __atomic_load_n( &state->generation, __ATOMIC_SEQ_CST );
    assert( generation != UINT_MAX );
    memset( state, 0, sizeof(*state) );
    __atomic_store_n( &state->generation, generation + 1, __ATOMIC_SEQ_CST );
    allocation_succeeded();
    return state;

fallback:
    allocation_fallbacks++;
    return NULL;
}

struct usync *create_usync( int low, int high, enum inproc_sync_type type )
{
    struct usync_state *state;
    struct usync *sync;

    if (!allocation_allowed())
    {
        allocation_fallbacks++;
        return NULL;
    }
    if (!(sync = mem_alloc( sizeof(*sync) )))
    {
        allocation_failed( ALLOC_OBJECT, ENOMEM, 0 );
        allocation_fallbacks++;
        return NULL;
    }
    if (!(state = alloc_state_slot( &sync->shm_idx )))
    {
        free( sync );
        return NULL;
    }

    state->low = low;
    state->high = high;
    state->type = type | (type == INPROC_SYNC_EVENT ? USYNC_EVENT_UNWAITED | USYNC_EVENT_PROTOCOL | USYNC_EVENT_DIRECT_PROTOCOL :
                         type == INPROC_SYNC_SEMAPHORE ?
                         USYNC_SEMAPHORE_PROTOCOL | USYNC_SEMAPHORE_UNWAITED :
                         type == INPROC_SYNC_MUTEX ? USYNC_MUTEX_PROTOCOL | USYNC_MUTEX_UNWAITED : 0);
    __atomic_store_n( &state->refcount, 1, __ATOMIC_SEQ_CST ); /* server-owned object reference */
    sync->server_waiters = 0;

    return sync;
}

unsigned int usync_get_index( struct usync *sync )
{
    return sync ? sync->shm_idx : 0;
}

static void wake_wait_slot( struct usync_state *slot )
{
    __atomic_store_n( &slot->low, 0, __ATOMIC_SEQ_CST );
    wake_address( &slot->low, 1 );
}

static int ensure_multi_waiter_count( unsigned int shm_idx )
{
    unsigned int new_count;
    unsigned int *new_counts;

    if (shm_idx < usync_multi_waiter_counts_count) return 1;

    new_count = usync_multi_waiter_counts_count ? usync_multi_waiter_counts_count * 2 : 256;
    while (shm_idx >= new_count) new_count *= 2;

    if (!(new_counts = realloc( usync_multi_waiter_counts, new_count * sizeof(*new_counts) ))) return 0;
    memset( new_counts + usync_multi_waiter_counts_count, 0,
            (new_count - usync_multi_waiter_counts_count) * sizeof(*new_counts) );
    usync_multi_waiter_counts = new_counts;
    usync_multi_waiter_counts_count = new_count;
    return 1;
}

static int add_multi_waiter( unsigned int shm_idx )
{
    struct usync_state *state = get_state( shm_idx );

    if (!state) return 0;
    switch (get_state_type( state ))
    {
    case INPROC_SYNC_EVENT:
    case INPROC_SYNC_SEMAPHORE:
    case INPROC_SYNC_MUTEX:
        break;
    default:
        return 0;
    }
    if (!ensure_multi_waiter_count( shm_idx ) || usync_multi_waiter_counts[shm_idx] == UINT_MAX) return 0;

    usync_multi_waiter_counts[shm_idx]++;
    __atomic_fetch_or( &state->type, USYNC_MULTI_WAITERS, __ATOMIC_SEQ_CST );
    return 1;
}

static void remove_multi_waiter( unsigned int shm_idx )
{
    struct usync_state *state = get_state( shm_idx );

    if (!state || shm_idx >= usync_multi_waiter_counts_count || !usync_multi_waiter_counts[shm_idx]) return;

    if (--usync_multi_waiter_counts[shm_idx]) return;
    __atomic_fetch_and( &state->type, ~USYNC_MULTI_WAITERS, __ATOMIC_SEQ_CST );
    if (!__atomic_load_n( &state->refcount, __ATOMIC_SEQ_CST )) recycle_state_slot( shm_idx );
}

static void remove_waiter( unsigned int slot_idx, struct thread *thread )
{
    struct usync_waiter **ptr = &usync_waiters;

    while (*ptr)
    {
        struct usync_waiter *waiter = *ptr;

        if (thread ? waiter->thread == thread : waiter->slot_idx == slot_idx)
        {
            unsigned int i;

            *ptr = waiter->next;
            for (i = 0; i < waiter->count; i++) remove_multi_waiter( waiter->shm_idxs[i] );
            free( waiter->shm_idxs );
            free( waiter );
            continue;
        }
        ptr = &waiter->next;
    }
}

static int usync_state_signaled( unsigned int shm_idx )
{
    struct usync_state *state = get_state( shm_idx );
    int owner;

    if (!state) return 0;

    switch (get_state_type( state ))
    {
    case INPROC_SYNC_EVENT:
        return __atomic_load_n( &state->low, __ATOMIC_SEQ_CST ) != 0;
    case INPROC_SYNC_SEMAPHORE:
        return __atomic_load_n( &state->low, __ATOMIC_SEQ_CST ) > 0;
    case INPROC_SYNC_MUTEX:
        owner = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST );
        return !owner || owner < 0;
    default:
        return 0;
    }
}

static void wake_waiters_for_object( unsigned int shm_idx )
{
    struct usync_waiter *waiter;

    for (waiter = usync_waiters; waiter; waiter = waiter->next)
    {
        struct usync_state *slot;
        unsigned int i;

        for (i = 0; i < waiter->count; i++)
        {
            if (waiter->shm_idxs[i] != shm_idx) continue;
            if ((slot = get_state( waiter->slot_idx ))) wake_wait_slot( slot );
            break;
        }
    }
}

unsigned int usync_alloc_wait_slot(void)
{
    struct usync_state *state;
    unsigned int shm_idx;

    if (!current || current->state == TERMINATED) return 0;
    if (current->usync_wait_slot) return current->usync_wait_slot;
    if (!(state = alloc_state_slot( &shm_idx ))) return 0;

    state->low = 0;
    state->high = 0;
    state->type = INPROC_SYNC_INTERNAL;
    __atomic_store_n( &state->refcount, 1, __ATOMIC_SEQ_CST );
    current->usync_wait_slot = shm_idx;

    return shm_idx;
}

int usync_register_wait( unsigned int slot_idx, const unsigned int *shm_idxs, unsigned int count )
{
    struct usync_waiter *waiter;
    struct usync_state *slot = get_state( slot_idx );
    unsigned int i;

    if (!current || current->state == TERMINATED || current->usync_wait_slot != slot_idx || !slot ||
        get_state_type( slot ) != INPROC_SYNC_INTERNAL || !count || count > 64 ||
        __atomic_load_n( &slot->high, __ATOMIC_SEQ_CST )) return 0;

    remove_waiter( slot_idx, NULL );

    __atomic_store_n( &slot->low, 1, __ATOMIC_SEQ_CST );

    for (i = 0; i < count; i++)
    {
        struct usync_state *state = get_state( shm_idxs[i] );

        if (!state)
        {
            wake_wait_slot( slot );
            return 0;
        }
        switch (get_state_type( state ))
        {
        case INPROC_SYNC_EVENT:
        case INPROC_SYNC_SEMAPHORE:
        case INPROC_SYNC_MUTEX:
            break;
        default:
            wake_wait_slot( slot );
            return 0;
        }
    }

    if (!(waiter = mem_alloc( sizeof(*waiter) )))
    {
        wake_wait_slot( slot );
        return 0;
    }
    if (!(waiter->shm_idxs = mem_alloc( count * sizeof(*waiter->shm_idxs) )))
    {
        free( waiter );
        wake_wait_slot( slot );
        return 0;
    }

    memcpy( waiter->shm_idxs, shm_idxs, count * sizeof(*waiter->shm_idxs) );
    waiter->thread = current;
    waiter->slot_idx = slot_idx;
    waiter->count = count;

    for (i = 0; i < count; i++)
    {
        if (!add_multi_waiter( shm_idxs[i] ))
        {
            while (i) remove_multi_waiter( shm_idxs[--i] );
            free( waiter->shm_idxs );
            free( waiter );
            wake_wait_slot( slot );
            return 0;
        }
    }

    waiter->next = usync_waiters;
    usync_waiters = waiter;

    for (i = 0; i < count; i++)
    {
        if (usync_state_signaled( shm_idxs[i] ))
        {
            wake_wait_slot( slot );
            break;
        }
    }
    return 1;
}

void usync_unregister_wait( unsigned int slot_idx )
{
    struct usync_state *slot;

    if (!current || current->usync_wait_slot != slot_idx) return;
    slot = get_state( slot_idx );
    if (slot && __atomic_load_n( &slot->high, __ATOMIC_SEQ_CST )) return;
    remove_waiter( slot_idx, NULL );
    if ((slot = get_state( slot_idx ))) wake_wait_slot( slot );
}

/* Called with the Event lock held. Slot cleanup takes its lock first, then
 * this Event lock, and cannot recycle the slot until the owner is cleared. */
static struct usync_state *get_direct_event_slot( struct usync_state *state, unsigned int event_idx )
{
    unsigned int idx = __atomic_load_n( &state->refcount, __ATOMIC_SEQ_CST ) >> 1;
    struct usync_state *slot;

    if (!(__atomic_load_n( &state->type, __ATOMIC_SEQ_CST ) & USYNC_EVENT_DIRECT_PROTOCOL) ||
        !idx || !(slot = get_state( idx )) || get_state_type( slot ) != INPROC_SYNC_INTERNAL ||
        __atomic_load_n( &slot->high, __ATOMIC_SEQ_CST ) != (int)event_idx ||
        ((unsigned int)__atomic_load_n( &slot->low, __ATOMIC_SEQ_CST ) >> 2) != slot->generation)
        return NULL;
    return slot;
}

static struct usync_state *signal_direct_event_slot( struct usync_state *state,
                                                       unsigned int event_idx )
{
    struct usync_state *slot = get_direct_event_slot( state, event_idx );
    unsigned int word;

    if (!slot) return NULL;
    word = __atomic_load_n( &slot->low, __ATOMIC_SEQ_CST );
    if (word & USYNC_DIRECT_COMPLETED) return NULL;
    __atomic_store_n( &slot->low, (word & ~USYNC_DIRECT_WAITING) |
                      USYNC_DIRECT_COMPLETED, __ATOMIC_SEQ_CST );
    return slot;
}

static void cancel_direct_event_slot( struct usync_state *state, unsigned int event_idx )
{
    struct usync_state *slot = get_direct_event_slot( state, event_idx );

    if (slot)
    {
        __atomic_store_n( &slot->high, 0, __ATOMIC_SEQ_CST );
        __atomic_store_n( &slot->low, 0, __ATOMIC_SEQ_CST );
        wake_address( &slot->low, 1 );
    }
    __atomic_fetch_and( &state->refcount, 1, __ATOMIC_SEQ_CST );
}

void usync_cleanup_thread( struct thread *thread )
{
    struct usync_state *slot, *state;
    unsigned int event_idx;

    remove_waiter( 0, thread );
    if (!thread->usync_wait_slot) return;
    slot = get_state( thread->usync_wait_slot );
    if (slot && lock_state( slot, INPROC_SYNC_INTERNAL ))
    {
        event_idx = __atomic_load_n( &slot->high, __ATOMIC_SEQ_CST );
        if (event_idx && (state = get_state( event_idx )) && lock_state( state, INPROC_SYNC_EVENT ))
        {
            if ((__atomic_load_n( &state->refcount, __ATOMIC_SEQ_CST ) >> 1) == thread->usync_wait_slot)
                cancel_direct_event_slot( state, event_idx );
            unlock_state( state );
        }
        /* Invalidating the type while holding the slot lock prevents a late
         * client registration from publishing after termination cleanup. */
        release_state_slot( thread->usync_wait_slot );
    }
    thread->usync_wait_slot = 0;
}

int usync_wake_waiters( unsigned int shm_idx )
{
    if (!get_state( shm_idx )) return 0;
    wake_waiters_for_object( shm_idx );
    return 1;
}

void usync_destroy( struct usync *sync )
{
    struct usync_state *state;

    if (!sync) return;
    state = get_state( sync->shm_idx );
    if (state && get_state_type( state ) == INPROC_SYNC_EVENT && lock_state( state, INPROC_SYNC_EVENT ))
        cancel_direct_event_slot( state, sync->shm_idx );
    release_state_slot( sync->shm_idx );
    free( sync );
}

void usync_add_server_waiter( struct usync *sync )
{
    struct usync_state *state;

    if (!sync || !(state = get_state( sync->shm_idx ))) return;
    sync->server_waiters++;
    __atomic_fetch_or( &state->type, USYNC_SERVER_WAITERS, __ATOMIC_SEQ_CST );
}

void usync_remove_server_waiter( struct usync *sync )
{
    struct usync_state *state;

    if (!sync || !sync->server_waiters || !(state = get_state( sync->shm_idx ))) return;
    if (--sync->server_waiters) return;
    __atomic_fetch_and( &state->type, (unsigned short)~USYNC_SERVER_WAITERS, __ATOMIC_SEQ_CST );
}

int usync_get_event_state( struct usync *sync, int *manual, int *signaled )
{
    struct usync_state *state;
    unsigned short type;

    if (!sync || !(state = get_state( sync->shm_idx ))) return 0;
    type = get_state_type( state );
    if ((type != INPROC_SYNC_EVENT && type != INPROC_SYNC_INTERNAL) || !lock_state( state, type )) return 0;

    if (manual) *manual = state->high;
    if (signaled) *signaled = state->low;
    unlock_state( state );
    return 1;
}

int usync_event_signaled( struct usync *sync )
{
    struct usync_state *state;
    unsigned short type;
    int signaled;

    if (!sync || !(state = get_state( sync->shm_idx ))) return 0;
    type = get_state_type( state );
    if ((type != INPROC_SYNC_EVENT && type != INPROC_SYNC_INTERNAL) || !lock_state( state, type )) return 0;
    signaled = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST ) != 0;
    unlock_state( state );
    return signaled;
}

void usync_event_satisfied( struct usync *sync )
{
    struct usync_state *state;
    unsigned short type;

    if (!sync || !(state = get_state( sync->shm_idx ))) return;
    type = get_state_type( state );
    if ((type != INPROC_SYNC_EVENT && type != INPROC_SYNC_INTERNAL) || !lock_state( state, type )) return;
    if (!state->high) __atomic_store_n( &state->low, 0, __ATOMIC_SEQ_CST );
    unlock_state( state );
}

void usync_set_event( struct usync *sync )
{
    struct usync_state *state, *slot = NULL;
    unsigned short type;
    int previous, legacy;

    if (!sync || !(state = get_state( sync->shm_idx ))) return;
    type = get_state_type( state );
    if ((type != INPROC_SYNC_EVENT && type != INPROC_SYNC_INTERNAL) || !lock_state( state, type )) return;
    previous = __atomic_exchange_n( &state->low, 1, __ATOMIC_SEQ_CST );
    legacy = !(state->type & USYNC_EVENT_DIRECT_PROTOCOL);
    if (!previous) slot = signal_direct_event_slot( state, sync->shm_idx );
    if (slot && !state->high) __atomic_store_n( &state->low, 0, __ATOMIC_SEQ_CST );
    unlock_state( state );
    if (slot) wake_address( &slot->low, 0 );
    else if (!previous && legacy) wake_address( &state->low, state->high );
    wake_waiters_for_object( sync->shm_idx );
}

void usync_reset_event( struct usync *sync )
{
    struct usync_state *state;
    unsigned short type;

    if (!sync || !(state = get_state( sync->shm_idx ))) return;
    type = get_state_type( state );
    if ((type != INPROC_SYNC_EVENT && type != INPROC_SYNC_INTERNAL) || !lock_state( state, type )) return;
    __atomic_store_n( &state->low, 0, __ATOMIC_SEQ_CST );
    unlock_state( state );
}

int usync_event_op( struct usync *sync, int signal, int *previous )
{
    struct usync_state *state, *slot = NULL;
    unsigned short type;
    int old, legacy;

    if (!sync || !(state = get_state( sync->shm_idx ))) return 0;
    type = get_state_type( state );
    if ((type != INPROC_SYNC_EVENT && type != INPROC_SYNC_INTERNAL) || !lock_state( state, type )) return 0;
    old = __atomic_exchange_n( &state->low, !!signal, __ATOMIC_SEQ_CST );
    legacy = !(state->type & USYNC_EVENT_DIRECT_PROTOCOL);
    if (signal && !old) slot = signal_direct_event_slot( state, sync->shm_idx );
    if (slot && !state->high) __atomic_store_n( &state->low, 0, __ATOMIC_SEQ_CST );
    unlock_state( state );
    if (previous) *previous = old;
    if (signal)
    {
        if (slot) wake_address( &slot->low, 0 );
        else if (!old && legacy) wake_address( &state->low, state->high );
        wake_waiters_for_object( sync->shm_idx );
    }
    return 1;
}

int usync_event_pulse_begin( struct usync *sync, int *previous )
{
    struct usync_state *state, *slot;
    int manual;

    if (!sync || !(state = get_state( sync->shm_idx )) || !lock_state( state, INPROC_SYNC_EVENT )) return 0;
    *previous = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST );
    /* Keep ordinary userspace consumers out of the transient server pulse.
     * Queue removals cannot clear this temporary server-waiter reference. */
    sync->server_waiters++;
    __atomic_fetch_or( &state->type, USYNC_SERVER_WAITERS, __ATOMIC_SEQ_CST );
    slot = signal_direct_event_slot( state, sync->shm_idx );
    manual = state->high;
    __atomic_store_n( &state->low, manual || !slot, __ATOMIC_SEQ_CST );
    unlock_state( state );
    if (slot) wake_address( &slot->low, 0 );
    return 1;
}

void usync_event_pulse_end( struct usync *sync )
{
    usync_reset_event( sync );
    usync_remove_server_waiter( sync );
}

int usync_semaphore_signaled( struct usync *sync )
{
    struct usync_state *state;
    int signaled;

    if (!sync || !(state = get_state( sync->shm_idx ))) return 0;
    if (!lock_state( state, INPROC_SYNC_SEMAPHORE )) return 0;
    signaled = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST ) > 0;
    unlock_state( state );
    return signaled;
}

void usync_semaphore_satisfied( struct usync *sync )
{
    struct usync_state *state;
    int count;

    if (!sync || !(state = get_state( sync->shm_idx ))) return;
    if (!lock_state( state, INPROC_SYNC_SEMAPHORE )) return;

    do
    {
        count = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST );
        if (count <= 0)
        {
            unlock_state( state );
            return;
        }
    } while (!__atomic_compare_exchange_n( &state->low, &count, count - 1, FALSE,
                                           __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ));
    unlock_state( state );
}

int usync_release_semaphore( struct usync *sync, unsigned int count, unsigned int *prev )
{
    struct usync_state *state;
    int old_count, new_count;

    if (!sync || !(state = get_state( sync->shm_idx ))) return 0;
    if (!lock_state( state, INPROC_SYNC_SEMAPHORE )) return 0;

    do
    {
        old_count = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST );
        if ((unsigned int)old_count + count < (unsigned int)old_count ||
            (unsigned int)old_count + count > (unsigned int)state->high)
        {
            set_error( STATUS_SEMAPHORE_LIMIT_EXCEEDED );
            unlock_state( state );
            return 0;
        }
        new_count = old_count + count;
    } while (!__atomic_compare_exchange_n( &state->low, &old_count, new_count, FALSE,
                                           __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ));

    if (prev) *prev = old_count;
    unlock_state( state );
    wake_address( &state->low, count > 1 );
    return 1;
}

int usync_get_semaphore_state( struct usync *sync, unsigned int *count, unsigned int *max )
{
    struct usync_state *state;

    if (!sync || !(state = get_state( sync->shm_idx ))) return 0;
    if (!lock_state( state, INPROC_SYNC_SEMAPHORE )) return 0;

    if (count) *count = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST );
    if (max) *max = state->high;
    unlock_state( state );
    return 1;
}

int usync_mutex_signaled( struct usync *sync, thread_id_t tid )
{
    struct usync_state *state;
    int owner, signaled;

    if (!sync || !(state = get_state( sync->shm_idx ))) return 0;
    if (!lock_state( state, INPROC_SYNC_MUTEX )) return 0;

    owner = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST );
    signaled = !owner || owner == (int)tid || owner < 0;
    unlock_state( state );
    return signaled;
}

int usync_mutex_satisfied( struct usync *sync, thread_id_t tid, int *abandoned )
{
    struct usync_state *state;
    int owner, count;

    if (!sync || !(state = get_state( sync->shm_idx ))) return 0;
    if (!lock_state( state, INPROC_SYNC_MUTEX )) return 0;

    owner = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST );
    count = __atomic_load_n( &state->high, __ATOMIC_SEQ_CST );

    if (owner == (int)tid)
    {
        __atomic_store_n( &state->high, count + 1, __ATOMIC_SEQ_CST );
        if (abandoned) *abandoned = 0;
        unlock_state( state );
        return 1;
    }

    if (!owner || owner < 0)
    {
        __atomic_store_n( &state->low, (int)tid, __ATOMIC_SEQ_CST );
        __atomic_store_n( &state->high, 1, __ATOMIC_SEQ_CST );
        if (owner < 0) advance_generation( state );
        if (abandoned) *abandoned = owner < 0;
        unlock_state( state );
        return 1;
    }

    unlock_state( state );
    return 0;
}

int usync_release_mutex( struct usync *sync, thread_id_t tid, unsigned int *prev )
{
    struct usync_state *state;
    int count;

    if (!sync || !(state = get_state( sync->shm_idx ))) return 0;
    if (!lock_state( state, INPROC_SYNC_MUTEX )) return 0;
    if (__atomic_load_n( &state->low, __ATOMIC_SEQ_CST ) != (int)tid ||
        !(count = __atomic_load_n( &state->high, __ATOMIC_SEQ_CST )))
    {
        set_error( STATUS_MUTANT_NOT_OWNED );
        unlock_state( state );
        return 0;
    }

    if (prev) *prev = count;
    if (count > 1)
    {
        __atomic_store_n( &state->high, count - 1, __ATOMIC_SEQ_CST );
        unlock_state( state );
        return 1;
    }

    __atomic_store_n( &state->high, 0, __ATOMIC_SEQ_CST );
    __atomic_store_n( &state->low, 0, __ATOMIC_SEQ_CST );
    unlock_state( state );
    wake_address( &state->low, 0 );
    return 1;
}

int usync_get_mutex_state( struct usync *sync, thread_id_t tid, unsigned int *count,
                             int *owned, int *abandoned )
{
    struct usync_state *state;
    int owner;

    if (!sync || !(state = get_state( sync->shm_idx ))) return 0;
    if (!lock_state( state, INPROC_SYNC_MUTEX )) return 0;

    owner = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST );
    if (count) *count = owner < 0 ? 0 : __atomic_load_n( &state->high, __ATOMIC_SEQ_CST );
    if (owned) *owned = owner == (int)tid;
    if (abandoned) *abandoned = owner < 0;
    unlock_state( state );
    return 1;
}

int usync_abandon_mutex( struct usync *sync, thread_id_t tid )
{
    struct usync_state *state;

    if (!sync || !(state = get_state( sync->shm_idx ))) return 0;
    if (!lock_state( state, INPROC_SYNC_MUTEX )) return 0;
    if (state->low != (int)tid || !state->high)
    {
        unlock_state( state );
        return 0;
    }

    state->low = -(int)tid;
    state->high = 0;
    advance_generation( state );
    unlock_state( state );
    wake_address( &state->low, 1 );
    return 1;
}

unsigned int usync_try_wait_all( const unsigned int *entries, unsigned int count, thread_id_t tid )
{
    struct usync_state *states[64];
    unsigned int i, guarded = 0;
    unsigned int status = STATUS_NOT_IMPLEMENTED;
    int abandoned = 0;

    if (!count || count > ARRAY_SIZE(states)) return STATUS_NOT_IMPLEMENTED;

    for (i = 0; i < count; i++)
    {
        unsigned int shm_idx = entries[i * 3];
        unsigned int generation = entries[i * 3 + 1];
        unsigned int type = entries[i * 3 + 2];
        struct usync_state *state = get_state( shm_idx );
        unsigned short state_type;

        if (!state) goto done;
        /* Direct Event completion may proceed with ordinary server waiters.
         * It must never run inside this legacy SERVER_WAITERS atomic guard. */
        if (type == INPROC_SYNC_EVENT && (state->type & USYNC_EVENT_DIRECT_PROTOCOL)) goto done;
        state_type = __atomic_load_n( &state->type, __ATOMIC_SEQ_CST );
        if ((state_type & (USYNC_STATE_LOCKED | USYNC_SERVER_WAITERS)) ||
            (state_type & USYNC_TYPE_MASK) != type ||
            __atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation)
            goto done;

        if (!__atomic_compare_exchange_n( &state->type, &state_type,
                                          state_type | USYNC_SERVER_WAITERS, FALSE,
                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ))
            goto done;
        states[guarded++] = state;
    }

    for (i = 0; i < count; i++)
    {
        unsigned int generation = entries[i * 3 + 1];
        unsigned int type = entries[i * 3 + 2];
        struct usync_state *state = states[i];
        unsigned short state_type;
        int low;

        state_type = __atomic_load_n( &state->type, __ATOMIC_SEQ_CST );
        if ((state_type & USYNC_TYPE_MASK) != type ||
            __atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation)
            goto done;

        low = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST );
        switch (type)
        {
        case INPROC_SYNC_EVENT:
            if (!low)
            {
                status = STATUS_TIMEOUT;
                goto done;
            }
            break;
        case INPROC_SYNC_SEMAPHORE:
            if (low <= 0)
            {
                status = STATUS_TIMEOUT;
                goto done;
            }
            break;
        case INPROC_SYNC_MUTEX:
            if (low && low != (int)tid && low > 0)
            {
                status = STATUS_TIMEOUT;
                goto done;
            }
            if (low == (int)tid && __atomic_load_n( &state->high, __ATOMIC_SEQ_CST ) == INT_MAX)
                goto done;
            if (low < 0) abandoned = 1;
            break;
        default:
            goto done;
        }
    }

    for (i = 0; i < count; i++)
    {
        struct usync_state *state = states[i];
        unsigned int type = entries[i * 3 + 2];

        switch (type)
        {
        case INPROC_SYNC_EVENT:
            if (!state->low) goto done;
            if (!state->high) state->low = 0;
            break;
        case INPROC_SYNC_SEMAPHORE:
            if (state->low <= 0) goto done;
            state->low--;
            break;
        case INPROC_SYNC_MUTEX:
            if (state->low == (int)tid)
            {
                if (state->high == INT_MAX) goto done;
                state->high++;
            }
            else if (!state->low || state->low < 0)
            {
                if (state->low < 0) advance_generation( state );
                state->low = tid;
                state->high = 1;
            }
            else goto done;
            break;
        }
    }

    status = abandoned ? STATUS_ABANDONED_WAIT_0 : STATUS_WAIT_0;

done:
    for (i = 0; i < guarded; i++)
        __atomic_fetch_and( &states[i]->type, (unsigned short)~USYNC_SERVER_WAITERS, __ATOMIC_SEQ_CST );
    return status;
}

static int get_wait_all_unsignaled( const unsigned int *entries, unsigned int count, thread_id_t tid,
                                    unsigned int *shm_idxs, unsigned int *wait_count )
{
    unsigned int i;

    *wait_count = 0;
    for (i = 0; i < count; i++)
    {
        unsigned int shm_idx = entries[i * 3];
        unsigned int generation = entries[i * 3 + 1];
        unsigned int type = entries[i * 3 + 2];
        struct usync_state *state = get_state( shm_idx );
        unsigned short state_type;
        int low;

        if (!state) return 0;
        state_type = __atomic_load_n( &state->type, __ATOMIC_SEQ_CST );
        if ((state_type & (USYNC_STATE_LOCKED | USYNC_SERVER_WAITERS)) ||
            (state_type & USYNC_TYPE_MASK) != type ||
            __atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation)
            return 0;

        low = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST );
        switch (type)
        {
        case INPROC_SYNC_EVENT:
            if (!low) shm_idxs[(*wait_count)++] = shm_idx;
            break;
        case INPROC_SYNC_SEMAPHORE:
            if (low <= 0) shm_idxs[(*wait_count)++] = shm_idx;
            break;
        case INPROC_SYNC_MUTEX:
            if (low && low != (int)tid && low > 0) shm_idxs[(*wait_count)++] = shm_idx;
            break;
        default:
            return 0;
        }
    }
    return 1;
}

#endif /* __APPLE__ */

DECL_HANDLER(get_usync_wait_slot)
{
#ifdef __APPLE__
    if (!do_usync())
    {
        set_error( STATUS_NOT_IMPLEMENTED );
        return;
    }
    if (!(reply->shm_idx = usync_alloc_wait_slot())) set_error( STATUS_NO_MEMORY );
#else
    set_error( STATUS_NOT_IMPLEMENTED );
#endif
}

DECL_HANDLER(register_usync_wait)
{
#ifdef __APPLE__
    data_size_t size = get_req_data_size();

    if (!do_usync() || !size || size % sizeof(unsigned int))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    if (!usync_register_wait( req->slot_idx, get_req_data(), size / sizeof(unsigned int) ))
        set_error( STATUS_NOT_IMPLEMENTED );
#else
    set_error( STATUS_NOT_IMPLEMENTED );
#endif
}

DECL_HANDLER(unregister_usync_wait)
{
#ifdef __APPLE__
    if (!do_usync())
    {
        set_error( STATUS_NOT_IMPLEMENTED );
        return;
    }
    usync_unregister_wait( req->slot_idx );
#else
    set_error( STATUS_NOT_IMPLEMENTED );
#endif
}

DECL_HANDLER(wake_usync_waiters)
{
#ifdef __APPLE__
    if (!do_usync() || !usync_wake_waiters( req->shm_idx ))
    {
        set_error( STATUS_NOT_IMPLEMENTED );
        return;
    }
#else
    set_error( STATUS_NOT_IMPLEMENTED );
#endif
}

DECL_HANDLER(try_wait_all_usync)
{
#ifdef __APPLE__
    const unsigned int *entries = get_req_data();
    data_size_t size = get_req_data_size();
    unsigned int attempts, count, status, wait_count, i;
    unsigned int shm_idxs[64];

    if (!do_usync() || !size || size % (3 * sizeof(unsigned int)))
    {
        set_error( STATUS_NOT_IMPLEMENTED );
        return;
    }

    count = size / (3 * sizeof(unsigned int));
    if (count > ARRAY_SIZE(shm_idxs))
    {
        set_error( STATUS_NOT_IMPLEMENTED );
        return;
    }

    if (req->slot_idx) usync_unregister_wait( req->slot_idx );
    /* Older clients can reach this request after rejecting the Event protocol
     * bit locally. A blocking WaitAll containing any Event needs the standard
     * server queue, even when only another member is currently unsignaled. */
    if (req->slot_idx)
    {
        for (i = 0; i < count; i++)
        {
            if (entries[i * 3 + 2] == INPROC_SYNC_EVENT)
            {
                set_error( STATUS_NOT_IMPLEMENTED );
                return;
            }
        }
    }
    for (attempts = 0; ; attempts++)
    {
        status = usync_try_wait_all( entries, count, req->tid );
        if (status != STATUS_TIMEOUT || !req->slot_idx) break;
        if (!get_wait_all_unsignaled( entries, count, req->tid, shm_idxs, &wait_count ))
        {
            status = STATUS_NOT_IMPLEMENTED;
            break;
        }
        if (wait_count)
        {
            status = usync_register_wait( req->slot_idx, shm_idxs, wait_count )
                     ? STATUS_PENDING : STATUS_NOT_IMPLEMENTED;
            break;
        }
        if (attempts == 3)
        {
            status = STATUS_NOT_IMPLEMENTED;
            break;
        }
    }
    if (status) set_error( status );
#else
    set_error( STATUS_NOT_IMPLEMENTED );
#endif
}
