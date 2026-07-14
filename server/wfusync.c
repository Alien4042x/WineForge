/*
 * WineForge userspace synchronization server state
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
#include "wfusync.h"

int do_wfusync(void)
{
    static int enabled = -1;

    if (enabled == -1) enabled = getenv( "WINEWFUSYNC" ) && atoi( getenv( "WINEWFUSYNC" ) );
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

#define UL_COMPARE_AND_WAIT_SHARED 3
#define ULF_WAKE_ALL               0x00000100
#define WFUSYNC_TYPE_MASK          0x1fff
#define WFUSYNC_STATE_LOCKED       0x2000
#define WFUSYNC_MULTI_WAITERS      0x4000
#define WFUSYNC_SERVER_WAITERS     0x8000

extern int __ulock_wake( uint32_t operation, void *addr, uint64_t wake_value );

struct wfusync_state
{
    int            low;
    int            high;
    unsigned int   generation;
    unsigned short type;
    unsigned short refcount;
};

struct wfusync
{
    unsigned int shm_idx;
    unsigned int server_waiters;
};

struct wfusync_waiter
{
    unsigned int slot_idx;
    unsigned int count;
    unsigned int *shm_idxs;
    struct wfusync_waiter *next;
};

static void **wfusync_pages;
static unsigned int wfusync_pages_count;
static unsigned int next_shm_idx = 1;
static unsigned int next_generation = 1;
static int shared_fd = -1;
static long page_size;
static struct wfusync_waiter *wfusync_waiters;
static unsigned int *wfusync_multi_waiter_counts;
static unsigned int wfusync_multi_waiter_counts_count;

static long get_page_size(void)
{
    if (!page_size) page_size = sysconf( _SC_PAGESIZE );
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

static struct wfusync_state *get_state( unsigned int idx )
{
    unsigned int entry = (idx * sizeof(struct wfusync_state)) / get_page_size();
    unsigned int offset = (idx * sizeof(struct wfusync_state)) % get_page_size();

    if (entry >= wfusync_pages_count || !wfusync_pages[entry]) return NULL;
    return (struct wfusync_state *)((char *)wfusync_pages[entry] + offset);
}

static unsigned short get_state_type( struct wfusync_state *state )
{
    return __atomic_load_n( &state->type, __ATOMIC_SEQ_CST ) & WFUSYNC_TYPE_MASK;
}

static int lock_state( struct wfusync_state *state, unsigned short expected_type )
{
    unsigned int attempts = 0;
    unsigned short type;

    for (;;)
    {
        type = __atomic_load_n( &state->type, __ATOMIC_SEQ_CST );
        if ((type & WFUSYNC_TYPE_MASK) != expected_type) return 0;
        if (!(type & WFUSYNC_STATE_LOCKED) &&
            __atomic_compare_exchange_n( &state->type, &type, type | WFUSYNC_STATE_LOCKED, FALSE,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ))
            return 1;
        if (!(++attempts % 64)) sched_yield();
    }
}

static void unlock_state( struct wfusync_state *state )
{
    __atomic_fetch_and( &state->type, ~WFUSYNC_STATE_LOCKED, __ATOMIC_SEQ_CST );
}

int wfusync_get_shared_fd(void)
{
    char name[64];
    unsigned int attempt;

    if (shared_fd != -1) return shared_fd;

    for (attempt = 0; attempt < 16; attempt++)
    {
        snprintf( name, sizeof(name), "/wine-wfusync-%x-%x", getpid(), arc4random() );
        shared_fd = shm_open( name, O_RDWR | O_CREAT | O_EXCL, 0600 );
        if (shared_fd != -1) break;
        if (errno != EEXIST) return -1;
    }
    if (shared_fd == -1) return -1;
    if (shm_unlink( name ) == -1)
    {
        close( shared_fd );
        shared_fd = -1;
        return -1;
    }

    fcntl( shared_fd, F_SETFD, FD_CLOEXEC );
    return shared_fd;
}

static int ensure_shared_file_size( unsigned int pages )
{
    off_t size = (off_t)pages * get_page_size();
    int fd = wfusync_get_shared_fd();

    if (fd == -1) return 0;
    return ftruncate( fd, size ) != -1;
}

static struct wfusync_state *alloc_state_slot( unsigned int *idx )
{
    struct wfusync_state *state;
    unsigned int entry, offset;
    void *page;

    *idx = next_shm_idx++;
    entry = (*idx * sizeof(struct wfusync_state)) / get_page_size();
    offset = (*idx * sizeof(struct wfusync_state)) % get_page_size();

    if (entry >= wfusync_pages_count)
    {
        unsigned int new_count = wfusync_pages_count ? wfusync_pages_count * 2 : 16;
        void **new_pages;

        while (entry >= new_count) new_count *= 2;
        if (!(new_pages = realloc( wfusync_pages, new_count * sizeof(*wfusync_pages) ))) return NULL;
        memset( new_pages + wfusync_pages_count, 0, (new_count - wfusync_pages_count) * sizeof(*wfusync_pages) );
        wfusync_pages = new_pages;
        wfusync_pages_count = new_count;
    }

    if (!wfusync_pages[entry])
    {
        if (!ensure_shared_file_size( entry + 1 )) return NULL;
        page = mmap( NULL, get_page_size(), PROT_READ | PROT_WRITE, MAP_SHARED,
                     wfusync_get_shared_fd(), (off_t)entry * get_page_size() );
        if (page == MAP_FAILED) return NULL;
        wfusync_pages[entry] = page;
    }

    state = (struct wfusync_state *)((char *)wfusync_pages[entry] + offset);
    memset( state, 0, sizeof(*state) );
    return state;
}

struct wfusync *create_wfusync( int low, int high, enum inproc_sync_type type )
{
    struct wfusync_state *state;
    struct wfusync *sync;

    if (!(sync = mem_alloc( sizeof(*sync) ))) return NULL;
    if (!(state = alloc_state_slot( &sync->shm_idx )))
    {
        free( sync );
        return NULL;
    }

    state->low = low;
    state->high = high;
    state->generation = next_generation++;
    state->type = type;
    state->refcount = 1; /* server-owned object reference */
    sync->server_waiters = 0;

    return sync;
}

