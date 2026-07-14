/*
 * WineForge userspace synchronization client state
 *
 * Copyright (C) 2026 Radim Vesely
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#ifdef __WINE_PE_BUILD__
#error "wfusync.c is for Unix only"
#endif

#include "config.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __APPLE__
# include <AvailabilityMacros.h>
# include <os/lock.h>
# ifdef MAC_OS_VERSION_14_4
#  include <os/os_sync_wait_on_address.h>
# endif
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/server.h"
#include "wine/unixlib.h"
#include "wine/debug.h"

#include "wfusync.h"

WINE_DEFAULT_DEBUG_CHANNEL(wfusync);

#define WFUSYNC_CACHE_BLOCK_SIZE  (65536 / sizeof(struct wfusync_cache_entry))
#define WFUSYNC_CACHE_ENTRIES     128
#define WFUSYNC_TICKS_PER_SEC     10000000LL
#define WFUSYNC_SPIN_LIMIT        32
#define WFUSYNC_BLOCKING_FALLBACK_NS 10000000ULL

#define WFUSYNC_KIND_UNKNOWN      0
#define WFUSYNC_KIND_EVENT        1
#define WFUSYNC_KIND_MUTEX        2
#define WFUSYNC_KIND_SEMAPHORE    3
#define WFUSYNC_KIND_UNSUPPORTED  4
#define WFUSYNC_TYPE_MASK         0x1fff
#define WFUSYNC_STATE_LOCKED      0x2000
#define WFUSYNC_MULTI_WAITERS     0x4000
#define WFUSYNC_SERVER_WAITERS    0x8000

#ifdef __APPLE__
#define UL_COMPARE_AND_WAIT        1
#define UL_COMPARE_AND_WAIT_SHARED 3
#define ULF_WAKE_ALL               0x00000100

extern int __ulock_wait( uint32_t operation, void *addr, uint64_t value, uint32_t timeout );
extern int __ulock_wake( uint32_t operation, void *addr, uint64_t wake_value );
#endif

struct wfusync_shared_state
{
    int            low;
    int            high;
    unsigned int   generation;
    unsigned short type;
    unsigned short refcount;
};

struct wfusync_cache_entry
{
    HANDLE handle;
    int valid;
    int local_event;
    int local_mutex;
    int local_semaphore;
    int signal_unsafe;
    struct wfusync_fast_state state;
};

static struct wfusync_cache_entry *wfusync_cache[WFUSYNC_CACHE_ENTRIES];
static unsigned char *wfusync_handle_kinds[WFUSYNC_CACHE_ENTRIES];
static unsigned char wfusync_handle_kinds_initial[WFUSYNC_CACHE_BLOCK_SIZE];
static unsigned char *wfusync_signal_unsafe_indexes;
static unsigned int wfusync_signal_unsafe_indexes_count;
static void **wfusync_pages;
static unsigned int wfusync_pages_count;
#ifdef __APPLE__
static os_unfair_lock wfusync_cache_lock = OS_UNFAIR_LOCK_INIT;
#else
static pthread_mutex_t wfusync_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
static int do_wfusync_env_cached = -1;
static int wfusync_shared_fd = -1;
static __thread unsigned int wfusync_wait_slot_idx;
static __thread struct wfusync_shared_state *wfusync_wait_slot;
static long page_size;

static void lock_wfusync_cache(void)
{
#ifdef __APPLE__
    os_unfair_lock_lock( &wfusync_cache_lock );
#else
    pthread_mutex_lock( &wfusync_cache_mutex );
#endif
}

static void unlock_wfusync_cache(void)
{
#ifdef __APPLE__
    os_unfair_lock_unlock( &wfusync_cache_lock );
#else
    pthread_mutex_unlock( &wfusync_cache_mutex );
#endif
}

int do_wfusync(void)
{
    if (do_wfusync_env_cached == -1)
    {
        const char *env = getenv( "WINEWFUSYNC" );
        do_wfusync_env_cached = (env && atoi( env )) ? 1 : 0;
    }

    return do_wfusync_env_cached;
}

static long get_page_size(void)
{
    if (!page_size) page_size = sysconf( _SC_PAGESIZE );
    return page_size;
}

static BOOL wfusync_handle_to_index( HANDLE handle, unsigned int *entry, unsigned int *idx )
{
    unsigned int handle_idx;

    if ((ULONG)(ULONG_PTR)handle >= 0xfffffffa) return FALSE;

    handle_idx = (wine_server_obj_handle( handle ) >> 2) - 1;
    *entry = handle_idx / WFUSYNC_CACHE_BLOCK_SIZE;
    *idx = handle_idx % WFUSYNC_CACHE_BLOCK_SIZE;
    return *entry < WFUSYNC_CACHE_ENTRIES;
}

static BOOL wfusync_supported_type( int type )
{
    switch (type)
    {
    case INPROC_SYNC_INTERNAL:
    case INPROC_SYNC_EVENT:
    case INPROC_SYNC_MUTEX:
    case INPROC_SYNC_SEMAPHORE:
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOL get_wfusync_cache_entry( HANDLE handle, struct wfusync_cache_entry **entry )
{
    unsigned int block, idx;

    if (!wfusync_handle_to_index( handle, &block, &idx )) return FALSE;
    if (!wfusync_cache[block])
        wfusync_cache[block] = calloc( WFUSYNC_CACHE_BLOCK_SIZE, sizeof(**wfusync_cache) );
    if (!wfusync_cache[block]) return FALSE;

    *entry = &wfusync_cache[block][idx];
    return TRUE;
}

static unsigned char wfusync_type_to_kind( int type )
{
    switch (type)
    {
    case INPROC_SYNC_EVENT:
        return WFUSYNC_KIND_EVENT;
    case INPROC_SYNC_MUTEX:
        return WFUSYNC_KIND_MUTEX;
    case INPROC_SYNC_SEMAPHORE:
        return WFUSYNC_KIND_SEMAPHORE;
    default:
        return WFUSYNC_KIND_UNKNOWN;
    }
}

static void wfusync_set_handle_kind( HANDLE handle, unsigned char kind )
{
    unsigned int block, idx;

    if (!wfusync_handle_to_index( handle, &block, &idx )) return;
    if (!wfusync_handle_kinds[block])
    {
        if (!block) wfusync_handle_kinds[block] = wfusync_handle_kinds_initial;
        else wfusync_handle_kinds[block] = calloc( WFUSYNC_CACHE_BLOCK_SIZE, sizeof(**wfusync_handle_kinds) );
    }
    if (!wfusync_handle_kinds[block]) return;

    __atomic_store_n( &wfusync_handle_kinds[block][idx], kind, __ATOMIC_RELEASE );
}

static unsigned char wfusync_get_handle_kind( HANDLE handle )
{
    unsigned int block, idx;

    if (!wfusync_handle_to_index( handle, &block, &idx )) return WFUSYNC_KIND_UNKNOWN;
    if (!wfusync_handle_kinds[block]) return WFUSYNC_KIND_UNKNOWN;

    return __atomic_load_n( &wfusync_handle_kinds[block][idx], __ATOMIC_ACQUIRE );
}

static BOOL wfusync_shm_idx_signal_unsafe( unsigned int shm_idx )
{
    if (!shm_idx || shm_idx >= wfusync_signal_unsafe_indexes_count) return FALSE;
    return __atomic_load_n( &wfusync_signal_unsafe_indexes[shm_idx], __ATOMIC_ACQUIRE );
}

static struct wfusync_shared_state *map_wfusync_state( unsigned int shm_idx, int fd )
{
    unsigned int page, offset;
    void **new_pages;
    void *ptr;

    if (!shm_idx)
    {
        TRACE( "invalid index %u\n", shm_idx );
        return NULL;
    }
    if (fd < 0)
    {
        TRACE( "fallback shm_idx %u has no shared fd\n", shm_idx );
        return NULL;
    }

    page = (shm_idx * sizeof(struct wfusync_shared_state)) / get_page_size();
    offset = (shm_idx * sizeof(struct wfusync_shared_state)) % get_page_size();
    if (offset + sizeof(struct wfusync_shared_state) > get_page_size())
    {
        TRACE( "invalid index %u crosses page boundary\n", shm_idx );
        return NULL;
    }

    if (page >= wfusync_pages_count)
    {
        unsigned int new_count = wfusync_pages_count ? wfusync_pages_count * 2 : 16;

        while (page >= new_count) new_count *= 2;
        if (!(new_pages = realloc( wfusync_pages, new_count * sizeof(*wfusync_pages) )))
        {
            TRACE( "fallback shm_idx %u page %u page-cache allocation failed\n", shm_idx, page );
            return NULL;
        }
        memset( new_pages + wfusync_pages_count, 0, (new_count - wfusync_pages_count) * sizeof(*wfusync_pages) );
        wfusync_pages = new_pages;
        wfusync_pages_count = new_count;
    }

    if (!wfusync_pages[page])
    {
        ptr = mmap( NULL, get_page_size(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)page * get_page_size() );
        if (ptr == MAP_FAILED)
        {
            TRACE( "fallback shm_idx %u page %u mmap failed errno %d\n", shm_idx, page, errno );
            return NULL;
        }
        wfusync_pages[page] = ptr;
    }

    return (struct wfusync_shared_state *)((char *)wfusync_pages[page] + offset);
}

void wfusync_mark_local_event( HANDLE handle )
{
    struct wfusync_cache_entry *entry;

    if (!do_wfusync()) return;

    lock_wfusync_cache();
    if (get_wfusync_cache_entry( handle, &entry ))
    {
        if (entry->handle && entry->handle != handle) memset( entry, 0, sizeof(*entry) );
        entry->handle = handle;
        entry->local_event = 1;
        wfusync_set_handle_kind( handle, WFUSYNC_KIND_EVENT );
    }
    else TRACE( "fallback handle %p local event marker allocation failed\n", handle );
    unlock_wfusync_cache();
}

BOOL wfusync_handle_is_local_event( HANDLE handle )
{
    return wfusync_get_handle_kind( handle ) == WFUSYNC_KIND_EVENT;
}

BOOL wfusync_handle_is_local_mutex( HANDLE handle )
{
    return wfusync_get_handle_kind( handle ) == WFUSYNC_KIND_MUTEX;
}

BOOL wfusync_handle_is_local_semaphore( HANDLE handle )
{
    return wfusync_get_handle_kind( handle ) == WFUSYNC_KIND_SEMAPHORE;
}

void wfusync_mark_local_mutex( HANDLE handle )
{
    struct wfusync_cache_entry *entry;

    if (!do_wfusync()) return;

    lock_wfusync_cache();
    if (get_wfusync_cache_entry( handle, &entry ))
    {
        if (entry->handle && entry->handle != handle) memset( entry, 0, sizeof(*entry) );
        entry->handle = handle;
        entry->local_mutex = 1;
        wfusync_set_handle_kind( handle, WFUSYNC_KIND_MUTEX );
    }
    else TRACE( "fallback handle %p local mutex marker allocation failed\n", handle );
    unlock_wfusync_cache();
}

void wfusync_mark_local_semaphore( HANDLE handle )
{
    struct wfusync_cache_entry *entry;

    if (!do_wfusync()) return;

    lock_wfusync_cache();
    if (get_wfusync_cache_entry( handle, &entry ))
    {
        if (entry->handle && entry->handle != handle) memset( entry, 0, sizeof(*entry) );
        entry->handle = handle;
        entry->local_semaphore = 1;
        wfusync_set_handle_kind( handle, WFUSYNC_KIND_SEMAPHORE );
    }
    else TRACE( "fallback handle %p local semaphore marker allocation failed\n", handle );
    unlock_wfusync_cache();
}

BOOL wfusync_get_cached_state( HANDLE handle, struct wfusync_fast_state *state )
{
    unsigned int entry, idx;
    BOOL ret = FALSE;

    if (!do_wfusync()) return FALSE;

    if (!wfusync_handle_to_index( handle, &entry, &idx ))
    {
        TRACE( "unsupported object handle %p; falling back to Wine server\n", handle );
        return FALSE;
    }

    lock_wfusync_cache();
    if (wfusync_cache[entry] && wfusync_cache[entry][idx].valid &&
        wfusync_cache[entry][idx].handle == handle)
    {
        if (state) *state = wfusync_cache[entry][idx].state;
        ret = TRUE;
    }
    unlock_wfusync_cache();

    return ret;
}

static NTSTATUS get_local_event_state_locked( HANDLE handle, ACCESS_MASK access,
                                              struct wfusync_cache_entry **entry )
{
    struct wfusync_cache_entry *cache;

    if (!get_wfusync_cache_entry( handle, &cache )) return STATUS_NOT_IMPLEMENTED;
    if (!cache->local_event || !cache->valid || cache->handle != handle || !cache->state.ptr)
        return STATUS_NOT_IMPLEMENTED;
    if (cache->state.type != INPROC_SYNC_EVENT)
    {
        TRACE( "fallback event handle %p wrong type %d\n", handle, cache->state.type );
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    if ((cache->state.access & access) != access)
    {
        TRACE( "fallback event handle %p access %#x missing %#x\n", handle, cache->state.access, access );
        return STATUS_ACCESS_DENIED;
    }

    *entry = cache;
    return STATUS_SUCCESS;
}

static NTSTATUS get_local_semaphore_state_locked( HANDLE handle, ACCESS_MASK access,
                                                  struct wfusync_cache_entry **entry )
{
    struct wfusync_cache_entry *cache;

    if (!get_wfusync_cache_entry( handle, &cache )) return STATUS_NOT_IMPLEMENTED;
    if (!cache->local_semaphore || !cache->valid || cache->handle != handle || !cache->state.ptr)
        return STATUS_NOT_IMPLEMENTED;
    if (cache->state.type != INPROC_SYNC_SEMAPHORE)
    {
        TRACE( "fallback semaphore handle %p wrong type %d\n", handle, cache->state.type );
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    if ((cache->state.access & access) != access)
    {
        TRACE( "fallback semaphore handle %p access %#x missing %#x\n", handle, cache->state.access, access );
        return STATUS_ACCESS_DENIED;
    }

    *entry = cache;
    return STATUS_SUCCESS;
}

static NTSTATUS get_local_mutex_state_locked( HANDLE handle, ACCESS_MASK access,
                                              struct wfusync_cache_entry **entry )
{
    struct wfusync_cache_entry *cache;

    if (!get_wfusync_cache_entry( handle, &cache )) return STATUS_NOT_IMPLEMENTED;
    if (!cache->local_mutex || !cache->valid || cache->handle != handle || !cache->state.ptr)
        return STATUS_NOT_IMPLEMENTED;
    if (cache->state.type != INPROC_SYNC_MUTEX)
    {
        TRACE( "fallback mutex handle %p wrong type %d\n", handle, cache->state.type );
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    if ((cache->state.access & access) != access)
    {
        TRACE( "fallback mutex handle %p access %#x missing %#x\n", handle, cache->state.access, access );
        return STATUS_ACCESS_DENIED;
    }

    *entry = cache;
    return STATUS_SUCCESS;
}

void wfusync_cache_inproc_sync( HANDLE handle, int type, unsigned int access, unsigned int shm_idx, int fd )
{
    unsigned int entry_idx, idx;
    int local_event = 0;
    int local_mutex = 0;
    int local_semaphore = 0;
    int signal_unsafe = 0;
    struct wfusync_shared_state *shared;
    struct wfusync_cache_entry *entry;

    if (!do_wfusync()) return;

    if (!shm_idx)
    {
        TRACE( "fallback handle %p type %d access %#x has no WFUSync shm_idx\n", handle, type, access );
        return;
    }
    if (!wfusync_supported_type( type ))
    {
        TRACE( "unsupported object handle %p type %d shm_idx %u; falling back to Wine server\n",
               handle, type, shm_idx );
        return;
    }
    if (!wfusync_handle_to_index( handle, &entry_idx, &idx ))
    {
        TRACE( "fallback handle %p shm_idx %u outside WFUSync cache range\n", handle, shm_idx );
        return;
    }

    lock_wfusync_cache();
    if (!(shared = map_wfusync_state( shm_idx, fd )))
    {
        unlock_wfusync_cache();
        return;
    }
    if (wfusync_shared_fd < 0) wfusync_shared_fd = dup( fd );

    if (get_wfusync_cache_entry( handle, &entry ))
    {
        if (entry->handle == handle)
        {
            local_event = entry->local_event;
            local_mutex = entry->local_mutex;
            local_semaphore = entry->local_semaphore;
            signal_unsafe = entry->signal_unsafe;
        }
        else memset( entry, 0, sizeof(*entry) );
        entry->handle = handle;
        entry->local_event = local_event;
        entry->local_mutex = local_mutex;
        entry->local_semaphore = local_semaphore;
        entry->signal_unsafe = signal_unsafe || wfusync_shm_idx_signal_unsafe( shm_idx );
        entry->state.type = type;
        entry->state.access = access;
        entry->state.shm_idx = shm_idx;
        entry->state.ptr = shared;
        entry->state.generation = shared->generation;
        entry->valid = 1;
        wfusync_set_handle_kind( handle, wfusync_type_to_kind( type ) );
    }
    else TRACE( "fallback handle %p shm_idx %u WFUSync cache allocation failed\n", handle, shm_idx );
    unlock_wfusync_cache();
}

void wfusync_drop_handle( HANDLE handle )
{
    unsigned int entry, idx;

    if (!do_wfusync()) return;
    if (!wfusync_handle_to_index( handle, &entry, &idx )) return;

    wfusync_set_handle_kind( handle, WFUSYNC_KIND_UNKNOWN );

    lock_wfusync_cache();
    if (wfusync_cache[entry] && wfusync_cache[entry][idx].handle == handle)
    {
        memset( &wfusync_cache[entry][idx], 0, sizeof(wfusync_cache[entry][idx]) );
    }
    unlock_wfusync_cache();
}

void wfusync_note_fallback( HANDLE handle, NTSTATUS status )
{
    if (!do_wfusync()) return;
    if (status == STATUS_NOT_IMPLEMENTED) wfusync_set_handle_kind( handle, WFUSYNC_KIND_UNSUPPORTED );
}

static BOOL wfusync_address_wait_supported(void)
{
#ifdef __APPLE__
    return TRUE;
#else
    return FALSE;
#endif
}

static int compare_timespec( const struct timespec *left, const struct timespec *right );

static unsigned long long timespec_diff_ns( const struct timespec *end, const struct timespec *start )
{
    if (compare_timespec( end, start ) <= 0) return 0;
    return (unsigned long long)(end->tv_sec - start->tv_sec) * 1000000000ULL + end->tv_nsec - start->tv_nsec;
}

static void ns_to_timespec( unsigned long long ns, struct timespec *timeout )
{
    timeout->tv_sec = ns / 1000000000ULL;
    timeout->tv_nsec = ns % 1000000000ULL;
}

static NTSTATUS wfusync_wait_address( int *addr, int value, const struct timespec *timeout )
{
#ifdef __APPLE__
    int ret;
#ifdef MAC_OS_VERSION_14_4
    if (__builtin_available( macOS 14.4, * ))
    {
        if (timeout)
        {
            unsigned long long ns_timeout = (unsigned long long)timeout->tv_sec * 1000000000ULL + timeout->tv_nsec;

            if (!ns_timeout) return STATUS_TIMEOUT;
            ret = os_sync_wait_on_address_with_timeout( addr, (uint64_t)value, sizeof(*addr),
                                                        OS_SYNC_WAIT_ON_ADDRESS_SHARED,
                                                        OS_CLOCK_MACH_ABSOLUTE_TIME, ns_timeout );
        }
        else ret = os_sync_wait_on_address( addr, (uint64_t)value, sizeof(*addr),
                                            OS_SYNC_WAIT_ON_ADDRESS_SHARED );
        if (ret >= 0) return STATUS_SUCCESS;
        if (errno == ETIMEDOUT) return STATUS_TIMEOUT;
        if (errno == EINTR || errno == ENOMEM || errno == EFAULT) return STATUS_SUCCESS;
        TRACE( "fallback address wait addr %p errno %d\n", addr, errno );
        return STATUS_NOT_IMPLEMENTED;
    }
#endif

    if (timeout)
    {
        uint64_t usec64 = (uint64_t)timeout->tv_sec * 1000000 + timeout->tv_nsec / 1000;
        uint32_t usec;

        if (!usec64) return STATUS_TIMEOUT;
        if (usec64 > UINT_MAX)
        {
            TRACE( "fallback address wait addr %p timeout too large for ulock\n", addr );
            return STATUS_NOT_IMPLEMENTED;
        }
        usec = usec64;
        ret = __ulock_wait( UL_COMPARE_AND_WAIT_SHARED, addr, (uint64_t)value, usec );
    }
    else ret = __ulock_wait( UL_COMPARE_AND_WAIT_SHARED, addr, (uint64_t)value, 0 );

    if (ret >= 0) return STATUS_SUCCESS;
    if (errno == ETIMEDOUT) return STATUS_TIMEOUT;
    if (errno == EINTR || errno == ENOMEM || errno == EFAULT) return STATUS_SUCCESS;
    TRACE( "fallback ulock wait addr %p errno %d\n", addr, errno );
    return STATUS_NOT_IMPLEMENTED;
#else
    TRACE( "fallback address wait addr %p unsupported platform\n", addr );
    return STATUS_NOT_IMPLEMENTED;
#endif
}

static int wfusync_atomic_load( int *addr )
{
    return __atomic_load_n( addr, __ATOMIC_SEQ_CST );
}

static unsigned short wfusync_state_type( struct wfusync_shared_state *state )
{
    return __atomic_load_n( &state->type, __ATOMIC_SEQ_CST ) & WFUSYNC_TYPE_MASK;
}

static BOOL wfusync_try_lock_state( struct wfusync_shared_state *state, unsigned short expected_type )
{
    unsigned short type = __atomic_load_n( &state->type, __ATOMIC_SEQ_CST );

    if ((type & WFUSYNC_TYPE_MASK) != expected_type ||
        (type & (WFUSYNC_STATE_LOCKED | WFUSYNC_SERVER_WAITERS)))
        return FALSE;

    return __atomic_compare_exchange_n( &state->type, &type, type | WFUSYNC_STATE_LOCKED, FALSE,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST );
}

static BOOL wfusync_lock_state( struct wfusync_shared_state *state, unsigned short expected_type )
{
    unsigned int attempts = 0;
    unsigned short type;

    for (;;)
    {
        if (wfusync_try_lock_state( state, expected_type )) return TRUE;
        type = __atomic_load_n( &state->type, __ATOMIC_SEQ_CST );
        if ((type & WFUSYNC_TYPE_MASK) != expected_type || (type & WFUSYNC_SERVER_WAITERS)) return FALSE;
        if (!(++attempts % 64)) sched_yield();
    }
}

static void wfusync_unlock_state( struct wfusync_shared_state *state )
{
    __atomic_fetch_and( &state->type, ~WFUSYNC_STATE_LOCKED, __ATOMIC_SEQ_CST );
}

static BOOL wfusync_has_server_waiters( struct wfusync_shared_state *state )
{
    return !!(__atomic_load_n( &state->type, __ATOMIC_SEQ_CST ) &
              (WFUSYNC_SERVER_WAITERS | WFUSYNC_MULTI_WAITERS));
}

static BOOL wfusync_has_wine_server_waiters( struct wfusync_shared_state *state )
{
    return !!(__atomic_load_n( &state->type, __ATOMIC_SEQ_CST ) & WFUSYNC_SERVER_WAITERS);
}

static BOOL wfusync_has_multi_waiters( struct wfusync_shared_state *state )
{
    return !!(__atomic_load_n( &state->type, __ATOMIC_SEQ_CST ) & WFUSYNC_MULTI_WAITERS);
}

static BOOL wfusync_entry_signal_hotpath_enabled( const char *op, HANDLE handle,
                                                  struct wfusync_cache_entry *entry )
{
    if (!entry->signal_unsafe && !wfusync_shm_idx_signal_unsafe( entry->state.shm_idx )) return TRUE;
    entry->signal_unsafe = 1;
    TRACE( "fallback %s handle %p shm_idx %u; object used by multi-wait\n",
           op, handle, entry->state.shm_idx );
    return FALSE;
}

static BOOL wfusync_handle_kind_matches( HANDLE handle, unsigned char kind )
{
    return wfusync_get_handle_kind( handle ) == kind;
}

static BOOL wfusync_atomic_compare_exchange( int *addr, int expected, int desired )
{
    return __atomic_compare_exchange_n( addr, &expected, desired, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST );
}

static void wfusync_wake_address( int *addr, BOOL wake_all )
{
#ifdef __APPLE__
#ifdef MAC_OS_VERSION_14_4
    if (__builtin_available( macOS 14.4, * ))
    {
        if (wake_all) os_sync_wake_by_address_all( addr, sizeof(*addr), OS_SYNC_WAKE_BY_ADDRESS_SHARED );
        else os_sync_wake_by_address_any( addr, sizeof(*addr), OS_SYNC_WAKE_BY_ADDRESS_SHARED );
        return;
    }
#endif
    __ulock_wake( UL_COMPARE_AND_WAIT_SHARED | (wake_all ? ULF_WAKE_ALL : 0), addr, 0 );
#endif
}

static void wfusync_wake_waiters( unsigned int shm_idx )
{
    SERVER_START_REQ( wake_wfusync_waiters )
    {
        req->shm_idx = shm_idx;
        wine_server_call( req );
    }
    SERVER_END_REQ;
}

NTSTATUS wfusync_set_event( HANDLE handle, LONG *prev_state )
{
    struct wfusync_cache_entry *entry;
    struct wfusync_shared_state *state;
    unsigned int generation;
    int previous;
    NTSTATUS ret;

    if (!do_wfusync()) return STATUS_NOT_IMPLEMENTED;
    if (!wfusync_handle_kind_matches( handle, WFUSYNC_KIND_EVENT )) return STATUS_NOT_IMPLEMENTED;

    lock_wfusync_cache();
    if ((ret = get_local_event_state_locked( handle, EVENT_MODIFY_STATE, &entry )))
    {
        unlock_wfusync_cache();
        return ret;
    }
    state = entry->state.ptr;
    generation = entry->state.generation;
    if (!wfusync_entry_signal_hotpath_enabled( "event set", handle, entry ))
    {
        unlock_wfusync_cache();
        return STATUS_NOT_IMPLEMENTED;
    }
    if (wfusync_has_server_waiters( state ))
    {
        TRACE( "fallback event set handle %p has server waiters\n", handle );
        unlock_wfusync_cache();
        return STATUS_NOT_IMPLEMENTED;
    }
    unlock_wfusync_cache();

    if (!wfusync_lock_state( state, INPROC_SYNC_EVENT ))
    {
        TRACE( "fallback event set handle %p stale or server-owned state\n", handle );
        return STATUS_NOT_IMPLEMENTED;
    }
    if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation ||
        wfusync_has_server_waiters( state ))
    {
        wfusync_unlock_state( state );
        TRACE( "fallback event set handle %p stale or server-owned state\n", handle );
        return STATUS_NOT_IMPLEMENTED;
    }
    previous = __atomic_exchange_n( &state->low, 1, __ATOMIC_SEQ_CST );
    if (prev_state) *prev_state = previous;
    wfusync_unlock_state( state );
    if (!previous) wfusync_wake_address( &state->low, state->high != 0 );
    if (!previous && wfusync_has_multi_waiters( state ))
        wfusync_wake_waiters( entry->state.shm_idx );
    return STATUS_SUCCESS;
}

NTSTATUS wfusync_reset_event( HANDLE handle, LONG *prev_state )
{
    struct wfusync_cache_entry *entry;
    struct wfusync_shared_state *state;
    unsigned int generation;
    int previous;
    NTSTATUS ret;

    if (!do_wfusync()) return STATUS_NOT_IMPLEMENTED;
    if (!wfusync_handle_kind_matches( handle, WFUSYNC_KIND_EVENT )) return STATUS_NOT_IMPLEMENTED;

    lock_wfusync_cache();
    if ((ret = get_local_event_state_locked( handle, EVENT_MODIFY_STATE, &entry )))
    {
        unlock_wfusync_cache();
        return ret;
    }
    state = entry->state.ptr;
    generation = entry->state.generation;
    if (!wfusync_entry_signal_hotpath_enabled( "event reset", handle, entry ))
    {
        unlock_wfusync_cache();
        return STATUS_NOT_IMPLEMENTED;
    }
    if (wfusync_has_server_waiters( state ))
    {
        TRACE( "fallback event reset handle %p has server waiters\n", handle );
        unlock_wfusync_cache();
        return STATUS_NOT_IMPLEMENTED;
    }
    unlock_wfusync_cache();

    if (!wfusync_lock_state( state, INPROC_SYNC_EVENT ))
    {
        TRACE( "fallback event reset handle %p stale or server-owned state\n", handle );
        return STATUS_NOT_IMPLEMENTED;
    }
    if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation ||
        wfusync_has_server_waiters( state ))
    {
        wfusync_unlock_state( state );
        TRACE( "fallback event reset handle %p stale or server-owned state\n", handle );
        return STATUS_NOT_IMPLEMENTED;
    }
    previous = __atomic_exchange_n( &state->low, 0, __ATOMIC_SEQ_CST );
    if (prev_state) *prev_state = previous;
    wfusync_unlock_state( state );
    return STATUS_SUCCESS;
}

NTSTATUS wfusync_query_event( HANDLE handle, EVENT_BASIC_INFORMATION *info )
{
    struct wfusync_cache_entry *entry;
    struct wfusync_shared_state *state;
    NTSTATUS ret;

    if (!do_wfusync()) return STATUS_NOT_IMPLEMENTED;

    lock_wfusync_cache();
    if (!(ret = get_local_event_state_locked( handle, EVENT_QUERY_STATE, &entry )))
    {
        state = entry->state.ptr;
        info->EventType = state->high ? NotificationEvent : SynchronizationEvent;
        info->EventState = wfusync_atomic_load( &state->low );
    }
    unlock_wfusync_cache();

    return ret;
}

NTSTATUS wfusync_release_mutex( HANDLE handle, LONG *prev_count )
{
    struct wfusync_cache_entry *entry;
    struct wfusync_shared_state *state;
    unsigned int generation;
    int tid = GetCurrentThreadId();
    int count;
    NTSTATUS ret;

    if (!do_wfusync()) return STATUS_NOT_IMPLEMENTED;
    if (!wfusync_handle_kind_matches( handle, WFUSYNC_KIND_MUTEX )) return STATUS_NOT_IMPLEMENTED;

    lock_wfusync_cache();
    if ((ret = get_local_mutex_state_locked( handle, 0, &entry )))
    {
        unlock_wfusync_cache();
        return ret;
    }
    state = entry->state.ptr;
    generation = entry->state.generation;
    if (wfusync_has_server_waiters( state ))
    {
        TRACE( "fallback mutex release handle %p has server waiters\n", handle );
        unlock_wfusync_cache();
        return STATUS_NOT_IMPLEMENTED;
    }
    unlock_wfusync_cache();

    if (!wfusync_lock_state( state, INPROC_SYNC_MUTEX ))
    {
        TRACE( "fallback mutex release handle %p stale or server-owned state\n", handle );
        return STATUS_NOT_IMPLEMENTED;
    }
    if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation ||
        wfusync_has_server_waiters( state ))
    {
        wfusync_unlock_state( state );
        TRACE( "fallback mutex release handle %p stale or server-owned state\n", handle );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (__atomic_load_n( &state->low, __ATOMIC_SEQ_CST ) != tid ||
        !(count = __atomic_load_n( &state->high, __ATOMIC_SEQ_CST )))
    {
        wfusync_unlock_state( state );
        return STATUS_MUTANT_NOT_OWNED;
    }

    if (prev_count) *prev_count = 1 - count;
    if (count > 1)
    {
        __atomic_store_n( &state->high, count - 1, __ATOMIC_SEQ_CST );
        wfusync_unlock_state( state );
        return STATUS_SUCCESS;
    }

    __atomic_store_n( &state->high, 0, __ATOMIC_SEQ_CST );
    __atomic_store_n( &state->low, 0, __ATOMIC_SEQ_CST );
    wfusync_unlock_state( state );
    wfusync_wake_address( &state->low, FALSE );
    if (wfusync_has_multi_waiters( state ))
        wfusync_wake_waiters( entry->state.shm_idx );
    return STATUS_SUCCESS;
}

NTSTATUS wfusync_query_mutex( HANDLE handle, MUTANT_BASIC_INFORMATION *info )
{
    struct wfusync_cache_entry *entry;
    struct wfusync_shared_state *state;
    int owner, count;
    NTSTATUS ret;

    if (!do_wfusync()) return STATUS_NOT_IMPLEMENTED;
    if (!wfusync_handle_kind_matches( handle, WFUSYNC_KIND_MUTEX )) return STATUS_NOT_IMPLEMENTED;

    lock_wfusync_cache();
    if (!(ret = get_local_mutex_state_locked( handle, MUTANT_QUERY_STATE, &entry )))
    {
        state = entry->state.ptr;
        if (!wfusync_lock_state( state, INPROC_SYNC_MUTEX ))
            ret = STATUS_NOT_IMPLEMENTED;
        else
        {
            owner = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST );
            count = owner < 0 ? 0 : __atomic_load_n( &state->high, __ATOMIC_SEQ_CST );
            info->CurrentCount = 1 - count;
            info->OwnedByCaller = owner == (int)GetCurrentThreadId();
            info->AbandonedState = owner < 0;
            wfusync_unlock_state( state );
        }
    }
    unlock_wfusync_cache();

    return ret;
}

NTSTATUS wfusync_release_semaphore( HANDLE handle, ULONG count, ULONG *previous )
{
    struct wfusync_cache_entry *entry;
    struct wfusync_shared_state *state;
    unsigned int generation;
    int old_count, new_count;
    NTSTATUS ret;

    if (!do_wfusync()) return STATUS_NOT_IMPLEMENTED;
    if (!wfusync_handle_kind_matches( handle, WFUSYNC_KIND_SEMAPHORE )) return STATUS_NOT_IMPLEMENTED;

    lock_wfusync_cache();
    if ((ret = get_local_semaphore_state_locked( handle, SEMAPHORE_MODIFY_STATE, &entry )))
    {
        unlock_wfusync_cache();
        return ret;
    }
    state = entry->state.ptr;
    generation = entry->state.generation;
    if (!wfusync_entry_signal_hotpath_enabled( "semaphore release", handle, entry ))
    {
        unlock_wfusync_cache();
        return STATUS_NOT_IMPLEMENTED;
    }
    if (wfusync_has_server_waiters( state ))
    {
        TRACE( "fallback semaphore release handle %p has server waiters\n", handle );
        unlock_wfusync_cache();
        return STATUS_NOT_IMPLEMENTED;
    }
    unlock_wfusync_cache();

    if (!wfusync_lock_state( state, INPROC_SYNC_SEMAPHORE ))
    {
        TRACE( "fallback semaphore release handle %p stale or server-owned state\n", handle );
        return STATUS_NOT_IMPLEMENTED;
    }
    if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation ||
        wfusync_has_server_waiters( state ))
    {
        wfusync_unlock_state( state );
        TRACE( "fallback semaphore release handle %p stale or server-owned state\n", handle );
        return STATUS_NOT_IMPLEMENTED;
    }

    do
    {
        old_count = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST );
        if ((ULONG)old_count + count < (ULONG)old_count || (ULONG)old_count + count > (ULONG)state->high)
        {
            TRACE( "semaphore release handle %p count %u current %d max %d exceeds limit\n",
                   handle, count, old_count, state->high );
            wfusync_unlock_state( state );
            return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
        }
        new_count = old_count + count;
    } while (!__atomic_compare_exchange_n( &state->low, &old_count, new_count, FALSE,
                                           __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ));

    if (previous) *previous = old_count;
    wfusync_unlock_state( state );
    if (!old_count) wfusync_wake_address( &state->low, count > 1 );
    if (!old_count && wfusync_has_multi_waiters( state ))
        wfusync_wake_waiters( entry->state.shm_idx );
    return STATUS_SUCCESS;
}

NTSTATUS wfusync_query_semaphore( HANDLE handle, SEMAPHORE_BASIC_INFORMATION *info )
{
    if (!do_wfusync()) return STATUS_NOT_IMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS consume_auto_event_state( struct wfusync_shared_state *state )
{
    if (!wfusync_atomic_load( &state->low )) return STATUS_TIMEOUT;
    if (state->high) return STATUS_NOT_IMPLEMENTED;
    if (!wfusync_atomic_compare_exchange( &state->low, 1, 0 )) return STATUS_TIMEOUT;
    return STATUS_WAIT_0;
}

static NTSTATUS consume_event_state( struct wfusync_shared_state *state )
{
    NTSTATUS ret;

    if (!wfusync_lock_state( state, INPROC_SYNC_EVENT )) return STATUS_NOT_IMPLEMENTED;
    ret = !wfusync_atomic_load( &state->low ) ? STATUS_TIMEOUT :
          state->high ? STATUS_WAIT_0 : consume_auto_event_state( state );
    wfusync_unlock_state( state );
    return ret;
}

static NTSTATUS consume_semaphore_state( struct wfusync_shared_state *state )
{
    int count;

    if (!wfusync_lock_state( state, INPROC_SYNC_SEMAPHORE )) return STATUS_NOT_IMPLEMENTED;

    do
    {
        count = wfusync_atomic_load( &state->low );
        if (count <= 0)
        {
            wfusync_unlock_state( state );
            return STATUS_TIMEOUT;
        }
    } while (!__atomic_compare_exchange_n( &state->low, &count, count - 1, FALSE,
                                           __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ));

    wfusync_unlock_state( state );
    return STATUS_WAIT_0;
}

static NTSTATUS consume_mutex_state_locked( struct wfusync_shared_state *state, int tid )
{
    int owner, count;

    owner = wfusync_atomic_load( &state->low );
    count = wfusync_atomic_load( &state->high );

    if (owner == tid)
    {
        if (count == INT_MAX) return STATUS_NOT_IMPLEMENTED;
        __atomic_store_n( &state->high, count + 1, __ATOMIC_SEQ_CST );
        return STATUS_WAIT_0;
    }

    if (!owner || owner < 0)
    {
        __atomic_store_n( &state->low, tid, __ATOMIC_SEQ_CST );
        __atomic_store_n( &state->high, 1, __ATOMIC_SEQ_CST );
        if (owner < 0)
        {
            __atomic_add_fetch( &state->generation, 1, __ATOMIC_SEQ_CST );
            return STATUS_ABANDONED;
        }
        return STATUS_WAIT_0;
    }

    return STATUS_TIMEOUT;
}

static NTSTATUS consume_mutex_state( struct wfusync_shared_state *state, int tid )
{
    NTSTATUS ret;

    if (!wfusync_lock_state( state, INPROC_SYNC_MUTEX )) return STATUS_NOT_IMPLEMENTED;
    ret = consume_mutex_state_locked( state, tid );
    wfusync_unlock_state( state );
    return ret;
}

static void wfusync_refresh_cache_generation( HANDLE handle, struct wfusync_shared_state *state )
{
    struct wfusync_cache_entry *entry;

    lock_wfusync_cache();
    if (get_wfusync_cache_entry( handle, &entry ) && entry->handle == handle &&
        entry->valid && entry->state.ptr == state)
        entry->state.generation = __atomic_load_n( &state->generation, __ATOMIC_SEQ_CST );
    unlock_wfusync_cache();
}

static void get_relative_timeout_abs( const LARGE_INTEGER *timeout, struct timespec *abstime )
{
    LONGLONG ticks = -timeout->QuadPart;
    struct timeval now;
    LONGLONG nsec;

    gettimeofday( &now, NULL );
    nsec = (LONGLONG)now.tv_usec * 1000 + (ticks % WFUSYNC_TICKS_PER_SEC) * 100;
    abstime->tv_sec = now.tv_sec + ticks / WFUSYNC_TICKS_PER_SEC + nsec / 1000000000;
    abstime->tv_nsec = nsec % 1000000000;
}

static void get_now_abs( struct timespec *now )
{
    struct timeval tv;

    gettimeofday( &tv, NULL );
    now->tv_sec = tv.tv_sec;
    now->tv_nsec = tv.tv_usec * 1000;
}

static int compare_timespec( const struct timespec *left, const struct timespec *right )
{
    if (left->tv_sec != right->tv_sec) return left->tv_sec < right->tv_sec ? -1 : 1;
    if (left->tv_nsec != right->tv_nsec) return left->tv_nsec < right->tv_nsec ? -1 : 1;
    return 0;
}

static BOOL wfusync_adaptive_spin( unsigned int *attempts )
{
    if (*attempts >= WFUSYNC_SPIN_LIMIT) return FALSE;

    (*attempts)++;
    return TRUE;
}

NTSTATUS wfusync_wait_event( HANDLE handle, BOOLEAN alertable, const LARGE_INTEGER *timeout )
{
    struct wfusync_cache_entry *entry;
    struct wfusync_shared_state *state;
    struct timespec deadline;
    unsigned int attempts = 0;
    unsigned int generation;
    unsigned char kind;
    NTSTATUS ret;

    if (!do_wfusync()) return STATUS_NOT_IMPLEMENTED;

    kind = wfusync_get_handle_kind( handle );
    if (kind && kind != WFUSYNC_KIND_EVENT) return STATUS_NOT_IMPLEMENTED;
    if (alertable)
        return kind == WFUSYNC_KIND_EVENT ? STATUS_NOT_IMPLEMENTED
                                          : STATUS_NOT_IMPLEMENTED;
    if (timeout && timeout->QuadPart > 0)
        return kind == WFUSYNC_KIND_EVENT ? STATUS_NOT_IMPLEMENTED
                                          : STATUS_NOT_IMPLEMENTED;

    lock_wfusync_cache();
    if ((ret = get_local_event_state_locked( handle, SYNCHRONIZE, &entry )))
    {
        unlock_wfusync_cache();
        if (ret == STATUS_NOT_IMPLEMENTED && kind == WFUSYNC_KIND_EVENT)
            return STATUS_NOT_IMPLEMENTED;
        return ret;
    }
    state = entry->state.ptr;
    generation = entry->state.generation;
    if (!wfusync_entry_signal_hotpath_enabled( "event wait", handle, entry ))
    {
        unlock_wfusync_cache();
        return STATUS_NOT_IMPLEMENTED;
    }
    if (wfusync_has_server_waiters( state ))
    {
        TRACE( "fallback event wait handle %p has server waiters\n", handle );
        unlock_wfusync_cache();
        return STATUS_NOT_IMPLEMENTED;
    }
    if (!wfusync_address_wait_supported())
    {
        unlock_wfusync_cache();
        return STATUS_NOT_IMPLEMENTED;
    }
    unlock_wfusync_cache();

    if ((ret = consume_event_state( state )) == STATUS_WAIT_0)
    {
        return ret;
    }
    if (timeout && !timeout->QuadPart)
    {
        return STATUS_TIMEOUT;
    }

    if (timeout) get_relative_timeout_abs( timeout, &deadline );

    for (;;)
    {
        struct timespec now, wait_timeout;
        struct timespec *wait_timeout_ptr = NULL;
        unsigned long long ns;

        if (wfusync_adaptive_spin( &attempts )) continue;
        if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation ||
            wfusync_state_type( state ) != INPROC_SYNC_EVENT ||
            wfusync_has_server_waiters( state ))
        {
            TRACE( "fallback event wait handle %p stale or server-owned state\n", handle );
            return STATUS_NOT_IMPLEMENTED;
        }
        if ((ret = consume_event_state( state )) == STATUS_WAIT_0) break;
        if (timeout)
        {
            get_now_abs( &now );
            ns = timespec_diff_ns( &deadline, &now );
            if (!ns)
            {
                return STATUS_TIMEOUT;
            }
            ns_to_timespec( ns, &wait_timeout );
            wait_timeout_ptr = &wait_timeout;
        }
        else
        {
            ns_to_timespec( WFUSYNC_BLOCKING_FALLBACK_NS, &wait_timeout );
            wait_timeout_ptr = &wait_timeout;
        }
        ret = wfusync_wait_address( &state->low, 0, wait_timeout_ptr );
        if (ret == STATUS_TIMEOUT)
        {
            if (!timeout) return STATUS_NOT_IMPLEMENTED;
            break;
        }
        if (ret == STATUS_NOT_IMPLEMENTED)
            return STATUS_NOT_IMPLEMENTED;
        if (ret != STATUS_SUCCESS) return ret;
    }

    return ret;
}

NTSTATUS wfusync_wait_mutex( HANDLE handle, BOOLEAN alertable, const LARGE_INTEGER *timeout )
{
    struct wfusync_cache_entry *entry;
    struct wfusync_shared_state *state;
    struct timespec deadline;
    unsigned int attempts = 0;
    unsigned int generation;
    unsigned char kind;
    int tid = GetCurrentThreadId();
    NTSTATUS ret;

    if (!do_wfusync()) return STATUS_NOT_IMPLEMENTED;

    kind = wfusync_get_handle_kind( handle );
    if (kind && kind != WFUSYNC_KIND_MUTEX) return STATUS_NOT_IMPLEMENTED;
    if (alertable)
        return kind == WFUSYNC_KIND_MUTEX ? STATUS_NOT_IMPLEMENTED
                                          : STATUS_NOT_IMPLEMENTED;
    if (timeout && timeout->QuadPart > 0)
        return kind == WFUSYNC_KIND_MUTEX ? STATUS_NOT_IMPLEMENTED
                                          : STATUS_NOT_IMPLEMENTED;

    lock_wfusync_cache();
    if ((ret = get_local_mutex_state_locked( handle, SYNCHRONIZE, &entry )))
    {
        unlock_wfusync_cache();
        if (ret == STATUS_NOT_IMPLEMENTED && kind == WFUSYNC_KIND_MUTEX)
            return STATUS_NOT_IMPLEMENTED;
        return ret;
    }
    state = entry->state.ptr;
    generation = entry->state.generation;
    if (wfusync_has_server_waiters( state ))
    {
        TRACE( "fallback mutex wait handle %p has server waiters\n", handle );
        unlock_wfusync_cache();
        return STATUS_NOT_IMPLEMENTED;
    }
    if (!wfusync_address_wait_supported())
    {
        unlock_wfusync_cache();
        return STATUS_NOT_IMPLEMENTED;
    }
    unlock_wfusync_cache();

    ret = consume_mutex_state( state, tid );
    if (ret == STATUS_WAIT_0 || ret == STATUS_ABANDONED)
    {
        if (ret == STATUS_ABANDONED) wfusync_refresh_cache_generation( handle, state );
        return ret;
    }
    if (ret != STATUS_TIMEOUT) return ret;
    if (timeout && !timeout->QuadPart)
    {
        return STATUS_TIMEOUT;
    }

    if (timeout) get_relative_timeout_abs( timeout, &deadline );

    for (;;)
    {
        struct timespec now, wait_timeout;
        struct timespec *wait_timeout_ptr = NULL;
        unsigned long long ns;
        int owner;

        if (wfusync_adaptive_spin( &attempts )) continue;
        if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation ||
            wfusync_state_type( state ) != INPROC_SYNC_MUTEX ||
            wfusync_has_server_waiters( state ))
        {
            TRACE( "fallback mutex wait handle %p stale or server-owned state\n", handle );
            return STATUS_NOT_IMPLEMENTED;
        }
        ret = consume_mutex_state( state, tid );
        if (ret == STATUS_WAIT_0 || ret == STATUS_ABANDONED)
        {
            if (ret == STATUS_ABANDONED) wfusync_refresh_cache_generation( handle, state );
            break;
        }
        if (ret != STATUS_TIMEOUT) return ret;
        if (timeout)
        {
            get_now_abs( &now );
            ns = timespec_diff_ns( &deadline, &now );
            if (!ns)
            {
                return STATUS_TIMEOUT;
            }
            ns_to_timespec( ns, &wait_timeout );
            wait_timeout_ptr = &wait_timeout;
        }
        else
        {
            ns_to_timespec( WFUSYNC_BLOCKING_FALLBACK_NS, &wait_timeout );
            wait_timeout_ptr = &wait_timeout;
        }

        owner = wfusync_atomic_load( &state->low );
        ret = wfusync_wait_address( &state->low, owner, wait_timeout_ptr );
        if (ret == STATUS_TIMEOUT)
        {
            if (!timeout) return STATUS_NOT_IMPLEMENTED;
            return STATUS_TIMEOUT;
        }
        if (ret == STATUS_NOT_IMPLEMENTED)
            return STATUS_NOT_IMPLEMENTED;
        if (ret != STATUS_SUCCESS) return ret;
    }

    return ret;
}

NTSTATUS wfusync_wait_semaphore( HANDLE handle, BOOLEAN alertable, const LARGE_INTEGER *timeout )
{
    struct wfusync_cache_entry *entry;
    struct wfusync_shared_state *state;
    struct timespec deadline;
    unsigned int attempts = 0;
    unsigned int generation;
    unsigned char kind;
    NTSTATUS ret;

    if (!do_wfusync()) return STATUS_NOT_IMPLEMENTED;

    kind = wfusync_get_handle_kind( handle );
    if (kind && kind != WFUSYNC_KIND_SEMAPHORE) return STATUS_NOT_IMPLEMENTED;
    if (alertable)
        return kind == WFUSYNC_KIND_SEMAPHORE ? STATUS_NOT_IMPLEMENTED
                                              : STATUS_NOT_IMPLEMENTED;
    if (timeout && timeout->QuadPart > 0)
        return kind == WFUSYNC_KIND_SEMAPHORE ? STATUS_NOT_IMPLEMENTED
                                              : STATUS_NOT_IMPLEMENTED;

    lock_wfusync_cache();
    if ((ret = get_local_semaphore_state_locked( handle, SYNCHRONIZE, &entry )))
    {
        unlock_wfusync_cache();
        if (ret == STATUS_NOT_IMPLEMENTED && kind == WFUSYNC_KIND_SEMAPHORE)
            return STATUS_NOT_IMPLEMENTED;
        return ret;
    }
    state = entry->state.ptr;
    generation = entry->state.generation;
    if (!wfusync_entry_signal_hotpath_enabled( "semaphore wait", handle, entry ))
    {
        unlock_wfusync_cache();
        return STATUS_NOT_IMPLEMENTED;
    }
    if (wfusync_has_server_waiters( state ))
    {
        TRACE( "fallback semaphore wait handle %p has server waiters\n", handle );
        unlock_wfusync_cache();
        return STATUS_NOT_IMPLEMENTED;
    }
    if (!wfusync_address_wait_supported())
    {
        unlock_wfusync_cache();
        return STATUS_NOT_IMPLEMENTED;
    }
    unlock_wfusync_cache();

    if ((ret = consume_semaphore_state( state )) == STATUS_WAIT_0)
    {
        return ret;
    }
    if (timeout && !timeout->QuadPart)
    {
        return STATUS_TIMEOUT;
    }

    if (timeout) get_relative_timeout_abs( timeout, &deadline );

    for (;;)
    {
        struct timespec now, wait_timeout;
        struct timespec *wait_timeout_ptr = NULL;
        unsigned long long ns;

        if (wfusync_adaptive_spin( &attempts )) continue;
        if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation ||
            wfusync_state_type( state ) != INPROC_SYNC_SEMAPHORE ||
            wfusync_has_server_waiters( state ))
        {
            TRACE( "fallback semaphore wait handle %p stale or server-owned state\n", handle );
            return STATUS_NOT_IMPLEMENTED;
        }
        if ((ret = consume_semaphore_state( state )) == STATUS_WAIT_0) break;
        if (timeout)
        {
            get_now_abs( &now );
            ns = timespec_diff_ns( &deadline, &now );
            if (!ns)
            {
                return STATUS_TIMEOUT;
            }
            ns_to_timespec( ns, &wait_timeout );
            wait_timeout_ptr = &wait_timeout;
        }
        else
        {
            ns_to_timespec( WFUSYNC_BLOCKING_FALLBACK_NS, &wait_timeout );
            wait_timeout_ptr = &wait_timeout;
        }
        ret = wfusync_wait_address( &state->low, 0, wait_timeout_ptr );
        if (ret == STATUS_TIMEOUT)
        {
            if (!timeout) return STATUS_NOT_IMPLEMENTED;
            break;
        }
        if (ret == STATUS_NOT_IMPLEMENTED)
            return STATUS_NOT_IMPLEMENTED;
        if (ret != STATUS_SUCCESS) return ret;
    }

    return ret;
}

static NTSTATUS get_wfusync_wait_slot(void)
{
    NTSTATUS ret = STATUS_SUCCESS;

    if (wfusync_wait_slot) return STATUS_SUCCESS;
    if (wfusync_shared_fd < 0)
    {
        TRACE( "fallback multiple wait no shared fd for wait slot\n" );
        return STATUS_NOT_IMPLEMENTED;
    }

    SERVER_START_REQ( get_wfusync_wait_slot )
    {
        ret = wine_server_call( req );
        if (!ret) wfusync_wait_slot_idx = reply->shm_idx;
    }
    SERVER_END_REQ;

    if (ret) return ret;
    if (!wfusync_wait_slot_idx) return STATUS_NOT_IMPLEMENTED;

    lock_wfusync_cache();
    wfusync_wait_slot = map_wfusync_state( wfusync_wait_slot_idx, wfusync_shared_fd );
    unlock_wfusync_cache();

    return wfusync_wait_slot ? STATUS_SUCCESS : STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS register_wfusync_wait( const unsigned int *shm_idxs, unsigned int count )
{
    NTSTATUS ret;

    SERVER_START_REQ( register_wfusync_wait )
    {
        req->slot_idx = wfusync_wait_slot_idx;
        wine_server_add_data( req, shm_idxs, count * sizeof(*shm_idxs) );
        ret = wine_server_call( req );
    }
    SERVER_END_REQ;

    return ret;
}

static void unregister_wfusync_wait(void)
{
    SERVER_START_REQ( unregister_wfusync_wait )
    {
        req->slot_idx = wfusync_wait_slot_idx;
        wine_server_call( req );
    }
    SERVER_END_REQ;
}

static NTSTATUS try_wait_all_local( struct wfusync_shared_state **states,
                                    const unsigned int *shm_idxs,
                                    const unsigned int *generations,
                                    const unsigned short *types, DWORD count )
{
    DWORD order[64];
    DWORD i, j, locked = 0;
    int abandoned = 0;
    int tid = GetCurrentThreadId();
    NTSTATUS ret = STATUS_NOT_IMPLEMENTED;

    for (i = 0; i < count; i++)
    {
        order[i] = i;
        for (j = i; j && shm_idxs[order[j - 1]] > shm_idxs[order[j]]; j--)
        {
            DWORD swap = order[j - 1];
            order[j - 1] = order[j];
            order[j] = swap;
        }
    }

    for (i = 0; i < count; i++)
    {
        DWORD index = order[i];

        if (i && shm_idxs[order[i - 1]] == shm_idxs[index]) goto done;
        if (types[index] != INPROC_SYNC_EVENT && types[index] != INPROC_SYNC_SEMAPHORE &&
            types[index] != INPROC_SYNC_MUTEX)
            goto done;
        if (!wfusync_lock_state( states[index], types[index] )) goto done;
        locked++;

        if (__atomic_load_n( &states[index]->generation, __ATOMIC_SEQ_CST ) != generations[index])
            goto done;
        if (types[index] == INPROC_SYNC_EVENT && states[index]->high) goto done;
    }

    for (i = 0; i < count; i++)
    {
        if ((types[i] == INPROC_SYNC_EVENT && !states[i]->low) ||
            (types[i] == INPROC_SYNC_SEMAPHORE && states[i]->low <= 0) ||
            (types[i] == INPROC_SYNC_MUTEX && states[i]->low > 0 && states[i]->low != tid))
        {
            ret = STATUS_TIMEOUT;
            goto done;
        }
        if (types[i] == INPROC_SYNC_MUTEX && states[i]->low == tid && states[i]->high == INT_MAX)
            goto done;
        if (types[i] == INPROC_SYNC_MUTEX && states[i]->low < 0) abandoned = 1;
    }

    for (i = 0; i < count; i++)
    {
        if (types[i] == INPROC_SYNC_EVENT) states[i]->low = 0;
        else if (types[i] == INPROC_SYNC_SEMAPHORE) states[i]->low--;
        else
        {
            NTSTATUS status = consume_mutex_state_locked( states[i], tid );

            if (status == STATUS_ABANDONED) abandoned = 1;
            else if (status != STATUS_WAIT_0) goto done;
        }
    }
    ret = abandoned ? STATUS_ABANDONED_WAIT_0 : STATUS_WAIT_0;

done:
    while (locked) wfusync_unlock_state( states[order[--locked]] );
    return ret;
}

static NTSTATUS try_wait_all_wfusync( const unsigned int *entries, DWORD count, unsigned int slot_idx )
{
    NTSTATUS ret;
    SERVER_START_REQ( try_wait_all_wfusync )
    {
        req->tid = GetCurrentThreadId();
        req->slot_idx = slot_idx;
        wine_server_add_data( req, entries, count * 3 * sizeof(*entries) );
        ret = wine_server_call( req );
    }
    SERVER_END_REQ;
    return ret;
}

static NTSTATUS consume_waitany_states( const HANDLE *handles, struct wfusync_shared_state **states,
                                        const unsigned int *generations, const unsigned short *types,
                                        DWORD count )
{
    int tid = GetCurrentThreadId();
    DWORD i;
    NTSTATUS ret;

    for (i = 0; i < count; i++)
    {
        if (__atomic_load_n( &states[i]->generation, __ATOMIC_SEQ_CST ) != generations[i] ||
            wfusync_state_type( states[i] ) != types[i])
        {
            TRACE( "fallback multiple wait stale state index %u\n", i );
            return STATUS_NOT_IMPLEMENTED;
        }

        switch (types[i])
        {
        case INPROC_SYNC_EVENT:
            ret = consume_event_state( states[i] );
            break;
        case INPROC_SYNC_SEMAPHORE:
            ret = consume_semaphore_state( states[i] );
            break;
        case INPROC_SYNC_MUTEX:
            ret = consume_mutex_state( states[i], tid );
            if (ret == STATUS_ABANDONED) wfusync_refresh_cache_generation( handles[i], states[i] );
            break;
        default:
            return STATUS_NOT_IMPLEMENTED;
        }

        if (ret == STATUS_WAIT_0)
        {
            return STATUS_WAIT_0 + i;
        }
        if (ret == STATUS_ABANDONED)
        {
            return STATUS_ABANDONED_WAIT_0 + i;
        }
        if (ret != STATUS_TIMEOUT) return ret;
    }

    return STATUS_TIMEOUT;
}

static BOOL wfusync_waitall_ret_satisfied( NTSTATUS ret, DWORD count )
{
    return ret == STATUS_WAIT_0 || (ret >= STATUS_ABANDONED_WAIT_0 && ret < STATUS_ABANDONED_WAIT_0 + count);
}

static void refresh_waitall_mutex_generations( const HANDLE *handles, struct wfusync_shared_state **states,
                                               const unsigned short *types, DWORD count )
{
    DWORD i;

    for (i = 0; i < count; i++)
        if (types[i] == INPROC_SYNC_MUTEX) wfusync_refresh_cache_generation( handles[i], states[i] );
}

NTSTATUS wfusync_wait_multiple( DWORD count, const HANDLE *handles, WAIT_TYPE type,
                                BOOLEAN alertable, const LARGE_INTEGER *timeout )
{
    struct wfusync_cache_entry *entry;
    struct wfusync_shared_state *states[64];
    unsigned int shm_idxs[64], generations[64];
    unsigned short types[64];
    struct timespec deadline;
    DWORD i;
    NTSTATUS ret;

    if (!do_wfusync()) return STATUS_NOT_IMPLEMENTED;

    if (type != WaitAny && type != WaitAll)
        return STATUS_NOT_IMPLEMENTED;
    if (alertable)
    {
        TRACE( "fallback multiple wait alertable\n" );
        return STATUS_NOT_IMPLEMENTED;
    }
    if (timeout && timeout->QuadPart > 0)
    {
        TRACE( "fallback multiple wait absolute timeout %s\n", wine_dbgstr_longlong(timeout->QuadPart) );
        return STATUS_NOT_IMPLEMENTED;
    }
    if (count < 2 || count > ARRAY_SIZE(states))
    {
        TRACE( "fallback multiple wait unsupported count %lu\n", (unsigned long)count );
        return STATUS_NOT_IMPLEMENTED;
    }
    if (type == WaitAny && !wfusync_address_wait_supported())
    {
        TRACE( "fallback waitany address wait unsupported\n" );
        return STATUS_NOT_IMPLEMENTED;
    }

    lock_wfusync_cache();
    for (i = 0; i < count; i++)
    {
        struct wfusync_shared_state *state;
        unsigned char kind = wfusync_get_handle_kind( handles[i] );

        switch (kind)
        {
        case WFUSYNC_KIND_EVENT:
            ret = get_local_event_state_locked( handles[i], SYNCHRONIZE, &entry );
            break;
        case WFUSYNC_KIND_SEMAPHORE:
            ret = get_local_semaphore_state_locked( handles[i], SYNCHRONIZE, &entry );
            break;
        case WFUSYNC_KIND_MUTEX:
            ret = get_local_mutex_state_locked( handles[i], SYNCHRONIZE, &entry );
            break;
        default:
            TRACE( "fallback multiple wait handle %p unknown kind %u\n", handles[i], kind );
            ret = STATUS_NOT_IMPLEMENTED;
            break;
        }
        if (ret)
        {
            unlock_wfusync_cache();
            if (ret == STATUS_NOT_IMPLEMENTED)
                return STATUS_NOT_IMPLEMENTED;
            return ret;
        }
        state = entry->state.ptr;
        if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != entry->state.generation)
        {
            if (entry->state.type == INPROC_SYNC_MUTEX &&
                wfusync_state_type( state ) == INPROC_SYNC_MUTEX && wfusync_atomic_load( &state->low ) < 0)
                entry->state.generation = __atomic_load_n( &state->generation, __ATOMIC_SEQ_CST );
            else
            {
                TRACE( "fallback multiple wait handle %p has stale generation\n", handles[i] );
                unlock_wfusync_cache();
                return STATUS_NOT_IMPLEMENTED;
            }
        }
        if (type == WaitAll ? wfusync_has_wine_server_waiters( state ) : wfusync_has_server_waiters( state ))
        {
            TRACE( "fallback multiple wait handle %p has server waiters\n", handles[i] );
            unlock_wfusync_cache();
            return STATUS_NOT_IMPLEMENTED;
        }
        states[i] = state;
        shm_idxs[i] = entry->state.shm_idx;
        generations[i] = entry->state.generation;
        types[i] = entry->state.type;
    }
    unlock_wfusync_cache();

    if (type == WaitAll)
    {
        unsigned int entries[ARRAY_SIZE(states) * 3];

        ret = try_wait_all_local( states, shm_idxs, generations, types, count );
        if (ret == STATUS_WAIT_0 || ret == STATUS_ABANDONED_WAIT_0)
        {
            if (ret == STATUS_ABANDONED_WAIT_0)
                refresh_waitall_mutex_generations( handles, states, types, count );
            return ret;
        }
        if (ret == STATUS_TIMEOUT && timeout && !timeout->QuadPart)
        {
            return ret;
        }

        for (i = 0; i < count; i++)
        {
            entries[i * 3] = shm_idxs[i];
            entries[i * 3 + 1] = generations[i];
            entries[i * 3 + 2] = types[i];
        }
        if (timeout && !timeout->QuadPart)
        {
            ret = try_wait_all_wfusync( entries, count, 0 );
            if (wfusync_waitall_ret_satisfied( ret, count ))
            {
                if (ret >= STATUS_ABANDONED_WAIT_0)
                    refresh_waitall_mutex_generations( handles, states, types, count );
                return ret;
            }
            if (ret == STATUS_TIMEOUT)
            {
                return STATUS_TIMEOUT;
            }
            return ret == STATUS_NOT_IMPLEMENTED ? STATUS_NOT_IMPLEMENTED : ret;
        }

        if ((ret = get_wfusync_wait_slot()))
            return ret == STATUS_NOT_IMPLEMENTED ? STATUS_NOT_IMPLEMENTED : ret;
        if (timeout) get_relative_timeout_abs( timeout, &deadline );

        for (;;)
        {
            struct timespec now, wait_timeout;
            struct timespec *wait_timeout_ptr = NULL;
            unsigned long long ns;

            ret = try_wait_all_wfusync( entries, count, wfusync_wait_slot_idx );
            if (wfusync_waitall_ret_satisfied( ret, count ))
            {
                if (ret >= STATUS_ABANDONED_WAIT_0)
                    refresh_waitall_mutex_generations( handles, states, types, count );
                return ret;
            }
            if (ret != STATUS_PENDING)
            {
                return ret == STATUS_NOT_IMPLEMENTED ? STATUS_NOT_IMPLEMENTED : ret;
            }

            if (timeout)
            {
                get_now_abs( &now );
                ns = timespec_diff_ns( &deadline, &now );
                if (!ns)
                {
                    unregister_wfusync_wait();
                    return STATUS_TIMEOUT;
                }
                ns_to_timespec( ns, &wait_timeout );
                wait_timeout_ptr = &wait_timeout;
            }
            ret = wfusync_wait_address( &wfusync_wait_slot->low, 1, wait_timeout_ptr );

            if (ret == STATUS_TIMEOUT)
            {
                unregister_wfusync_wait();
                return STATUS_TIMEOUT;
            }
            if (ret == STATUS_NOT_IMPLEMENTED)
            {
                unregister_wfusync_wait();
                return STATUS_NOT_IMPLEMENTED;
            }
            if (ret != STATUS_SUCCESS)
            {
                unregister_wfusync_wait();
                return ret;
            }
        }
    }

    ret = consume_waitany_states( handles, states, generations, types, count );
    if (ret != STATUS_TIMEOUT)
    {
        if (ret == STATUS_NOT_IMPLEMENTED)
            return STATUS_NOT_IMPLEMENTED;
        return ret;
    }
    if (timeout && !timeout->QuadPart)
    {
        return STATUS_TIMEOUT;
    }

    if ((ret = get_wfusync_wait_slot()))
        return ret == STATUS_NOT_IMPLEMENTED ? STATUS_NOT_IMPLEMENTED : ret;
    if (timeout) get_relative_timeout_abs( timeout, &deadline );

    for (;;)
    {
        struct timespec now, wait_timeout;
        struct timespec *wait_timeout_ptr = NULL;
        unsigned long long ns;

        if ((ret = register_wfusync_wait( shm_idxs, count )))
            return ret == STATUS_NOT_IMPLEMENTED ? STATUS_NOT_IMPLEMENTED : ret;

        ret = consume_waitany_states( handles, states, generations, types, count );
        if (ret != STATUS_TIMEOUT)
        {
            unregister_wfusync_wait();
            if (ret == STATUS_NOT_IMPLEMENTED)
                return STATUS_NOT_IMPLEMENTED;
            return ret;
        }

        if (timeout)
        {
            get_now_abs( &now );
            ns = timespec_diff_ns( &deadline, &now );
            if (!ns)
            {
                unregister_wfusync_wait();
                return STATUS_TIMEOUT;
            }
            ns_to_timespec( ns, &wait_timeout );
            wait_timeout_ptr = &wait_timeout;
        }
        else
        {
            ns_to_timespec( WFUSYNC_BLOCKING_FALLBACK_NS, &wait_timeout );
            wait_timeout_ptr = &wait_timeout;
        }
        ret = wfusync_wait_address( &wfusync_wait_slot->low, 1, wait_timeout_ptr );
        unregister_wfusync_wait();

        if (ret == STATUS_TIMEOUT)
        {
            if (!timeout) return STATUS_NOT_IMPLEMENTED;
            return STATUS_TIMEOUT;
        }
        if (ret == STATUS_NOT_IMPLEMENTED)
            return STATUS_NOT_IMPLEMENTED;
        if (ret != STATUS_SUCCESS) return ret;

        ret = consume_waitany_states( handles, states, generations, types, count );
        if (ret != STATUS_TIMEOUT)
        {
            if (ret == STATUS_NOT_IMPLEMENTED)
                return STATUS_NOT_IMPLEMENTED;
            return ret;
        }
    }
}
