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

extern int do_usync(void);

struct usync;
struct thread;

#ifdef __APPLE__

extern int usync_get_shared_fd(void);
extern struct usync *create_usync( int low, int high, enum inproc_sync_type type );
extern unsigned int usync_get_index( struct usync *sync );
extern unsigned int usync_alloc_wait_slot(void);
extern int usync_register_wait( unsigned int slot_idx, const unsigned int *shm_idxs, unsigned int count );
extern void usync_unregister_wait( unsigned int slot_idx );
extern void usync_cleanup_thread( struct thread *thread );
extern int usync_wake_waiters( unsigned int shm_idx );
extern void usync_destroy( struct usync *sync );
extern void usync_add_server_waiter( struct usync *sync );
extern void usync_remove_server_waiter( struct usync *sync );
extern int usync_get_event_state( struct usync *sync, int *manual, int *signaled );
extern int usync_event_signaled( struct usync *sync );
extern void usync_event_satisfied( struct usync *sync );
extern void usync_set_event( struct usync *sync );
extern void usync_reset_event( struct usync *sync );
extern int usync_event_op( struct usync *sync, int signal, int *previous );
extern int usync_event_pulse_begin( struct usync *sync, int *previous );
extern void usync_event_pulse_end( struct usync *sync );
extern int usync_semaphore_signaled( struct usync *sync );
extern void usync_semaphore_satisfied( struct usync *sync );
extern int usync_release_semaphore( struct usync *sync, unsigned int count, unsigned int *prev );
extern int usync_get_semaphore_state( struct usync *sync, unsigned int *count, unsigned int *max );
extern int usync_mutex_signaled( struct usync *sync, thread_id_t tid );
extern int usync_mutex_satisfied( struct usync *sync, thread_id_t tid, int *abandoned );
extern int usync_release_mutex( struct usync *sync, thread_id_t tid, unsigned int *prev );
extern int usync_get_mutex_state( struct usync *sync, thread_id_t tid, unsigned int *count,
                                    int *owned, int *abandoned );
extern int usync_abandon_mutex( struct usync *sync, thread_id_t tid );
extern unsigned int usync_try_wait_all( const unsigned int *entries, unsigned int count, thread_id_t tid );

#endif /* __APPLE__ */
