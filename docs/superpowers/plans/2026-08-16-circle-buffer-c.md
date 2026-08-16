# 纯 C Circle Buffer 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `native/common` 下实现独立可复用的纯 C SPSC 环形缓冲，语义对齐 `sample/TCircleBuffer.*`，并带冒烟测试。

**Architecture:** 不透明 `circle_buffer*`；块为 `malloc(block_size)`，头含 `_Atomic` 的 read/write 游标与 next 指针，payload 紧随其后。生产者用 release 发布，消费者用 acquire 观察。逻辑按 `TCircleBuffer.inl` 移植；`WriteBuffer` 扩容失败返回错误且不发布，避免半帧可见。

**Tech Stack:** C11、`stdatomic.h`、CMake、现有 `native/common` 布局

**Spec:** `docs/superpowers/specs/2026-08-16-circle-buffer-c-design.md`

## Global Constraints

- 仅 SPSC：一个线程 push*，一个线程 pop*/can_pop/waiting_count
- C11 `_Atomic`，acquire/release 模式与 C++ 参考一致
- 构造至少 2 个块成环；`block_size` / `free_empty_buffer` 运行时可配
- Context 为 `void*` + `size`；帧布局对齐 C++（merge / 非 merge）
- Push 扩容失败：返回非 0，**不**发布 `write_pos` / 写块指针
- 本阶段不链接 `luadap` / `asyncsocket`
- Commit 信息英文、简洁
- Build：`cmake -S . -B build "-DLUA_VERSION=5.4"` 后 `cmake --build build --config Release --target circle_buffer_test`

---

## File Structure

| File | Responsibility |
|------|----------------|
| `native/common/circle_buffer.h` | 公开类型与 API |
| `native/common/circle_buffer.c` | 块分配、读写辅助、push/pop、查询 |
| `native/common/CMakeLists.txt` | `circle_buffer` 静态库 + `circle_buffer_test` |
| `native/common/circle_buffer_test.c` | 冒烟测试 |

**Locked public API（必须与此一致）：**

```c
#ifndef CIRCLE_BUFFER_H
#define CIRCLE_BUFFER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct circle_buffer circle_buffer;

typedef struct circle_buf {
    const void *data;
    uint32_t size;
} circle_buf;

circle_buffer *circle_buffer_create(uint32_t block_size, int free_empty_buffer);
void circle_buffer_destroy(circle_buffer *cb);

int circle_buffer_push_raw(circle_buffer *cb, const circle_buf *bufs, uint32_t count);
int circle_buffer_push_raw_one(circle_buffer *cb, const void *data, uint32_t size);
uint32_t circle_buffer_pop_raw(circle_buffer *cb, void *out, uint32_t out_size);

int circle_buffer_push_buffer(circle_buffer *cb, const void *context, size_t context_size,
                              const circle_buf *bufs, uint32_t count, int merge);
int circle_buffer_pop_buffer(circle_buffer *cb, void *context_out, size_t context_size,
                             circle_buf *bufs, uint32_t *inout_count);

int circle_buffer_can_pop(const circle_buffer *cb);
uint32_t circle_buffer_waiting_count(const circle_buffer *cb);

#ifdef __cplusplus
}
#endif

#endif
```

**Locked internal shapes（`circle_buffer.c`）：**

```c
typedef struct cb_block {
    _Atomic uint32_t read_pos;
    _Atomic uint32_t write_pos;
    _Atomic(struct cb_block *) next;
    /* payload bytes follow in the malloc'd region */
} cb_block;

struct circle_buffer {
    uint32_t block_size;
    uint32_t payload_cap; /* block_size - sizeof(cb_block) */
    int free_empty;
    _Atomic(cb_block *) read_buf;
    _Atomic(cb_block *) write_buf;
    _Atomic uint64_t push_count;
    _Atomic uint64_t pop_count;
    uint8_t *scratch;
    size_t scratch_cap;
};
```

**Payload 访问：** `((uint8_t *)block) + sizeof(cb_block)`，长度为 `payload_cap`。

