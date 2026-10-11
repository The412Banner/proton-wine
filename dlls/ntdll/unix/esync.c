#if 0
#pragma makedep unix
#endif
/* droiddeck-esync: the client side of eventfd and shared memory synchronization.
 * Credit: based on the esync design by Elizabeth Figura for Wine and Proton. */

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/server.h"
#include "wine/debug.h"

#include "unix_private.h"
#include "esync.h"

WINE_DEFAULT_DEBUG_CHANNEL(esync);

#define ESYNC_CHUNK_SIZE 0x10000
#define ESYNC_SLOT_SIZE  16
#define ESYNC_PER_CHUNK  (ESYNC_CHUNK_SIZE / ESYNC_SLOT_SIZE)
#define ESYNC_CHUNKS     8192
#define ESYNC_MANUAL     1
#define ESYNC_WAITER     0x100
#define ESYNC_RELEASING  1
#define ESYNC_PULSE_NS   10000000
#define ESYNC_STALL_NS   2000000
#define ESYNC_TOKEN_MS   100
#define ESYNC_TOKEN_TRIES 10
#define ESYNC_SPINS      16
#define ESYNC_SPIN_NS    50000
#define ESYNC_SLICE_NS   1000000000

struct esync_slot
{
    union
    {
        struct
        {
            int state;
            int aux;
        };
        LONG64 pair;
    };
    int flags;
    int ref;
};

C_ASSERT( sizeof(struct esync_slot) == ESYNC_SLOT_SIZE );

union esync_pair
{
    struct { int state; int aux; } s;
    LONG64 word;
};

C_ASSERT( offsetof( struct esync_slot, state ) == offsetof( union esync_pair, s.state ) );
C_ASSERT( offsetof( struct esync_slot, aux ) == offsetof( union esync_pair, s.aux ) );

static BOOL swap_pair( struct esync_slot *slot, int state, int aux, int new_state, int new_aux )
{
    union esync_pair cur = { .s = { state, aux } }, next = { .s = { new_state, new_aux } };

    return __atomic_compare_exchange_n( &slot->pair, &cur.word, next.word, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST );
}

enum esync_kind
{
    ESYNC_KIND_SEMAPHORE,
    ESYNC_KIND_MUTEX,
    ESYNC_KIND_AUTO,
    ESYNC_KIND_MANUAL,
};

struct esync_obj
{
    int                fd;
    enum esync_kind    kind;
    struct esync_slot *slot;
};

enum esync_clock
{
    ESYNC_NO_TIMEOUT,
    ESYNC_MONOTONIC,
    ESYNC_REALTIME,
};

struct esync_deadline
{
    enum esync_clock clock;
    ULONGLONG        end;
};

struct esync_held
{
    struct esync_slot *slot;
    int                fd;
};

struct esync_waits
{
    struct esync_slot *slots[MAXIMUM_WAIT_OBJECTS];
    DWORD              count;
    struct esync_held  held[MAXIMUM_WAIT_OBJECTS];
    DWORD              held_count;
    DWORD              tid;
    int                unix_tid;
};

static int shm_fd = -1;
static void *chunks[ESYNC_CHUNKS];
static sigset_t block_set;
static __thread struct esync_waits waits;
static pthread_key_t waits_key;
static pthread_once_t waits_once = PTHREAD_ONCE_INIT;

#ifdef __linux__

void esync_init( int fd )
{
    shm_fd = fd;
    block_set = server_block_set;
    sigaddset( &block_set, SIGQUIT );
    TRACE( "shm fd %d\n", fd );
}

static struct esync_slot *get_slot( unsigned int idx )
{
    unsigned int chunk = idx / ESYNC_PER_CHUNK;
    void *base, *addr;

    if (!idx || chunk >= ESYNC_CHUNKS) return NULL;
    if (!(base = __atomic_load_n( &chunks[chunk], __ATOMIC_ACQUIRE )))
    {
        addr = mmap( NULL, ESYNC_CHUNK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd,
                     (off_t)chunk * ESYNC_CHUNK_SIZE );
        if (addr == MAP_FAILED)
        {
            ERR( "cannot map chunk %u: %s\n", chunk, strerror( errno ) );
            return NULL;
        }
        if (__atomic_compare_exchange_n( &chunks[chunk], &base, addr, FALSE, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE ))
            base = addr;
        else
            munmap( addr, ESYNC_CHUNK_SIZE );
    }
    return (struct esync_slot *)((char *)base + (idx % ESYNC_PER_CHUNK) * ESYNC_SLOT_SIZE);
}

void esync_release( unsigned int shm_idx )
{
    if (!shm_idx) return;
    SERVER_START_REQ( fsync_free_shm_idx )
    {
        req->shm_idx = shm_idx;
        wine_server_call( req );
    }
    SERVER_END_REQ;
}

