#include "dap_sync.h"

#ifdef _WIN32

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

void dap_cond_init(dap_cond *c) {
    if (!c) return;
    InitializeConditionVariable(&c->cv);
}

void dap_cond_destroy(dap_cond *c) { (void)c; }

void dap_cond_wait(dap_cond *c) {
    if (!c) return;
    SleepConditionVariableCS(&c->cv, &g_cs, INFINITE);
}

int dap_cond_timedwait(dap_cond *c, unsigned timeout_ms) {
    if (!c) return 1;
    if (SleepConditionVariableCS(&c->cv, &g_cs, timeout_ms))
        return 0;
    return 1;
}

void dap_cond_signal(dap_cond *c) {
    if (!c) return;
    WakeConditionVariable(&c->cv);
}

#else
#include <errno.h>
#include <time.h>

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_inited;

void dap_mutex_init(void) {
    if (g_inited) return;
    g_inited = 1;
}

void dap_mutex_lock(void) { pthread_mutex_lock(&g_mutex); }

void dap_mutex_unlock(void) { pthread_mutex_unlock(&g_mutex); }

void dap_mutex_destroy(void) { g_inited = 0; }

void dap_cond_init(dap_cond *c) {
    if (!c) return;
    pthread_cond_init(&c->cv, NULL);
}

void dap_cond_destroy(dap_cond *c) {
    if (!c) return;
    pthread_cond_destroy(&c->cv);
}

void dap_cond_wait(dap_cond *c) {
    if (!c) return;
    pthread_cond_wait(&c->cv, &g_mutex);
}

int dap_cond_timedwait(dap_cond *c, unsigned timeout_ms) {
    struct timespec ts;
    if (!c) return 1;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 1;
    ts.tv_sec += (time_t)(timeout_ms / 1000u);
    ts.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    if (pthread_cond_timedwait(&c->cv, &g_mutex, &ts) == ETIMEDOUT)
        return 1;
    return 0;
}

void dap_cond_signal(dap_cond *c) {
    if (!c) return;
    pthread_cond_signal(&c->cv);
}

#endif