**返回约定：** push 成功 `0`，失败非 `0`；pop_buffer / can_pop 成功/可读为 `1`，否则 `0`；pop_raw 返回字节数。

---

### Task 1: 头文件、CMake、create/destroy

**Files:**
- Create: `native/common/circle_buffer.h`
- Create: `native/common/circle_buffer.c`（仅 create/destroy + 内部 alloc/free）
- Modify: `native/common/CMakeLists.txt`
- Create: `native/common/circle_buffer_test.c`（先只测 create/destroy）

**Interfaces:**
- Consumes: 无
- Produces: `circle_buffer_create` / `circle_buffer_destroy`；内部 `cb_block` 布局

- [ ] **Step 1: 写失败测试（create/destroy）**

创建 `native/common/circle_buffer_test.c`：

```c
#include "circle_buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail = 1; \
    } \
} while (0)

static void test_create_destroy(void) {
    CHECK(circle_buffer_create(8, 0) == NULL); /* too small for header+payload */
    circle_buffer *cb = circle_buffer_create(64, 0);
    CHECK(cb != NULL);
    circle_buffer_destroy(cb);
    circle_buffer_destroy(NULL); /* must be safe */
}

int main(void) {
    test_create_destroy();
    if (g_fail) {
        fprintf(stderr, "circle_buffer_test FAILED\n");
        return 1;
    }
    printf("circle_buffer_test OK\n");
    return 0;
}
```

- [ ] **Step 2: 更新 CMake（先让测试能链接）**

在 `native/common/CMakeLists.txt` 末尾追加：

```cmake
add_library(circle_buffer STATIC circle_buffer.c)
target_include_directories(circle_buffer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_property(TARGET circle_buffer PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET circle_buffer PROPERTY FOLDER "native")

add_executable(circle_buffer_test circle_buffer_test.c)
target_link_libraries(circle_buffer_test PRIVATE circle_buffer)
set_property(TARGET circle_buffer_test PROPERTY FOLDER "native")
```

保留原有 `lua_compat` 目标不动。

- [ ] **Step 3: 写公开头文件**

创建 `native/common/circle_buffer.h`，内容使用上文 **Locked public API** 整段。

- [ ] **Step 4: 实现 create/destroy**

创建 `native/common/circle_buffer.c`：

