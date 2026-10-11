/*
 * In-process synchronization primitives
 *
 * Copyright (C) 2021-2022 Elizabeth Figura for CodeWeavers
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"

#include "file.h"
#include "handle.h"
#include "request.h"
#include "process.h"
#include "thread.h"
#include "user.h"

#include "esync.h"
#include "fsync.h"

#include "ntsync_tmp.h"

#ifdef NTSYNC_IOC_EVENT_READ

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

int get_inproc_device_fd(void)
{
    static int fd = -2;
    if (fd == -2)
    {
        if (getenv( "PROTON_NO_NTSYNC" ) && atoi(getenv( "PROTON_NO_NTSYNC" )))
            fd = -1;
        else
            fd = open( "/dev/ntsync", O_CLOEXEC | O_RDONLY );
        if (fd >= 0)
        {
            do_fsync_cached = 0;
            fprintf( stderr, "droiddeck-ntsync: up and running.\n" );
        }
        else if (do_fsync()) fd = FSYNC_USED_BY_SERVER;
        else if (esync_init()) fd = ESYNC_USED_BY_SERVER;
        else fprintf( stderr, "wineserver: using server-side synchronization.\n" );
    }
    return fd;
}

struct inproc_sync
{
    struct object          obj;  /* object header */
    enum inproc_sync_type  type;
    int                    fd;
    struct list            entry;
    unsigned int           shm_idx;
};

static struct list inproc_mutexes = LIST_INIT( inproc_mutexes );

static void inproc_sync_dump( struct object *obj, int verbose );
static int inproc_sync_signal( struct object *obj, unsigned int access, int signal );
static void inproc_sync_destroy( struct object *obj );

static const struct object_ops inproc_sync_ops =
{
    sizeof(struct inproc_sync), /* size */
    &no_type,                   /* type */
    inproc_sync_dump,           /* dump */
    no_add_queue,               /* add_queue */
    NULL,                       /* remove_queue */
    NULL,                       /* signaled */
    NULL,                       /* satisfied */
    inproc_sync_signal,         /* signal */
    no_get_fd,                  /* get_fd */
    default_get_sync,           /* get_sync */
    default_map_access,         /* map_access */
    default_get_sd,             /* get_sd */
    default_set_sd,             /* set_sd */
    default_get_full_name,      /* get_full_name */
    no_lookup_name,             /* lookup_name */
    directory_link_name,        /* link_name */
    default_unlink_name,        /* unlink_name */
    no_open_file,               /* open_file */
    no_kernel_obj_list,         /* get_kernel_obj_list */
    no_close_handle,            /* close_handle */
    inproc_sync_destroy,        /* destroy */
};

int get_inproc_sync_fd( struct inproc_sync *sync )
{
    if (!sync) return -1;
    if (do_esync() && sync->fd == ESYNC_NO_FD && (sync->fd = esync_open( sync->shm_idx )) == -1)
    {
        sync->fd = ESYNC_NO_FD;
        set_error( STATUS_TOO_MANY_OPENED_FILES );
        return -1;
    }
    return sync->fd;
}

struct inproc_sync *create_inproc_internal_sync( int manual, int signaled )
{
    struct ntsync_event_args args = {.signaled = signaled, .manual = manual};
    struct inproc_sync *event;

    if (!(event = alloc_object( &inproc_sync_ops ))) return NULL;
    event->shm_idx = 0;
    if (do_fsync())
    {
        event->type = manual ? FSYNC_MANUAL_SERVER : FSYNC_AUTO_SERVER;
        event->fd = fsync_alloc_shm( signaled, 0xdeadbeef );
    }
    else if (do_esync())
    {
        event->type = INPROC_SYNC_INTERNAL;
        event->fd   = esync_create( 0, manual, !!signaled, !!signaled, 0, 1, &event->shm_idx );
    }
    else
    {
        event->type = INPROC_SYNC_INTERNAL;
        event->fd   = ioctl( get_inproc_device_fd(), NTSYNC_IOC_CREATE_EVENT, &args );
    }
    list_init( &event->entry );