static void block_signals( sigset_t *old )
{
    pthread_sigmask( SIG_BLOCK, &block_set, old );
}

static void restore_signals( const sigset_t *old )
{
    pthread_sigmask( SIG_SETMASK, old, NULL );
}

static ssize_t read_fd( int fd, uint64_t *value )
{
    return syscall( __NR_read, fd, value, sizeof(*value) );
}

static int poll_fds( struct pollfd *fds, nfds_t count, const struct timespec *timeout )
{
    struct timespec left;

    if (timeout) left = *timeout;
    return syscall( __NR_ppoll, fds, count, timeout ? &left : NULL, NULL, 0 );
}

static NTSTATUS wake_fd( int fd, uint64_t count )
{
    if (syscall( __NR_write, fd, &count, sizeof(count) ) == -1) return errno_to_status( errno );
    return STATUS_SUCCESS;
}

static BOOL drain_fd( int fd )
{
    uint64_t value;

    if (read_fd( fd, &value ) == sizeof(value)) return TRUE;
    if (errno != EAGAIN) WARN( "read fd %d: %s\n", fd, strerror( errno ) );
    return FALSE;
}

static BOOL fd_readable( int fd )
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    struct timespec now = { 0, 0 };
    int ret;

    while ((ret = poll_fds( &pfd, 1, &now )) < 0 && errno == EINTR);
    return ret > 0 && (pfd.revents & POLLIN);
}

static BOOL consume_token( int fd )
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    struct timespec pause = { 0, ESYNC_TOKEN_MS * 1000000 };
    unsigned int tries = 0;
    uint64_t value;

    while (read_fd( fd, &value ) != sizeof(value))
    {
        if (errno != EAGAIN && errno != EINTR)
        {
            WARN( "read fd %d: %s\n", fd, strerror( errno ) );
            return TRUE;
        }
        if (tries == ESYNC_TOKEN_TRIES)
        {
            WARN( "no token on fd %d, the count it was released with is lost\n", fd );
            return FALSE;
        }
        if (!poll_fds( &pfd, 1, &pause )) tries++;
    }
    return TRUE;
}

static int current_unix_tid(void)
{
    if (!waits.unix_tid) waits.unix_tid = syscall( __NR_gettid );
    return waits.unix_tid;
}

static void repair_slot( struct esync_slot *slot, int fd )
{
    BOOL readable = fd_readable( fd );

    if (__atomic_load_n( &slot->state, __ATOMIC_SEQ_CST ))
    {
        if (!readable) wake_fd( fd, 1 );
    }
    else if (readable) drain_fd( fd );
}

static void lock_slot( struct esync_slot *slot, int fd, int self )
{
    unsigned int spins = 0;
    int owner;

    for (;;)
    {
        owner = 0;
        if (__atomic_compare_exchange_n( &slot->aux, &owner, self, FALSE, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED )) return;
        if (++spins < 128)
        {
            YieldProcessor();
            continue;
        }
        if (!(spins & 1023) && owner > 0 && kill( owner, 0 ) == -1 && errno == ESRCH &&
            __atomic_compare_exchange_n( &slot->aux, &owner, self, FALSE, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED ))
        {
            WARN( "lock owner %d is gone, taking over\n", owner );
            repair_slot( slot, fd );
            return;
        }
        sched_yield();
    }
}

static void unlock_slot( struct esync_slot *slot, int self )
{
    __atomic_compare_exchange_n( &slot->aux, &self, 0, FALSE, __ATOMIC_RELEASE, __ATOMIC_RELAXED );
}

static void drop_waits( void *arg )
{
    struct esync_waits *list = arg;
    struct esync_held *held;

    while (list->count) __atomic_sub_fetch( &list->slots[--list->count]->flags, ESYNC_WAITER, __ATOMIC_SEQ_CST );
    while (list->held_count)
    {
        held = &list->held[--list->held_count];
        if (swap_pair( held->slot, list->tid, __atomic_load_n( &held->slot->aux, __ATOMIC_SEQ_CST ), ~0, 0 ))
            wake_fd( held->fd, 1 );
    }
}

static void init_waits_key(void)
{
    pthread_key_create( &waits_key, drop_waits );
}

static void arm_waits(void)
{
    pthread_once( &waits_once, init_waits_key );
    if (!pthread_getspecific( waits_key )) pthread_setspecific( waits_key, &waits );
}

static void add_waits( DWORD count, const struct esync_obj *objs )
{
    DWORD i;

    arm_waits();
    for (i = 0; i < count && waits.count < MAXIMUM_WAIT_OBJECTS; i++)
    {
        if (objs[i].kind != ESYNC_KIND_AUTO && objs[i].kind != ESYNC_KIND_MANUAL) continue;
        waits.slots[waits.count] = objs[i].slot;
        __atomic_add_fetch( &objs[i].slot->flags, ESYNC_WAITER, __ATOMIC_SEQ_CST );
        __atomic_store_n( &waits.count, waits.count + 1, __ATOMIC_RELAXED );
    }
}

