/*
 * USync client shared state
 * WineForge-Internal: usync-v6.
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
#error "usync.c is for Unix only"
#endif

#include "config.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
# include <AvailabilityMacros.h>
# include <mach/mach_time.h>
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

#include "usync.h"
#include "unix_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(usync);

/* Owned and initialized by the ntdll Unix server support. */
extern sigset_t server_block_set;

#define USYNC_CACHE_BLOCK_SIZE  (65536 / sizeof(struct usync_cache_entry))
#define USYNC_CACHE_ENTRIES     128
#define USYNC_TICKS_PER_SEC     10000000LL
#define USYNC_SPIN_LIMIT        32
#define USYNC_BLOCKING_FALLBACK_NS 10000000ULL

#define USYNC_KIND_UNKNOWN      0
#define USYNC_KIND_EVENT        1
#define USYNC_KIND_MUTEX        2
#define USYNC_KIND_SEMAPHORE    3
#define USYNC_KIND_UNSUPPORTED  4
#define USYNC_TYPE_MASK         0x003f
#define USYNC_EVENT_DIRECT_PROTOCOL 0x0040
#define USYNC_DIRECT_WAITING       1u
#define USYNC_DIRECT_COMPLETED        2u
#define USYNC_DIRECT_MAX_GENERATION (INT_MAX >> 2)
#define USYNC_DIRECT_MAX_SLOT      (USHRT_MAX >> 1)
#define USYNC_EVENT_PROTOCOL    0x0080
#define USYNC_MUTEX_PROTOCOL    0x0100
#define USYNC_MUTEX_UNWAITED    0x0200
#define USYNC_SEMAPHORE_UNWAITED 0x0400
#define USYNC_SEMAPHORE_PROTOCOL 0x0800
#define USYNC_EVENT_UNWAITED    0x1000
#define USYNC_STATE_LOCKED      0x2000
#define USYNC_MULTI_WAITERS     0x4000
#define USYNC_SERVER_WAITERS    0x8000

#ifdef __APPLE__
#define UL_COMPARE_AND_WAIT        1
#define UL_COMPARE_AND_WAIT_SHARED 3
#define ULF_WAKE_ALL               0x00000100

extern int __ulock_wait( uint32_t operation, void *addr, uint64_t value, uint32_t timeout );
extern int __ulock_wake( uint32_t operation, void *addr, uint64_t wake_value );
#endif

struct usync_shared_state
{
    int            low;
    int            high;
    unsigned int   generation;
    unsigned short type;
    unsigned short refcount;
};
C_ASSERT( sizeof(struct usync_shared_state) == 16 );

struct usync_cache_entry
{
    HANDLE handle;
    int valid;
    int local_event;
    int local_mutex;
    int local_semaphore;
    int signal_unsafe;
    struct usync_fast_state state;
};

static struct usync_cache_entry *usync_cache[USYNC_CACHE_ENTRIES];
static unsigned char *usync_handle_kinds[USYNC_CACHE_ENTRIES];
static unsigned char usync_handle_kinds_initial[USYNC_CACHE_BLOCK_SIZE];
static unsigned char *usync_signal_unsafe_indexes;
static unsigned int usync_signal_unsafe_indexes_count;
static void **usync_pages;
static unsigned int usync_pages_count;
#ifdef __APPLE__
static os_unfair_lock usync_cache_lock = OS_UNFAIR_LOCK_INIT;
#else
static pthread_mutex_t usync_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
static int do_usync_env_cached = -1;
static int usync_shared_fd = -1;
static __thread unsigned int usync_wait_slot_idx;
static __thread struct usync_shared_state *usync_wait_slot;
static __thread unsigned int usync_event_slot_generation;
static NTSTATUS get_direct_event_wait_slot(void);
static long usync_page_size;

static __thread unsigned int usync_lock_depth;
static __thread sigset_t usync_lock_sigset;

/* Native probes supply controlled handlers. Real ARM Wine retains masking
 * until its complete suspension and context protocol is separately verified. */
#if defined(__APPLE__) && (defined(__x86_64__) || defined(USYNC_GATE_NATIVE_TEST))
#define USYNC_USE_DEFERRED_SIGNALS
#endif

#define USYNC_DEFER_QUIT 1
#define USYNC_DEFER_USR1 2
#define USYNC_DEFER_INT  4

/* Called by Wine's own handlers after their architecture-specific setup. */
BOOL usync_defer_signal( struct thread_data *data, int signal )
{
#ifdef USYNC_USE_DEFERRED_SIGNALS
    sig_atomic_t pending;

    if (!data || !data->usync_lock_depth) return FALSE;
    switch (signal)
    {
    case SIGQUIT: pending = USYNC_DEFER_QUIT; break;
    case SIGUSR1: pending = USYNC_DEFER_USR1; break;
    case SIGINT:  pending = USYNC_DEFER_INT; break;
    default: return FALSE;
    }
    data->usync_pending_signals |= pending;
    return TRUE;
#else
    (void)data;
    (void)signal;
    return FALSE;
#endif
}

#ifdef USYNC_USE_DEFERRED_SIGNALS
static void __attribute__((cold,noinline)) usync_replay_signals( struct thread_data *data )
{
    int saved_errno = errno;
    sig_atomic_t pending;
    sigset_t previous;

    if (pthread_sigmask( SIG_BLOCK, &server_block_set, &previous )) abort();
    pending = data->usync_pending_signals;
    data->usync_pending_signals = 0;
    TRACE( "USYNC signal gate replay pending %#x.\n", pending );
    if ((pending & USYNC_DEFER_QUIT) && raise( SIGQUIT )) abort();
    if ((pending & USYNC_DEFER_USR1) && raise( SIGUSR1 )) abort();
    if ((pending & USYNC_DEFER_INT) && raise( SIGINT )) abort();
    if (pthread_sigmask( SIG_SETMASK, &previous, NULL )) abort();
    errno = saved_errno;
}
#endif

/* Wine binds thread_data before entering Nt APIs. A native caller without
 * that data retains the masked path, including protection of lazy TLS access.
 * A scope must include required wakes as well as the shared state update. */
static void __attribute__((cold,noinline)) usync_enter_masked_scope(void)
{
    sigset_t previous;

    pthread_sigmask( SIG_BLOCK, &server_block_set, &previous );
    if (!usync_lock_depth) usync_lock_sigset = previous;
    usync_lock_depth++;
}

static void __attribute__((cold,noinline)) usync_leave_masked_scope(void)
{
    if (!--usync_lock_depth)
    {
        sigset_t previous = usync_lock_sigset;

        pthread_sigmask( SIG_SETMASK, &previous, NULL );
    }
}

/* The token belongs to this operation and uses Wine's existing thread data. */
static inline struct thread_data *usync_enter_scope(void)
{
#ifdef USYNC_USE_DEFERRED_SIGNALS
    struct thread_data *data = get_thread_data();

    if (data)
    {
        if (data->usync_lock_depth == INT_MAX) abort();
        data->usync_lock_depth++;
        return data;
    }
#endif
    usync_enter_masked_scope();
    return NULL;
}

static inline void usync_leave_scope( struct thread_data *data )
{
#ifdef USYNC_USE_DEFERRED_SIGNALS
    if (data)
    {
        if (data->usync_lock_depth <= 0) abort();
        if (!--data->usync_lock_depth && data->usync_pending_signals)
            usync_replay_signals( data );
        return;
    }
#else
    (void)data;
#endif
    usync_leave_masked_scope();
}

/* Legacy scoped helpers retain their existing contracts. */
static void usync_enter_lock_scope(void)
{
    (void)usync_enter_scope();
}

static void usync_leave_lock_scope(void)
{
#ifdef USYNC_USE_DEFERRED_SIGNALS
    struct thread_data *data = get_thread_data();

    if (data && data->usync_lock_depth)
    {
        usync_leave_scope( data );
        return;
    }
#endif
    usync_leave_masked_scope();
}

static void lock_usync_cache_in_scope(void)
{
#ifdef __APPLE__
    os_unfair_lock_lock( &usync_cache_lock );
#else
    pthread_mutex_lock( &usync_cache_mutex );
#endif
}

static void lock_usync_cache(void)
{
    usync_enter_lock_scope();
    lock_usync_cache_in_scope();
}

static void unlock_usync_cache_in_scope(void)
{
#ifdef __APPLE__
    os_unfair_lock_unlock( &usync_cache_lock );
#else
    pthread_mutex_unlock( &usync_cache_mutex );
#endif
}

static void unlock_usync_cache(void)
{
    unlock_usync_cache_in_scope();
    usync_leave_lock_scope();
}

int do_usync(void)
{
    if (do_usync_env_cached == -1)
    {
        const char *env = getenv( "WINEUSYNC" );
        do_usync_env_cached = (env && atoi( env )) ? 1 : 0;
    }

    return do_usync_env_cached;
}

static long get_page_size(void)
{
    if (!usync_page_size)
    {
        long size = sysconf( _SC_PAGESIZE );

        if (size <= 0 || size > UINT_MAX || size % sizeof(struct usync_shared_state)) return 0;
        usync_page_size = size;
    }
    return usync_page_size;
}

static BOOL usync_handle_to_index( HANDLE handle, unsigned int *entry, unsigned int *idx )
{
    unsigned int handle_idx;

    if ((ULONG)(ULONG_PTR)handle >= 0xfffffffa) return FALSE;

    handle_idx = (wine_server_obj_handle( handle ) >> 2) - 1;
    *entry = handle_idx / USYNC_CACHE_BLOCK_SIZE;
    *idx = handle_idx % USYNC_CACHE_BLOCK_SIZE;
    return *entry < USYNC_CACHE_ENTRIES;
}

static BOOL usync_supported_type( int type )
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

static BOOL get_usync_cache_entry( HANDLE handle, struct usync_cache_entry **entry )
{
    unsigned int block, idx;

    if (!usync_handle_to_index( handle, &block, &idx )) return FALSE;
    if (!usync_cache[block])
        usync_cache[block] = calloc( USYNC_CACHE_BLOCK_SIZE, sizeof(**usync_cache) );
    if (!usync_cache[block]) return FALSE;

    *entry = &usync_cache[block][idx];
    return TRUE;
}

static unsigned char usync_type_to_kind( int type )
{
    switch (type)
    {
    case INPROC_SYNC_EVENT:
        return USYNC_KIND_EVENT;
    case INPROC_SYNC_MUTEX:
        return USYNC_KIND_MUTEX;
    case INPROC_SYNC_SEMAPHORE:
        return USYNC_KIND_SEMAPHORE;
    default:
        return USYNC_KIND_UNKNOWN;
    }
}