    if (event->fd == -1)
    {
        set_error( STATUS_TOO_MANY_OPENED_FILES );
        release_object( event );
        return NULL;
    }
    return event;
}

struct inproc_sync *create_inproc_event_sync( int manual, int signaled )
{
    struct ntsync_event_args args = {.signaled = signaled, .manual = manual};
    struct inproc_sync *event;

    if (!(event = alloc_object( &inproc_sync_ops ))) return NULL;
    event->shm_idx = 0;
    if (do_fsync())
    {
        event->type = manual ? FSYNC_MANUAL_EVENT : FSYNC_AUTO_EVENT;
        event->fd = fsync_alloc_shm( signaled, 0xdeadbeef );
    }
    else if (do_esync())
    {
        event->type = INPROC_SYNC_EVENT;
        event->fd   = esync_create( 0, manual, !!signaled, !!signaled, 0, 0, &event->shm_idx );
    }
    else
    {
        event->type = INPROC_SYNC_EVENT;
        event->fd   = ioctl( get_inproc_device_fd(), NTSYNC_IOC_CREATE_EVENT, &args );
    }
    list_init( &event->entry );

    if (event->fd == -1)
    {
        set_error( STATUS_TOO_MANY_OPENED_FILES );
        release_object( event );
        return NULL;
    }
    return event;
}

struct inproc_sync *create_inproc_mutex_sync( thread_id_t owner, unsigned int count )
{
    struct ntsync_mutex_args args = {.owner = owner, .count = count};
    struct inproc_sync *mutex;

    if (!(mutex = alloc_object( &inproc_sync_ops ))) return NULL;
    mutex->shm_idx = 0;
    if (do_fsync())
    {
        mutex->type = FSYNC_MUTEX;
        mutex->fd = fsync_alloc_shm( owner, count );
    }
    else if (do_esync())
    {
        mutex->type = INPROC_SYNC_MUTEX;
        mutex->fd   = esync_create( 0, 0, owner ? 0 : 1, owner, owner ? count : 0, 0, &mutex->shm_idx );
    }
    else
    {
        mutex->type = INPROC_SYNC_MUTEX;
        mutex->fd   = ioctl( get_inproc_device_fd(), NTSYNC_IOC_CREATE_MUTEX, &args );
    }
    list_add_tail( &inproc_mutexes, &mutex->entry );

    if (mutex->fd == -1)
    {
        set_error( STATUS_TOO_MANY_OPENED_FILES );
        release_object( mutex );
        return NULL;
    }
    return mutex;
}

struct inproc_sync *create_inproc_semaphore_sync( unsigned int initial, unsigned int max )
{
    struct ntsync_sem_args args = {.count = initial, .max = max};
    struct inproc_sync *sem;

    if (!(sem = alloc_object( &inproc_sync_ops ))) return NULL;
    sem->shm_idx = 0;

    if (do_fsync())
    {
        sem->type = FSYNC_SEMAPHORE;
        sem->fd = fsync_alloc_shm( initial, max );
    }
    else if (do_esync())
    {
        sem->type = INPROC_SYNC_SEMAPHORE;
        sem->fd   = esync_create( 1, 0, initial, initial, max, 0, &sem->shm_idx );
    }
    else
    {
        sem->type = INPROC_SYNC_SEMAPHORE;
        sem->fd   = ioctl( get_inproc_device_fd(), NTSYNC_IOC_CREATE_SEM, &args );
    }
    list_init( &sem->entry );

    if (sem->fd == -1)
    {
        set_error( STATUS_TOO_MANY_OPENED_FILES );
        release_object( sem );
        return NULL;
    }
    return sem;
}

static void inproc_sync_dump( struct object *obj, int verbose )
{
    struct inproc_sync *sync = (struct inproc_sync *)obj;
    assert( obj->ops == &inproc_sync_ops );
    fprintf( stderr, "Inproc sync type=%d, fd=%d\n", sync->type, sync->fd );
}

