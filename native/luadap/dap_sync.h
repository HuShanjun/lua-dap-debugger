#ifndef DAP_SYNC_H
#define DAP_SYNC_H

#ifdef _WIN32
#include <windows.h>
typedef struct dap_cond {
    CONDITION_VARIABLE cv;
} dap_cond;
#else
#include <pthread.h>
typedef struct dap_cond {
    pthread_cond_t cv;
} dap_cond;
#endif

/*
 * Session mutex for cross-thread DAP access.
 *
 * Recursive on Win32 (CRITICAL_SECTION): pause_loop may call dap_session_update
 * while the same thread already holds the lock from an outer start/update path.
 * dap_cond_wait / dap_cond_timedwait require lock depth 1 (they release once).
 */
void dap_mutex_init(void);
void dap_mutex_lock(void);
void dap_mutex_unlock(void);
void dap_mutex_destroy(void);

void dap_cond_init(dap_cond *c);
void dap_cond_destroy(dap_cond *c);
/* Must hold mutex; atomically unlock+wait; re-acquire. */
void dap_cond_wait(dap_cond *c);
/* 0 = signaled, 1 = timeout. Must hold mutex. */
int dap_cond_timedwait(dap_cond *c, unsigned timeout_ms);
void dap_cond_signal(dap_cond *c);

#endif