static void usync_set_handle_kind( HANDLE handle, unsigned char kind )
{
    unsigned int block, idx;
    unsigned char *table, *expected;

    if (!usync_handle_to_index( handle, &block, &idx )) return;
    table = __atomic_load_n( &usync_handle_kinds[block], __ATOMIC_ACQUIRE );
    if (!table)
    {
        table = block ? calloc( USYNC_CACHE_BLOCK_SIZE, sizeof(*table) ) : usync_handle_kinds_initial;
        if (!table) return;
        expected = NULL;
        if (!__atomic_compare_exchange_n( &usync_handle_kinds[block], &expected, table, FALSE,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE ))
        {
            if (block) free( table );
            table = expected;
        }
    }

    __atomic_store_n( &table[idx], kind, __ATOMIC_RELEASE );
}

static unsigned char usync_get_handle_kind( HANDLE handle )
{
    unsigned int block, idx;
    unsigned char *table;

    if (!usync_handle_to_index( handle, &block, &idx )) return USYNC_KIND_UNKNOWN;
    if (!(table = __atomic_load_n( &usync_handle_kinds[block], __ATOMIC_ACQUIRE )))
        return USYNC_KIND_UNKNOWN;

    return __atomic_load_n( &table[idx], __ATOMIC_ACQUIRE );
}

static BOOL usync_shm_idx_signal_unsafe( unsigned int shm_idx )
{
    if (!shm_idx || shm_idx >= usync_signal_unsafe_indexes_count) return FALSE;
    return __atomic_load_n( &usync_signal_unsafe_indexes[shm_idx], __ATOMIC_ACQUIRE );
}

static struct usync_shared_state *map_usync_state( unsigned int shm_idx, int fd )
{
    unsigned int page, offset, max_pages;
    uint64_t byte_offset = (uint64_t)shm_idx * sizeof(struct usync_shared_state);
    long size = get_page_size();
    struct stat st;
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

    if (size <= 0 || size > UINT_MAX || size % sizeof(struct usync_shared_state) ||
        fstat( fd, &st ) == -1 || st.st_size <= 0 || st.st_size > UINT_MAX || st.st_size % size ||
        byte_offset + sizeof(struct usync_shared_state) > (uint64_t)st.st_size)
    {
        TRACE( "fallback shm_idx %u invalid shared memory bounds\n", shm_idx );
        return NULL;
    }
    page = byte_offset / size;
    offset = byte_offset % size;
    max_pages = st.st_size / size;

    if (page >= usync_pages_count)
    {
        unsigned int new_count = usync_pages_count ? usync_pages_count * 2 : 16;

        while (page >= new_count)
        {
            if (new_count > max_pages / 2)
            {
                new_count = max_pages;
                break;
            }
            new_count *= 2;
        }
        if (new_count > max_pages) new_count = max_pages;
        if (SIZE_MAX / new_count < sizeof(*new_pages)) return NULL;
        if (!(new_pages = realloc( usync_pages, new_count * sizeof(*usync_pages) )))
        {
            TRACE( "fallback shm_idx %u page %u page-cache allocation failed\n", shm_idx, page );
            return NULL;
        }
        memset( new_pages + usync_pages_count, 0, (new_count - usync_pages_count) * sizeof(*usync_pages) );
        usync_pages = new_pages;
        usync_pages_count = new_count;
    }

    if (!usync_pages[page])
    {
        ptr = mmap( NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)page * size );
        if (ptr == MAP_FAILED)
        {
            TRACE( "fallback shm_idx %u page %u mmap failed errno %d\n", shm_idx, page, errno );
            return NULL;
        }
        usync_pages[page] = ptr;
    }

    return (struct usync_shared_state *)((char *)usync_pages[page] + offset);
}

void usync_mark_local_event( HANDLE handle )
{
    struct usync_cache_entry *entry;

    if (!do_usync()) return;

    lock_usync_cache();
    if (get_usync_cache_entry( handle, &entry ))
    {
        if (entry->handle && entry->handle != handle) memset( entry, 0, sizeof(*entry) );
        entry->handle = handle;
        entry->local_event = 1;
        usync_set_handle_kind( handle, USYNC_KIND_EVENT );
    }
    else TRACE( "fallback handle %p local event marker allocation failed\n", handle );
    unlock_usync_cache();
}

BOOL usync_handle_is_local_event( HANDLE handle )
{
    return usync_get_handle_kind( handle ) == USYNC_KIND_EVENT;
}

BOOL usync_handle_is_local_mutex( HANDLE handle )
{
    return usync_get_handle_kind( handle ) == USYNC_KIND_MUTEX;
}

BOOL usync_handle_is_local_semaphore( HANDLE handle )
{
    return usync_get_handle_kind( handle ) == USYNC_KIND_SEMAPHORE;
}

void usync_mark_local_mutex( HANDLE handle )
{
    struct usync_cache_entry *entry;

    if (!do_usync()) return;

    lock_usync_cache();
    if (get_usync_cache_entry( handle, &entry ))
    {
        if (entry->handle && entry->handle != handle) memset( entry, 0, sizeof(*entry) );
        entry->handle = handle;
        entry->local_mutex = 1;
        usync_set_handle_kind( handle, USYNC_KIND_MUTEX );
    }
    else TRACE( "fallback handle %p local mutex marker allocation failed\n", handle );
    unlock_usync_cache();
}

void usync_mark_local_semaphore( HANDLE handle )
{
    struct usync_cache_entry *entry;

    if (!do_usync()) return;

    lock_usync_cache();
    if (get_usync_cache_entry( handle, &entry ))
    {
        if (entry->handle && entry->handle != handle) memset( entry, 0, sizeof(*entry) );
        entry->handle = handle;
        entry->local_semaphore = 1;
        usync_set_handle_kind( handle, USYNC_KIND_SEMAPHORE );
    }
    else TRACE( "fallback handle %p local semaphore marker allocation failed\n", handle );
    unlock_usync_cache();
}

BOOL usync_get_cached_state( HANDLE handle, struct usync_fast_state *state )
{
    unsigned int entry, idx;
    BOOL ret = FALSE;

    if (!do_usync()) return FALSE;

    if (!usync_handle_to_index( handle, &entry, &idx ))
    {
        TRACE( "unsupported object handle %p; falling back to Wine server\n", handle );
        return FALSE;
    }

    lock_usync_cache();
    if (usync_cache[entry] && usync_cache[entry][idx].valid &&
        usync_cache[entry][idx].handle == handle)
    {
        if (state) *state = usync_cache[entry][idx].state;
        ret = TRUE;
    }
    unlock_usync_cache();

    return ret;
}

static NTSTATUS get_local_event_state_locked( HANDLE handle, ACCESS_MASK access,
                                              struct usync_cache_entry **entry )
{
    struct usync_cache_entry *cache;

    if (!get_usync_cache_entry( handle, &cache )) return STATUS_NOT_IMPLEMENTED;
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
                                                  struct usync_cache_entry **entry )
{
    struct usync_cache_entry *cache;

    if (!get_usync_cache_entry( handle, &cache )) return STATUS_NOT_IMPLEMENTED;
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
                                              struct usync_cache_entry **entry )
{
    struct usync_cache_entry *cache;

    if (!get_usync_cache_entry( handle, &cache )) return STATUS_NOT_IMPLEMENTED;
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

void usync_cache_inproc_sync( HANDLE handle, int type, unsigned int access, unsigned int shm_idx, int fd )
{
    unsigned int entry_idx, idx;
    int local_event = 0;
    int local_mutex = 0;
    int local_semaphore = 0;
    int signal_unsafe = 0;
    struct usync_shared_state *shared;
    struct usync_cache_entry *entry;

    if (!do_usync()) return;

    if (!shm_idx)
    {
        TRACE( "fallback handle %p type %d access %#x has no USync shm_idx\n", handle, type, access );
        return;
    }
    if (!usync_supported_type( type ))
    {
        TRACE( "unsupported object handle %p type %d shm_idx %u; falling back to Wine server\n",
               handle, type, shm_idx );
        return;
    }
    if (!usync_handle_to_index( handle, &entry_idx, &idx ))
    {
        TRACE( "fallback handle %p shm_idx %u outside USync cache range\n", handle, shm_idx );
        return;
    }

    lock_usync_cache();
    if (!(shared = map_usync_state( shm_idx, fd )))
    {
        unlock_usync_cache();
        return;
    }
    if (usync_shared_fd < 0) usync_shared_fd = dup( fd );

    if (get_usync_cache_entry( handle, &entry ))
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
        entry->signal_unsafe = signal_unsafe || usync_shm_idx_signal_unsafe( shm_idx );
        entry->state.type = type;
        entry->state.access = access;
        entry->state.shm_idx = shm_idx;
        entry->state.ptr = shared;
        entry->state.generation = shared->generation;
        entry->valid = 1;
        usync_set_handle_kind( handle, usync_type_to_kind( type ) );
    }
    else TRACE( "fallback handle %p shm_idx %u USync cache allocation failed\n", handle, shm_idx );
    unlock_usync_cache();
}

void usync_drop_handle( HANDLE handle )
{
    unsigned int entry, idx;

    if (!do_usync()) return;
    if (!usync_handle_to_index( handle, &entry, &idx )) return;

    usync_set_handle_kind( handle, USYNC_KIND_UNKNOWN );

    lock_usync_cache();
    if (usync_cache[entry] && usync_cache[entry][idx].handle == handle)
    {
        memset( &usync_cache[entry][idx], 0, sizeof(usync_cache[entry][idx]) );
    }
    unlock_usync_cache();
}

void usync_note_fallback( HANDLE handle, NTSTATUS status )
{
    if (!do_usync()) return;
    if (status == STATUS_NOT_IMPLEMENTED) usync_set_handle_kind( handle, USYNC_KIND_UNSUPPORTED );
}

static BOOL usync_address_wait_supported(void)
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

static NTSTATUS usync_wait_address( int *addr, int value, const struct timespec *timeout )
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

static int usync_atomic_load( int *addr )
{
    return __atomic_load_n( addr, __ATOMIC_SEQ_CST );
}

static unsigned short usync_state_type( struct usync_shared_state *state )
{
    return __atomic_load_n( &state->type, __ATOMIC_SEQ_CST ) & USYNC_TYPE_MASK;
}

static BOOL usync_try_lock_state( struct usync_shared_state *state, unsigned short expected_type )
{
    unsigned short type = __atomic_load_n( &state->type, __ATOMIC_SEQ_CST );

    if ((type & USYNC_TYPE_MASK) != expected_type ||
        (type & (USYNC_STATE_LOCKED | USYNC_SERVER_WAITERS)))
        return FALSE;

    return __atomic_compare_exchange_n( &state->type, &type, type | USYNC_STATE_LOCKED, FALSE,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST );
}

