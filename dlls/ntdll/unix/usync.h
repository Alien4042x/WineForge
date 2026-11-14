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

#ifndef __WINE_NTDLL_UNIX_USYNC_H
#define __WINE_NTDLL_UNIX_USYNC_H

#include "ntstatus.h"
#include "windef.h"

struct usync_fast_state
{
    int type;
    unsigned int access;
    unsigned int shm_idx;
    void *ptr;
    unsigned int generation;
};

extern int do_usync(void);
extern void usync_mark_local_event( HANDLE handle );
extern void usync_mark_local_mutex( HANDLE handle );
extern void usync_mark_local_semaphore( HANDLE handle );
extern BOOL usync_handle_is_local_event( HANDLE handle );
extern BOOL usync_handle_is_local_mutex( HANDLE handle );
extern BOOL usync_handle_is_local_semaphore( HANDLE handle );
extern BOOL usync_get_cached_state( HANDLE handle, struct usync_fast_state *state );
extern void usync_cache_inproc_sync( HANDLE handle, int type, unsigned int access, unsigned int shm_idx, int fd );
extern void usync_drop_handle( HANDLE handle );
extern void usync_note_fallback( HANDLE handle, NTSTATUS status );
extern NTSTATUS usync_set_event( HANDLE handle, LONG *prev_state );
extern NTSTATUS usync_reset_event( HANDLE handle, LONG *prev_state );
extern NTSTATUS usync_query_event( HANDLE handle, EVENT_BASIC_INFORMATION *info );
extern NTSTATUS usync_wait_event( HANDLE handle, BOOLEAN alertable, const LARGE_INTEGER *timeout );
extern NTSTATUS usync_release_mutex( HANDLE handle, LONG *prev_count );
extern NTSTATUS usync_query_mutex( HANDLE handle, MUTANT_BASIC_INFORMATION *info );
extern NTSTATUS usync_wait_mutex( HANDLE handle, BOOLEAN alertable, const LARGE_INTEGER *timeout );
extern NTSTATUS usync_wait_multiple( DWORD count, const HANDLE *handles, WAIT_TYPE type,
                                       BOOLEAN alertable, const LARGE_INTEGER *timeout );
extern NTSTATUS usync_release_semaphore( HANDLE handle, ULONG count, ULONG *previous );
extern NTSTATUS usync_query_semaphore( HANDLE handle, SEMAPHORE_BASIC_INFORMATION *info );
extern NTSTATUS usync_wait_semaphore( HANDLE handle, BOOLEAN alertable, const LARGE_INTEGER *timeout );

#endif