```c
#include "circle_buffer.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct cb_block {
    _Atomic uint32_t read_pos;
    _Atomic uint32_t write_pos;
    _Atomic(struct cb_block *) next;
} cb_block;

struct circle_buffer {
    uint32_t block_size;
    uint32_t payload_cap;
    int free_empty;
    _Atomic(cb_block *) read_buf;
    _Atomic(cb_block *) write_buf;
    _Atomic uint64_t push_count;
    _Atomic uint64_t pop_count;
    uint8_t *scratch;
    size_t scratch_cap;
};

static uint8_t *cb_payload(cb_block *b) {
    return (uint8_t *)b + sizeof(cb_block);
}

static cb_block *cb_alloc_block(uint32_t block_size) {
    cb_block *b = (cb_block *)calloc(1, block_size);
    if (!b)
        return NULL;
    atomic_init(&b->read_pos, 0);
    atomic_init(&b->write_pos, 0);
    atomic_init(&b->next, (cb_block *)NULL);
    return b;
}

static void cb_free_block(cb_block *b) {
    free(b);
}

circle_buffer *circle_buffer_create(uint32_t block_size, int free_empty_buffer) {
    if (block_size <= (uint32_t)sizeof(cb_block))
        return NULL;

    circle_buffer *cb = (circle_buffer *)calloc(1, sizeof(*cb));
    if (!cb)
        return NULL;

    cb->block_size = block_size;
    cb->payload_cap = block_size - (uint32_t)sizeof(cb_block);
    cb->free_empty = free_empty_buffer ? 1 : 0;
    atomic_init(&cb->push_count, 0);
    atomic_init(&cb->pop_count, 0);
    cb->scratch = NULL;
    cb->scratch_cap = 0;

    cb_block *w = cb_alloc_block(block_size);
    cb_block *n = cb_alloc_block(block_size);
    if (!w || !n) {
        cb_free_block(w);
        cb_free_block(n);
        free(cb);
        return NULL;
    }
    atomic_store_explicit(&n->next, w, memory_order_relaxed);
    atomic_store_explicit(&w->next, n, memory_order_relaxed);
    atomic_init(&cb->write_buf, w);
    atomic_init(&cb->read_buf, w);
    (void)cb_payload; /* used by later tasks */
    return cb;
}

void circle_buffer_destroy(circle_buffer *cb) {
    if (!cb)
        return;
    cb_block *start = atomic_load_explicit(&cb->write_buf, memory_order_relaxed);
    if (start) {
        cb_block *cur = start;
        for (;;) {
            cb_block *next = atomic_load_explicit(&cur->next, memory_order_relaxed);
            cb_free_block(cur);
            if (next == start)
                break;
            cur = next;
        }
    }
    free(cb->scratch);
    free(cb);
}

/* stubs so later tasks compile if linked early — remove as functions are filled */
int circle_buffer_push_raw(circle_buffer *cb, const circle_buf *bufs, uint32_t count) {
    (void)cb; (void)bufs; (void)count; return -1;
}
int circle_buffer_push_raw_one(circle_buffer *cb, const void *data, uint32_t size) {
    (void)cb; (void)data; (void)size; return -1;
}
uint32_t circle_buffer_pop_raw(circle_buffer *cb, void *out, uint32_t out_size) {
    (void)cb; (void)out; (void)out_size; return 0;
}
int circle_buffer_push_buffer(circle_buffer *cb, const void *context, size_t context_size,
                              const circle_buf *bufs, uint32_t count, int merge) {
    (void)cb; (void)context; (void)context_size; (void)bufs; (void)count; (void)merge;
    return -1;
}
int circle_buffer_pop_buffer(circle_buffer *cb, void *context_out, size_t context_size,
                             circle_buf *bufs, uint32_t *inout_count) {
    (void)cb; (void)context_out; (void)context_size; (void)bufs; (void)inout_count;
    return 0;
}
int circle_buffer_can_pop(const circle_buffer *cb) {
    (void)cb; return 0;
}
uint32_t circle_buffer_waiting_count(const circle_buffer *cb) {
    (void)cb; return 0;
}
```

- [ ] **Step 5: 编译并运行测试**

```bash
cmake -S . -B build "-DLUA_VERSION=5.4"
cmake --build build --config Release --target circle_buffer_test
./bin/circle_buffer_test
```

Windows（本机）：

```powershell
cmake -S . -B build "-DLUA_VERSION=5.4"
cmake --build build --config Release --target circle_buffer_test
.\bin\circle_buffer_test.exe
```

Expected: 打印 `circle_buffer_test OK`，退出码 0。

- [ ] **Step 6: Commit**

```bash
git add native/common/circle_buffer.h native/common/circle_buffer.c native/common/circle_buffer_test.c native/common/CMakeLists.txt
git commit -m "feat(common): scaffold pure-C circle_buffer create/destroy"
```

---

### Task 2: write/read 辅助 + push_raw / pop_raw

**Files:**
- Modify: `native/common/circle_buffer.c`
- Modify: `native/common/circle_buffer_test.c`

**Interfaces:**
- Consumes: Task 1 的 `cb_block` / `circle_buffer` / `cb_alloc_block` / `payload_cap`
- Produces: `circle_buffer_push_raw`、`circle_buffer_push_raw_one`、`circle_buffer_pop_raw`；内部 `write_bytes` / `publish_write` / 空检测

- [ ] **Step 1: 扩展失败测试（raw 往返 + 跨块）**

在 `circle_buffer_test.c` 的 `main` 前加入：

