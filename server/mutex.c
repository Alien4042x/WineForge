/*
 * Server-side mutex management
 *
 * Copyright (C) 1998 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/types.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"

#include "handle.h"
#include "thread.h"
#include "request.h"
#include "security.h"
#include "wfusync.h"

static const WCHAR mutex_name[] = {'M','u','t','a','n','t'};

struct type_descr mutex_type =
{
    { mutex_name, sizeof(mutex_name) },   /* name */
    MUTANT_ALL_ACCESS,                    /* valid_access */
    {                                     /* mapping */
        STANDARD_RIGHTS_READ | MUTANT_QUERY_STATE,
        STANDARD_RIGHTS_WRITE,
        STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
        MUTANT_ALL_ACCESS
    },
};

struct mutex_sync
{
    struct object       obj;                /* object header */
    struct thread      *owner;              /* mutex owner */
    unsigned int        count;              /* recursion count */
    int                 abandoned;          /* has it been abandoned? */
    struct list         entry;              /* entry in owner thread mutex list */
    struct list         wfusync_entry;      /* entry in WFUSync mutex list */
    struct wfusync     *wfusync;            /* optional shared fast-path state */
};

static void mutex_sync_dump( struct object *obj, int verbose );
static int mutex_sync_add_queue( struct object *obj, struct wait_queue_entry *entry );
static void mutex_sync_remove_queue( struct object *obj, struct wait_queue_entry *entry );
static int mutex_sync_signaled( struct object *obj, struct wait_queue_entry *entry );
static void mutex_sync_satisfied( struct object *obj, struct wait_queue_entry *entry );
static void mutex_sync_destroy( struct object *obj );

static struct list wfusync_mutexes = LIST_INIT( wfusync_mutexes );

static const struct object_ops mutex_sync_ops =
{
    sizeof(struct mutex_sync), /* size */
    &no_type,                  /* type */
    mutex_sync_dump,           /* dump */
    mutex_sync_add_queue,      /* add_queue */
    mutex_sync_remove_queue,   /* remove_queue */
    mutex_sync_signaled,       /* signaled */
    mutex_sync_satisfied,      /* satisfied */
    no_signal,                 /* signal */
    no_get_fd,                 /* get_fd */
    default_get_sync,          /* get_sync */
    default_map_access,        /* map_access */
    default_get_sd,            /* get_sd */
    default_set_sd,            /* set_sd */
    default_get_full_name,     /* get_full_name */
    no_lookup_name,            /* lookup_name */
    directory_link_name,       /* link_name */
    default_unlink_name,       /* unlink_name */
    no_open_file,              /* open_file */
    no_kernel_obj_list,        /* get_kernel_obj_list */
    no_close_handle,           /* close_handle */
    mutex_sync_destroy,        /* destroy */
};

/* grab a mutex for a given thread */
static void do_grab( struct mutex_sync *mutex, struct thread *thread )
{
    assert( !mutex->count || (mutex->owner == thread) );

    if (!mutex->count++)  /* FIXME: avoid wrap-around */
    {
        assert( !mutex->owner );
        grab_object( mutex );
        mutex->owner = thread;
        list_add_head( &thread->mutex_list, &mutex->entry );
    }
}

/* release a mutex once the recursion count is 0 */
static int do_release( struct mutex_sync *mutex, struct thread *thread, int count )
{
    if (!mutex->count || (mutex->owner != thread))
    {
        set_error( STATUS_MUTANT_NOT_OWNED );
        return 0;
    }
    if (!(mutex->count -= count))
    {
        /* remove the mutex from the thread list of owned mutexes */
        list_remove( &mutex->entry );
        mutex->owner = NULL;
        wake_up( &mutex->obj, 0 );
        release_object( mutex );
    }
    return 1;
}

static void mutex_sync_dump( struct object *obj, int verbose )
{
    struct mutex_sync *mutex = (struct mutex_sync *)obj;
    assert( obj->ops == &mutex_sync_ops );
    fprintf( stderr, "Mutex count=%u owner=%p\n", mutex->count, mutex->owner );
}

static int mutex_sync_add_queue( struct object *obj, struct wait_queue_entry *entry )
{
    struct mutex_sync *mutex = (struct mutex_sync *)obj;
    assert( obj->ops == &mutex_sync_ops );
    if (mutex->wfusync) wfusync_add_server_waiter( mutex->wfusync );
    return add_queue( obj, entry );
}

static void mutex_sync_remove_queue( struct object *obj, struct wait_queue_entry *entry )
{
    struct mutex_sync *mutex = (struct mutex_sync *)obj;
    assert( obj->ops == &mutex_sync_ops );
    if (mutex->wfusync) wfusync_remove_server_waiter( mutex->wfusync );
    remove_queue( obj, entry );
}

