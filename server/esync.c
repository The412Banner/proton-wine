/* droiddeck-esync: eventfd and shared memory synchronization for Wine.
 * Credit: based on the esync design by Elizabeth Figura for Wine and Proton. */
#include "config.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#ifdef HAVE_SYS_EVENTFD_H
# include <sys/eventfd.h>
#endif
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"

#include "handle.h"
#include "request.h"
#include "esync.h"

#define ESYNC_CHUNK_SIZE 0x10000
#define ESYNC_SLOT_SIZE  16
#define ESYNC_PER_CHUNK  (ESYNC_CHUNK_SIZE / ESYNC_SLOT_SIZE)
#define ESYNC_CHUNKS     8192
#define ESYNC_MANUAL     1
#define ESYNC_RELEASING  1

struct esync_slot
{
    union
    {
        struct
        {
            int state;
            int aux;
        };
        int64_t pair;
    };
    int flags;
    int ref;
};

union esync_pair
{
    struct { int state; int aux; } s;
    int64_t word;
};

struct holder
{
    process_id_t   pid;
    unsigned int   count;
    struct holder *next;
};

int esync_active;

static int shm_fd = -1;
static unsigned int shm_chunks;
static void *chunks[ESYNC_CHUNKS];
static struct holder *holders[ESYNC_CHUNKS];
static uint64_t *free_map;
static unsigned int free_words;
static unsigned int free_hint;

#if defined(HAVE_SYS_EVENTFD_H) && defined(HAVE_MEMFD_CREATE)

static void *map_chunk( unsigned int chunk )
{
    void *addr;

    if (chunk >= ESYNC_CHUNKS) return NULL;
    if (chunks[chunk]) return chunks[chunk];
    if (!holders[chunk] && !(holders[chunk] = calloc( ESYNC_PER_CHUNK, sizeof(struct holder) ))) return NULL;
    if (chunk >= shm_chunks)
    {
        if (ftruncate( shm_fd, (off_t)(chunk + 1) * ESYNC_CHUNK_SIZE ) == -1)
        {
            perror( "droiddeck-esync: ftruncate" );
            return NULL;
        }
        shm_chunks = chunk + 1;
    }
    addr = mmap( NULL, ESYNC_CHUNK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd,
                 (off_t)chunk * ESYNC_CHUNK_SIZE );
    if (addr == MAP_FAILED)
    {
        perror( "droiddeck-esync: mmap" );
        return NULL;
    }
    return chunks[chunk] = addr;
}

static struct esync_slot *get_slot( unsigned int idx )
{
    char *base = chunks[idx / ESYNC_PER_CHUNK];
    return (struct esync_slot *)(base + (idx % ESYNC_PER_CHUNK) * ESYNC_SLOT_SIZE);
}

static struct holder *get_holder( unsigned int idx )
{
    return &holders[idx / ESYNC_PER_CHUNK][idx % ESYNC_PER_CHUNK];
}

static void clear_holders( unsigned int idx )
{
    struct holder *head = get_holder( idx ), *next;

    while ((next = head->next))
    {
        head->next = next->next;
        free( next );
    }
    head->pid = 0;
    head->count = 0;
}

static unsigned int alloc_slot(void)
{
    unsigned int i, bit, idx;

    for (i = free_hint; i < free_words; i++) if (free_map[i]) break;
    if (i == free_words)
    {
        unsigned int words = free_words * 2;
        uint64_t *map;

        if (words > ESYNC_CHUNKS * (ESYNC_PER_CHUNK / 64)) words = ESYNC_CHUNKS * (ESYNC_PER_CHUNK / 64);
        if (words == free_words) return 0;
        if (!(map = realloc( free_map, words * sizeof(*map) ))) return 0;
        memset( map + free_words, 0xff, (words - free_words) * sizeof(*map) );
        free_map = map;
        free_words = words;
    }
    bit = __builtin_ctzll( free_map[i] );
    idx = i * 64 + bit;
    if (!map_chunk( idx / ESYNC_PER_CHUNK )) return 0;
    free_map[i] &= ~((uint64_t)1 << bit);
    free_hint = i;
    return idx;
}

static void free_slot( unsigned int idx )
{
    clear_holders( idx );
    free_map[idx / 64] |= (uint64_t)1 << (idx % 64);
    if (idx / 64 < free_hint) free_hint = idx / 64;
}

static int slot_in_use( unsigned int idx )
{
    if (!idx || idx / 64 >= free_words) return 0;
    return !(free_map[idx / 64] & ((uint64_t)1 << (idx % 64)));
}

static void put_ref( unsigned int idx )
{
    struct esync_slot *slot = get_slot( idx );

    if (slot->ref > 0 && !--slot->ref) free_slot( idx );
}

static void report_fd_error( const char *what )
{
    static int warned;
    struct rlimit limit;

    if (warned) return;
    warned = 1;
    if ((errno == EMFILE || errno == ENFILE) && !getrlimit( RLIMIT_NOFILE, &limit ))
        fprintf( stderr, "droiddeck-esync: out of file descriptors in %s (limit %llu); set PROTON_NO_ESYNC=1 for this game if it fails.\n",
                 what, (unsigned long long)limit.rlim_cur );
    else
        fprintf( stderr, "droiddeck-esync: %s failed: %s\n", what, strerror( errno ));
}