```c
static void test_raw_roundtrip(void) {
    circle_buffer *cb = circle_buffer_create(64, 0);
    CHECK(cb != NULL);

    CHECK(circle_buffer_can_pop(cb) == 0);
    CHECK(circle_buffer_waiting_count(cb) == 0);

    const char *msg = "hello-circle";
    CHECK(circle_buffer_push_raw_one(cb, msg, (uint32_t)strlen(msg)) == 0);
    CHECK(circle_buffer_waiting_count(cb) == 1);
    CHECK(circle_buffer_can_pop(cb) == 1);

    char out[64];
    memset(out, 0, sizeof(out));
    uint32_t n = circle_buffer_pop_raw(cb, out, sizeof(out));
    CHECK(n == (uint32_t)strlen(msg));
    CHECK(memcmp(out, msg, n) == 0);
    CHECK(circle_buffer_waiting_count(cb) == 0);
    CHECK(circle_buffer_can_pop(cb) == 0);

    /* force multi-block: payload_cap for block_size=64 is < 64 */
    uint8_t big[200];
    for (int i = 0; i < 200; i++)
        big[i] = (uint8_t)(i & 0xff);
    CHECK(circle_buffer_push_raw_one(cb, big, 200) == 0);
    uint8_t big_out[200];
    memset(big_out, 0, sizeof(big_out));
    n = circle_buffer_pop_raw(cb, big_out, 200);
    CHECK(n == 200);
    CHECK(memcmp(big_out, big, 200) == 0);

    circle_buf parts[2] = {
        { "AB", 2 },
        { "CD", 2 },
    };
    CHECK(circle_buffer_push_raw(cb, parts, 2) == 0);
    char four[8] = {0};
    n = circle_buffer_pop_raw(cb, four, 4);
    CHECK(n == 4);
    CHECK(memcmp(four, "ABCD", 4) == 0);

    circle_buffer_destroy(cb);
}
```

在 `main` 中调用 `test_raw_roundtrip();`（可先注释 can_pop/waiting 断言，若 Task 4 才实现——**本计划要求 Task 2 同步实现 can_pop/waiting，因测试需要**）。

说明：`can_pop` / `waiting_count` 在 Task 2 一并实现（规格要求且测试依赖），Task 4 只补 free_empty 冒烟与 framed。

- [ ] **Step 2: 运行测试确认失败**

```powershell
cmake --build build --config Release --target circle_buffer_test
.\bin\circle_buffer_test.exe
```

Expected: FAIL（push_raw stub 返回 -1 或 pop 得到 0）。

- [ ] **Step 3: 实现 write_bytes、publish、push_raw、pop_raw、can_pop、waiting_count**

在 `circle_buffer.c` 中用下列实现**替换** Task 1 的 stub（保留 create/destroy）。关键逻辑：