static void mutex_sync_destroy( struct object *obj )
{
    struct mutex_sync *mutex = (struct mutex_sync *)obj;
    assert( obj->ops == &mutex_sync_ops );
    if (mutex->wfusync)
    {
        list_remove( &mutex->wfusync_entry );
        wfusync_destroy( mutex->wfusync );
    }
    assert( !mutex->count );
}

static int mutex_sync_signaled( struct object *obj, struct wait_queue_entry *entry )
{
    struct mutex_sync *mutex = (struct mutex_sync *)obj;
    assert( obj->ops == &mutex_sync_ops );
    if (mutex->wfusync) return wfusync_mutex_signaled( mutex->wfusync, get_wait_queue_thread( entry )->id );
    return (!mutex->count || (mutex->owner == get_wait_queue_thread( entry )));
}

static void mutex_sync_satisfied( struct object *obj, struct wait_queue_entry *entry )
{
    struct mutex_sync *mutex = (struct mutex_sync *)obj;
    int abandoned;

    assert( obj->ops == &mutex_sync_ops );

    if (mutex->wfusync)
    {
        if (wfusync_mutex_satisfied( mutex->wfusync, get_wait_queue_thread( entry )->id, &abandoned ) &&
            abandoned)
            make_wait_abandoned( entry );
        return;
    }

    do_grab( mutex, get_wait_queue_thread( entry ));
    if (mutex->abandoned) make_wait_abandoned( entry );
    mutex->abandoned = 0;
}

static struct object *create_mutex_sync( int owned, int inproc )
{
    struct mutex_sync *mutex;

    if (!(mutex = alloc_object( &mutex_sync_ops ))) return NULL;
    mutex->count = 0;
    mutex->owner = NULL;
    mutex->abandoned = 0;
    list_init( &mutex->wfusync_entry );
    mutex->wfusync = NULL;
    if (inproc && do_wfusync())
    {
        mutex->wfusync = create_wfusync( owned ? current->id : 0, owned ? 1 : 0, INPROC_SYNC_MUTEX );
        if (mutex->wfusync)
        {
            list_add_tail( &wfusync_mutexes, &mutex->wfusync_entry );
            return &mutex->obj;
        }
        clear_error();
    }
    if (owned) do_grab( mutex, current );

    return &mutex->obj;
}

struct mutex
{
    struct object       obj;             /* object header */
    struct object      *sync;            /* mutex sync object */
};

static void mutex_dump( struct object *obj, int verbose );
static struct object *mutex_get_sync( struct object *obj );
static int mutex_signal( struct object *obj, unsigned int access, int signal );
static void mutex_destroy( struct object *obj );

static const struct object_ops mutex_ops =
{
    sizeof(struct mutex),      /* size */
    &mutex_type,               /* type */
    mutex_dump,                /* dump */
    NULL,                      /* add_queue */
    NULL,                      /* remove_queue */
    NULL,                      /* signaled */
    NULL,                      /* satisfied */
    mutex_signal,              /* signal */
    no_get_fd,                 /* get_fd */
    mutex_get_sync,            /* get_sync */
    default_map_access,        /* map_access */
    default_get_sd,            /* get_sd */
    default_set_sd,            /* set_sd */
    default_get_full_name,     /* get_full_name */
    no_lookup_name,            /* lookup_name */
    directory_link_name,       /* link_name */
    default_unlink_name,       /* unlink_name */
    no_open_file,              /* open_file */
    no_kernel_obj_list,        /* get_kernel_obj_list */
    no_close_handle,           /* close_handle */
    mutex_destroy,             /* destroy */
};

static struct mutex *create_mutex( struct object *root, const struct unicode_str *name,
                                   unsigned int attr, int owned, const struct security_descriptor *sd )
{
    struct mutex *mutex;

    if ((mutex = create_named_object( root, &mutex_ops, name, attr, sd )))
    {
        if (get_error() != STATUS_OBJECT_NAME_EXISTS)
        {
            /* initialize it if it didn't already exist */
            mutex->sync = NULL;

            if (!(mutex->sync = create_mutex_sync( owned, 1 )))
            {
                release_object( mutex );
                return NULL;
            }
        }
    }
    return mutex;
}

void abandon_mutexes( struct thread *thread )
{
    struct list *ptr;

    while ((ptr = list_head( &thread->mutex_list )) != NULL)
    {
        struct mutex_sync *mutex = LIST_ENTRY( ptr, struct mutex_sync, entry );
        assert( mutex->owner == thread );
        mutex->abandoned = 1;
        do_release( mutex, thread, mutex->count );
    }

    LIST_FOR_EACH( ptr, &wfusync_mutexes )
    {
        struct mutex_sync *mutex = LIST_ENTRY( ptr, struct mutex_sync, wfusync_entry );

        if (wfusync_abandon_mutex( mutex->wfusync, thread->id ))
        {
            wake_up( &mutex->obj, 0 );
            wfusync_wake_waiters( wfusync_get_index( mutex->wfusync ) );
        }
    }

    abandon_inproc_mutexes( thread->id );
}