unsigned int wfusync_get_index( struct wfusync *sync )
{
    return sync ? sync->shm_idx : 0;
}

static void wake_wait_slot( struct wfusync_state *slot )
{
    __atomic_store_n( &slot->low, 0, __ATOMIC_SEQ_CST );
    wake_address( &slot->low, 1 );
}

static int ensure_multi_waiter_count( unsigned int shm_idx )
{
    unsigned int new_count;
    unsigned int *new_counts;

    if (shm_idx < wfusync_multi_waiter_counts_count) return 1;

    new_count = wfusync_multi_waiter_counts_count ? wfusync_multi_waiter_counts_count * 2 : 256;
    while (shm_idx >= new_count) new_count *= 2;

    if (!(new_counts = realloc( wfusync_multi_waiter_counts, new_count * sizeof(*new_counts) ))) return 0;
    memset( new_counts + wfusync_multi_waiter_counts_count, 0,
            (new_count - wfusync_multi_waiter_counts_count) * sizeof(*new_counts) );
    wfusync_multi_waiter_counts = new_counts;
    wfusync_multi_waiter_counts_count = new_count;
    return 1;
}

static int add_multi_waiter( unsigned int shm_idx )
{
    struct wfusync_state *state = get_state( shm_idx );

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
    if (!ensure_multi_waiter_count( shm_idx )) return 0;

    wfusync_multi_waiter_counts[shm_idx]++;
    __atomic_fetch_or( &state->type, WFUSYNC_MULTI_WAITERS, __ATOMIC_SEQ_CST );
    return 1;
}

static void remove_multi_waiter( unsigned int shm_idx )
{
    struct wfusync_state *state = get_state( shm_idx );

    if (!state || shm_idx >= wfusync_multi_waiter_counts_count || !wfusync_multi_waiter_counts[shm_idx]) return;

    if (--wfusync_multi_waiter_counts[shm_idx]) return;
    __atomic_fetch_and( &state->type, ~WFUSYNC_MULTI_WAITERS, __ATOMIC_SEQ_CST );
}