static void remove_waits( DWORD base )
{
    struct esync_slot *slot;

    while (waits.count > base)
    {
        slot = waits.slots[waits.count - 1];
        __atomic_store_n( &waits.count, waits.count - 1, __ATOMIC_RELAXED );
        __atomic_sub_fetch( &slot->flags, ESYNC_WAITER, __ATOMIC_SEQ_CST );
    }
}

static void hold_mutex( struct esync_obj *obj, DWORD tid )
{
    arm_waits();
    waits.tid = tid;
    waits.held[waits.held_count].slot = obj->slot;
    waits.held[waits.held_count].fd = obj->fd;
    waits.held_count++;
}

static ULONGLONG monotonic_ns(void)
{
    struct timespec ts;

    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (ULONGLONG)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

NTSTATUS esync_release_semaphore( int fd, unsigned int shm_idx, ULONG count, ULONG *prev )
{
    struct esync_slot *slot = get_slot( shm_idx );
    NTSTATUS ret = STATUS_SUCCESS;
    sigset_t old;
    int current;

    if (!slot) return STATUS_INVALID_HANDLE;
    block_signals( &old );
    current = __atomic_load_n( &slot->state, __ATOMIC_SEQ_CST );
    do
    {
        if ((ULONGLONG)(unsigned int)current + count > (unsigned int)slot->aux)
        {
            restore_signals( &old );
            return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
        }
    }
    while (!__atomic_compare_exchange_n( &slot->state, &current, (int)((unsigned int)current + count),
                                         FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ));
    if (count) ret = wake_fd( fd, count );
    restore_signals( &old );
    if (!ret && prev) *prev = current;
    return ret;
}

NTSTATUS esync_query_semaphore( int fd, unsigned int shm_idx, SEMAPHORE_BASIC_INFORMATION *info )
{
    struct esync_slot *slot = get_slot( shm_idx );

    if (!slot) return STATUS_INVALID_HANDLE;
    info->CurrentCount = __atomic_load_n( &slot->state, __ATOMIC_SEQ_CST );
    info->MaximumCount = slot->aux;
    return STATUS_SUCCESS;
}

static NTSTATUS set_manual( struct esync_slot *slot, int fd, LONG *prev )
{
    int self = current_unix_tid();
    NTSTATUS ret = STATUS_SUCCESS;
    sigset_t old;
    LONG current;

    if (__atomic_load_n( &slot->state, __ATOMIC_SEQ_CST ) == 1)
    {
        if (prev) *prev = 1;
        return STATUS_SUCCESS;
    }
    block_signals( &old );
    lock_slot( slot, fd, self );
    if (!(current = __atomic_exchange_n( &slot->state, 1, __ATOMIC_SEQ_CST ))) ret = wake_fd( fd, 1 );
    unlock_slot( slot, self );
    restore_signals( &old );
    if (prev) *prev = current;
    return ret;
}

static LONG reset_manual( struct esync_slot *slot, int fd )
{
    int self = current_unix_tid();
    sigset_t old;
    LONG current;

    if (!__atomic_load_n( &slot->state, __ATOMIC_SEQ_CST )) return 0;
    block_signals( &old );
    lock_slot( slot, fd, self );
    if ((current = __atomic_exchange_n( &slot->state, 0, __ATOMIC_SEQ_CST ))) drain_fd( fd );
    unlock_slot( slot, self );
    restore_signals( &old );
    return current;
}

NTSTATUS esync_set_event( int fd, unsigned int shm_idx, LONG *prev )
{
    struct esync_slot *slot = get_slot( shm_idx );
    NTSTATUS ret;
    LONG current;

    if (!slot) return STATUS_INVALID_HANDLE;
    if (slot->flags & ESYNC_MANUAL) return set_manual( slot, fd, prev );
    current = prev ? fd_readable( fd ) : 0;
    ret = wake_fd( fd, 1 );
    __atomic_store_n( &slot->state, 1, __ATOMIC_SEQ_CST );
    if (prev) *prev = current;
    return ret;
}

NTSTATUS esync_reset_event( int fd, unsigned int shm_idx, LONG *prev )
{
    struct esync_slot *slot = get_slot( shm_idx );
    LONG current;

    if (!slot) return STATUS_INVALID_HANDLE;
    if (slot->flags & ESYNC_MANUAL) current = reset_manual( slot, fd );
    else
    {
        __atomic_store_n( &slot->state, 0, __ATOMIC_SEQ_CST );
        current = drain_fd( fd );
    }
    if (prev) *prev = current;
    return STATUS_SUCCESS;
}

static void pulse_wait( struct esync_slot *slot, int fd, BOOL manual )
{
    ULONGLONG now = monotonic_ns(), deadline = now + ESYNC_PULSE_NS, stall = now + ESYNC_STALL_NS;
    int waiters = __atomic_load_n( &slot->flags, __ATOMIC_SEQ_CST ) / ESYNC_WAITER, left;

    while (waiters > 0 && now < deadline && now < stall)
    {
        if (!manual && !fd_readable( fd )) break;
        sched_yield();
        left = __atomic_load_n( &slot->flags, __ATOMIC_SEQ_CST ) / ESYNC_WAITER;
        now = monotonic_ns();
        if (left < waiters) stall = now + ESYNC_STALL_NS;
        waiters = left;
    }
}

NTSTATUS esync_pulse_event( int fd, unsigned int shm_idx, LONG *prev )
{
    struct esync_slot *slot = get_slot( shm_idx );
    NTSTATUS ret;
    sigset_t old;
    LONG current;

    if (!slot) return STATUS_INVALID_HANDLE;
    block_signals( &old );
    if (slot->flags & ESYNC_MANUAL)
    {
        if (!(ret = set_manual( slot, fd, &current ))) pulse_wait( slot, fd, TRUE );
        reset_manual( slot, fd );
    }
    else
    {
        current = prev ? fd_readable( fd ) : 0;
        ret = wake_fd( fd, 1 );
        __atomic_store_n( &slot->state, 1, __ATOMIC_SEQ_CST );
        if (!ret) pulse_wait( slot, fd, FALSE );
        __atomic_store_n( &slot->state, 0, __ATOMIC_SEQ_CST );
        drain_fd( fd );
    }
    restore_signals( &old );
    if (prev) *prev = current;
    return ret;
}

NTSTATUS esync_query_event( int fd, unsigned int shm_idx, EVENT_BASIC_INFORMATION *info )
{
    struct esync_slot *slot = get_slot( shm_idx );

    if (!slot) return STATUS_INVALID_HANDLE;
    if (slot->flags & ESYNC_MANUAL)
    {
        info->EventState = !!__atomic_load_n( &slot->state, __ATOMIC_SEQ_CST );
        info->EventType = NotificationEvent;
    }
    else
    {
        info->EventState = fd_readable( fd );
        info->EventType = SynchronizationEvent;
    }
    return STATUS_SUCCESS;
}

NTSTATUS esync_release_mutex( int fd, unsigned int shm_idx, LONG *prev )
{
    struct esync_slot *slot = get_slot( shm_idx );
    DWORD tid = GetCurrentThreadId();
    NTSTATUS ret = STATUS_SUCCESS;
    int owner = tid | ESYNC_RELEASING, count;
    sigset_t old;

    if (!slot) return STATUS_INVALID_HANDLE;
    if ((DWORD)__atomic_load_n( &slot->state, __ATOMIC_SEQ_CST ) != tid) return STATUS_MUTANT_NOT_OWNED;
    block_signals( &old );
    count = __atomic_load_n( &slot->aux, __ATOMIC_SEQ_CST );
    if (count > 1)
    {
        if (!swap_pair( slot, tid, count, tid, count - 1 )) ret = STATUS_MUTANT_NOT_OWNED;
    }
    else if (!swap_pair( slot, tid, count, tid | ESYNC_RELEASING, 0 )) ret = STATUS_MUTANT_NOT_OWNED;
    else if ((ret = wake_fd( fd, 1 ))) swap_pair( slot, tid | ESYNC_RELEASING, 0, tid, count );
    else __atomic_compare_exchange_n( &slot->state, &owner, 0, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST );
    restore_signals( &old );
    if (!ret && prev) *prev = 1 - count;
    return ret;
}

NTSTATUS esync_query_mutex( int fd, unsigned int shm_idx, MUTANT_BASIC_INFORMATION *info )
{
    struct esync_slot *slot = get_slot( shm_idx );
    DWORD owner;

    if (!slot) return STATUS_INVALID_HANDLE;
    owner = __atomic_load_n( &slot->state, __ATOMIC_SEQ_CST );
    info->CurrentCount = 1 - __atomic_load_n( &slot->aux, __ATOMIC_SEQ_CST );
    info->OwnedByCaller = owner == GetCurrentThreadId();
    info->AbandonedState = owner == ~0u;
    return STATUS_SUCCESS;
}

static void get_deadline( const LARGE_INTEGER *timeout, struct esync_deadline *end )
{
    ULONGLONG now, rel;

    end->clock = ESYNC_NO_TIMEOUT;
    if (!timeout || timeout->QuadPart == TIMEOUT_INFINITE) return;
    if (timeout->QuadPart > 0)
    {
        end->clock = ESYNC_REALTIME;
        end->end = timeout->QuadPart;
        return;
    }
    now = monotonic_ns();
    rel = -(ULONGLONG)timeout->QuadPart;
    if (rel > (ULLONG_MAX - now) / 100) return;
    end->clock = ESYNC_MONOTONIC;
    end->end = now + rel * 100;
}

static BOOL time_left( const struct esync_deadline *end, struct timespec *ts, BOOL *sliced )
{
    ULONGLONG left = 0, now;
    struct timespec rt;
    LONGLONG ticks;

    *sliced = FALSE;
    switch (end->clock)
    {
    case ESYNC_NO_TIMEOUT:
        return FALSE;
    case ESYNC_MONOTONIC:
        now = monotonic_ns();
        if (now < end->end) left = end->end - now;
        break;
    case ESYNC_REALTIME:
        clock_gettime( CLOCK_REALTIME, &rt );
        ticks = ticks_from_time_t( rt.tv_sec ) + rt.tv_nsec / 100;
        if ((LONGLONG)end->end > ticks)
        {
            left = (ULONGLONG)((LONGLONG)end->end - ticks);
            left = left > ESYNC_SLICE_NS / 100 ? ESYNC_SLICE_NS : left * 100;
        }
        if (left == ESYNC_SLICE_NS) *sliced = TRUE;
        break;
    }
    ts->tv_sec = left / 1000000000;
    ts->tv_nsec = left % 1000000000;
    return TRUE;
}

static BOOL expired( const struct esync_deadline *end )
{
    struct timespec ts;
    BOOL sliced;

    return time_left( end, &ts, &sliced ) && !sliced && !ts.tv_sec && !ts.tv_nsec;
}

static int do_poll( struct pollfd *fds, nfds_t count, const struct esync_deadline *end )
{
    struct timespec ts;
    BOOL sliced;
    int ret;

    for (;;)
    {
        ret = poll_fds( fds, count, time_left( end, &ts, &sliced ) ? &ts : NULL );
        if (ret < 0 && errno == EINTR) continue;
        if (!ret && sliced) continue;
        return ret;
    }
}

static NTSTATUS user_apc(void)
{
    static const LARGE_INTEGER zero;

    return server_wait( NULL, 0, SELECT_INTERRUPTIBLE | SELECT_ALERTABLE, &zero );
}

static BOOL take_semaphore( struct esync_obj *obj )
{
    int current = __atomic_load_n( &obj->slot->state, __ATOMIC_SEQ_CST );
    sigset_t old;
    BOOL ret;

    if (current <= 0) return FALSE;
    block_signals( &old );
    do
    {
        if (current <= 0)
        {
            restore_signals( &old );
            return FALSE;
        }
    }
    while (!__atomic_compare_exchange_n( &obj->slot->state, &current, current - 1, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ));
    ret = consume_token( obj->fd );
    restore_signals( &old );
    return ret;
}

static void back_off( unsigned int *spins )
{
    struct timespec pause = { 0, 1000000 };

    if (++*spins <= ESYNC_SPINS) sched_yield();
    else nanosleep( &pause, NULL );
}

static BOOL mutex_free( int state )
{
    return !state || (state & ESYNC_RELEASING);
}

static BOOL claim_mutex( struct esync_obj *obj, DWORD tid, int *prev )
{
    int owner = tid;
    uint64_t value;

    if (!mutex_free( *prev )) return FALSE;
    if (!__atomic_compare_exchange_n( &obj->slot->state, prev, tid, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ))
        return FALSE;
    if (read_fd( obj->fd, &value ) != sizeof(value))
    {
        __atomic_compare_exchange_n( &obj->slot->state, &owner, *prev, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST );
        return FALSE;
    }
    if ((DWORD)__atomic_load_n( &obj->slot->state, __ATOMIC_SEQ_CST ) != tid)
    {
        wake_fd( obj->fd, 1 );
        return FALSE;
    }
    return TRUE;
}

static void unclaim_mutex( struct esync_obj *obj, DWORD tid, int prev )
{
    int owner = tid;

    if ((DWORD)prev == ~0u)
    {
        if (__atomic_compare_exchange_n( &obj->slot->state, &owner, ~0, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ))
            wake_fd( obj->fd, 1 );
        return;
    }
    if (!__atomic_compare_exchange_n( &obj->slot->state, &owner, tid | ESYNC_RELEASING, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ))
        return;
    wake_fd( obj->fd, 1 );
    owner = tid | ESYNC_RELEASING;
    __atomic_compare_exchange_n( &obj->slot->state, &owner, prev, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST );
}

static BOOL take_mutex( struct esync_obj *obj, DWORD tid, BOOL *abandoned )
{
    int prev = __atomic_load_n( &obj->slot->state, __ATOMIC_SEQ_CST );
    BOOL ret = FALSE;
    sigset_t old;

    if (!mutex_free( prev )) return FALSE;
    block_signals( &old );
    if (claim_mutex( obj, tid, &prev ))
    {
        if ((DWORD)prev == ~0u) *abandoned = TRUE;
        hold_mutex( obj, tid );
        swap_pair( obj->slot, tid, __atomic_load_n( &obj->slot->aux, __ATOMIC_SEQ_CST ), tid, 1 );
        ret = TRUE;
    }
    restore_signals( &old );
    waits.held_count = 0;
    return ret;
}

static BOOL take_auto( struct esync_obj *obj )
{
    uint64_t value;

    __atomic_store_n( &obj->slot->state, 0, __ATOMIC_SEQ_CST );
    return read_fd( obj->fd, &value ) == sizeof(value);
}

static NTSTATUS recurse_mutex( struct esync_obj *obj, DWORD i, DWORD tid )
{
    int count = __atomic_load_n( &obj->slot->aux, __ATOMIC_SEQ_CST );

    if (count == INT_MAX) return STATUS_MUTANT_LIMIT_EXCEEDED;
    swap_pair( obj->slot, tid, count, tid, count + 1 );
    return i;
}

static NTSTATUS try_now( struct esync_obj *obj, DWORD i, DWORD tid )
{
    int state = __atomic_load_n( &obj->slot->state, __ATOMIC_SEQ_CST );
    BOOL abandoned = FALSE;

    switch (obj->kind)
    {
    case ESYNC_KIND_MUTEX:
        if ((DWORD)state == tid) return recurse_mutex( obj, i, tid );
        if (!mutex_free( state )) break;
        if (take_mutex( obj, tid, &abandoned )) return abandoned ? STATUS_ABANDONED_WAIT_0 + i : i;
        break;
    case ESYNC_KIND_SEMAPHORE:
        if (take_semaphore( obj )) return i;
        break;
    case ESYNC_KIND_AUTO:
        if (state && take_auto( obj )) return i;
        break;
    case ESYNC_KIND_MANUAL:
        if (state) return i;
        break;
    }
    return STATUS_PENDING;
}

static NTSTATUS try_ready( struct esync_obj *obj, DWORD i, DWORD tid )
{
    BOOL abandoned = FALSE;

    switch (obj->kind)
    {
    case ESYNC_KIND_MUTEX:
        if (take_mutex( obj, tid, &abandoned )) return abandoned ? STATUS_ABANDONED_WAIT_0 + i : i;
        break;
    case ESYNC_KIND_SEMAPHORE:
        if (take_semaphore( obj )) return i;
        break;
    case ESYNC_KIND_AUTO:
        if (take_auto( obj )) return i;
        break;
    case ESYNC_KIND_MANUAL:
        return i;
    }
    return STATUS_PENDING;
}

static NTSTATUS wait_any( DWORD count, struct esync_obj *objs, int alert_fd, const struct esync_deadline *end, BOOL track )
{
    struct pollfd fds[MAXIMUM_WAIT_OBJECTS + 1];
    DWORD tid = GetCurrentThreadId(), i;
    unsigned int spins = 0;
    ULONGLONG polled;
    NTSTATUS status;
    BOOL busy;
    nfds_t nfds;
    int ret;

    for (;;)
    {
        for (i = 0; i < count; i++)
            if ((status = try_now( &objs[i], i, tid )) != STATUS_PENDING) return status;
        if (track)
        {
            add_waits( count, objs );
            track = FALSE;
            continue;
        }

        for (i = 0; i < count; i++)
        {
            fds[i].fd = objs[i].fd;
            fds[i].events = POLLIN;
            fds[i].revents = 0;
        }
        nfds = count;
        if (alert_fd >= 0)
        {
            fds[nfds].fd = alert_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }

        polled = monotonic_ns();
        if ((ret = do_poll( fds, nfds, end )) < 0) return errno_to_status( errno );
        if (!ret) return STATUS_TIMEOUT;

        busy = FALSE;
        for (i = 0; i < count; i++)
        {
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) return STATUS_INVALID_HANDLE;
            if (!(fds[i].revents & POLLIN)) continue;
            if ((status = try_ready( &objs[i], i, tid )) != STATUS_PENDING) return status;
            busy = TRUE;
        }
        if (alert_fd >= 0 && (fds[count].revents & POLLIN) && (status = user_apc()) != STATUS_TIMEOUT)
            return status;
        if (expired( end )) return STATUS_TIMEOUT;
        if (busy && monotonic_ns() - polled < ESYNC_SPIN_NS) back_off( &spins );
        else spins = 0;
    }
}