static BOOL usync_lock_state_in_scope( struct usync_shared_state *state, unsigned short expected_type )
{
    unsigned int attempts = 0;
    unsigned short type;

    for (;;)
    {
        if (usync_try_lock_state( state, expected_type )) return TRUE;
        type = __atomic_load_n( &state->type, __ATOMIC_SEQ_CST );
        if ((type & USYNC_TYPE_MASK) != expected_type || (type & USYNC_SERVER_WAITERS))
        {
            return FALSE;
        }
        if (!(++attempts % 64)) sched_yield();
    }
}

static BOOL usync_lock_state( struct usync_shared_state *state, unsigned short expected_type )
{
    usync_enter_lock_scope();
    if (usync_lock_state_in_scope( state, expected_type )) return TRUE;
    usync_leave_lock_scope();
    return FALSE;
}

static void usync_unlock_state_in_scope( struct usync_shared_state *state )
{
    __atomic_fetch_and( &state->type, ~USYNC_STATE_LOCKED, __ATOMIC_SEQ_CST );
}

static void usync_unlock_state( struct usync_shared_state *state )
{
    usync_unlock_state_in_scope( state );
    usync_leave_lock_scope();
}

/* The established direct owner must finish even after another thread joins
 * a standard server queue. Unregistered consumers retain the normal lock. */
static BOOL usync_lock_direct_state( struct usync_shared_state *state, unsigned short expected )
{
    unsigned short type;
    unsigned int attempts = 0;

    usync_enter_lock_scope();
    for (;;)
    {
        type = __atomic_load_n( &state->type, __ATOMIC_SEQ_CST );
        if ((type & USYNC_TYPE_MASK) != expected)
        {
            usync_leave_lock_scope();
            return FALSE;
        }
        if (!(type & USYNC_STATE_LOCKED) &&
            __atomic_compare_exchange_n( &state->type, &type, type | USYNC_STATE_LOCKED, FALSE,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST )) return TRUE;
        if (!(++attempts % 64)) sched_yield();
    }
}

static BOOL usync_has_server_waiters( struct usync_shared_state *state )
{
    return !!(__atomic_load_n( &state->type, __ATOMIC_SEQ_CST ) &
              (USYNC_SERVER_WAITERS | USYNC_MULTI_WAITERS));
}

static BOOL usync_has_wine_server_waiters( struct usync_shared_state *state )
{
    return !!(__atomic_load_n( &state->type, __ATOMIC_SEQ_CST ) & USYNC_SERVER_WAITERS);
}

static BOOL usync_has_multi_waiters( struct usync_shared_state *state )
{
    return !!(__atomic_load_n( &state->type, __ATOMIC_SEQ_CST ) & USYNC_MULTI_WAITERS);
}

static BOOL usync_entry_signal_hotpath_enabled( const char *op, HANDLE handle,
                                                  struct usync_cache_entry *entry )
{
    if (!entry->signal_unsafe && !usync_shm_idx_signal_unsafe( entry->state.shm_idx )) return TRUE;
    entry->signal_unsafe = 1;
    TRACE( "fallback %s handle %p shm_idx %u; object used by multi-wait\n",
           op, handle, entry->state.shm_idx );
    return FALSE;
}

static BOOL usync_handle_kind_matches( HANDLE handle, unsigned char kind )
{
    return usync_get_handle_kind( handle ) == kind;
}

static BOOL usync_atomic_compare_exchange( int *addr, int expected, int desired )
{
    return __atomic_compare_exchange_n( addr, &expected, desired, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST );
}

static void usync_wake_address( int *addr, BOOL wake_all )
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

static void usync_wake_waiters( unsigned int shm_idx )
{
    SERVER_START_REQ( wake_usync_waiters )
    {
        req->shm_idx = shm_idx;
        wine_server_call( req );
    }
    SERVER_END_REQ;
}

/* Keep the logical scope until all notifications are complete. Physical
 * locks are released before either a kernel wake or a server request. */
static void usync_complete_state_update_in_scope( struct usync_shared_state *state, unsigned int shm_idx,
                                          int *address, BOOL wake, BOOL wake_all, BOOL wake_server )
{
    __atomic_fetch_and( &state->type, ~USYNC_STATE_LOCKED, __ATOMIC_SEQ_CST );
    if (wake) usync_wake_address( address, wake_all );
    if (wake_server) usync_wake_waiters( shm_idx );
}

static void usync_complete_state_update( struct usync_shared_state *state, unsigned int shm_idx,
                                          int *address, BOOL wake, BOOL wake_all, BOOL wake_server )
{
    usync_complete_state_update_in_scope( state, shm_idx, address, wake, wake_all, wake_server );
    usync_leave_lock_scope();
}

/* Called with the state lock held. The server sets UNWAITED on new Events.
 * A direct waiter clears it before rechecking. It stays clear until slot reuse,
 * so timeout and thread exit need no cleanup. Older servers leave it clear and
 * retain unconditional wakes. The permanent Event protocol marker keeps older
 * clients on the server path even after UNWAITED is cleared. prev_state refers
 * to local storage. The public caller publishes its output after wake work. */
static int usync_set_event_state_in_scope( struct usync_shared_state *state, unsigned int shm_idx, LONG *prev_state )
{
    int previous = __atomic_exchange_n( &state->low, 1, __ATOMIC_SEQ_CST );
    BOOL wake = !(__atomic_load_n( &state->type, __ATOMIC_SEQ_CST ) & USYNC_EVENT_UNWAITED);

    if (prev_state) *prev_state = previous;
    usync_complete_state_update_in_scope( state, shm_idx, &state->low, !previous && wake,
                                  state->high != 0, !previous && usync_has_multi_waiters( state ) );
    return previous;
}

static int usync_set_event_state( struct usync_shared_state *state, unsigned int shm_idx, LONG *prev_state )
{
    int previous = usync_set_event_state_in_scope( state, shm_idx, prev_state );

    usync_leave_lock_scope();
    return previous;
}

/* Called under the cache lock. A cached page was already bounds-checked by
 * map_usync_state() and remains mapped for the client lifetime. Slot identity
 * and generation are still checked under the Event lock before publication. */
static struct usync_shared_state *map_direct_event_owner( unsigned int idx )
{
    unsigned int page, offset;
    long size = get_page_size();

    if (!idx || idx > USYNC_DIRECT_MAX_SLOT || size <= 0) return NULL;
    page = (idx * sizeof(struct usync_shared_state)) / size;
    offset = (idx * sizeof(struct usync_shared_state)) % size;
    if (page < usync_pages_count && usync_pages[page])
        return (struct usync_shared_state *)((char *)usync_pages[page] + offset);
    return map_usync_state( idx, usync_shared_fd );
}

static NTSTATUS usync_set_event_in_scope( HANDLE handle, LONG *prev_state )
{
    struct usync_cache_entry *entry;
    struct usync_shared_state *state, *direct_slot = NULL;
    unsigned int generation, shm_idx, owner = 0;
    BOOL direct_protocol;
    int previous;
    NTSTATUS ret;

    lock_usync_cache_in_scope();
    if ((ret = get_local_event_state_locked( handle, EVENT_MODIFY_STATE, &entry )))
    {
        unlock_usync_cache_in_scope();
        return ret;
    }
    state = entry->state.ptr;
    generation = entry->state.generation;
    shm_idx = entry->state.shm_idx;
    if (!usync_entry_signal_hotpath_enabled( "event set", handle, entry ))
    {
        unlock_usync_cache_in_scope();
        return STATUS_NOT_IMPLEMENTED;
    }
    if (usync_has_server_waiters( state ))
    {
        TRACE( "fallback event set handle %p has server waiters\n", handle );
        unlock_usync_cache_in_scope();
        return STATUS_NOT_IMPLEMENTED;
    }
    direct_protocol = !!(state->type & USYNC_EVENT_DIRECT_PROTOCOL);
    if (direct_protocol) owner = __atomic_load_n( &state->refcount, __ATOMIC_SEQ_CST ) >> 1;
    if (owner && !(direct_slot = map_direct_event_owner( owner )))
    {
        unlock_usync_cache_in_scope();
        return STATUS_NOT_IMPLEMENTED;
    }
    unlock_usync_cache_in_scope();

    if (!usync_lock_state_in_scope( state, INPROC_SYNC_EVENT ))
    {
        TRACE( "fallback event set handle %p stale or server-owned state\n", handle );
        return STATUS_NOT_IMPLEMENTED;
    }
    if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation ||
        usync_has_server_waiters( state ))
    {
        usync_unlock_state_in_scope( state );
        TRACE( "fallback event set handle %p stale or server-owned state\n", handle );
        return STATUS_NOT_IMPLEMENTED;
    }
    if (direct_protocol)
    {
        unsigned int word = 0;
        BOOL wake = FALSE;

        if ((__atomic_load_n( &state->refcount, __ATOMIC_SEQ_CST ) >> 1) != owner ||
            (owner && (usync_state_type( direct_slot ) != INPROC_SYNC_INTERNAL ||
                       usync_atomic_load( &direct_slot->high ) != (int)shm_idx ||
                       ((unsigned int)usync_atomic_load( &direct_slot->low ) >> 2) !=
                       __atomic_load_n( &direct_slot->generation, __ATOMIC_SEQ_CST ))))
        {
            usync_unlock_state_in_scope( state );
            return STATUS_NOT_IMPLEMENTED;
        }
        previous = __atomic_exchange_n( &state->low, 1, __ATOMIC_SEQ_CST );
        if (!previous && direct_slot)
        {
            word = usync_atomic_load( &direct_slot->low );
            if (!(word & USYNC_DIRECT_COMPLETED))
            {
                __atomic_store_n( &direct_slot->low, (word & ~USYNC_DIRECT_WAITING) |
                                  USYNC_DIRECT_COMPLETED, __ATOMIC_SEQ_CST );
                if (!state->high) __atomic_store_n( &state->low, 0, __ATOMIC_SEQ_CST );
                wake = TRUE;
            }
        }
        usync_complete_state_update_in_scope( state, shm_idx,
                                      direct_slot ? &direct_slot->low : NULL, wake, FALSE,
                                      !previous && usync_has_multi_waiters( state ) );
    }
    else usync_set_event_state_in_scope( state, shm_idx, &previous );
    if (prev_state) *prev_state = previous;
    return STATUS_SUCCESS;
}

