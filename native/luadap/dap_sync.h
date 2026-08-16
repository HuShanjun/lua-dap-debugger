#ifndef DAP_SYNC_H
#define DAP_SYNC_H

/*
 * Session mutex for cross-thread DAP access.
 *
 * Recursive on Win32 (CRITICAL_SECTION): pause_loop may call dap_session_update
 * while the same thread already holds the lock from an outer start/update path.
 */
void dap_mutex_init(void);
void dap_mutex_lock(void);
void dap_mutex_unlock(void);
void dap_mutex_destroy(void);

#endif