```c
static int write_bytes(circle_buffer *cb, cb_block **p_write, uint32_t *p_wpos,
                       cb_block *read_buf, const void *src, size_t n) {
    const uint8_t *cur = (const uint8_t *)src;
    while (n) {
        uint32_t left = cb->payload_cap - *p_wpos;
        uint32_t chunk = (uint32_t)((n < left) ? n : left);
        memcpy(cb_payload(*p_write) + *p_wpos, cur, chunk);
        cur += chunk;
        *p_wpos += chunk;
        n -= chunk;

        if (*p_wpos != cb->payload_cap) {
            cb_block *next = atomic_load_explicit(&(*p_write)->next, memory_order_acquire);
            if (cb->free_empty && next != read_buf && next != *p_write) {
                cb_block *check = atomic_load_explicit(&next->next, memory_order_acquire);
                while (check != read_buf && check != *p_write) {
                    cb_block *victim = check;
                    check = atomic_load_explicit(&victim->next, memory_order_acquire);
                    atomic_store_explicit(&next->next, check, memory_order_release);
                    cb_free_block(victim);
                }
            }
            break;
        }

        cb_block *cur_next = atomic_load_explicit(&(*p_write)->next, memory_order_acquire);
        if (cur_next == read_buf) {
            cb_block *neu = cb_alloc_block(cb->block_size);
            if (!neu)
                return -1;
            atomic_store_explicit(&neu->next, read_buf, memory_order_relaxed);
            atomic_store_explicit(&(*p_write)->next, neu, memory_order_release);
            cur_next = neu;
        }
        *p_write = cur_next;
        *p_wpos = 0;
    }
    return 0;
}

static void publish_write(circle_buffer *cb, cb_block *start, cb_block *end, uint32_t wpos) {
    while (start != end) {
        atomic_store_explicit(&start->write_pos, cb->payload_cap, memory_order_release);
        start = atomic_load_explicit(&start->next, memory_order_acquire);
    }
    atomic_store_explicit(&end->write_pos, wpos, memory_order_release);
    atomic_store_explicit(&cb->write_buf, end, memory_order_release);
    atomic_fetch_add_explicit(&cb->push_count, 1, memory_order_relaxed);
}

static int begin_write(circle_buffer *cb, cb_block **start, cb_block **end,
                       cb_block **read_buf, uint32_t *wpos) {
    *start = atomic_load_explicit(&cb->write_buf, memory_order_relaxed);
    *end = *start;
    *read_buf = atomic_load_explicit(&cb->read_buf, memory_order_acquire);
    *wpos = atomic_load_explicit(&(*start)->write_pos, memory_order_relaxed);
    return 0;
}

int circle_buffer_push_raw(circle_buffer *cb, const circle_buf *bufs, uint32_t count) {
    if (!cb || count == 0)
        return 0;
    cb_block *start, *end, *read_buf;
    uint32_t wpos;
    begin_write(cb, &start, &end, &read_buf, &wpos);
    for (uint32_t i = 0; i < count; i++) {
        if (write_bytes(cb, &end, &wpos, read_buf, bufs[i].data, bufs[i].size) != 0)
            return -1; /* do not publish */
    }
    publish_write(cb, start, end, wpos);
    return 0;
}

int circle_buffer_push_raw_one(circle_buffer *cb, const void *data, uint32_t size) {
    circle_buf one = { data, size };
    return circle_buffer_push_raw(cb, &one, 1);
}

uint32_t circle_buffer_pop_raw(circle_buffer *cb, void *out, uint32_t out_size) {
    if (!cb || !out || out_size == 0)
        return 0;

    cb_block *write_buf = atomic_load_explicit(&cb->write_buf, memory_order_acquire);
    uint32_t write_pos = atomic_load_explicit(&write_buf->write_pos, memory_order_acquire);
    if (write_pos == cb->payload_cap)
        return 0;

    cb_block *read_buf = atomic_load_explicit(&cb->read_buf, memory_order_relaxed);
    uint32_t read_pos = atomic_load_explicit(&read_buf->read_pos, memory_order_relaxed);
    if (read_buf == write_buf && read_pos == write_pos)
        return 0;

    uint8_t *dst = (uint8_t *)out;
    uint32_t remain = out_size;
    while (remain) {
        uint32_t cur_w = atomic_load_explicit(&read_buf->write_pos, memory_order_acquire);
        uint32_t left = cur_w - read_pos;
        if (left == 0)
            break;
        uint32_t n = remain < left ? remain : left;
        memcpy(dst, cb_payload(read_buf) + read_pos, n);
        dst += n;
        read_pos += n;
        remain -= n;
        if (read_pos != cb->payload_cap)
            break;
        read_buf = atomic_load_explicit(&read_buf->next, memory_order_acquire);
        read_pos = 0;
    }

    atomic_store_explicit(&read_buf->read_pos, read_pos, memory_order_release);
    atomic_store_explicit(&cb->read_buf, read_buf, memory_order_release);
    atomic_fetch_add_explicit(&cb->pop_count, 1, memory_order_relaxed);
    return (uint32_t)(dst - (uint8_t *)out);
}

int circle_buffer_can_pop(const circle_buffer *cb) {
    if (!cb)
        return 0;
    cb_block *write_buf = atomic_load_explicit(&cb->write_buf, memory_order_acquire);
    uint32_t write_pos = atomic_load_explicit(&write_buf->write_pos, memory_order_acquire);
    if (write_pos == cb->payload_cap)
        return 0;
    cb_block *read_buf = atomic_load_explicit(&cb->read_buf, memory_order_acquire);
    uint32_t read_pos = atomic_load_explicit(&read_buf->read_pos, memory_order_acquire);
    if (read_buf == write_buf && read_pos == write_pos)
        return 0;
    return 1;
}

uint32_t circle_buffer_waiting_count(const circle_buffer *cb) {
    if (!cb)
        return 0;
    uint64_t p = atomic_load_explicit(&cb->push_count, memory_order_acquire);
    uint64_t q = atomic_load_explicit(&cb->pop_count, memory_order_acquire);
    return (uint32_t)(p - q);
}
```