NTSTATUS usync_set_event( HANDLE handle, LONG *prev_state )
{
    struct thread_data *scope;
    LONG previous;
    NTSTATUS ret;

    if (!do_usync()) return STATUS_NOT_IMPLEMENTED;
    if (!usync_handle_kind_matches( handle, USYNC_KIND_EVENT )) return STATUS_NOT_IMPLEMENTED;

    scope = usync_enter_scope();
    ret = usync_set_event_in_scope( handle, &previous );
    usync_leave_scope( scope );
    if (!ret && prev_state) *prev_state = previous;
    return ret;
}

NTSTATUS usync_reset_event( HANDLE handle, LONG *prev_state )
{
    struct usync_cache_entry *entry;
    struct usync_shared_state *state;
    unsigned int generation;
    int previous;
    NTSTATUS ret;

    if (!do_usync()) return STATUS_NOT_IMPLEMENTED;
    if (!usync_handle_kind_matches( handle, USYNC_KIND_EVENT )) return STATUS_NOT_IMPLEMENTED;

    lock_usync_cache();
    if ((ret = get_local_event_state_locked( handle, EVENT_MODIFY_STATE, &entry )))
    {
        unlock_usync_cache();
        return ret;
    }
    state = entry->state.ptr;
    generation = entry->state.generation;
    if (!usync_entry_signal_hotpath_enabled( "event reset", handle, entry ))
    {
        unlock_usync_cache();
        return STATUS_NOT_IMPLEMENTED;
    }
    if (usync_has_server_waiters( state ))
    {
        TRACE( "fallback event reset handle %p has server waiters\n", handle );
        unlock_usync_cache();
        return STATUS_NOT_IMPLEMENTED;
    }
    unlock_usync_cache();

    if (!usync_lock_state( state, INPROC_SYNC_EVENT ))
    {
        TRACE( "fallback event reset handle %p stale or server-owned state\n", handle );
        return STATUS_NOT_IMPLEMENTED;
    }
    if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation ||
        usync_has_server_waiters( state ))
    {
        usync_unlock_state( state );
        TRACE( "fallback event reset handle %p stale or server-owned state\n", handle );
        return STATUS_NOT_IMPLEMENTED;
    }
    previous = __atomic_exchange_n( &state->low, 0, __ATOMIC_SEQ_CST );
    usync_unlock_state( state );
    if (prev_state) *prev_state = previous;
    return STATUS_SUCCESS;
}

NTSTATUS usync_query_event( HANDLE handle, EVENT_BASIC_INFORMATION *info )
{
    struct usync_cache_entry *entry;
    struct usync_shared_state *state;
    EVENT_BASIC_INFORMATION snapshot;
    NTSTATUS ret;

    if (!do_usync()) return STATUS_NOT_IMPLEMENTED;

    lock_usync_cache();
    if (!(ret = get_local_event_state_locked( handle, EVENT_QUERY_STATE, &entry )))
    {
        state = entry->state.ptr;
        snapshot.EventType = state->high ? NotificationEvent : SynchronizationEvent;
        snapshot.EventState = usync_atomic_load( &state->low );
    }
    unlock_usync_cache();

    if (!ret)
    {
        info->EventType = snapshot.EventType;
        info->EventState = snapshot.EventState;
    }
    return ret;
}

static NTSTATUS usync_release_mutex_in_scope( HANDLE handle, int tid, LONG *prev_count )
{
    struct usync_cache_entry *entry;
    struct usync_shared_state *state;
    unsigned int generation, shm_idx;
    int count;
    BOOL wake;
    NTSTATUS ret;

    lock_usync_cache_in_scope();
    if ((ret = get_local_mutex_state_locked( handle, 0, &entry )))
    {
        unlock_usync_cache_in_scope();
        return ret;
    }
    state = entry->state.ptr;
    generation = entry->state.generation;
    shm_idx = entry->state.shm_idx;
    if (usync_has_server_waiters( state ))
    {
        TRACE( "fallback mutex release handle %p has server waiters\n", handle );
        unlock_usync_cache_in_scope();
        return STATUS_NOT_IMPLEMENTED;
    }
    unlock_usync_cache_in_scope();

    if (!usync_lock_state_in_scope( state, INPROC_SYNC_MUTEX ))
    {
        TRACE( "fallback mutex release handle %p stale or server-owned state\n", handle );
        return STATUS_NOT_IMPLEMENTED;
    }
    if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation ||
        usync_has_server_waiters( state ))
    {
        usync_unlock_state_in_scope( state );
        TRACE( "fallback mutex release handle %p stale or server-owned state\n", handle );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (__atomic_load_n( &state->low, __ATOMIC_SEQ_CST ) != tid ||
        !(count = __atomic_load_n( &state->high, __ATOMIC_SEQ_CST )))
    {
        usync_unlock_state_in_scope( state );
        return STATUS_MUTANT_NOT_OWNED;
    }

    if (count > 1)
    {
        __atomic_store_n( &state->high, count - 1, __ATOMIC_SEQ_CST );
        usync_unlock_state_in_scope( state );
    }
    else
    {
        __atomic_store_n( &state->high, 0, __ATOMIC_SEQ_CST );
        __atomic_store_n( &state->low, 0, __ATOMIC_SEQ_CST );
        wake = !(__atomic_load_n( &state->type, __ATOMIC_SEQ_CST ) & USYNC_MUTEX_UNWAITED);
        usync_complete_state_update_in_scope( state, shm_idx, &state->low,
                                      wake, FALSE, usync_has_multi_waiters( state ) );
    }
    if (prev_count) *prev_count = 1 - count;
    return STATUS_SUCCESS;
}

NTSTATUS usync_release_mutex( HANDLE handle, LONG *prev_count )
{
    struct thread_data *scope;
    int tid = GetCurrentThreadId();
    LONG previous;
    NTSTATUS ret;

    if (!do_usync()) return STATUS_NOT_IMPLEMENTED;
    if (!usync_handle_kind_matches( handle, USYNC_KIND_MUTEX )) return STATUS_NOT_IMPLEMENTED;

    scope = usync_enter_scope();
    ret = usync_release_mutex_in_scope( handle, tid, &previous );
    usync_leave_scope( scope );
    if (!ret && prev_count) *prev_count = previous;
    return ret;
}

NTSTATUS usync_query_mutex( HANDLE handle, MUTANT_BASIC_INFORMATION *info )
{
    struct usync_cache_entry *entry;
    struct usync_shared_state *state;
    int owner, count;
    MUTANT_BASIC_INFORMATION snapshot;
    NTSTATUS ret;

    if (!do_usync()) return STATUS_NOT_IMPLEMENTED;
    if (!usync_handle_kind_matches( handle, USYNC_KIND_MUTEX )) return STATUS_NOT_IMPLEMENTED;

    lock_usync_cache();
    if (!(ret = get_local_mutex_state_locked( handle, MUTANT_QUERY_STATE, &entry )))
    {
        state = entry->state.ptr;
        if (!usync_lock_state( state, INPROC_SYNC_MUTEX ))
            ret = STATUS_NOT_IMPLEMENTED;
        else
        {
            owner = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST );
            count = owner < 0 ? 0 : __atomic_load_n( &state->high, __ATOMIC_SEQ_CST );
            snapshot.CurrentCount = 1 - count;
            snapshot.OwnedByCaller = owner == (int)GetCurrentThreadId();
            snapshot.AbandonedState = owner < 0;
            usync_unlock_state( state );
        }
    }
    unlock_usync_cache();

    if (!ret)
    {
        info->CurrentCount = snapshot.CurrentCount;
        info->OwnedByCaller = snapshot.OwnedByCaller;
        info->AbandonedState = snapshot.AbandonedState;
    }
    return ret;
}

/* Called with the state lock held. The permanent protocol bit keeps older
 * clients on the server path even after UNWAITED is cleared. Output arguments
 * refer to local storage. The public caller publishes outputs after wake work. */
static NTSTATUS usync_release_semaphore_state_in_scope( struct usync_shared_state *state, HANDLE handle,
                                                unsigned int shm_idx, ULONG count, ULONG *previous, int *old_count_out )
{
    int old_count, new_count;
    BOOL wake;

    do
    {
        old_count = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST );
        if ((ULONG)old_count + count < (ULONG)old_count || (ULONG)old_count + count > (ULONG)state->high)
        {
            TRACE( "semaphore release handle %p count %u current %d max %d exceeds limit\n",
                   handle, count, old_count, state->high );
            usync_unlock_state_in_scope( state );
            return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
        }
        new_count = old_count + count;
    } while (!__atomic_compare_exchange_n( &state->low, &old_count, new_count, FALSE,
                                           __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ));

    if (previous) *previous = old_count;
    *old_count_out = old_count;
    wake = !(__atomic_load_n( &state->type, __ATOMIC_SEQ_CST ) & USYNC_SEMAPHORE_UNWAITED);
    /* A prior release may have woken a thread which has not consumed yet.
     * New units must also wake sleepers when the old count is still positive. */
    usync_complete_state_update_in_scope( state, shm_idx, &state->low, wake, count > 1,
                                  !old_count && usync_has_multi_waiters( state ) );
    return STATUS_SUCCESS;
}

static NTSTATUS usync_release_semaphore_state( struct usync_shared_state *state, HANDLE handle,
                                                unsigned int shm_idx, ULONG count, ULONG *previous, int *old_count_out )
{
    NTSTATUS ret = usync_release_semaphore_state_in_scope( state, handle, shm_idx, count, previous, old_count_out );

    usync_leave_lock_scope();
    return ret;
}

static NTSTATUS usync_release_semaphore_in_scope( HANDLE handle, ULONG count, ULONG *previous )
{
    struct usync_cache_entry *entry;
    struct usync_shared_state *state;
    unsigned int generation, shm_idx;
    int old_count;
    NTSTATUS ret;

    lock_usync_cache_in_scope();
    if ((ret = get_local_semaphore_state_locked( handle, SEMAPHORE_MODIFY_STATE, &entry )))
    {
        unlock_usync_cache_in_scope();
        return ret;
    }
    state = entry->state.ptr;
    generation = entry->state.generation;
    shm_idx = entry->state.shm_idx;
    if (!usync_entry_signal_hotpath_enabled( "semaphore release", handle, entry ))
    {
        unlock_usync_cache_in_scope();
        return STATUS_NOT_IMPLEMENTED;
    }
    if (usync_has_server_waiters( state ))
    {
        TRACE( "fallback semaphore release handle %p has server waiters\n", handle );
        unlock_usync_cache_in_scope();
        return STATUS_NOT_IMPLEMENTED;
    }
    unlock_usync_cache_in_scope();

    if (!usync_lock_state_in_scope( state, INPROC_SYNC_SEMAPHORE ))
    {
        TRACE( "fallback semaphore release handle %p stale or server-owned state\n", handle );
        return STATUS_NOT_IMPLEMENTED;
    }
    if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation ||
        usync_has_server_waiters( state ))
    {
        usync_unlock_state_in_scope( state );
        TRACE( "fallback semaphore release handle %p stale or server-owned state\n", handle );
        return STATUS_NOT_IMPLEMENTED;
    }

    ret = usync_release_semaphore_state_in_scope( state, handle, shm_idx, count, NULL, &old_count );
    if (ret) return ret;
    if (previous) *previous = old_count;
    return STATUS_SUCCESS;
}

