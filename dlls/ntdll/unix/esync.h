#define ESYNC_USED_BY_SERVER 0x7eadeeef
#define ESYNC_SHM_TOKEN      0x7eadeef0

static inline int do_esync(void)
{
    return inproc_device_fd == ESYNC_USED_BY_SERVER;
}

extern void esync_init( int shm_fd );
extern void esync_release( unsigned int shm_idx );
extern NTSTATUS esync_release_semaphore( int fd, unsigned int shm_idx, ULONG count, ULONG *prev );
extern NTSTATUS esync_query_semaphore( int fd, unsigned int shm_idx, SEMAPHORE_BASIC_INFORMATION *info );
extern NTSTATUS esync_set_event( int fd, unsigned int shm_idx, LONG *prev );
extern NTSTATUS esync_reset_event( int fd, unsigned int shm_idx, LONG *prev );
extern NTSTATUS esync_pulse_event( int fd, unsigned int shm_idx, LONG *prev );
extern NTSTATUS esync_query_event( int fd, unsigned int shm_idx, EVENT_BASIC_INFORMATION *info );
extern NTSTATUS esync_release_mutex( int fd, unsigned int shm_idx, LONG *prev );
extern NTSTATUS esync_query_mutex( int fd, unsigned int shm_idx, MUTANT_BASIC_INFORMATION *info );
extern NTSTATUS esync_wait( DWORD count, const int *fds, const unsigned int *shm_idxs, const unsigned short *types,
                            WAIT_TYPE type, int alert_fd, const LARGE_INTEGER *timeout );