注意：`write_bytes` 失败时已可能把新块链入环——按规格允许；只要不 `publish_write`，消费者看不到半帧。

- [ ] **Step 4: 运行测试确认通过**

```powershell
cmake --build build --config Release --target circle_buffer_test
.\bin\circle_buffer_test.exe
```

Expected: `circle_buffer_test OK`

- [ ] **Step 5: Commit**

```bash
git add native/common/circle_buffer.c native/common/circle_buffer_test.c
git commit -m "feat(common): implement circle_buffer push_raw/pop_raw"
```

---

### Task 3: push_buffer / pop_buffer（带帧）

**Files:**
- Modify: `native/common/circle_buffer.c`
- Modify: `native/common/circle_buffer_test.c`

**Interfaces:**
- Consumes: `write_bytes` / `publish_write` / `begin_write`；scratch 缓冲
- Produces: `circle_buffer_push_buffer` / `circle_buffer_pop_buffer`

- [ ] **Step 1: 写 framed 测试**

```c
static void test_framed(void) {
    circle_buffer *cb = circle_buffer_create(128, 0);
    CHECK(cb != NULL);

    uint32_t ctx_in = 0x12345678u;
    circle_buf parts[2] = { { "hello", 5 }, { "world", 5 } };
    CHECK(circle_buffer_push_buffer(cb, &ctx_in, sizeof(ctx_in), parts, 2, 0) == 0);

    uint32_t ctx_out = 0;
    circle_buf out[2];
    uint32_t count = 2;
    CHECK(circle_buffer_pop_buffer(cb, &ctx_out, sizeof(ctx_out), out, &count) == 1);
    CHECK(ctx_out == ctx_in);
    CHECK(count == 2);
    CHECK(out[0].size == 5 && memcmp(out[0].data, "hello", 5) == 0);
    CHECK(out[1].size == 5 && memcmp(out[1].data, "world", 5) == 0);

    /* merge: one segment, concatenated bytes */
    CHECK(circle_buffer_push_buffer(cb, &ctx_in, sizeof(ctx_in), parts, 2, 1) == 0);
    count = 1;
    CHECK(circle_buffer_pop_buffer(cb, &ctx_out, sizeof(ctx_out), out, &count) == 1);
    CHECK(count == 1);
    CHECK(out[0].size == 10);
    CHECK(memcmp(out[0].data, "helloworld", 10) == 0);

    circle_buffer_destroy(cb);
}
```

在 `main` 中调用。

- [ ] **Step 2: 运行确认失败**

Expected: FAIL（framed stub）。

- [ ] **Step 3: 实现 framed API**

