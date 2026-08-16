#include "dap_sync.h"

#ifdef _WIN32
#include <windows.h>

static CRITICAL_SECTION g_cs;
static int g_inited;

void dap_mutex_init(void) {
    if (g_inited) return;
    InitializeCriticalSection(&g_cs);
    g_inited = 1;
}

void dap_mutex_lock(void) { EnterCriticalSection(&g_cs); }

void dap_mutex_unlock(void) { LeaveCriticalSection(&g_cs); }

void dap_mutex_destroy(void) {
    if (!g_inited) return;
    DeleteCriticalSection(&g_cs);
    g_inited = 0;
}

/* dap_cond: CONDITION_VARIABLE + SleepConditionVariableCS — future task */

#else
#include <pthread.h>

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_inited;

void dap_mutex_init(void) {
    if (g_inited) return;
    g_inited = 1;
}

void dap_mutex_lock(void) { pthread_mutex_lock(&g_mutex); }

void dap_mutex_unlock(void) { pthread_mutex_unlock(&g_mutex); }

void dap_mutex_destroy(void) { g_inited = 0; }

#endif