static BOOL owned_mutex( struct esync_obj *obj, DWORD tid )
{
    return obj->kind == ESYNC_KIND_MUTEX && (DWORD)__atomic_load_n( &obj->slot->state, __ATOMIC_SEQ_CST ) == tid;
}

static BOOL ready_now( struct esync_obj *obj, DWORD tid )
{
    int state = __atomic_load_n( &obj->slot->state, __ATOMIC_SEQ_CST );

    switch (obj->kind)
    {
    case ESYNC_KIND_MUTEX:
        return (DWORD)state == tid || (mutex_free( state ) && fd_readable( obj->fd ));
    case ESYNC_KIND_SEMAPHORE:
        return state > 0;
    case ESYNC_KIND_AUTO:
        return fd_readable( obj->fd );
    case ESYNC_KIND_MANUAL:
        return state != 0;
    }
    return FALSE;
}

static NTSTATUS grab_all( DWORD count, struct esync_obj *objs, DWORD tid )
{
    BOOL grabbed[MAXIMUM_WAIT_OBJECTS], owned[MAXIMUM_WAIT_OBJECTS], abandoned = FALSE;
    DWORD order[MAXIMUM_WAIT_OBJECTS], n = 0, i, j;
    int current, prev[MAXIMUM_WAIT_OBJECTS];
    NTSTATUS status = STATUS_PENDING;
    sigset_t old;

    for (i = 0; i < count; i++)
        if (!ready_now( &objs[i], tid )) return STATUS_PENDING;
    for (i = 0; i < count; i++)
    {
        grabbed[i] = owned[i] = FALSE;
        if (objs[i].kind != ESYNC_KIND_AUTO) order[n++] = i;
    }
    for (i = 0; i < count; i++)
        if (objs[i].kind == ESYNC_KIND_AUTO) order[n++] = i;

    block_signals( &old );
    for (j = 0; j < count; j++)
    {
        struct esync_obj *obj = &objs[i = order[j]];

        switch (obj->kind)
        {
        case ESYNC_KIND_MUTEX:
            current = __atomic_load_n( &obj->slot->state, __ATOMIC_SEQ_CST );
            if ((DWORD)current == tid)
            {
                if (__atomic_load_n( &obj->slot->aux, __ATOMIC_SEQ_CST ) == INT_MAX)
                {
                    status = STATUS_MUTANT_LIMIT_EXCEEDED;
                    goto rollback;
                }
                owned[i] = TRUE;
                break;
            }
            prev[i] = current;
            if (!claim_mutex( obj, tid, &prev[i] )) goto rollback;
            grabbed[i] = TRUE;
            break;
        case ESYNC_KIND_SEMAPHORE:
            current = __atomic_load_n( &obj->slot->state, __ATOMIC_SEQ_CST );
            do if (current <= 0) goto rollback;
            while (!__atomic_compare_exchange_n( &obj->slot->state, &current, current - 1, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ));
            if (!consume_token( obj->fd )) goto rollback;
            grabbed[i] = TRUE;
            break;
        case ESYNC_KIND_AUTO:
            if (!take_auto( obj )) goto rollback;
            grabbed[i] = TRUE;
            break;
        case ESYNC_KIND_MANUAL:
            if (!__atomic_load_n( &obj->slot->state, __ATOMIC_SEQ_CST )) goto rollback;
            break;
        }
    }

    for (i = 0; i < count; i++)
    {
        struct esync_slot *slot = objs[i].slot;

        if (owned[i])
        {
            current = __atomic_load_n( &slot->aux, __ATOMIC_SEQ_CST );
            swap_pair( slot, tid, current, tid, current + 1 );
        }
        else if (grabbed[i] && objs[i].kind == ESYNC_KIND_MUTEX)
        {
            if ((DWORD)prev[i] == ~0u) abandoned = TRUE;
            hold_mutex( &objs[i], tid );
            swap_pair( slot, tid, __atomic_load_n( &slot->aux, __ATOMIC_SEQ_CST ), tid, 1 );
        }
    }
    restore_signals( &old );
    waits.held_count = 0;
    return abandoned ? STATUS_ABANDONED : STATUS_SUCCESS;

rollback:
    while (j--)
    {
        i = order[j];
        if (!grabbed[i]) continue;
        switch (objs[i].kind)
        {
        case ESYNC_KIND_MUTEX:
            unclaim_mutex( &objs[i], tid, prev[i] );
            break;
        case ESYNC_KIND_SEMAPHORE:
            current = __atomic_load_n( &objs[i].slot->state, __ATOMIC_SEQ_CST );
            do if (current >= objs[i].slot->aux) break;
            while (!__atomic_compare_exchange_n( &objs[i].slot->state, &current, current + 1, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ));
            if (current < objs[i].slot->aux) wake_fd( objs[i].fd, 1 );
            break;
        case ESYNC_KIND_AUTO:
            wake_fd( objs[i].fd, 1 );
            __atomic_store_n( &objs[i].slot->state, 1, __ATOMIC_SEQ_CST );
            break;
        case ESYNC_KIND_MANUAL:
            break;
        }
    }
    restore_signals( &old );
    return status;
}