```c
static int ensure_scratch(circle_buffer *cb, size_t need) {
    if (cb->scratch_cap >= need)
        return 0;
    uint8_t *p = (uint8_t *)realloc(cb->scratch, need);
    if (!p)
        return -1;
    cb->scratch = p;
    cb->scratch_cap = need;
    return 0;
}

static int read_bytes(circle_buffer *cb, cb_block **p_read, uint32_t *p_rpos,
                      void *dst, size_t n) {
    uint8_t *cur = (uint8_t *)dst;
    while (n) {
        uint32_t wpos = atomic_load_explicit(&(*p_read)->write_pos, memory_order_acquire);
        uint32_t left = wpos - *p_rpos;
        assert(left > 0);
        uint32_t chunk = (uint32_t)n;
        if (n > left) {
            assert(wpos == cb->payload_cap);
            chunk = left;
        }
        memcpy(cur, cb_payload(*p_read) + *p_rpos, chunk);
        cur += chunk;
        *p_rpos += chunk;
        n -= chunk;
        if (*p_rpos != cb->payload_cap)
            break;
        *p_read = atomic_load_explicit(&(*p_read)->next, memory_order_acquire);
        *p_rpos = 0;
    }
    return 0;
}

int circle_buffer_push_buffer(circle_buffer *cb, const void *context, size_t context_size,
                              const circle_buf *bufs, uint32_t count, int merge) {
    if (!cb)
        return -1;
    if (context_size && !context)
        return -1;

    cb_block *start, *end, *read_buf;
    uint32_t wpos;
    begin_write(cb, &start, &end, &read_buf, &wpos);

    if (context_size) {
        if (write_bytes(cb, &end, &wpos, read_buf, context, context_size) != 0)
            return -1;
    }

    if (merge && count) {
        uint32_t one = 1;
        if (write_bytes(cb, &end, &wpos, read_buf, &one, sizeof(one)) != 0)
            return -1;
        uint32_t total = 0;
        for (uint32_t i = 0; i < count; i++)
            total += bufs[i].size;
        if (write_bytes(cb, &end, &wpos, read_buf, &total, sizeof(total)) != 0)
            return -1;
        for (uint32_t i = 0; i < count; i++) {
            if (write_bytes(cb, &end, &wpos, read_buf, bufs[i].data, bufs[i].size) != 0)
                return -1;
        }
    } else {
        if (write_bytes(cb, &end, &wpos, read_buf, &count, sizeof(count)) != 0)
            return -1;
        for (uint32_t i = 0; i < count; i++) {
            if (write_bytes(cb, &end, &wpos, read_buf, &bufs[i].size, sizeof(uint32_t)) != 0)
                return -1;
            if (write_bytes(cb, &end, &wpos, read_buf, bufs[i].data, bufs[i].size) != 0)
                return -1;
        }
    }

    publish_write(cb, start, end, wpos);
    return 0;
}

int circle_buffer_pop_buffer(circle_buffer *cb, void *context_out, size_t context_size,
                             circle_buf *bufs, uint32_t *inout_count) {
    if (!cb || !inout_count || !bufs)
        return 0;

    cb_block *write_buf = atomic_load_explicit(&cb->write_buf, memory_order_acquire);
    uint32_t write_pos = atomic_load_explicit(&write_buf->write_pos, memory_order_acquire);
    if (write_pos == cb->payload_cap)
        return 0;

    cb_block *read_buf = atomic_load_explicit(&cb->read_buf, memory_order_relaxed);
    uint32_t read_pos = atomic_load_explicit(&read_buf->read_pos, memory_order_relaxed);
    if (read_buf == write_buf && read_pos == write_pos)
        return 0;

    if (context_size) {
        if (!context_out)
            return 0;
        read_bytes(cb, &read_buf, &read_pos, context_out, context_size);
    }

    uint32_t ncount = 0;
    read_bytes(cb, &read_buf, &read_pos, &ncount, sizeof(ncount));
    /* Caller must pass capacity >= frame count (same contract as C++ GammaAst). */
    assert(ncount <= *inout_count);
    *inout_count = ncount;

    size_t total = 0;
    for (uint32_t i = 0; i < ncount; i++) {
        uint32_t sz = 0;
        read_bytes(cb, &read_buf, &read_pos, &sz, sizeof(sz));
        bufs[i].size = sz;
        size_t need = total + sz;
        if (ensure_scratch(cb, need) != 0)
            return 0;
        read_bytes(cb, &read_buf, &read_pos, cb->scratch + total, sz);
        total = need;
    }
    for (uint32_t i = 0, off = 0; i < ncount; off += bufs[i].size, i++)
        bufs[i].data = cb->scratch + off;

    atomic_store_explicit(&read_buf->read_pos, read_pos, memory_order_release);
    atomic_store_explicit(&cb->read_buf, read_buf, memory_order_release);
    atomic_fetch_add_explicit(&cb->pop_count, 1, memory_order_relaxed);
    return 1;
}
```