void signal_inproc_sync( struct inproc_sync *sync )
{
    __u32 count;
    if (debug_level) fprintf( stderr, "set_inproc_event %d\n", sync->fd );
    if (do_fsync()) fsync_set_event( sync->fd );
    else if (do_esync()) esync_set( sync->fd, sync->shm_idx, sync->type == INPROC_SYNC_EVENT );
    else            ioctl( sync->fd, NTSYNC_IOC_EVENT_SET, &count );
}

void reset_inproc_sync( struct inproc_sync *sync )
{
    __u32 count;
    if (debug_level) fprintf( stderr, "reset_inproc_event %d\n", sync->fd );
    if (do_fsync()) fsync_reset_event( sync->fd );
    else if (do_esync()) esync_reset( sync->fd, sync->shm_idx, sync->type == INPROC_SYNC_EVENT );
    else            ioctl( sync->fd, NTSYNC_IOC_EVENT_RESET, &count );
}

static int inproc_sync_signal( struct object *obj, unsigned int access, int signal )
{
    struct inproc_sync *sync = (struct inproc_sync *)obj;
    assert( obj->ops == &inproc_sync_ops );

    if (!do_fsync())
        assert( sync->type == INPROC_SYNC_INTERNAL || sync->type == INPROC_SYNC_EVENT ); /* never called for mutex / semaphore */
    assert( signal == 0 || signal == 1 ); /* never called from signal_object */

    if (signal) signal_inproc_sync( sync );
    else reset_inproc_sync( sync );
    return 1;
}

static void inproc_sync_destroy( struct object *obj )
{
    struct inproc_sync *sync = (struct inproc_sync *)obj;
    assert( obj->ops == &inproc_sync_ops );
    list_remove( &sync->entry );
    if (do_fsync()) fsync_free_shm_idx( sync->fd );
    else if (do_esync()) esync_destroy( sync->fd, sync->shm_idx );
    else            close( sync->fd );
}

void abandon_inproc_mutexes( thread_id_t tid )
{
    struct inproc_sync *mutex;

    if (do_fsync())
    {
        LIST_FOR_EACH_ENTRY( mutex, &inproc_mutexes, struct inproc_sync, entry )
            fsync_abandon_mutex( mutex->fd, tid );
        return;
    }
    if (do_esync())
    {
        LIST_FOR_EACH_ENTRY( mutex, &inproc_mutexes, struct inproc_sync, entry )
            esync_abandon_mutex( mutex->fd, mutex->shm_idx, tid, 0 );
        return;
    }

    LIST_FOR_EACH_ENTRY( mutex, &inproc_mutexes, struct inproc_sync, entry )
        ioctl( mutex->fd, NTSYNC_IOC_MUTEX_KILL, &tid );
}

struct mutex_reaper
{
    struct list          entry;
    struct thread       *thread;
    struct timeout_user *timeout;
    unsigned int         checks;
};

static struct list mutex_reapers = LIST_INIT( mutex_reapers );

static void finish_reaper( struct mutex_reaper *reaper )
{
    if (reaper->timeout) remove_timeout_user( reaper->timeout );
    list_remove( &reaper->entry );
    abandon_inproc_mutexes( reaper->thread->id );
    release_object( reaper->thread );
    free( reaper );
}

static void reap_inproc_mutexes( void *private )
{
    struct mutex_reaper *reaper = private;

    reaper->timeout = NULL;
    if (++reaper->checks < 20 && send_thread_signal( reaper->thread, 0 ) &&
        (reaper->timeout = add_timeout_user( -TICKS_PER_SEC / 20, reap_inproc_mutexes, reaper )))
        return;
    finish_reaper( reaper );
}

void abandon_inproc_mutexes_later( struct thread *thread )
{
    struct mutex_reaper *reaper;

    if (list_empty( &inproc_mutexes ) || thread->process->is_terminating) return;
    if (!(reaper = malloc( sizeof(*reaper) ))) return;
    reaper->thread = (struct thread *)grab_object( thread );
    reaper->checks = 0;
    if (!(reaper->timeout = add_timeout_user( -TICKS_PER_SEC / 100, reap_inproc_mutexes, reaper )))
    {
        release_object( thread );
        free( reaper );
        return;
    }
    list_add_tail( &mutex_reapers, &reaper->entry );
}