NTSTATUS usync_release_semaphore( HANDLE handle, ULONG count, ULONG *previous )
{
    struct thread_data *scope;
    ULONG old_count;
    NTSTATUS ret;

    if (!do_usync()) return STATUS_NOT_IMPLEMENTED;
    if ((LONG)count <= 0) return STATUS_INVALID_PARAMETER;
    if (!usync_handle_kind_matches( handle, USYNC_KIND_SEMAPHORE )) return STATUS_NOT_IMPLEMENTED;

    scope = usync_enter_scope();
    ret = usync_release_semaphore_in_scope( handle, count, &old_count );
    usync_leave_scope( scope );
    if (!ret && previous) *previous = old_count;
    return ret;
}

NTSTATUS usync_query_semaphore( HANDLE handle, SEMAPHORE_BASIC_INFORMATION *info )
{
    if (!do_usync()) return STATUS_NOT_IMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS consume_auto_event_state( struct usync_shared_state *state )
{
    if (!usync_atomic_load( &state->low )) return STATUS_TIMEOUT;
    if (state->high) return STATUS_NOT_IMPLEMENTED;
    if (!usync_atomic_compare_exchange( &state->low, 1, 0 )) return STATUS_TIMEOUT;
    return STATUS_WAIT_0;
}

static NTSTATUS consume_event_state_in_scope( struct usync_shared_state *state )
{
    NTSTATUS ret;

    if (!usync_lock_state_in_scope( state, INPROC_SYNC_EVENT )) return STATUS_NOT_IMPLEMENTED;
    ret = !usync_atomic_load( &state->low ) ? STATUS_TIMEOUT :
          state->high ? STATUS_WAIT_0 : consume_auto_event_state( state );
    usync_unlock_state_in_scope( state );
    return ret;
}

static NTSTATUS consume_event_state( struct usync_shared_state *state )
{
    NTSTATUS ret;

    if (!usync_lock_state( state, INPROC_SYNC_EVENT )) return STATUS_NOT_IMPLEMENTED;
    ret = !usync_atomic_load( &state->low ) ? STATUS_TIMEOUT :
          state->high ? STATUS_WAIT_0 : consume_auto_event_state( state );
    usync_unlock_state( state );
    return ret;
}

static NTSTATUS usync_prepare_event_wait( struct usync_shared_state *state, unsigned int generation )
{
    NTSTATUS ret;

    if (!usync_lock_state( state, INPROC_SYNC_EVENT )) return STATUS_NOT_IMPLEMENTED;
    if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation ||
        usync_has_server_waiters( state ))
    {
        usync_unlock_state( state );
        return STATUS_NOT_IMPLEMENTED;
    }
    /* Publish before rechecking under the same lock used by SetEvent. If the
     * signal follows this check it wakes us, or the kernel's value comparison
     * prevents sleeping after the signal. Other waiters may consume an auto
     * Event first, in which case this waiter is still entitled to wait. */
    if (__atomic_load_n( &state->type, __ATOMIC_SEQ_CST ) & USYNC_EVENT_UNWAITED)
        __atomic_fetch_and( &state->type, (unsigned short)~USYNC_EVENT_UNWAITED, __ATOMIC_SEQ_CST );
    ret = !usync_atomic_load( &state->low ) ? STATUS_TIMEOUT :
          state->high ? STATUS_WAIT_0 : consume_auto_event_state( state );
    usync_unlock_state( state );
    return ret;
}

static NTSTATUS consume_semaphore_state_locked( struct usync_shared_state *state )
{
    int count;

    do
    {
        count = usync_atomic_load( &state->low );
        if (count <= 0)
        {
            return STATUS_TIMEOUT;
        }
    } while (!__atomic_compare_exchange_n( &state->low, &count, count - 1, FALSE,
                                           __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ));

    return STATUS_WAIT_0;
}

static NTSTATUS consume_semaphore_state_in_scope( struct usync_shared_state *state )
{
    NTSTATUS ret;

    if (!usync_lock_state_in_scope( state, INPROC_SYNC_SEMAPHORE )) return STATUS_NOT_IMPLEMENTED;
    ret = consume_semaphore_state_locked( state );
    usync_unlock_state_in_scope( state );
    return ret;
}

static NTSTATUS consume_semaphore_state( struct usync_shared_state *state )
{
    NTSTATUS ret;

    if (!usync_lock_state( state, INPROC_SYNC_SEMAPHORE )) return STATUS_NOT_IMPLEMENTED;
    ret = consume_semaphore_state_locked( state );
    usync_unlock_state( state );
    return ret;
}

/* The lock order is thread slot, then Event. Set/Pulse only take the Event
 * lock and publish to the owner's slot atomically. They never take its lock. */
static NTSTATUS usync_register_direct_event( struct usync_shared_state *state,
                                              unsigned int event_idx, unsigned int generation )
{
    NTSTATUS ret = STATUS_NOT_IMPLEMENTED;

    if (!generation || usync_event_slot_generation > USYNC_DIRECT_MAX_GENERATION || !usync_wait_slot_idx ||
        usync_wait_slot_idx > USYNC_DIRECT_MAX_SLOT) return ret;
    if (!usync_lock_direct_state( usync_wait_slot, INPROC_SYNC_INTERNAL )) return ret;
    if (usync_wait_slot->generation != usync_event_slot_generation ||
        usync_wait_slot->high || usync_atomic_load( &usync_wait_slot->low ))
        goto done_slot;
    if (!usync_lock_direct_state( state, INPROC_SYNC_EVENT )) goto done_slot;
    if (state->generation != generation || !(state->type & USYNC_EVENT_DIRECT_PROTOCOL) ||
        usync_has_server_waiters( state )) goto done_event;
    if (state->low)
    {
        if (!state->high) __atomic_store_n( &state->low, 0, __ATOMIC_SEQ_CST );
        ret = STATUS_WAIT_0;
        goto done_event;
    }
    if (__atomic_load_n( &state->refcount, __ATOMIC_SEQ_CST ) != 1) goto done_event;
    __atomic_store_n( &usync_wait_slot->low, (usync_event_slot_generation << 2) | USYNC_DIRECT_WAITING, __ATOMIC_SEQ_CST );
    __atomic_store_n( &usync_wait_slot->high, event_idx, __ATOMIC_SEQ_CST );
    __atomic_store_n( &state->refcount, (usync_wait_slot_idx << 1) | 1, __ATOMIC_SEQ_CST );
    __atomic_fetch_and( &state->type, (unsigned short)~USYNC_EVENT_UNWAITED, __ATOMIC_SEQ_CST );
    TRACE( "direct Event registered handle-index %u slot %u generation %u\n",
           event_idx, usync_wait_slot_idx, generation );
    ret = STATUS_PENDING;
done_event:
    usync_unlock_state( state );
done_slot:
    usync_unlock_state( usync_wait_slot );
    return ret;
}


static NTSTATUS usync_check_direct_event( struct usync_shared_state *state,
                                           unsigned int generation, NTSTATUS cancel, int *comparison )
{
    NTSTATUS ret = STATUS_NOT_IMPLEMENTED;
    BOOL event_locked = FALSE;
    unsigned int word;

    if (!usync_lock_direct_state( usync_wait_slot, INPROC_SYNC_INTERNAL )) return ret;
    if (usync_wait_slot->generation != usync_event_slot_generation) goto done_slot;
    if (!(event_locked = usync_lock_direct_state( state, INPROC_SYNC_EVENT ))) goto clear_slot;
    if (state->generation != generation ||
        (__atomic_load_n( &state->refcount, __ATOMIC_SEQ_CST ) >> 1) != usync_wait_slot_idx)
        goto clear_slot;
    word = usync_atomic_load( &usync_wait_slot->low );
    if ((word >> 2) != usync_event_slot_generation) ret = STATUS_NOT_IMPLEMENTED;
    else if (word & USYNC_DIRECT_COMPLETED) ret = STATUS_WAIT_0;
    else if (state->low)
    {
        if (!state->high) __atomic_store_n( &state->low, 0, __ATOMIC_SEQ_CST );
        ret = STATUS_WAIT_0;
    }
    else ret = cancel;
    if (ret == STATUS_PENDING)
    {
        *comparison = (usync_event_slot_generation << 2) | USYNC_DIRECT_WAITING;
        __atomic_store_n( &usync_wait_slot->low, *comparison, __ATOMIC_SEQ_CST );
        goto done_event;
    }
    __atomic_store_n( &state->refcount, 1, __ATOMIC_SEQ_CST );
clear_slot:
    __atomic_store_n( &usync_wait_slot->high, 0, __ATOMIC_SEQ_CST );
    __atomic_store_n( &usync_wait_slot->low, 0, __ATOMIC_SEQ_CST );
done_event:
    if (event_locked) usync_unlock_state( state );
done_slot:
    usync_unlock_state( usync_wait_slot );
    return ret;
}


static NTSTATUS usync_prepare_semaphore_wait( struct usync_shared_state *state, unsigned int generation )
{
    NTSTATUS ret;

    if (!usync_lock_state( state, INPROC_SYNC_SEMAPHORE )) return STATUS_NOT_IMPLEMENTED;
    if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation ||
        usync_has_server_waiters( state ))
    {
        usync_unlock_state( state );
        return STATUS_NOT_IMPLEMENTED;
    }
    /* Register before the last count check, under the release lock. Keep the
     * flag clear until slot reuse so timeout and thread exit need no cleanup. */
    if (__atomic_load_n( &state->type, __ATOMIC_SEQ_CST ) & USYNC_SEMAPHORE_UNWAITED)
        __atomic_fetch_and( &state->type, (unsigned short)~USYNC_SEMAPHORE_UNWAITED, __ATOMIC_SEQ_CST );
    ret = consume_semaphore_state_locked( state );
    usync_unlock_state( state );
    return ret;
}