**实现注意：** `pop_buffer` 在 `ncount > *inout_count` 时 `assert`（程序员错误）；测试始终给足容量。

- [ ] **Step 4: 跑通测试**

Expected: `circle_buffer_test OK`

- [ ] **Step 5: Commit**

```bash
git add native/common/circle_buffer.c native/common/circle_buffer_test.c
git commit -m "feat(common): implement circle_buffer framed push/pop"
```

---

### Task 4: free_empty 冒烟 + 收尾清理

**Files:**
- Modify: `native/common/circle_buffer_test.c`
- Modify: `native/common/circle_buffer.c`（仅当 stub/`(void)cb_payload` 等残留需清理）

**Interfaces:**
- Consumes: 完整 API
- Produces: 覆盖规格测试第 4 条

- [ ] **Step 1: 加 free_empty 冒烟测试**

```c
static void test_free_empty_smoke(void) {
    circle_buffer *cb = circle_buffer_create(64, 1);
    CHECK(cb != NULL);
    uint8_t chunk[40];
    memset(chunk, 0xab, sizeof(chunk));
    for (int i = 0; i < 50; i++)
        CHECK(circle_buffer_push_raw_one(cb, chunk, sizeof(chunk)) == 0);
    uint8_t out[40];
    while (circle_buffer_can_pop(cb)) {
        uint32_t n = circle_buffer_pop_raw(cb, out, sizeof(out));
        CHECK(n > 0);
    }
    /* push again after drain — reclaim path exercised inside write_bytes */
    for (int i = 0; i < 20; i++)
        CHECK(circle_buffer_push_raw_one(cb, chunk, sizeof(chunk)) == 0);
    while (circle_buffer_can_pop(cb))
        (void)circle_buffer_pop_raw(cb, out, sizeof(out));
    circle_buffer_destroy(cb);
}
```

`main` 顺序：`test_create_destroy` → `test_raw_roundtrip` → `test_framed` → `test_free_empty_smoke`。

- [ ] **Step 2: 全量跑测**

```powershell
cmake --build build --config Release --target circle_buffer_test
.\bin\circle_buffer_test.exe
```

Expected: `circle_buffer_test OK`

- [ ] **Step 3: 清理**

- 删除 Task 1 残留的无用 stub / `(void)cb_payload`
- 确认 `circle_buffer.c` 无未使用静态函数
- 确认未改动 `luadap` / `asyncsocket` 的 CMake 链接

- [ ] **Step 4: Commit**

```bash
git add native/common/circle_buffer.c native/common/circle_buffer_test.c
git commit -m "test(common): cover free_empty smoke for circle_buffer"
```

---

## Spec Coverage Checklist

| Spec 项 | Task |
|---------|------|
| opaque `circle_buffer*` + `circle_buf` | 1 |
| create/destroy，非法 block_size → NULL | 1 |
| 至少 2 块成环 | 1 |
| push_raw / push_raw_one / pop_raw | 2 |
| can_pop / waiting_count | 2 |
| 跨块 raw | 2 |
| write 扩容 OOM 不发布 | 2（`write_bytes` 返回 -1） |
| push_buffer / pop_buffer merge+非 merge | 3 |
| scratch 指针生命周期 | 3 |
| free_empty_buffer 回收路径 | 2 实现 + 4 测试 |
| CMake `circle_buffer` 独立库 | 1 |
| 不链接 luadap/asyncsocket | 1/4 |
| circle_buffer_test | 1–4 |

## Self-Review Notes

- `pop_buffer` 容量不足：以 assert + 测试保证为准（规格允许 debug assert）。
- C++ merge 用 `size_t` 写 `sizeof(uint32_t)`；本实现统一 `uint32_t`（更清晰且与帧宽一致）。
- MSVC 下需 C11 atomics（工程已 `CMAKE_C_STANDARD 11`）；若编译报 `_Atomic` 问题，确认 `/std:c11` 与 `<stdatomic.h>`。