static NTSTATUS wait_all( DWORD count, struct esync_obj *objs, int alert_fd, const struct esync_deadline *end, BOOL track )
{
    struct pollfd fds[2];
    DWORD tid = GetCurrentThreadId(), i, j;
    NTSTATUS status;
    nfds_t nfds;
    int ret;

    for (i = 0; i < count; i++)
        for (j = i + 1; j < count; j++)
            if (objs[i].slot == objs[j].slot) return STATUS_INVALID_PARAMETER;

    for (;;)
    {
        if ((status = grab_all( count, objs, tid )) != STATUS_PENDING) return status;
        if (track)
        {
            add_waits( count, objs );
            track = FALSE;
            continue;
        }

        for (i = 0; i < count; i++)
        {
            if (owned_mutex( &objs[i], tid )) continue;
            fds[0].fd = objs[i].fd;
            fds[0].events = POLLIN;
            fds[0].revents = 0;
            nfds = 1;
            if (alert_fd >= 0)
            {
                fds[1].fd = alert_fd;
                fds[1].events = POLLIN;
                fds[1].revents = 0;
                nfds = 2;
            }
            if ((ret = do_poll( fds, nfds, end )) < 0) return errno_to_status( errno );
            if (!ret) return STATUS_TIMEOUT;
            if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) return STATUS_INVALID_HANDLE;
            if (!(fds[0].revents & POLLIN) && nfds == 2 && (fds[1].revents & POLLIN))
            {
                if ((status = grab_all( count, objs, tid )) != STATUS_PENDING) return status;
                if ((status = user_apc()) != STATUS_TIMEOUT) return status;
                break;
            }
        }
        if (i == count)
        {
            if (expired( end )) return STATUS_TIMEOUT;
            sched_yield();
        }
    }
}