static void wake_fd( int fd )
{
    static const uint64_t value = 1;
    if (fd >= 0 && write( fd, &value, sizeof(value) ) == -1 && errno != EAGAIN) perror( "droiddeck-esync: write" );
}

static void drain_fd( int fd )
{
    uint64_t value;
    if (fd >= 0 && read( fd, &value, sizeof(value) ) == -1 && errno != EAGAIN) perror( "droiddeck-esync: read" );
}

static int fd_readable( int fd )
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    return fd >= 0 && poll( &pfd, 1, 0 ) > 0 && (pfd.revents & POLLIN);
}

static int owner_dead( int owner )
{
    return owner > 0 && kill( owner, 0 ) == -1 && errno == ESRCH;
}

static void repair_slot( struct esync_slot *slot, int fd )
{
    int readable;

    if (fd < 0) return;
    readable = fd_readable( fd );
    if (__atomic_load_n( &slot->state, __ATOMIC_SEQ_CST ))
    {
        if (!readable) wake_fd( fd );
    }
    else if (readable) drain_fd( fd );
}

static void lock_slot( struct esync_slot *slot, int fd )
{
    int self = getpid(), owner;
    unsigned int spins = 0;

    for (;;)
    {
        owner = 0;
        if (__atomic_compare_exchange_n( &slot->aux, &owner, self, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED )) return;
        if (++spins < 128) continue;
        if (!(spins & 1023) && owner_dead( owner ) &&
            __atomic_compare_exchange_n( &slot->aux, &owner, self, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED ))
        {
            repair_slot( slot, fd );
            return;
        }
        sched_yield();
    }
}

static void unlock_slot( struct esync_slot *slot )
{
    int self = getpid();
    __atomic_compare_exchange_n( &slot->aux, &self, 0, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED );
}

int esync_init(void)
{
    const char *env = getenv( "WINEESYNC" );
    struct rlimit limit;
    int probe;

    if (!env || !atoi( env )) return 0;
    if ((probe = eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK )) == -1)
    {
        perror( "droiddeck-esync: eventfd" );
        return 0;
    }
    close( probe );
    if ((shm_fd = memfd_create( "droiddeck-esync", MFD_CLOEXEC )) == -1)
    {
        perror( "droiddeck-esync: memfd_create" );
        return 0;
    }
    free_words = 64;
    if (!(free_map = malloc( free_words * sizeof(*free_map) )) || !map_chunk( 0 ))
    {
        free( free_map );
        free_map = NULL;
        free_words = 0;
        close( shm_fd );
        shm_fd = -1;
        return 0;
    }
    memset( free_map, 0xff, free_words * sizeof(*free_map) );
    free_map[0] &= ~(uint64_t)1;
    esync_active = 1;
    fprintf( stderr, "droiddeck-esync: up and running.\n" );
    if (!getrlimit( RLIMIT_NOFILE, &limit ))
        fprintf( stderr, "droiddeck-esync: file descriptor limit %llu.\n", (unsigned long long)limit.rlim_cur );
    return 1;
}

int esync_shm_fd(void)
{
    return shm_fd;
}

int esync_create( int semaphore, int manual, unsigned int initval, int state, int aux, int lazy, unsigned int *shm_idx )
{
    struct esync_slot *slot;
    unsigned int idx;
    int fd = ESYNC_NO_FD;

    if (!lazy && (fd = eventfd( initval, EFD_CLOEXEC | EFD_NONBLOCK | (semaphore ? EFD_SEMAPHORE : 0) )) == -1)
    {
        report_fd_error( "eventfd" );
        return -1;
    }
    if (!(idx = alloc_slot()))
    {
        if (fd >= 0) close( fd );
        return -1;
    }
    slot = get_slot( idx );
    slot->state = state;
    slot->aux = aux;
    slot->flags = manual ? ESYNC_MANUAL : 0;
    __atomic_store_n( &slot->ref, 1, __ATOMIC_SEQ_CST );
    *shm_idx = idx;
    return fd;
}

int esync_open( unsigned int shm_idx )
{
    int fd;

    if (!slot_in_use( shm_idx )) return -1;
    if ((fd = eventfd( __atomic_load_n( &get_slot( shm_idx )->state, __ATOMIC_SEQ_CST ) ? 1 : 0,
                       EFD_CLOEXEC | EFD_NONBLOCK )) == -1)
        report_fd_error( "eventfd" );
    return fd;
}

void esync_destroy( int fd, unsigned int shm_idx )
{
    if (fd >= 0) close( fd );
    if (slot_in_use( shm_idx )) put_ref( shm_idx );
}

