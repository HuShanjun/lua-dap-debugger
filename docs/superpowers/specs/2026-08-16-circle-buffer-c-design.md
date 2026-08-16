# 纯 C 环形缓冲设计

日期：2026-08-16  
状态：已批准，待写实现计划  
参考实现：`sample/TCircleBuffer.h` + `sample/TCircleBuffer.inl`（C++ SPSC 环形缓冲）

## 目标

提供独立可复用的**纯 C** 实现，语义对齐现有 C++ `TCircleBuffer`：单生产者 / 单消费者（SPSC）、固定大小块组成的环、C11 原子操作。本阶段不接入 DAP 或 asyncsocket。

## 非目标

- 多生产者或多消费者
- 与 C++ API 二进制/源码级兼容，或包装该模板
- 本工作流内把缓冲接到 `luadap` / `asyncsocket`
- 替换 `sample/TCircleBuffer.*`（保留作参考）

## 方案

选定方案：**`circle_buffer.h` + `circle_buffer.c`，不透明句柄 `circle_buffer*`**。

未采纳：

- 纯头文件 / 宏展开 — 难调试、代码膨胀
- 单块固定数组 ring — 扩容/回收语义与 C++ 原版不同

## 公开 API

### 类型

```c
typedef struct circle_buffer circle_buffer;

typedef struct circle_buf {
    const void *data;
    uint32_t    size;
} circle_buf;
```

### 生命周期

- `circle_buffer *circle_buffer_create(uint32_t block_size, int free_empty_buffer);`
  - 若要对齐 C++ 默认：`block_size = 8192`，`free_empty_buffer = 0`
  - 非法大小或分配失败返回 `NULL`
- `void circle_buffer_destroy(circle_buffer *cb);`

### 原始字节 API

- `int circle_buffer_push_raw(circle_buffer *cb, const circle_buf *bufs, uint32_t count);`
- `int circle_buffer_push_raw_one(circle_buffer *cb, const void *data, uint32_t size);`
- `uint32_t circle_buffer_pop_raw(circle_buffer *cb, void *out, uint32_t out_size);`

### 带帧 API（context 为任意字节）

对应 C++ `template<class ContextType> PushBuffer/PopBuffer`：

- `int circle_buffer_push_buffer(circle_buffer *cb, const void *context, size_t context_size, const circle_buf *bufs, uint32_t count, int merge);`
- `int circle_buffer_pop_buffer(circle_buffer *cb, void *context_out, size_t context_size, circle_buf *bufs, uint32_t *inout_count);`

帧布局（与 C++ 一致）：

- 非 merge：`context | count | (size, bytes)*`
- merge：`context | count=1 | total_size | 拼接后的 bytes`

`pop_buffer` 将 payload 拷入内部可增长暂存区，并把 `bufs[i].data` 指向该暂存区（对应 C++ `m_strBuffer`）。指针在同实例下一次成功的 `pop_buffer` 之前有效。

### 查询

- `int circle_buffer_can_pop(const circle_buffer *cb);`
- `uint32_t circle_buffer_waiting_count(const circle_buffer *cb);`

### 并发约定

- **仅 SPSC**：一个线程调 push*；一个线程调 pop / can_pop / waiting_count
- 使用 C11 `_Atomic`，acquire/release 发布模式与 C++ 实现一致

## 内部布局

每块为一次 `malloc(block_size)`：

```
[read_pos  : _Atomic uint32_t]
[write_pos : _Atomic uint32_t]
[next      : _Atomic block*]
[payload   : block_size - sizeof(header)]
```

规则：

- 构造时至少分配 **2** 个块并连成环（只有一格空位时 write 追上 read 无法扩容）
- 写满一块且 next == read 时，插入新块
- `free_empty_buffer != 0` 时，写路径回收 write→read 之间多余空块（算法同 C++ `bFreeEmptyBuffer`）

### 发布顺序（生产者）

1. 用本地写游标把字节拷进 payload
2. 对已写满的块：`write_pos = payload_capacity`（`memory_order_release`）
3. 对当前活动写块：store 最终 `write_pos`（release）
4. store 全局写块指针（release）
5. `push_count++`（relaxed）

### 消费顺序（消费者）

1. acquire 加载写块指针及其 `write_pos`
2. 若 `write_pos == payload_capacity` → 写尚未发布完成 → 空 / false
3. 若读游标与写游标在同一块且重合 → 空 / false
4. 拷出数据后，release 更新块内 `read_pos` 与全局读块指针
5. `pop_count++`（relaxed）

## 错误处理

| 情况 | 行为 |
|------|------|
| `create` 的 `block_size` 非法（payload ≤ 0） | 返回 `NULL` |
| `create` / 扩块分配失败（OOM） | 返回 `NULL` / push 返回非 0 |
| `push_*` 无法分配扩容块 | 返回非 0；绝不发布半帧（见下方失败策略） |
| `pop_raw` 无数据 | 返回 `0` |
| `pop_buffer` 无数据或暂存区 realloc 失败 | 返回 `0` |
| 调试断言 | 可用可选 `assert`；发布路径不硬崩 |

Push 失败策略：扩容在写帧中途失败时，**在发布 write_pos / 写块指针之前**中止本次 push，消费者看不到半帧。未发布块上的本地写入可丢弃或回滚。

## 构建集成

- 文件：`native/common/circle_buffer.h`、`native/common/circle_buffer.c`
- CMake：在 `native/common/CMakeLists.txt` 增加静态库 target `circle_buffer`（与现有 `lua_compat` 并列）
- 本工作流**不**链接到 `luadap` / `asyncsocket`
- 需要 C11（工程已设置）

## 测试

可选可执行文件 `circle_buffer_test`，覆盖：

1. raw 往返，含跨块大包
2. 带帧 push/pop（merge / 非 merge）
3. `can_pop` / `waiting_count` 正确性
4. 冒烟：`free_empty_buffer=1` 在重写再排空下不崩溃

## 文件清单

| 路径 | 作用 |
|------|------|
| `native/common/circle_buffer.h` | 公开 API |
| `native/common/circle_buffer.c` | 实现 |
| `native/common/CMakeLists.txt` | 增加 `circle_buffer` target |
| `native/common/circle_buffer_test.c` | 可选单元冒烟测试 |

## 成功标准

- SPSC 下 raw / 带帧 push/pop 行为对齐 C++ 参考（含跨块写入、未完成发布检测 `write_pos == payload_capacity`）
- 纯 C 可用，不依赖 C++
- 在 `native/common` 下可独立编成静态库