NTSTATUS esync_wait( DWORD count, const int *fds, const unsigned int *shm_idxs, const unsigned short *types,
                     WAIT_TYPE type, int alert_fd, const LARGE_INTEGER *timeout )
{
    struct esync_obj objs[MAXIMUM_WAIT_OBJECTS];
    struct esync_deadline end;
    DWORD i, base = waits.count;
    BOOL track = FALSE;
    NTSTATUS ret;

    assert( count <= MAXIMUM_WAIT_OBJECTS );
    for (i = 0; i < count; i++)
    {
        if (!(objs[i].slot = get_slot( shm_idxs[i] ))) return STATUS_INVALID_HANDLE;
        objs[i].fd = fds[i];
        switch (types[i])
        {
        case INPROC_SYNC_SEMAPHORE:
            objs[i].kind = ESYNC_KIND_SEMAPHORE;
            break;
        case INPROC_SYNC_MUTEX:
            objs[i].kind = ESYNC_KIND_MUTEX;
            break;
        default:
            objs[i].kind = (objs[i].slot->flags & ESYNC_MANUAL) ? ESYNC_KIND_MANUAL : ESYNC_KIND_AUTO;
            track = TRUE;
            break;
        }
    }

    get_deadline( timeout, &end );
    if (timeout && !timeout->QuadPart) track = FALSE;
    if (type != WaitAll || count <= 1) ret = wait_any( count, objs, alert_fd, &end, track );
    else ret = wait_all( count, objs, alert_fd, &end, track );
    remove_waits( base );
    return ret;
}

#else

void esync_init( int fd )
{
}

void esync_release( unsigned int shm_idx )
{
}

NTSTATUS esync_release_semaphore( int fd, unsigned int shm_idx, ULONG count, ULONG *prev )
{
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS esync_query_semaphore( int fd, unsigned int shm_idx, SEMAPHORE_BASIC_INFORMATION *info )
{
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS esync_set_event( int fd, unsigned int shm_idx, LONG *prev )
{
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS esync_reset_event( int fd, unsigned int shm_idx, LONG *prev )
{
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS esync_pulse_event( int fd, unsigned int shm_idx, LONG *prev )
{
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS esync_query_event( int fd, unsigned int shm_idx, EVENT_BASIC_INFORMATION *info )
{
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS esync_release_mutex( int fd, unsigned int shm_idx, LONG *prev )
{
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS esync_query_mutex( int fd, unsigned int shm_idx, MUTANT_BASIC_INFORMATION *info )
{
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS esync_wait( DWORD count, const int *fds, const unsigned int *shm_idxs, const unsigned short *types,
                     WAIT_TYPE type, int alert_fd, const LARGE_INTEGER *timeout )
{
    return STATUS_NOT_IMPLEMENTED;
}

#endif
