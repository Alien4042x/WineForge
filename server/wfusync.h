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

extern int do_wfusync(void);

struct wfusync;

#ifdef __APPLE__

extern int wfusync_get_shared_fd(void);
extern struct wfusync *create_wfusync( int low, int high, enum inproc_sync_type type );
extern unsigned int wfusync_get_index( struct wfusync *sync );
extern unsigned int wfusync_alloc_wait_slot(void);
extern int wfusync_register_wait( unsigned int slot_idx, const unsigned int *shm_idxs, unsigned int count );
extern void wfusync_unregister_wait( unsigned int slot_idx );
extern int wfusync_wake_waiters( unsigned int shm_idx );
extern void wfusync_grab_object( struct wfusync *sync );
extern void wfusync_destroy( struct wfusync *sync );
extern void wfusync_add_server_waiter( struct wfusync *sync );
extern void wfusync_remove_server_waiter( struct wfusync *sync );
extern int wfusync_get_event_state( struct wfusync *sync, int *manual, int *signaled );
extern int wfusync_event_signaled( struct wfusync *sync );
extern void wfusync_event_satisfied( struct wfusync *sync );
extern void wfusync_set_event( struct wfusync *sync );
extern void wfusync_reset_event( struct wfusync *sync );
extern int wfusync_semaphore_signaled( struct wfusync *sync );
extern void wfusync_semaphore_satisfied( struct wfusync *sync );
extern int wfusync_release_semaphore( struct wfusync *sync, unsigned int count, unsigned int *prev );
extern int wfusync_get_semaphore_state( struct wfusync *sync, unsigned int *count, unsigned int *max );
extern int wfusync_mutex_signaled( struct wfusync *sync, thread_id_t tid );
extern int wfusync_mutex_satisfied( struct wfusync *sync, thread_id_t tid, int *abandoned );
extern int wfusync_release_mutex( struct wfusync *sync, thread_id_t tid, unsigned int *prev );
extern int wfusync_get_mutex_state( struct wfusync *sync, thread_id_t tid, unsigned int *count,
                                    int *owned, int *abandoned );
extern int wfusync_abandon_mutex( struct wfusync *sync, thread_id_t tid );
extern unsigned int wfusync_try_wait_all( const unsigned int *entries, unsigned int count, thread_id_t tid );

#endif /* __APPLE__ */
