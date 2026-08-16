#include "state_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int id;
    lua_State *mainL;
    char name[64];
} state_entry;

static state_entry *g_states;
static size_t g_n;
static size_t g_cap;

static int states_grow(void) {
    size_t cap = g_cap ? g_cap * 2 : 4;
    state_entry *n = (state_entry *)realloc(g_states, cap * sizeof(*n));
    if (!n) return -1;
    g_states = n;
    g_cap = cap;
    return 0;
}

int state_registry_count(void) { return (int)g_n; }

int state_registry_has(lua_State *mainL) {
    size_t i;
    if (!mainL) return 0;
    for (i = 0; i < g_n; i++) {
        if (g_states[i].mainL == mainL)
            return 1;
    }
    return 0;
}

const char *state_registry_name(lua_State *mainL) {
    size_t i;
    if (!mainL) return NULL;
    for (i = 0; i < g_n; i++) {
        if (g_states[i].mainL == mainL)
            return g_states[i].name;
    }
    return NULL;
}

lua_State *state_registry_main_at(int index) {
    if (index < 0 || (size_t)index >= g_n) return NULL;
    return g_states[index].mainL;
}

int state_registry_add(lua_State *mainL, const char *name_opt) {
    state_entry *e;
    int id;
    size_t i;

    if (!mainL) return 0;
    for (i = 0; i < g_n; i++) {
        if (g_states[i].mainL == mainL)
            return g_states[i].id;
    }
    if (g_n >= g_cap && states_grow() != 0) return 0;

    id = (int)g_n + 1;
    e = &g_states[g_n];
    memset(e, 0, sizeof(*e));
    e->id = id;
    e->mainL = mainL;
    if (name_opt && name_opt[0])
        snprintf(e->name, sizeof(e->name), "%s", name_opt);
    else if (g_n == 0)
        snprintf(e->name, sizeof(e->name), "main");
    else
        snprintf(e->name, sizeof(e->name), "state-%d", id);
    g_n++;
    return id;
}

void state_registry_clear(void) {
    free(g_states);
    g_states = NULL;
    g_n = 0;
    g_cap = 0;
}