static NTSTATUS consume_mutex_state_locked( struct usync_shared_state *state, int tid )
{
    int owner, count;

    owner = usync_atomic_load( &state->low );
    count = usync_atomic_load( &state->high );

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
            unsigned int generation = __atomic_load_n( &state->generation, __ATOMIC_SEQ_CST );

            /* Saturate so the server can retire this slot instead of reusing a wrapped generation. */
            if (generation != UINT_MAX)
                __atomic_store_n( &state->generation, generation + 1, __ATOMIC_SEQ_CST );
            return STATUS_ABANDONED;
        }
        return STATUS_WAIT_0;
    }

    return STATUS_TIMEOUT;
}

static NTSTATUS consume_mutex_state_in_scope( struct usync_shared_state *state, int tid )
{
    NTSTATUS ret;

    if (!usync_lock_state_in_scope( state, INPROC_SYNC_MUTEX )) return STATUS_NOT_IMPLEMENTED;
    ret = consume_mutex_state_locked( state, tid );
    usync_unlock_state_in_scope( state );
    return ret;
}

static NTSTATUS consume_mutex_state( struct usync_shared_state *state, int tid )
{
    NTSTATUS ret;

    if (!usync_lock_state( state, INPROC_SYNC_MUTEX )) return STATUS_NOT_IMPLEMENTED;
    ret = consume_mutex_state_locked( state, tid );
    usync_unlock_state( state );
    return ret;
}

/* Publish possible sleep before the final ownership check. Capture the owner
 * under that lock so a release cannot make us wait on an already free mutex. */
static NTSTATUS usync_prepare_mutex_wait( struct usync_shared_state *state, unsigned int generation,
                                           int tid, int *owner )
{
    NTSTATUS ret;

    if (!usync_lock_state( state, INPROC_SYNC_MUTEX )) return STATUS_NOT_IMPLEMENTED;
    if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation ||
        usync_has_server_waiters( state ))
    {
        usync_unlock_state( state );
        return STATUS_NOT_IMPLEMENTED;
    }
    /* Keep UNWAITED clear until slot reuse, including timeout and thread exit.
     * The permanent protocol bit keeps older wait implementations on the server. */
    if (__atomic_load_n( &state->type, __ATOMIC_SEQ_CST ) & USYNC_MUTEX_UNWAITED)
        __atomic_fetch_and( &state->type, (unsigned short)~USYNC_MUTEX_UNWAITED, __ATOMIC_SEQ_CST );
    ret = consume_mutex_state_locked( state, tid );
    *owner = __atomic_load_n( &state->low, __ATOMIC_SEQ_CST );
    usync_unlock_state( state );
    return ret;
}

static void usync_refresh_cache_generation_in_scope( HANDLE handle, struct usync_shared_state *state )
{
    struct usync_cache_entry *entry;

    lock_usync_cache_in_scope();
    if (get_usync_cache_entry( handle, &entry ) && entry->handle == handle &&
        entry->valid && entry->state.ptr == state)
        entry->state.generation = __atomic_load_n( &state->generation, __ATOMIC_SEQ_CST );
    unlock_usync_cache_in_scope();
}

static void usync_refresh_cache_generation( HANDLE handle, struct usync_shared_state *state )
{
    struct thread_data *scope = usync_enter_scope();

    usync_refresh_cache_generation_in_scope( handle, state );
    usync_leave_scope( scope );
}

static void get_now_abs( struct timespec *now );

static void get_relative_timeout_abs( const LARGE_INTEGER *timeout, struct timespec *abstime )
{
    ULONGLONG ticks = 0 - (ULONGLONG)timeout->QuadPart;
    struct timespec now;
    ULONGLONG nsec;

    get_now_abs( &now );
    nsec = now.tv_nsec + (ticks % USYNC_TICKS_PER_SEC) * 100;
    abstime->tv_sec = now.tv_sec + ticks / USYNC_TICKS_PER_SEC + nsec / 1000000000;
    abstime->tv_nsec = nsec % 1000000000;
}

static void get_now_abs( struct timespec *now )
{
#ifdef __APPLE__
    mach_timebase_info_data_t timebase;
    __uint128_t nsec;

    mach_timebase_info( &timebase );
    nsec = (__uint128_t)mach_continuous_time() * timebase.numer / timebase.denom;
    now->tv_sec = nsec / 1000000000;
    now->tv_nsec = nsec % 1000000000;
#elif defined(HAVE_CLOCK_GETTIME)
    clock_gettime( CLOCK_MONOTONIC, now );
#else
    struct timeval tv;

    gettimeofday( &tv, NULL );
    now->tv_sec = tv.tv_sec;
    now->tv_nsec = tv.tv_usec * 1000;
#endif
}

static int compare_timespec( const struct timespec *left, const struct timespec *right )
{
    if (left->tv_sec != right->tv_sec) return left->tv_sec < right->tv_sec ? -1 : 1;
    if (left->tv_nsec != right->tv_nsec) return left->tv_nsec < right->tv_nsec ? -1 : 1;
    return 0;
}

static BOOL usync_adaptive_spin( unsigned int *attempts )
{
    if (*attempts >= USYNC_SPIN_LIMIT) return FALSE;

    (*attempts)++;
    return TRUE;
}

static NTSTATUS usync_try_wait_event_in_scope( HANDLE handle, unsigned char kind,
                                                  struct usync_shared_state **state_out,
                                                  unsigned int *generation_out, unsigned int *event_idx_out )
{
    struct usync_cache_entry *entry;
    struct usync_shared_state *state;
    unsigned int generation, event_idx;
    NTSTATUS ret;

    lock_usync_cache_in_scope();
    if ((ret = get_local_event_state_locked( handle, SYNCHRONIZE, &entry )))
    {
        unlock_usync_cache_in_scope();
        if (ret == STATUS_NOT_IMPLEMENTED && kind == USYNC_KIND_EVENT)
            return STATUS_NOT_IMPLEMENTED;
        return ret;
    }
    state = entry->state.ptr;
    generation = entry->state.generation;
    event_idx = entry->state.shm_idx;
    if (!usync_entry_signal_hotpath_enabled( "event wait", handle, entry ))
    {
        unlock_usync_cache_in_scope();
        return STATUS_NOT_IMPLEMENTED;
    }
    if (usync_has_server_waiters( state ))
    {
        TRACE( "fallback event wait handle %p has server waiters\n", handle );
        unlock_usync_cache_in_scope();
        return STATUS_NOT_IMPLEMENTED;
    }
    if (!usync_address_wait_supported())
    {
        unlock_usync_cache_in_scope();
        return STATUS_NOT_IMPLEMENTED;
    }
    unlock_usync_cache_in_scope();

    *state_out = state;
    *generation_out = generation;
    *event_idx_out = event_idx;
    return consume_event_state_in_scope( state );
}

NTSTATUS usync_wait_event( HANDLE handle, BOOLEAN alertable, const LARGE_INTEGER *timeout )
{
    struct usync_shared_state *state = NULL;
    struct thread_data *scope;
    unsigned int generation, event_idx;
    struct timespec deadline;
    unsigned char kind;
    int comparison;
    NTSTATUS ret;

    if (!do_usync()) return STATUS_NOT_IMPLEMENTED;

    kind = usync_get_handle_kind( handle );
    if (kind && kind != USYNC_KIND_EVENT) return STATUS_NOT_IMPLEMENTED;
    if (alertable)
        return kind == USYNC_KIND_EVENT ? STATUS_NOT_IMPLEMENTED
                                          : STATUS_NOT_IMPLEMENTED;
    if (timeout && timeout->QuadPart > 0)
        return kind == USYNC_KIND_EVENT ? STATUS_NOT_IMPLEMENTED
                                          : STATUS_NOT_IMPLEMENTED;

    scope = usync_enter_scope();
    ret = usync_try_wait_event_in_scope( handle, kind, &state, &generation, &event_idx );
    usync_leave_scope( scope );

    if (ret == STATUS_WAIT_0)
    {
        return ret;
    }
    if (ret != STATUS_TIMEOUT || !state) return ret;
    if (timeout && !timeout->QuadPart)
    {
        return STATUS_TIMEOUT;
    }

    if (!(state->type & USYNC_EVENT_DIRECT_PROTOCOL))
        return STATUS_NOT_IMPLEMENTED;
    if (timeout) get_relative_timeout_abs( timeout, &deadline );
    if ((ret = get_direct_event_wait_slot())) return ret;
    if ((ret = usync_register_direct_event( state, event_idx, generation )) != STATUS_PENDING) return ret;
    /* Registration checked the level under the Event lock before publishing
     * this generation-tagged word. A completion before the kernel call changes
     * the word, so the address comparison prevents sleeping past that signal. */
    comparison = (usync_event_slot_generation << 2) | USYNC_DIRECT_WAITING;
    for (;;)
    {
        struct timespec now, wait_timeout;
        struct timespec *wait_timeout_ptr = NULL;
        unsigned long long ns;

        if (timeout)
        {
            get_now_abs( &now );
            ns = timespec_diff_ns( &deadline, &now );
            if (!ns) return usync_check_direct_event( state, generation, STATUS_TIMEOUT, &comparison );
            ns_to_timespec( ns, &wait_timeout );
            wait_timeout_ptr = &wait_timeout;
        }
        TRACE( "direct Event sleeping index %u slot %u comparison %d\n", event_idx, usync_wait_slot_idx, comparison );
        ret = usync_wait_address( &usync_wait_slot->low, comparison, wait_timeout_ptr );
        ret = usync_check_direct_event( state, generation,
                                           ret == STATUS_SUCCESS ? STATUS_PENDING : ret, &comparison );
        if (ret != STATUS_PENDING) return ret;
    }
}

static NTSTATUS usync_try_wait_mutex_in_scope( HANDLE handle, unsigned char kind, int tid,
                                                  struct usync_shared_state **state_out,
                                                  unsigned int *generation_out )
{
    struct usync_cache_entry *entry;
    struct usync_shared_state *state;
    unsigned int generation;
    NTSTATUS ret;

    lock_usync_cache_in_scope();
    if ((ret = get_local_mutex_state_locked( handle, SYNCHRONIZE, &entry )))
    {
        unlock_usync_cache_in_scope();
        if (ret == STATUS_NOT_IMPLEMENTED && kind == USYNC_KIND_MUTEX)
            return STATUS_NOT_IMPLEMENTED;
        return ret;
    }
    state = entry->state.ptr;
    generation = entry->state.generation;
    if (usync_has_server_waiters( state ))
    {
        TRACE( "fallback mutex wait handle %p has server waiters\n", handle );
        unlock_usync_cache_in_scope();
        return STATUS_NOT_IMPLEMENTED;
    }
    if (!usync_address_wait_supported())
    {
        unlock_usync_cache_in_scope();
        return STATUS_NOT_IMPLEMENTED;
    }
    unlock_usync_cache_in_scope();

    *state_out = state;
    *generation_out = generation;
    ret = consume_mutex_state_in_scope( state, tid );
    if (ret == STATUS_ABANDONED) usync_refresh_cache_generation_in_scope( handle, state );
    return ret;
}