static void remove_waiter( unsigned int slot_idx )
{
    struct wfusync_waiter **ptr = &wfusync_waiters;

    while (*ptr)
    {
        struct wfusync_waiter *waiter = *ptr;

        if (waiter->slot_idx == slot_idx)
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

static int wfusync_state_signaled( unsigned int shm_idx )
{
    struct wfusync_state *state = get_state( shm_idx );
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
    struct wfusync_waiter *waiter;

    for (waiter = wfusync_waiters; waiter; waiter = waiter->next)
    {
        struct wfusync_state *slot;
        unsigned int i;

        for (i = 0; i < waiter->count; i++)
        {
            if (waiter->shm_idxs[i] != shm_idx) continue;
            if ((slot = get_state( waiter->slot_idx ))) wake_wait_slot( slot );
            break;
        }
    }
}

unsigned int wfusync_alloc_wait_slot(void)
{
    struct wfusync_state *state;
    unsigned int shm_idx;

    if (!(state = alloc_state_slot( &shm_idx ))) return 0;

    state->low = 0;
    state->high = 0;
    state->generation = next_generation++;
    state->type = INPROC_SYNC_INTERNAL;
    state->refcount = 1;

    return shm_idx;
}

int wfusync_register_wait( unsigned int slot_idx, const unsigned int *shm_idxs, unsigned int count )
{
    struct wfusync_waiter *waiter;
    struct wfusync_state *slot = get_state( slot_idx );
    unsigned int i;

    if (!slot || get_state_type( slot ) != INPROC_SYNC_INTERNAL || !count || count > 64) return 0;

    remove_waiter( slot_idx );

    __atomic_store_n( &slot->low, 1, __ATOMIC_SEQ_CST );
    slot->generation = next_generation++;

    for (i = 0; i < count; i++)
    {
        struct wfusync_state *state = get_state( shm_idxs[i] );

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

    waiter->next = wfusync_waiters;
    wfusync_waiters = waiter;

    for (i = 0; i < count; i++)
    {
        if (wfusync_state_signaled( shm_idxs[i] ))
        {
            wake_wait_slot( slot );
            break;
        }
    }
    return 1;
}

void wfusync_unregister_wait( unsigned int slot_idx )
{
    struct wfusync_state *slot;

    remove_waiter( slot_idx );
    if ((slot = get_state( slot_idx ))) wake_wait_slot( slot );
}

int wfusync_wake_waiters( unsigned int shm_idx )
{
    if (!get_state( shm_idx )) return 0;
    wake_waiters_for_object( shm_idx );
    return 1;
}

void wfusync_grab_object( struct wfusync *sync )
{
    struct wfusync_state *state;

    if (!sync || !(state = get_state( sync->shm_idx ))) return;
    assert( state->refcount );
    state->refcount++;
}

void wfusync_destroy( struct wfusync *sync )
{
    struct wfusync_state *state;

    if (!sync) return;
    if ((state = get_state( sync->shm_idx )))
    {
        assert( state->refcount );
        __atomic_store_n( &state->type, INPROC_SYNC_UNKNOWN, __ATOMIC_SEQ_CST );
        state->generation = next_generation++;
        state->refcount--;
    }
    free( sync );
}

void wfusync_add_server_waiter( struct wfusync *sync )
{
    struct wfusync_state *state;

    if (!sync || !(state = get_state( sync->shm_idx ))) return;
    sync->server_waiters++;
    __atomic_fetch_or( &state->type, WFUSYNC_SERVER_WAITERS, __ATOMIC_SEQ_CST );
}

void wfusync_remove_server_waiter( struct wfusync *sync )
{
    struct wfusync_state *state;

    if (!sync || !sync->server_waiters || !(state = get_state( sync->shm_idx ))) return;
    if (--sync->server_waiters) return;
    __atomic_fetch_and( &state->type, (unsigned short)~WFUSYNC_SERVER_WAITERS, __ATOMIC_SEQ_CST );
}

int wfusync_get_event_state( struct wfusync *sync, int *manual, int *signaled )
{
    struct wfusync_state *state;
    unsigned short type;

    if (!sync || !(state = get_state( sync->shm_idx ))) return 0;
    type = get_state_type( state );
    if ((type != INPROC_SYNC_EVENT && type != INPROC_SYNC_INTERNAL) || !lock_state( state, type )) return 0;

    if (manual) *manual = state->high;
    if (signaled) *signaled = state->low;
    unlock_state( state );
    return 1;
}

int wfusync_event_signaled( struct wfusync *sync )
{
    struct wfusync_state *state;
    unsigned short type;
    int signaled;

    if (!sync || !(state = get_state( sync->shm_idx ))) return 0;
    type = get_state_type( state );
    if ((type != INPROC_SYNC_EVENT && type != INPROC_SYNC_INTERNAL) || !lock_state( state, type )) return 0;
    signaled = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST ) != 0;
    unlock_state( state );
    return signaled;
}

void wfusync_event_satisfied( struct wfusync *sync )
{
    struct wfusync_state *state;
    unsigned short type;

    if (!sync || !(state = get_state( sync->shm_idx ))) return;
    type = get_state_type( state );
    if ((type != INPROC_SYNC_EVENT && type != INPROC_SYNC_INTERNAL) || !lock_state( state, type )) return;
    if (!state->high) __atomic_store_n( &state->low, 0, __ATOMIC_SEQ_CST );
    unlock_state( state );
}

void wfusync_set_event( struct wfusync *sync )
{
    struct wfusync_state *state;
    unsigned short type;
    int previous;

    if (!sync || !(state = get_state( sync->shm_idx ))) return;
    type = get_state_type( state );
    if ((type != INPROC_SYNC_EVENT && type != INPROC_SYNC_INTERNAL) || !lock_state( state, type )) return;
    previous = __atomic_exchange_n( &state->low, 1, __ATOMIC_SEQ_CST );
    unlock_state( state );
    if (!previous) wake_address( &state->low, state->high );
    wake_waiters_for_object( sync->shm_idx );
}

void wfusync_reset_event( struct wfusync *sync )
{
    struct wfusync_state *state;
    unsigned short type;

    if (!sync || !(state = get_state( sync->shm_idx ))) return;
    type = get_state_type( state );
    if ((type != INPROC_SYNC_EVENT && type != INPROC_SYNC_INTERNAL) || !lock_state( state, type )) return;
    __atomic_store_n( &state->low, 0, __ATOMIC_SEQ_CST );
    unlock_state( state );
}

int wfusync_semaphore_signaled( struct wfusync *sync )
{
    struct wfusync_state *state;
    int signaled;

    if (!sync || !(state = get_state( sync->shm_idx ))) return 0;
    if (!lock_state( state, INPROC_SYNC_SEMAPHORE )) return 0;
    signaled = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST ) > 0;
    unlock_state( state );
    return signaled;
}

void wfusync_semaphore_satisfied( struct wfusync *sync )
{
    struct wfusync_state *state;
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

int wfusync_release_semaphore( struct wfusync *sync, unsigned int count, unsigned int *prev )
{
    struct wfusync_state *state;
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
    if (!old_count) wake_address( &state->low, count > 1 );
    return 1;
}

int wfusync_get_semaphore_state( struct wfusync *sync, unsigned int *count, unsigned int *max )
{
    struct wfusync_state *state;

    if (!sync || !(state = get_state( sync->shm_idx ))) return 0;
    if (!lock_state( state, INPROC_SYNC_SEMAPHORE )) return 0;

    if (count) *count = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST );
    if (max) *max = state->high;
    unlock_state( state );
    return 1;
}

int wfusync_mutex_signaled( struct wfusync *sync, thread_id_t tid )
{
    struct wfusync_state *state;
    int owner, signaled;

    if (!sync || !(state = get_state( sync->shm_idx ))) return 0;
    if (!lock_state( state, INPROC_SYNC_MUTEX )) return 0;

    owner = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST );
    signaled = !owner || owner == (int)tid || owner < 0;
    unlock_state( state );
    return signaled;
}