void abandon_dead_inproc_mutexes( struct process *process )
{
    unsigned int error = get_error();
    struct mutex_reaper *reaper, *next;
    struct ntsync_mutex_args args;
    struct inproc_sync *mutex;
    struct thread *thread;
    thread_id_t owner;
    int dead, gone;

    LIST_FOR_EACH_ENTRY_SAFE( reaper, next, &mutex_reapers, struct mutex_reaper, entry )
        if (reaper->thread->process == process) finish_reaper( reaper );
    if (!do_fsync())
    {
        LIST_FOR_EACH_ENTRY( mutex, &inproc_mutexes, struct inproc_sync, entry )
        {
            if (do_esync()) owner = esync_mutex_owner( mutex->shm_idx );
            else if (ioctl( mutex->fd, NTSYNC_IOC_MUTEX_READ, &args ) == -1 && errno != EOWNERDEAD) continue;
            else owner = args.owner;
            if (!owner) continue;
            thread = get_thread_from_id( owner );
            dead = !thread || thread->state == TERMINATED;
            gone = !thread || thread->process == process;
            if (thread) release_object( thread );
            if (!dead) continue;
            if (do_esync()) esync_abandon_mutex( mutex->fd, mutex->shm_idx, owner, gone );
            else ioctl( mutex->fd, NTSYNC_IOC_MUTEX_KILL, &owner );
        }
    }
    set_error( error );
}

static int get_obj_inproc_sync( struct object *obj, int *type, unsigned int *shm_idx )
{
    struct object *sync;
    int fd = -1;

    if (!(sync = get_obj_sync( obj ))) return -1;
    if (sync->ops == &inproc_sync_ops)
    {
        struct inproc_sync *inproc = (struct inproc_sync *)sync;
        *type = inproc->type;
        *shm_idx = inproc->shm_idx;
        fd = get_inproc_sync_fd( inproc );
    }

    release_object( sync );
    return fd;
}

#else /* NTSYNC_IOC_EVENT_READ */

int get_inproc_device_fd(void)
{
    return -1;
}

int get_inproc_sync_fd( struct inproc_sync *sync )
{
    return -1;
}

struct inproc_sync *create_inproc_internal_sync( int manual, int signaled )
{
    return NULL;
}

struct inproc_sync *create_inproc_event_sync( int manual, int signaled )
{
    return NULL;
}

struct inproc_sync *create_inproc_mutex_sync( thread_id_t owner, unsigned int count )
{
    return NULL;
}

struct inproc_sync *create_inproc_semaphore_sync( unsigned int initial, unsigned int max )
{
    return NULL;
}

void signal_inproc_sync( struct inproc_sync *sync )
{
}

void reset_inproc_sync( struct inproc_sync *sync )
{
}

void abandon_inproc_mutexes( thread_id_t tid )
{
}

void abandon_inproc_mutexes_later( struct thread *thread )
{
}

void abandon_dead_inproc_mutexes( struct process *process )
{
}

static int get_obj_inproc_sync( struct object *obj, int *type, unsigned int *shm_idx )
{
    return -1;
}

#endif /* NTSYNC_IOC_EVENT_READ */

DECL_HANDLER(get_inproc_sync_fd)
{
    struct object *obj;
    unsigned int shm_idx = 0;
    int fd;

    if (!(obj = get_handle_obj( current->process, req->handle, 0, NULL ))) return;

    reply->access = get_handle_access( current->process, req->handle );

    if ((fd = get_obj_inproc_sync( obj, &reply->type, &shm_idx )) < 0)
    {
        if (!get_error()) set_error( STATUS_NOT_IMPLEMENTED );
    }
    else if (do_fsync()) reply->fsync_shm_idx = fsync_grab_shm_idx( fd );
    else if (do_esync())
    {
        if (!(reply->fsync_shm_idx = esync_grab( get_process_id( current->process ), shm_idx ))) set_error( STATUS_NO_MEMORY );
        else send_client_fd( current->process, fd, req->handle );
    }
    else send_client_fd( current->process, fd, req->handle );

    release_object( obj );
}
