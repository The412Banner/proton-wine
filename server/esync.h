#define ESYNC_USED_BY_SERVER 0x7eadeeef
#define ESYNC_SHM_TOKEN      0x7eadeef0
#define ESYNC_NO_FD          (-2)

extern int esync_active;
static inline int do_esync(void)
{
    return esync_active;
}

extern int esync_init(void);
extern int esync_shm_fd(void);
extern int esync_create( int semaphore, int manual, unsigned int initval, int state, int aux, int lazy, unsigned int *shm_idx );
extern int esync_open( unsigned int shm_idx );
extern void esync_destroy( int fd, unsigned int shm_idx );
extern unsigned int esync_grab( process_id_t pid, unsigned int shm_idx );
extern void esync_client_release( process_id_t pid, unsigned int shm_idx );
extern void esync_cleanup_process( process_id_t pid );
extern void esync_set( int fd, unsigned int shm_idx, int locked );
extern void esync_reset( int fd, unsigned int shm_idx, int locked );
extern void esync_abandon_mutex( int fd, unsigned int shm_idx, thread_id_t tid, int releasing );
extern thread_id_t esync_mutex_owner( unsigned int shm_idx );