unsigned int esync_grab( process_id_t pid, unsigned int shm_idx )
{
    struct holder *head, *node;

    if (!slot_in_use( shm_idx )) return 0;
    head = get_holder( shm_idx );
    for (node = head; node; node = node->next)
    {
        if (node->count && node->pid == pid)
        {
            node->count++;
            get_slot( shm_idx )->ref++;
            return shm_idx;
        }
    }
    if (!head->count)
    {
        head->pid = pid;
        head->count = 1;
    }
    else
    {
        if (!(node = malloc( sizeof(*node) ))) return 0;
        node->pid = pid;
        node->count = 1;
        node->next = head->next;
        head->next = node;
    }
    get_slot( shm_idx )->ref++;
    return shm_idx;
}

static unsigned int drop_holder( unsigned int idx, process_id_t pid, int all )
{
    struct holder *head = get_holder( idx ), *prev = NULL, *node, *next;
    unsigned int dropped;

    for (node = head; node; prev = node, node = node->next)
        if (node->count && node->pid == pid) break;
    if (!node) return 0;
    dropped = all ? node->count : 1;
    if ((node->count -= dropped)) return dropped;
    if (node != head)
    {
        prev->next = node->next;
        free( node );
    }
    else if ((next = head->next))
    {
        *head = *next;
        free( next );
    }
    else head->pid = 0;
    return dropped;
}

void esync_client_release( process_id_t pid, unsigned int shm_idx )
{
    if (!slot_in_use( shm_idx )) return;
    if (drop_holder( shm_idx, pid, 0 )) put_ref( shm_idx );
}

void esync_cleanup_process( process_id_t pid )
{
    unsigned int i, idx, count;
    uint64_t used;

    for (i = 0; i < free_words; i++)
    {
        used = ~free_map[i];
        while (used)
        {
            idx = i * 64 + __builtin_ctzll( used );
            used &= used - 1;
            if (!idx || !slot_in_use( idx )) continue;
            for (count = drop_holder( idx, pid, 1 ); count && slot_in_use( idx ); count--) put_ref( idx );
        }
    }
}

void esync_set( int fd, unsigned int shm_idx, int locked )
{
    struct esync_slot *slot = get_slot( shm_idx );

    if (!(slot->flags & ESYNC_MANUAL))
    {
        wake_fd( fd );
        __atomic_store_n( &slot->state, 1, __ATOMIC_SEQ_CST );
        return;
    }
    if (locked) lock_slot( slot, fd );
    if (!__atomic_exchange_n( &slot->state, 1, __ATOMIC_SEQ_CST )) wake_fd( fd );
    if (locked) unlock_slot( slot );
}

void esync_reset( int fd, unsigned int shm_idx, int locked )
{
    struct esync_slot *slot = get_slot( shm_idx );

    if (!(slot->flags & ESYNC_MANUAL))
    {
        __atomic_store_n( &slot->state, 0, __ATOMIC_SEQ_CST );
        drain_fd( fd );
        return;
    }
    if (locked) lock_slot( slot, fd );
    if (__atomic_exchange_n( &slot->state, 0, __ATOMIC_SEQ_CST )) drain_fd( fd );
    if (locked) unlock_slot( slot );
}

void esync_abandon_mutex( int fd, unsigned int shm_idx, thread_id_t tid, int releasing )
{
    struct esync_slot *slot = get_slot( shm_idx );
    union esync_pair cur, next = { .s = { ~0, 0 } };

    cur.word = __atomic_load_n( &slot->pair, __ATOMIC_SEQ_CST );
    for (;;)
    {
        if (cur.s.state != (int)tid &&
            (!releasing || cur.s.state != (int)(tid | ESYNC_RELEASING) || fd_readable( fd ))) return;
        if (__atomic_compare_exchange_n( &slot->pair, &cur.word, next.word, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST )) break;
    }
    wake_fd( fd );
}

thread_id_t esync_mutex_owner( unsigned int shm_idx )
{
    int owner;

    if (!slot_in_use( shm_idx )) return 0;
    owner = __atomic_load_n( &get_slot( shm_idx )->state, __ATOMIC_SEQ_CST );
    return owner == ~0 ? 0 : owner & ~ESYNC_RELEASING;
}

#else

int esync_init(void)
{
    return 0;
}

int esync_shm_fd(void)
{
    return -1;
}

int esync_create( int semaphore, int manual, unsigned int initval, int state, int aux, int lazy, unsigned int *shm_idx )
{
    errno = ENOSYS;
    return -1;
}

int esync_open( unsigned int shm_idx )
{
    return -1;
}

void esync_destroy( int fd, unsigned int shm_idx )
{
}

unsigned int esync_grab( process_id_t pid, unsigned int shm_idx )
{
    return 0;
}

void esync_client_release( process_id_t pid, unsigned int shm_idx )
{
}

void esync_cleanup_process( process_id_t pid )
{
}

void esync_set( int fd, unsigned int shm_idx, int locked )
{
}

void esync_reset( int fd, unsigned int shm_idx, int locked )
{
}

void esync_abandon_mutex( int fd, unsigned int shm_idx, thread_id_t tid, int releasing )
{
}

thread_id_t esync_mutex_owner( unsigned int shm_idx )
{
    return 0;
}

#endif