int wfusync_mutex_satisfied( struct wfusync *sync, thread_id_t tid, int *abandoned )
{
    struct wfusync_state *state;
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
        if (owner < 0) state->generation++;
        if (abandoned) *abandoned = owner < 0;
        unlock_state( state );
        return 1;
    }

    unlock_state( state );
    return 0;
}

int wfusync_release_mutex( struct wfusync *sync, thread_id_t tid, unsigned int *prev )
{
    struct wfusync_state *state;
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

int wfusync_get_mutex_state( struct wfusync *sync, thread_id_t tid, unsigned int *count,
                             int *owned, int *abandoned )
{
    struct wfusync_state *state;
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

int wfusync_abandon_mutex( struct wfusync *sync, thread_id_t tid )
{
    struct wfusync_state *state;

    if (!sync || !(state = get_state( sync->shm_idx ))) return 0;
    if (!lock_state( state, INPROC_SYNC_MUTEX )) return 0;
    if (state->low != (int)tid || !state->high)
    {
        unlock_state( state );
        return 0;
    }

    state->low = -(int)tid;
    state->high = 0;
    state->generation++;
    unlock_state( state );
    wake_address( &state->low, 1 );
    return 1;
}

unsigned int wfusync_try_wait_all( const unsigned int *entries, unsigned int count, thread_id_t tid )
{
    struct wfusync_state *states[64];
    unsigned int i, guarded = 0;
    unsigned int status = STATUS_NOT_IMPLEMENTED;
    int abandoned = 0;

    if (!count || count > ARRAY_SIZE(states)) return STATUS_NOT_IMPLEMENTED;

    for (i = 0; i < count; i++)
    {
        unsigned int shm_idx = entries[i * 3];
        unsigned int generation = entries[i * 3 + 1];
        unsigned int type = entries[i * 3 + 2];
        struct wfusync_state *state = get_state( shm_idx );
        unsigned short state_type;

        if (!state) goto done;
        state_type = __atomic_load_n( &state->type, __ATOMIC_SEQ_CST );
        if ((state_type & (WFUSYNC_STATE_LOCKED | WFUSYNC_SERVER_WAITERS)) ||
            (state_type & WFUSYNC_TYPE_MASK) != type ||
            __atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation)
            goto done;

        if (!__atomic_compare_exchange_n( &state->type, &state_type,
                                          state_type | WFUSYNC_SERVER_WAITERS, FALSE,
                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ))
            goto done;
        states[guarded++] = state;
    }

    for (i = 0; i < count; i++)
    {
        unsigned int generation = entries[i * 3 + 1];
        unsigned int type = entries[i * 3 + 2];
        struct wfusync_state *state = states[i];
        unsigned short state_type;
        int low;

        state_type = __atomic_load_n( &state->type, __ATOMIC_SEQ_CST );
        if ((state_type & WFUSYNC_TYPE_MASK) != type ||
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
        struct wfusync_state *state = states[i];
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
                if (state->low < 0) state->generation++;
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
        __atomic_fetch_and( &states[i]->type, (unsigned short)~WFUSYNC_SERVER_WAITERS, __ATOMIC_SEQ_CST );
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
        struct wfusync_state *state = get_state( shm_idx );
        unsigned short state_type;
        int low;

        if (!state) return 0;
        state_type = __atomic_load_n( &state->type, __ATOMIC_SEQ_CST );
        if ((state_type & (WFUSYNC_STATE_LOCKED | WFUSYNC_SERVER_WAITERS)) ||
            (state_type & WFUSYNC_TYPE_MASK) != type ||
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

DECL_HANDLER(get_wfusync_wait_slot)
{
#ifdef __APPLE__
    if (!do_wfusync())
    {
        set_error( STATUS_NOT_IMPLEMENTED );
        return;
    }
    if (!(reply->shm_idx = wfusync_alloc_wait_slot())) set_error( STATUS_NO_MEMORY );
#else
    set_error( STATUS_NOT_IMPLEMENTED );
#endif
}

DECL_HANDLER(register_wfusync_wait)
{
#ifdef __APPLE__
    data_size_t size = get_req_data_size();

    if (!do_wfusync() || !size || size % sizeof(unsigned int))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    if (!wfusync_register_wait( req->slot_idx, get_req_data(), size / sizeof(unsigned int) ))
        set_error( STATUS_NOT_IMPLEMENTED );
#else
    set_error( STATUS_NOT_IMPLEMENTED );
#endif
}

DECL_HANDLER(unregister_wfusync_wait)
{
#ifdef __APPLE__
    if (!do_wfusync())
    {
        set_error( STATUS_NOT_IMPLEMENTED );
        return;
    }
    wfusync_unregister_wait( req->slot_idx );
#else
    set_error( STATUS_NOT_IMPLEMENTED );
#endif
}

DECL_HANDLER(wake_wfusync_waiters)
{
#ifdef __APPLE__
    if (!do_wfusync() || !wfusync_wake_waiters( req->shm_idx ))
    {
        set_error( STATUS_NOT_IMPLEMENTED );
        return;
    }
#else
    set_error( STATUS_NOT_IMPLEMENTED );
#endif
}

DECL_HANDLER(try_wait_all_wfusync)
{
#ifdef __APPLE__
    const unsigned int *entries = get_req_data();
    data_size_t size = get_req_data_size();
    unsigned int attempts, count, status, wait_count;
    unsigned int shm_idxs[64];

    if (!do_wfusync() || !size || size % (3 * sizeof(unsigned int)))
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

    if (req->slot_idx) wfusync_unregister_wait( req->slot_idx );
    for (attempts = 0; ; attempts++)
    {
        status = wfusync_try_wait_all( entries, count, req->tid );
        if (status != STATUS_TIMEOUT || !req->slot_idx) break;
        if (!get_wait_all_unsignaled( entries, count, req->tid, shm_idxs, &wait_count ))
        {
            status = STATUS_NOT_IMPLEMENTED;
            break;
        }
        if (wait_count)
        {
            status = wfusync_register_wait( req->slot_idx, shm_idxs, wait_count )
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