NTSTATUS usync_wait_mutex( HANDLE handle, BOOLEAN alertable, const LARGE_INTEGER *timeout )
{
    struct usync_shared_state *state = NULL;
    struct thread_data *scope;
    struct timespec deadline;
    unsigned int attempts = 0;
    unsigned int generation;
    unsigned char kind;
    int tid = GetCurrentThreadId();
    NTSTATUS ret;

    if (!do_usync()) return STATUS_NOT_IMPLEMENTED;

    kind = usync_get_handle_kind( handle );
    if (kind && kind != USYNC_KIND_MUTEX) return STATUS_NOT_IMPLEMENTED;
    if (alertable)
        return kind == USYNC_KIND_MUTEX ? STATUS_NOT_IMPLEMENTED
                                          : STATUS_NOT_IMPLEMENTED;
    if (timeout && timeout->QuadPart > 0)
        return kind == USYNC_KIND_MUTEX ? STATUS_NOT_IMPLEMENTED
                                          : STATUS_NOT_IMPLEMENTED;

    scope = usync_enter_scope();
    ret = usync_try_wait_mutex_in_scope( handle, kind, tid, &state, &generation );
    usync_leave_scope( scope );

    if (ret == STATUS_WAIT_0 || ret == STATUS_ABANDONED)
    {
        return ret;
    }
    if (ret != STATUS_TIMEOUT || !state) return ret;
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

        if (usync_adaptive_spin( &attempts )) continue;
        if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation ||
            usync_state_type( state ) != INPROC_SYNC_MUTEX ||
            usync_has_server_waiters( state ))
        {
            TRACE( "fallback mutex wait handle %p stale or server-owned state\n", handle );
            return STATUS_NOT_IMPLEMENTED;
        }
        ret = usync_prepare_mutex_wait( state, generation, tid, &owner );
        if (ret == STATUS_WAIT_0 || ret == STATUS_ABANDONED)
        {
            if (ret == STATUS_ABANDONED) usync_refresh_cache_generation( handle, state );
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
            ns_to_timespec( USYNC_BLOCKING_FALLBACK_NS, &wait_timeout );
            wait_timeout_ptr = &wait_timeout;
        }

        ret = usync_wait_address( &state->low, owner, wait_timeout_ptr );
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

static NTSTATUS usync_try_wait_semaphore_in_scope( HANDLE handle, unsigned char kind,
                                                  struct usync_shared_state **state_out,
                                                  unsigned int *generation_out )
{
    struct usync_cache_entry *entry;
    struct usync_shared_state *state;
    unsigned int generation;
    NTSTATUS ret;

    lock_usync_cache_in_scope();
    if ((ret = get_local_semaphore_state_locked( handle, SYNCHRONIZE, &entry )))
    {
        unlock_usync_cache_in_scope();
        if (ret == STATUS_NOT_IMPLEMENTED && kind == USYNC_KIND_SEMAPHORE)
            return STATUS_NOT_IMPLEMENTED;
        return ret;
    }
    state = entry->state.ptr;
    generation = entry->state.generation;
    if (!usync_entry_signal_hotpath_enabled( "semaphore wait", handle, entry ))
    {
        unlock_usync_cache_in_scope();
        return STATUS_NOT_IMPLEMENTED;
    }
    if (usync_has_server_waiters( state ))
    {
        TRACE( "fallback semaphore wait handle %p has server waiters\n", handle );
        unlock_usync_cache_in_scope();
        return STATUS_NOT_IMPLEMENTED;
    }
    if (!usync_address_wait_supported())
    {
        unlock_usync_cache_in_scope();
        return STATUS_NOT_IMPLEMENTED;
    }
    unlock_usync_cache_in_scope();

    *state_out = state;
    *generation_out = generation;
    return consume_semaphore_state_in_scope( state );
}

NTSTATUS usync_wait_semaphore( HANDLE handle, BOOLEAN alertable, const LARGE_INTEGER *timeout )
{
    struct usync_shared_state *state = NULL;
    struct thread_data *scope;
    struct timespec deadline;
    unsigned int attempts = 0;
    unsigned int generation;
    unsigned char kind;
    NTSTATUS ret;

    if (!do_usync()) return STATUS_NOT_IMPLEMENTED;

    kind = usync_get_handle_kind( handle );
    if (kind && kind != USYNC_KIND_SEMAPHORE) return STATUS_NOT_IMPLEMENTED;
    if (alertable)
        return kind == USYNC_KIND_SEMAPHORE ? STATUS_NOT_IMPLEMENTED
                                              : STATUS_NOT_IMPLEMENTED;
    if (timeout && timeout->QuadPart > 0)
        return kind == USYNC_KIND_SEMAPHORE ? STATUS_NOT_IMPLEMENTED
                                              : STATUS_NOT_IMPLEMENTED;

    scope = usync_enter_scope();
    ret = usync_try_wait_semaphore_in_scope( handle, kind, &state, &generation );
    usync_leave_scope( scope );

    if (ret == STATUS_WAIT_0)
    {
        return ret;
    }
    if (ret != STATUS_TIMEOUT || !state) return ret;
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

        if (usync_adaptive_spin( &attempts )) continue;
        if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != generation ||
            usync_state_type( state ) != INPROC_SYNC_SEMAPHORE ||
            usync_has_server_waiters( state ))
        {
            TRACE( "fallback semaphore wait handle %p stale or server-owned state\n", handle );
            return STATUS_NOT_IMPLEMENTED;
        }
        if ((ret = usync_prepare_semaphore_wait( state, generation )) == STATUS_WAIT_0) break;
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
            ns_to_timespec( USYNC_BLOCKING_FALLBACK_NS, &wait_timeout );
            wait_timeout_ptr = &wait_timeout;
        }
        ret = usync_wait_address( &state->low, 0, wait_timeout_ptr );
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

