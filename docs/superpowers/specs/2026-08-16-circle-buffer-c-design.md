# Pure-C Circle Buffer Design

Date: 2026-08-16  
Status: Approved for implementation planning  
Source reference: `sample/TCircleBuffer.h` + `sample/TCircleBuffer.inl` (C++ SPSC circle buffer)

## Goal

Provide a standalone, reusable **pure C** implementation of the existing C++ `TCircleBuffer` semantics: a single-producer / single-consumer (SPSC) ring of fixed-size blocks with C11 atomics, without wiring it into DAP or asyncsocket yet.

## Non-goals

- Multi-producer or multi-consumer support
- Drop-in C++ API compatibility / wrapping the C++ template
- Integrating the buffer into `luadap` or `asyncsocket` in this workstream
- Replacing `sample/TCircleBuffer.*` (kept as reference)

## Approach

Chosen approach: **`circle_buffer.h` + `circle_buffer.c` with an opaque `circle_buffer*` handle**.

Rejected alternatives:

- Header-only / macro expansion — harder to debug, code bloat
- Fixed single-array ring — different growth/reclaim semantics from the C++ original

## Public API

### Types

```c
typedef struct circle_buffer circle_buffer;

typedef struct circle_buf {
    const void *data;
    uint32_t    size;
} circle_buf;
```

### Lifecycle

- `circle_buffer *circle_buffer_create(uint32_t block_size, int free_empty_buffer);`
  - Defaults for callers who want C++ parity: `block_size = 8192`, `free_empty_buffer = 0`
  - Returns `NULL` on invalid size or allocation failure
- `void circle_buffer_destroy(circle_buffer *cb);`

### Raw byte APIs

- `int circle_buffer_push_raw(circle_buffer *cb, const circle_buf *bufs, uint32_t count);`
- `int circle_buffer_push_raw_one(circle_buffer *cb, const void *data, uint32_t size);`
- `uint32_t circle_buffer_pop_raw(circle_buffer *cb, void *out, uint32_t out_size);`

### Framed APIs (context as opaque bytes)

Replaces C++ `template<class ContextType> PushBuffer/PopBuffer`:

- `int circle_buffer_push_buffer(circle_buffer *cb, const void *context, size_t context_size, const circle_buf *bufs, uint32_t count, int merge);`
- `int circle_buffer_pop_buffer(circle_buffer *cb, void *context_out, size_t context_size, circle_buf *bufs, uint32_t *inout_count);`

Frame layout (same as C++):

- Non-merge: `context | count | (size, bytes)*`
- Merge: `context | count=1 | total_size | concatenated bytes`

`pop_buffer` copies payload into an internal growable scratch buffer and points `bufs[i].data` into that scratch (same role as C++ `m_strBuffer`). Pointers remain valid until the next successful `pop_buffer` on the same instance.

### Queries

- `int circle_buffer_can_pop(const circle_buffer *cb);`
- `uint32_t circle_buffer_waiting_count(const circle_buffer *cb);`

### Concurrency contract

- **SPSC only**: one thread may call push APIs; one thread may call pop / can_pop / waiting_count
- Uses C11 `_Atomic` with the same acquire/release publish pattern as the C++ implementation

## Internal layout

Each block is a single `malloc(block_size)` allocation:

```
[read_pos  : _Atomic uint32_t]
[write_pos : _Atomic uint32_t]
[next      : _Atomic block*]
[payload   : block_size - sizeof(header)]
```

Rules:

- Constructor allocates **at least two** blocks linked in a ring (write cannot expand if next would equal read with only one free slot)
- When write fills a block and next == read, allocate and insert a new block
- When `free_empty_buffer != 0`, the write path reclaims surplus empty blocks between write and read (same algorithm as C++ `bFreeEmptyBuffer`)

### Publish order (producer)

1. Copy bytes into payload using a local write cursor
2. For completed full blocks: store `write_pos = payload_capacity` with `memory_order_release`
3. Store final active block `write_pos` with release
4. Store global write-buffer pointer with release
5. Increment `push_count` (relaxed)

### Consume order (consumer)

1. Acquire-load write-buffer pointer and its `write_pos`
2. If `write_pos == payload_capacity`, write is mid-publish → empty / false
3. If read cursor equals write cursor on the same block → empty / false
4. Copy out data; release-store per-block `read_pos` and global read-buffer pointer
5. Increment `pop_count` (relaxed)

## Error handling

| Case | Behavior |
|------|----------|
| `create` invalid `block_size` (payload would be ≤ 0) | return `NULL` |
| `create` / block alloc OOM | return `NULL` / push returns non-zero error |
| `push_*` cannot allocate expansion block | return non-zero; never publish a partial frame (see push failure policy below) |
| `pop_raw` empty | return `0` |
| `pop_buffer` empty or scratch realloc fail | return `0` |
| Debug asserts | optional `assert` for invariant checks; no hard abort in release paths |

Push failure policy (explicit): if expansion fails mid-write of a frame, abort the push **before** publishing write positions / write-buffer pointer, so the consumer never observes a partial frame. Local cursor work on unpublished blocks may be discarded or rolled back.

## Build integration

- Files: `native/common/circle_buffer.h`, `native/common/circle_buffer.c`
- CMake: add static library target `circle_buffer` under `native/common/CMakeLists.txt` (alongside existing `lua_compat`)
- Do **not** link into `luadap` / `asyncsocket` in this workstream
- Requires C11 (already set project-wide)

## Testing

Optional executable `circle_buffer_test` covering:

1. Raw round-trip, including payloads larger than one block
2. Framed push/pop with merge and non-merge
3. `can_pop` / `waiting_count` correctness
4. Smoke: `free_empty_buffer=1` does not crash under write-heavy then drain

## File map

| Path | Role |
|------|------|
| `native/common/circle_buffer.h` | Public API |
| `native/common/circle_buffer.c` | Implementation |
| `native/common/CMakeLists.txt` | Add `circle_buffer` target |
| `native/common/circle_buffer_test.c` | Optional unit smoke tests |

## Success criteria

- Behavior matches C++ reference for SPSC raw and framed push/pop, including cross-block writes and incomplete-publish detection (`write_pos == payload_capacity`)
- Opaque C API usable without C++
- Builds as an independent static library under `native/common`