static void mutex_dump( struct object *obj, int verbose )
{
    struct mutex *mutex = (struct mutex *)obj;
    assert( obj->ops == &mutex_ops );
    mutex->sync->ops->dump( mutex->sync, verbose );
}

static struct object *mutex_get_sync( struct object *obj )
{
    struct mutex *mutex = (struct mutex *)obj;
    assert( obj->ops == &mutex_ops );
    return grab_object( mutex->sync );
}

static int mutex_signal( struct object *obj, unsigned int access, int signal )
{
    struct mutex *mutex = (struct mutex *)obj;
    assert( obj->ops == &mutex_ops );

    assert( mutex->sync->ops == &mutex_sync_ops ); /* never called with inproc syncs */
    assert( signal == -1 ); /* always called from signal_object */

    if (!(access & SYNCHRONIZE))
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }
    if (((struct mutex_sync *)mutex->sync)->wfusync)
    {
        unsigned int prev;

        if (!wfusync_release_mutex( ((struct mutex_sync *)mutex->sync)->wfusync, current->id, &prev ))
            return 0;
        if (prev == 1) wake_up( mutex->sync, 0 );
        return 1;
    }
    return do_release( (struct mutex_sync *)mutex->sync, current, 1 );
}

static void mutex_destroy( struct object *obj )
{
    struct mutex *mutex = (struct mutex *)obj;
    assert( obj->ops == &mutex_ops );
    if (mutex->sync) release_object( mutex->sync );
}

int get_mutex_wfusync_idx( struct object *obj, int *type )
{
    struct mutex_sync *sync;
    struct mutex *mutex;

    if (!do_wfusync() || !obj || obj->ops != &mutex_ops) return -1;
    mutex = (struct mutex *)obj;
    if (!mutex->sync || mutex->sync->ops != &mutex_sync_ops) return -1;
    sync = (struct mutex_sync *)mutex->sync;
    if (!sync->wfusync) return -1;
    wfusync_grab_object( sync->wfusync );
    *type = INPROC_SYNC_MUTEX;
    return wfusync_get_index( sync->wfusync );
}

/* create a mutex */
DECL_HANDLER(create_mutex)
{
    struct mutex *mutex;
    struct unicode_str name;
    struct object *root;
    const struct security_descriptor *sd;
    const struct object_attributes *objattr = get_req_object_attributes( &sd, &name, &root );

    if (!objattr) return;

    if ((mutex = create_mutex( root, &name, objattr->attributes, req->owned, sd )))
    {
        if (get_error() == STATUS_OBJECT_NAME_EXISTS)
            reply->handle = alloc_handle( current->process, mutex, req->access, objattr->attributes );
        else
            reply->handle = alloc_handle_no_access_check( current->process, mutex,
                                                          req->access, objattr->attributes );
        release_object( mutex );
    }

    if (root) release_object( root );
}

/* open a handle to a mutex */
DECL_HANDLER(open_mutex)
{
    struct unicode_str name = get_req_unicode_str();

    reply->handle = open_object( current->process, req->rootdir, req->access,
                                 &mutex_ops, &name, req->attributes );
}

/* release a mutex */
DECL_HANDLER(release_mutex)
{
    struct mutex *mutex;

    if ((mutex = (struct mutex *)get_handle_obj( current->process, req->handle,
                                                 0, &mutex_ops )))
    {
        struct mutex_sync *sync = (struct mutex_sync *)mutex->sync;
        assert( mutex->sync->ops == &mutex_sync_ops ); /* never called with inproc syncs */

        if (sync->wfusync)
        {
            if (wfusync_release_mutex( sync->wfusync, current->id, &reply->prev_count ) &&
                reply->prev_count == 1)
            {
                wake_up( mutex->sync, 0 );
                wfusync_wake_waiters( wfusync_get_index( sync->wfusync ) );
            }
        }
        else
        {
            reply->prev_count = sync->count;
            do_release( sync, current, 1 );
        }
        release_object( mutex );
    }
}

/* return details about the mutex */
DECL_HANDLER(query_mutex)
{
    struct mutex *mutex;

    if ((mutex = (struct mutex *)get_handle_obj( current->process, req->handle,
                                                 MUTANT_QUERY_STATE, &mutex_ops )))
    {
        struct mutex_sync *sync = (struct mutex_sync *)mutex->sync;
        assert( mutex->sync->ops == &mutex_sync_ops ); /* never called with inproc syncs */

        if (sync->wfusync)
            wfusync_get_mutex_state( sync->wfusync, current->id, &reply->count,
                                     &reply->owned, &reply->abandoned );
        else
        {
            reply->count = sync->count;
            reply->owned = (sync->owner == current);
            reply->abandoned = sync->abandoned;
        }

        release_object( mutex );
    }
}