static NTSTATUS get_usync_wait_slot(void)
{
    NTSTATUS ret = STATUS_SUCCESS;

    if (usync_wait_slot) return STATUS_SUCCESS;
    if (usync_shared_fd < 0)
    {
        TRACE( "fallback multiple wait no shared fd for wait slot\n" );
        return STATUS_NOT_IMPLEMENTED;
    }

    SERVER_START_REQ( get_usync_wait_slot )
    {
        ret = wine_server_call( req );
        if (!ret) usync_wait_slot_idx = reply->shm_idx;
    }
    SERVER_END_REQ;

    if (ret) return STATUS_NOT_IMPLEMENTED;
    if (!usync_wait_slot_idx) return STATUS_NOT_IMPLEMENTED;

    lock_usync_cache();
    usync_wait_slot = map_usync_state( usync_wait_slot_idx, usync_shared_fd );
    unlock_usync_cache();

    return usync_wait_slot ? STATUS_SUCCESS : STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS get_direct_event_wait_slot(void)
{
    unsigned int generation, confirmed = 0;
    NTSTATUS ret;

    if (usync_event_slot_generation) return STATUS_SUCCESS;
    if ((ret = get_usync_wait_slot())) return ret;
    if (usync_wait_slot_idx > USYNC_DIRECT_MAX_SLOT)
    {
        TRACE( "fallback direct Event wait slot %u exceeds owner index limit %u\n",
               usync_wait_slot_idx, USYNC_DIRECT_MAX_SLOT );
        return STATUS_NOT_IMPLEMENTED;
    }
    generation = __atomic_load_n( &usync_wait_slot->generation, __ATOMIC_SEQ_CST );
    SERVER_START_REQ( get_usync_wait_slot )
    {
        ret = wine_server_call( req );
        if (!ret) confirmed = reply->shm_idx;
    }
    SERVER_END_REQ;
    if (ret || confirmed != usync_wait_slot_idx || !generation || generation > USYNC_DIRECT_MAX_GENERATION ||
        __atomic_load_n( &usync_wait_slot->generation, __ATOMIC_SEQ_CST ) != generation ||
        usync_state_type( usync_wait_slot ) != INPROC_SYNC_INTERNAL) return STATUS_NOT_IMPLEMENTED;
    usync_event_slot_generation = generation;
    return STATUS_SUCCESS;
}

static NTSTATUS register_usync_wait( const unsigned int *shm_idxs, unsigned int count )
{
    NTSTATUS ret;

    SERVER_START_REQ( register_usync_wait )
    {
        req->slot_idx = usync_wait_slot_idx;
        wine_server_add_data( req, shm_idxs, count * sizeof(*shm_idxs) );
        ret = wine_server_call( req );
    }
    SERVER_END_REQ;

    return ret;
}

static void unregister_usync_wait(void)
{
    SERVER_START_REQ( unregister_usync_wait )
    {
        req->slot_idx = usync_wait_slot_idx;
        wine_server_call( req );
    }
    SERVER_END_REQ;
}

static NTSTATUS try_wait_all_local_in_scope( struct usync_shared_state **states,
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
        if (!usync_lock_state_in_scope( states[index], types[index] )) goto done;
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
    while (locked) usync_unlock_state_in_scope( states[order[--locked]] );
    return ret;
}

static NTSTATUS try_wait_all_local( struct usync_shared_state **states,
                                    const unsigned int *shm_idxs,
                                    const unsigned int *generations,
                                    const unsigned short *types, DWORD count )
{
    struct thread_data *scope = usync_enter_scope();
    NTSTATUS ret = try_wait_all_local_in_scope( states, shm_idxs, generations, types, count );

    usync_leave_scope( scope );
    return ret;
}

static NTSTATUS try_wait_all_usync( const unsigned int *entries, DWORD count, unsigned int slot_idx )
{
    NTSTATUS ret;
    SERVER_START_REQ( try_wait_all_usync )
    {
        req->tid = GetCurrentThreadId();
        req->slot_idx = slot_idx;
        wine_server_add_data( req, entries, count * 3 * sizeof(*entries) );
        ret = wine_server_call( req );
    }
    SERVER_END_REQ;
    return ret;
}

static NTSTATUS consume_waitany_states_in_scope( const HANDLE *handles, struct usync_shared_state **states,
                                        const unsigned int *generations, const unsigned short *types,
                                        DWORD count )
{
    int tid = GetCurrentThreadId();
    DWORD i;
    NTSTATUS ret;

    for (i = 0; i < count; i++)
    {
        if (__atomic_load_n( &states[i]->generation, __ATOMIC_SEQ_CST ) != generations[i] ||
            usync_state_type( states[i] ) != types[i])
        {
            TRACE( "fallback multiple wait stale state index %u\n", i );
            return STATUS_NOT_IMPLEMENTED;
        }

        switch (types[i])
        {
        case INPROC_SYNC_EVENT:
            ret = consume_event_state_in_scope( states[i] );
            break;
        case INPROC_SYNC_SEMAPHORE:
            ret = consume_semaphore_state_in_scope( states[i] );
            break;
        case INPROC_SYNC_MUTEX:
            ret = consume_mutex_state_in_scope( states[i], tid );
            if (ret == STATUS_ABANDONED) usync_refresh_cache_generation_in_scope( handles[i], states[i] );
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

static NTSTATUS consume_waitany_states( const HANDLE *handles, struct usync_shared_state **states,
                                        const unsigned int *generations, const unsigned short *types,
                                        DWORD count )
{
    int tid = GetCurrentThreadId();
    DWORD i;
    NTSTATUS ret;

    for (i = 0; i < count; i++)
    {
        if (__atomic_load_n( &states[i]->generation, __ATOMIC_SEQ_CST ) != generations[i] ||
            usync_state_type( states[i] ) != types[i])
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
            if (ret == STATUS_ABANDONED) usync_refresh_cache_generation( handles[i], states[i] );
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

static BOOL usync_waitall_ret_satisfied( NTSTATUS ret, DWORD count )
{
    return ret == STATUS_WAIT_0 || (ret >= STATUS_ABANDONED_WAIT_0 && ret < STATUS_ABANDONED_WAIT_0 + count);
}

static void refresh_waitall_mutex_generations( const HANDLE *handles, struct usync_shared_state **states,
                                               const unsigned short *types, DWORD count )
{
    DWORD i;

    for (i = 0; i < count; i++)
        if (types[i] == INPROC_SYNC_MUTEX) usync_refresh_cache_generation( handles[i], states[i] );
}

/* All arrays and outputs here are private copies owned by the public wrapper.
 * Return cache preparation separately from the initial wait result so a cache
 * rejection cannot accidentally enter the later server fallback. */
static NTSTATUS usync_try_wait_multiple_in_scope( DWORD count, const HANDLE *handles, WAIT_TYPE type,
                                                   struct usync_shared_state **states,
                                                   unsigned int *shm_idxs, unsigned int *generations,
                                                   unsigned short *types, BOOL *has_event_out,
                                                   NTSTATUS *wait_result_out )
{
    struct usync_cache_entry *entry;
    DWORD i;
    NTSTATUS ret;

    lock_usync_cache_in_scope();
    for (i = 0; i < count; i++)
    {
        struct usync_shared_state *state;
        unsigned char kind = usync_get_handle_kind( handles[i] );

        switch (kind)
        {
        case USYNC_KIND_EVENT:
            ret = get_local_event_state_locked( handles[i], SYNCHRONIZE, &entry );
            break;
        case USYNC_KIND_SEMAPHORE:
            ret = get_local_semaphore_state_locked( handles[i], SYNCHRONIZE, &entry );
            break;
        case USYNC_KIND_MUTEX:
            ret = get_local_mutex_state_locked( handles[i], SYNCHRONIZE, &entry );
            break;
        default:
            TRACE( "fallback multiple wait handle %p unknown kind %u\n", handles[i], kind );
            ret = STATUS_NOT_IMPLEMENTED;
            break;
        }
        if (ret)
        {
            unlock_usync_cache_in_scope();
            if (ret == STATUS_NOT_IMPLEMENTED)
                return STATUS_NOT_IMPLEMENTED;
            return ret;
        }
        state = entry->state.ptr;
        if (__atomic_load_n( &state->generation, __ATOMIC_SEQ_CST ) != entry->state.generation)
        {
            if (entry->state.type == INPROC_SYNC_MUTEX &&
                usync_state_type( state ) == INPROC_SYNC_MUTEX && usync_atomic_load( &state->low ) < 0)
                entry->state.generation = __atomic_load_n( &state->generation, __ATOMIC_SEQ_CST );
            else
            {
                TRACE( "fallback multiple wait handle %p has stale generation\n", handles[i] );
                unlock_usync_cache_in_scope();
                return STATUS_NOT_IMPLEMENTED;
            }
        }
        if (type == WaitAll ? usync_has_wine_server_waiters( state ) : usync_has_server_waiters( state ))
        {
            TRACE( "fallback multiple wait handle %p has server waiters\n", handles[i] );
            unlock_usync_cache_in_scope();
            return STATUS_NOT_IMPLEMENTED;
        }
        states[i] = state;
        shm_idxs[i] = entry->state.shm_idx;
        generations[i] = entry->state.generation;
        types[i] = entry->state.type;
        if (types[i] == INPROC_SYNC_EVENT) *has_event_out = TRUE;
    }
    unlock_usync_cache_in_scope();

    *wait_result_out = type == WaitAll
        ? try_wait_all_local_in_scope( states, shm_idxs, generations, types, count )
        : consume_waitany_states_in_scope( handles, states, generations, types, count );
    return STATUS_SUCCESS;
}

NTSTATUS usync_wait_multiple( DWORD count, const HANDLE *handles, WAIT_TYPE type,
                                BOOLEAN alertable, const LARGE_INTEGER *timeout )
{
    struct usync_shared_state *states[64];
    HANDLE local_handles[ARRAY_SIZE(states)];
    struct thread_data *scope;
    unsigned int shm_idxs[64], generations[64];
    unsigned short types[64];
    struct timespec deadline;
    DWORD i;
    BOOL has_event = FALSE;
    NTSTATUS ret, initial_ret;

    if (!do_usync()) return STATUS_NOT_IMPLEMENTED;

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
    if (type == WaitAny && !usync_address_wait_supported())
    {
        TRACE( "fallback waitany address wait unsupported\n" );
        return STATUS_NOT_IMPLEMENTED;
    }

    /* Copy caller memory before protecting internal state. All later paths,
     * including abandoned refresh and blocking fallback, use this snapshot. */
    memcpy( local_handles, handles, count * sizeof(*local_handles) );
    scope = usync_enter_scope();
    ret = usync_try_wait_multiple_in_scope( count, local_handles, type, states, shm_idxs,
                                             generations, types, &has_event, &initial_ret );
    usync_leave_scope( scope );
    if (ret) return ret;
    ret = initial_ret;

    if (type == WaitAll)
    {
        unsigned int entries[ARRAY_SIZE(states) * 3];

        if (ret == STATUS_WAIT_0 || ret == STATUS_ABANDONED_WAIT_0)
        {
            if (ret == STATUS_ABANDONED_WAIT_0)
                refresh_waitall_mutex_generations( local_handles, states, types, count );
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
            ret = try_wait_all_usync( entries, count, 0 );
            if (usync_waitall_ret_satisfied( ret, count ))
            {
                if (ret >= STATUS_ABANDONED_WAIT_0)
                    refresh_waitall_mutex_generations( local_handles, states, types, count );
                return ret;
            }
            if (ret == STATUS_TIMEOUT)
            {
                return STATUS_TIMEOUT;
            }
            return ret == STATUS_NOT_IMPLEMENTED ? STATUS_NOT_IMPLEMENTED : ret;
        }

        if (has_event)
        {
            TRACE( "fallback blocking Event WaitAll for PulseEvent completion\n" );
            return STATUS_NOT_IMPLEMENTED;
        }
        if ((ret = get_usync_wait_slot()))
            return ret == STATUS_NOT_IMPLEMENTED ? STATUS_NOT_IMPLEMENTED : ret;
        if (timeout) get_relative_timeout_abs( timeout, &deadline );

        for (;;)
        {
            struct timespec now, wait_timeout;
            struct timespec *wait_timeout_ptr = NULL;
            unsigned long long ns;

            ret = try_wait_all_usync( entries, count, usync_wait_slot_idx );
            if (usync_waitall_ret_satisfied( ret, count ))
            {
                if (ret >= STATUS_ABANDONED_WAIT_0)
                    refresh_waitall_mutex_generations( local_handles, states, types, count );
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
                    unregister_usync_wait();
                    return STATUS_TIMEOUT;
                }
                ns_to_timespec( ns, &wait_timeout );
                wait_timeout_ptr = &wait_timeout;
            }
            ret = usync_wait_address( &usync_wait_slot->low, 1, wait_timeout_ptr );

            if (ret == STATUS_TIMEOUT)
            {
                unregister_usync_wait();
                return STATUS_TIMEOUT;
            }
            if (ret == STATUS_NOT_IMPLEMENTED)
            {
                unregister_usync_wait();
                return STATUS_NOT_IMPLEMENTED;
            }
            if (ret != STATUS_SUCCESS)
            {
                unregister_usync_wait();
                return ret;
            }
        }
    }

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

    if (has_event)
    {
        TRACE( "fallback blocking Event WaitAny for PulseEvent completion\n" );
        return STATUS_NOT_IMPLEMENTED;
    }
    if ((ret = get_usync_wait_slot()))
        return ret == STATUS_NOT_IMPLEMENTED ? STATUS_NOT_IMPLEMENTED : ret;
    if (timeout) get_relative_timeout_abs( timeout, &deadline );

    for (;;)
    {
        struct timespec now, wait_timeout;
        struct timespec *wait_timeout_ptr = NULL;
        unsigned long long ns;

        if ((ret = register_usync_wait( shm_idxs, count )))
            return ret == STATUS_NOT_IMPLEMENTED ? STATUS_NOT_IMPLEMENTED : ret;

        ret = consume_waitany_states( local_handles, states, generations, types, count );
        if (ret != STATUS_TIMEOUT)
        {
            unregister_usync_wait();
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
                unregister_usync_wait();
                return STATUS_TIMEOUT;
            }
            ns_to_timespec( ns, &wait_timeout );
            wait_timeout_ptr = &wait_timeout;
        }
        else
        {
            ns_to_timespec( USYNC_BLOCKING_FALLBACK_NS, &wait_timeout );
            wait_timeout_ptr = &wait_timeout;
        }
        ret = usync_wait_address( &usync_wait_slot->low, 1, wait_timeout_ptr );
        unregister_usync_wait();

        if (ret == STATUS_TIMEOUT)
        {
            if (!timeout) return STATUS_NOT_IMPLEMENTED;
            return STATUS_TIMEOUT;
        }
        if (ret == STATUS_NOT_IMPLEMENTED)
            return STATUS_NOT_IMPLEMENTED;
        if (ret != STATUS_SUCCESS) return ret;

        ret = consume_waitany_states( local_handles, states, generations, types, count );
        if (ret != STATUS_TIMEOUT)
        {
            if (ret == STATUS_NOT_IMPLEMENTED)
                return STATUS_NOT_IMPLEMENTED;
            return ret;
        }
    }
}
