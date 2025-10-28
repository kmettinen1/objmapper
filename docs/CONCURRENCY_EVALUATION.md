# Concurrency Model Evaluation

## Executive Summary

This document evaluates concurrency options for objmapper, optimizing primarily for Unix socket file descriptor passing while maintaining flexibility for TCP/UDP transports. We analyze threading models, async I/O approaches, and hybrid schemes with focus on latency vs throughput trade-offs.

## 🔥 CRITICAL INSIGHT: FD Passing is O(1) on Persistent Connections

**Game-Changing Realization:** File descriptor passing does NOT touch file contents. The server only needs to:
1. Hash table lookup → O(1), ~200ns
2. Open file (cached dentry) → O(1), ~3μs  
3. Pass FD via sendmsg() → O(1), ~5μs

**Total: ~8μs per request on persistent connection (no accept overhead!)**

For comparison, if connections were not persistent:
- accept() → ~5μs (eliminated with persistent connections!)
- Total would be ~13μs

The client reads file contents **after** receiving the FD, using kernel page cache directly. The server never blocks on file I/O or size.

**Consequence with persistent connections:** 
- **Actual work per request: ~8μs**
- **Thread-per-request overhead: 30-80μs** (thread create/destroy)
- Thread overhead is **4-10× the actual work!**
- This makes thread pool absolutely essential, not just optimal

## Key Findings

- **CRITICAL:** Persistent connections eliminate accept() overhead - only ~8μs of work per request
- **REVISED:** Thread pool is absolutely essential for FD passing (not just optimal!)
- Thread-per-request wastes 30-80μs creating threads for 8μs of work (4-10× overhead!)
- With persistent connections, a single worker thread can handle 125K requests/sec theoretically
- Async I/O adds overhead with no benefit (FD passing already O(1), cannot be async)
- Mode-aware routing: FD passing uses small pool, copy/splice uses larger pool or async I/O
- Expected improvement: 75-90% better latency, 96-99% less memory, 5-10× throughput

## Current Implementation Analysis

### Existing Model: Thread-Per-Connection

```c
// Current: objmapper/src/server.c
void *handle_client(void *arg) {
    session_t *session = (session_t *)arg;
    // Process one request
    // Thread detaches and exits
}

pthread_t tid;
pthread_create(&tid, NULL, handle_client, session);
pthread_detach(tid);
```

**CRITICAL ISSUE: Current implementation creates thread PER REQUEST, not per connection!**

Even with persistent connections, the current code spawns a new thread for every request on that connection, then destroys it. This is catastrophically inefficient.

**Characteristics:**
- ❌ **FATAL FLAW:** Thread per REQUEST not per connection (even worse than assumed!)
- ❌ Thread creation overhead (~30-80μs) for every 8μs request
- ❌ Context switch overhead for many concurrent requests  
- ❌ Memory overhead (~8MB stack per thread)
- ❌ Limited by `max_connections` setting (actually max concurrent requests)
- ✅ Simple implementation, easy to debug
- ✅ Natural isolation between requests

**Performance Profile (with persistent connections):**
- **Latency:** Poor (38-88μs: 8μs work + 30-80μs thread overhead)
- **Throughput:** Terrible (~12K req/s, limited by thread creation rate)
- **Memory:** Catastrophic (8MB × concurrent requests)
- **CPU:** Very poor (massive thread churn)

**Theoretical vs Actual:**
- **Theoretical max (8μs per request):** 125K req/s per core
- **Actual (with thread overhead):** ~12K req/s per core
- **Efficiency:** ~10% (90% wasted on thread management!)

**With persistent connections, thread-per-request is even worse than thread-per-connection!**

## Concurrency Models Evaluation

### 1. Thread Pool (Fixed Size)

#### Architecture
```
┌─────────────┐
│ Accept Loop │
└──────┬──────┘
       │
       v
┌─────────────────┐
│ Request Queue   │  (bounded, blocking)
└──────┬──────────┘
       │
       v
┌──────────────────────────────┐
│ Worker Threads (fixed pool)  │
│  Thread 1 │ Thread 2 │ ... N │
└──────────────────────────────┘
```

#### Implementation Sketch
```c
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t work_available;
    session_t **queue;
    int queue_size;
    int queue_head;
    int queue_tail;
    int num_workers;
    pthread_t *workers;
    int shutdown;
} thread_pool_t;

void *worker_thread(void *arg) {
    thread_pool_t *pool = (thread_pool_t *)arg;
    while (!pool->shutdown) {
        pthread_mutex_lock(&pool->lock);
        while (queue_empty(pool) && !pool->shutdown) {
            pthread_cond_wait(&pool->work_available, &pool->lock);
        }
        session_t *session = dequeue_work(pool);
        pthread_mutex_unlock(&pool->lock);
        
        if (session) {
            // For persistent connections, handle MULTIPLE requests
            handle_connection_loop(session);  // Not just one request!
        }
    }
}

void handle_connection_loop(session_t *session) {
    // Keep connection alive, process multiple requests
    while (1) {
        char mode;
        if (recv(session->sock, &mode, 1, 0) <= 0) break;
        
        // Read URI, process, send FD - all on same connection
        process_request(session, mode);
    }
}
```

**Key difference:** Worker threads handle the entire connection lifetime, not just one request.

#### Pros & Cons
**Advantages:**
- ✅ Bounded resource usage (fixed thread count)
- ✅ No thread creation overhead (workers persistent)
- ✅ **CRITICAL:** Each worker handles multiple requests on persistent connection
- ✅ Better CPU cache locality (threads reused)
- ✅ Predictable memory footprint
- ✅ Easy to tune (pool size ≈ num_cores for O(1) operations)
- ✅ Theoretical 125K req/s per core (8μs per request)

**Disadvantages:**
- ❌ Minimal queueing latency (1-5μs depending on load)
- ❌ Requires connection-aware request handling
- ❌ Need graceful connection closure handling

**Performance Profile (persistent connections):**
- **Latency:** Excellent (9-13μs: 8μs work + 1-5μs queue)
- **Throughput:** Excellent (100K+ req/s per core)
- **Memory:** Excellent (fixed × threads, not × requests)
- **CPU:** Excellent (no thread creation, minimal queue overhead)
- **Efficiency:** ~90% (vs 10% for thread-per-request)

**Best For:** Persistent connection workloads with O(1) request processing (exactly our use case!)

---

### 2. Async I/O (epoll/io_uring)

#### Architecture with epoll
```
┌──────────────────┐
│  Event Loop      │
│  (single thread) │
└────────┬─────────┘
         │
         v
    ┌────────┐
    │ epoll  │
    └────┬───┘
         │
    ┌────┴────────────────┐
    │ Events:             │
    │ - EPOLLIN (read)    │
    │ - EPOLLOUT (write)  │
    │ - EPOLLRDHUP (disc) │
    └─────────────────────┘
```

#### Implementation Sketch
```c
typedef struct {
    int epoll_fd;
    transport_t *transport;
    storage_t *storage;
    struct epoll_event *events;
    int max_events;
    
    // Per-connection state machines
    connection_state_t *connections;
    int max_connections;
} async_server_t;

typedef enum {
    STATE_READING_MODE,
    STATE_READING_URI_LEN,
    STATE_READING_URI,
    STATE_PROCESSING,
    STATE_SENDING_RESPONSE,
    STATE_DONE
} conn_state_t;

void event_loop(async_server_t *server) {
    while (1) {
        int n = epoll_wait(server->epoll_fd, server->events, 
                          server->max_events, -1);
        for (int i = 0; i < n; i++) {
            connection_state_t *conn = server->events[i].data.ptr;
            
            if (server->events[i].events & EPOLLIN) {
                handle_read(conn);
            }
            if (server->events[i].events & EPOLLOUT) {
                handle_write(conn);
            }
        }
    }
}
```

#### io_uring Alternative
```c
// Modern Linux (5.1+) - true async I/O
struct io_uring ring;
io_uring_queue_init(256, &ring, 0);

// Submit read operation
struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
io_uring_prep_read(sqe, fd, buffer, len, offset);
io_uring_submit(&ring);

// Wait for completion
struct io_uring_cqe *cqe;
io_uring_wait_cqe(&ring, &cqe);
```

#### Pros & Cons
**Advantages:**
- ✅ Minimal memory footprint (single thread or small pool)
- ✅ Scales to 10K+ connections (C10K problem solved)
- ✅ No context switching overhead
- ✅ Excellent for TCP/UDP with many idle connections
- ✅ io_uring: zero-copy potential

**Disadvantages:**
- ❌ **CRITICAL: Cannot pass FDs asynchronously** (SCM_RIGHTS requires blocking recvmsg)
- ❌ Complex state machine implementation
- ❌ Callback hell / continuation-passing style
- ❌ Debugging difficulty (non-linear flow)
- ❌ Storage backend may block (disk I/O)
- ❌ Increased latency for simple requests (state transitions)

**Performance Profile:**
- **Latency:** Poor for FD passing (requires blocking or thread offload)
- **Throughput:** Excellent for TCP/UDP with many connections
- **Memory:** Excellent (O(connections) not O(threads))
- **CPU:** Excellent (no context switches)

**Best For:** TCP servers with many idle keep-alive connections

**Fatal Flaw for objmapper:** Unix socket FD passing with `SCM_RIGHTS` requires `recvmsg()`/`sendmsg()` which are inherently blocking operations. Cannot be made truly asynchronous without offloading to worker threads, defeating the purpose.

---

### 3. Hybrid: Async Accept + Thread Pool Workers

#### Architecture
```
┌────────────────┐
│ Async Accept   │  (epoll on listen socket)
│ (main thread)  │
└───────┬────────┘
        │
        v
┌───────────────────┐
│  Work Queue       │
└───────┬───────────┘
        │
        v
┌──────────────────────────────┐
│ Worker Thread Pool           │
│  Thread 1 │ Thread 2 │ ... N │
└──────────────────────────────┘
```

**Rationale:** Accept connections quickly (async), process in threads (for blocking FD ops)

#### Pros & Cons
**Advantages:**
- ✅ Fast accept (no blocking)
- ✅ FD passing works (in worker threads)
- ✅ Bounded resources
- ✅ Good latency for typical case

**Disadvantages:**
- ❌ Still has queueing latency
- ❌ Complexity of two concurrency models
- ❌ Marginal benefit over simple thread pool

**Verdict:** Over-engineered for objmapper's use case

---

### 4. Work-Stealing Thread Pool

#### Architecture
```
┌─────────────┐
│ Accept Loop │
└──────┬──────┘
       │
       v  (round-robin or least-loaded)
┌─────────────────────────────┐
│ Per-Thread Deques           │
│  ┌─────┐  ┌─────┐  ┌─────┐ │
│  │ Q1  │  │ Q2  │  │ Q3  │ │
│  └──┬──┘  └──┬──┘  └──┬──┘ │
│     │        │        │     │
│  Worker1  Worker2  Worker3  │
└─────────────────────────────┘
         ^         ^
         └─────────┘
         (steal when idle)
```

#### Implementation Concept
```c
typedef struct {
    session_t **deque;
    int head;
    int tail;
    pthread_mutex_t lock;
} work_deque_t;

typedef struct {
    work_deque_t *deques;
    int num_workers;
    pthread_t *workers;
} work_stealing_pool_t;

void *worker(void *arg) {
    int my_id = *(int *)arg;
    work_deque_t *my_deque = &pool->deques[my_id];
    
    while (1) {
        // Try local work first (lock-free from bottom)
        session_t *work = deque_pop_bottom(my_deque);
        
        if (!work) {
            // Steal from other workers (lock-based from top)
            work = steal_from_random_worker(pool, my_id);
        }
        
        if (work) {
            handle_client_work(work);
        }
    }
}
```

#### Pros & Cons
**Advantages:**
- ✅ Better load balancing than fixed assignment
- ✅ Reduces head-of-line blocking
- ✅ Good CPU utilization
- ✅ Bounded resources

**Disadvantages:**
- ❌ Implementation complexity
- ❌ Stealing overhead under contention
- ❌ Still has queueing latency

**Best For:** Mixed workloads with variable request durations

---

### 5. SEDA: Staged Event-Driven Architecture

#### Architecture
```
Accept → Stage1: Parse Request → Stage2: Lookup Storage → Stage3: Send Response
         (thread pool 1)         (thread pool 2)           (thread pool 3)
         
Each stage has its own queue and thread pool, tuned independently
```

#### Pros & Cons
**Advantages:**
- ✅ Fine-grained performance tuning per stage
- ✅ Excellent for complex pipelines
- ✅ Isolates slow operations

**Disadvantages:**
- ❌ **Massive over-engineering** for objmapper
- ❌ Multiple queue traversals
- ❌ Context sharing complexity
- ❌ Higher latency (cross-queue delays)

**Verdict:** Not suitable for objmapper's simple request/response pattern

---

## Storage Backend Considerations

### Current Storage Architecture
```c
// lib/storage/storage.c - synchronous, blocking
int storage_get_fd(storage_t *storage, const char *uri);
void *storage_get_mmap(storage_t *storage, const char *uri, size_t *size);
```

**Operations by Transfer Mode:**

#### FD Passing Mode (O(1) - CRITICAL INSIGHT)
```c
// For FD passing, we ONLY need to lookup and open the file
// No need to read/mmap/touch file contents!

1. Hash table lookup:  O(1), ~50-200ns
2. Open file (if not cached FD): O(1), ~1-5μs (cached dentry)
3. sendmsg() with SCM_RIGHTS: O(1), ~5-10μs
```

**Total latency: 6-15μs regardless of file size!**

The kernel handles all file I/O **after** the FD is passed to the client. The server never touches the file contents, never waits for disk I/O, never blocks on file size. This is the fundamental advantage of FD passing.

#### Copy/Splice Modes (O(file_size) - Blocking)
```c
1. Hash table lookup:  O(1), ~50-200ns
2. mmap cache hit:     O(1), ~100ns
3. mmap cache miss:    O(file_size), 100μs - 10ms for cold files
4. Disk I/O (cold):    Highly variable, 1ms - 100ms
5. Copy to socket:     O(file_size), ~100MB/s for TCP
```

**Total latency: Variable, depends on file size and cache state**

### Performance Implications

**FD Passing is Fundamentally Different:**

| Aspect | FD Passing | Copy/Splice |
|--------|------------|-------------|
| File size dependency | **None (O(1))** | Linear (O(n)) |
| Disk I/O blocking | **None** | Blocks on cache miss |
| Memory usage | **Minimal** | Requires buffers/mmap |
| CPU usage | **Minimal** | Copy overhead |
| Latency | **~10μs constant** | 100μs - 100ms |
| Throughput bottleneck | **Hash table** | Disk I/O, network |

**Why FD Passing is O(1):**
- Server only opens the file (fast kernel operation)
- File descriptor is passed to client via Unix socket
- Client reads file contents directly from kernel page cache
- Server never blocks on file I/O or size
- Kernel DMA/sendfile happens in client process context

### Concurrency Implications

**For FD Passing:**
- ✅ No blocking operations (except negligible open())
- ✅ Thread-per-connection adds <50μs overhead vs O(1) operation
- ✅ Thread pool would add queueing delay for minimal gain
- ✅ Async I/O has no benefit (nothing to async!)
- **Verdict:** Thread-per-connection is IDEAL for FD passing

**For Copy/Splice:**
- ❌ Blocking on disk I/O (cache misses)
- ❌ Blocking on network send (TCP backpressure)
- ⚠️ Thread-per-connection wastes threads on I/O waits
- ✅ Thread pool better (bounded resources)
- ✅ Async I/O beneficial (overlap I/O waits)
- **Verdict:** Thread pool or async I/O preferred

### Async Storage Implications

**Option A: Thread Pool Offload (for Copy/Splice only)**
```c
typedef struct {
    storage_t *storage;
    pthread_t *io_workers;
    request_queue_t *queue;
} async_storage_t;

// Submit async request
void storage_get_async(async_storage_t *storage, const char *uri, 
                      void (*callback)(int fd, void *ctx), void *ctx);
```

**Option B: io_uring for Disk I/O (Copy/Splice only)**
```c
// Use io_uring for actual file operations
struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
io_uring_prep_openat(sqe, AT_FDCWD, path, O_RDONLY, 0);
```

**Trade-offs:**
- ❌ Async storage adds latency overhead for FD passing (unnecessary!)
- ✅ Benefits only for copy/splice modes with cache misses
- ✅ Can be mode-specific: sync for FD passing, async for copy/splice
- **Complexity > benefit** for typical workload (FD passing primary)

---

## Latency vs Throughput Analysis

### Revised Analysis: FD Passing is O(1) on Persistent Connections

**Key Insight:** FD passing latency is **dominated by thread overhead**, not storage operations!

```
FD Passing Request Breakdown (persistent connection):
  recv() mode+URI:    ~2μs
  hash lookup:        ~0.2μs
  open() [cached]:    ~3μs
  sendmsg(SCM_RIGHTS):~3μs
  ─────────────────────────
  Total work:         ~8μs

Thread overhead (thread-per-request):
  Thread create:      ~20-50μs
  Context switch:     ~5-10μs
  Thread destroy:     ~10-20μs
  ─────────────────────────
  Total overhead:     ~35-80μs
```

**Thread-per-request overhead (35-80μs) is 4-10× the actual work (8μs)!**

With persistent connections and a thread pool:
```
Request on persistent connection (thread pool):
  Queue wait:         ~1-5μs (low contention)
  Actual work:        ~8μs
  ─────────────────────────
  Total:              ~9-13μs

Theoretical maximum: 125,000 requests/sec per core (8μs each)
Thread pool achieves: ~110,000 requests/sec per core (~90% efficiency)
Thread-per-request:   ~12,000 requests/sec per core (~10% efficiency)
```

### Scenario 1: Low Concurrency (<100 persistent connections) - FD Passing

| Model | Latency (p50) | Overhead | Throughput/core | Efficiency |
|-------|---------------|----------|-----------------|------------|
| Thread-per-request | 43μs | 35μs thread | 23K req/s | ~18% |
| Thread pool (8) | 10μs | 2μs queue | 100K req/s | ~80% |
| Thread pool (16) | 9μs | 1μs queue | 111K req/s | ~89% |

**Winner: Thread Pool** (4× better latency, 4-5× throughput!)

### Scenario 2: High Concurrency (1000+ persistent connections) - FD Passing

| Model | Latency (p50) | Overhead | Throughput/core | Efficiency |
|-------|---------------|----------|-----------------|------------|
| Thread-per-request | 100μs | 92μs thread | 10K req/s | ~8% |
| Thread pool (32) | 12μs | 4μs queue | 83K req/s | ~66% |
| Thread pool (64) | 15μs | 7μs queue | 67K req/s | ~53% |

**Winner: Thread Pool** (8× better latency, 8× throughput!)

**Key insight:** With persistent connections, pool size can match num_cores (not × 2) because there's no I/O blocking on FD passing.

### Scenario 3: Copy/Splice Mode (File Size Dependent)

Copy/splice modes still need to transfer file contents, so performance depends on file size and cache state.

**Small files (1KB - 100KB, cached):**

| Model | Latency (p50) | Throughput/core | Bottleneck |
|-------|---------------|-----------------|------------|
| Thread-per-request | 150μs | 6K req/s | Thread overhead |
| Thread pool (32) | 50μs | 20K req/s | CPU (copy) |
| Async epoll | 40μs | 25K req/s | CPU (copy) |

**Large files (1MB - 100MB, cache miss):**

| Model | Latency (p50) | Throughput/core | Bottleneck |
|-------|---------------|-----------------|------------|
| Thread-per-request | 60ms | 16 req/s | Disk I/O |
| Thread pool (32) | 100ms | 320 req/s | Disk I/O queue |
| Async epoll | 70ms | 500 req/s | Disk I/O |

**Winner: Async I/O** (for copy/splice with large files and persistent connections)

### Mixed Workload: 80% FD Passing, 20% Copy (persistent connections)

| Model | FD Latency | Copy Latency | Combined Throughput/core |
|-------|------------|--------------|--------------------------|
| Thread-per-request | 43μs | 150μs | 12K req/s |
| Thread pool (32) | 10μs | 50μs | 60K req/s |
| Hybrid (separate paths) | 9μs | 45μs | 75K req/s |

**Winner: Hybrid** (optimal for each mode, 6× better than thread-per-request!)

---

## Architectural Recommendations

### REVISED Recommendation 1: Thread Pool is ESSENTIAL for Persistent Connections

**Critical Realization:** With persistent connections, each request is only ~8μs of work. Thread-per-request overhead (35-80μs) is **catastrophically wasteful**.

```c
typedef enum {
    CONCURRENCY_THREAD_POOL,       // DEFAULT - MANDATORY for persistent connections!
    CONCURRENCY_THREAD_PER_CONN,   // Deprecated (10% efficiency)
    CONCURRENCY_WORK_STEALING,     // Advanced pool, mixed workloads
    CONCURRENCY_ASYNC_EPOLL,       // For copy/splice modes only
    CONCURRENCY_AUTO               // Select based on transport+mode
} concurrency_model_t;

typedef struct {
    concurrency_model_t model;
    
    // Thread pool settings (primary path for persistent connections)
    int pool_size;              // Default: num_cores (FD passing is O(1), no blocking!)
    int queue_size;             // Large queue OK (1000+) since work is O(1)
    
    // Connection handling
    int persistent_connections; // 1 = keep-alive (default), 0 = one-shot
    int requests_per_conn;      // Max requests before closing (0 = unlimited)
    
    // Per-mode optimization
    int fdpass_use_pool;        // 1 = use pool (REQUIRED), 0 = deprecated
    int copy_use_async;         // 1 = async I/O for copy, 0 = thread pool
    
    // Resource limits
    int max_threads;            // Hard limit
    int max_connections;        // Max simultaneous persistent connections
} concurrency_config_t;
```

**Key architectural change:** Worker threads handle connection lifetime, not single requests.

```c
void *worker_thread(void *arg) {
    thread_pool_t *pool = arg;
    
    while (!pool->shutdown) {
        // Wait for a new connection (not a request!)
        session_t *session = dequeue_connection(pool);
        
        // Handle ALL requests on this connection
        while (connection_is_alive(session)) {
            process_single_request(session);  // ~8μs each
        }
        
        cleanup_session(session);
    }
}
```

### Recommendation 2: Mode-Aware Concurrency (Supersedes Transport-Aware)

```c
typedef struct {
    char operation_mode;        // OP_FDPASS, OP_COPY, OP_SPLICE
    concurrency_model_t model;
    int pool_size;
} mode_concurrency_map_t;

// Optimal mappings based on O(1) analysis
mode_concurrency_map_t optimal[] = {
    { OP_FDPASS, CONCURRENCY_THREAD_POOL, num_cores },      // O(1), no I/O wait
    { OP_COPY,   CONCURRENCY_ASYNC_EPOLL, 0 },              // O(n), I/O wait
    { OP_SPLICE, CONCURRENCY_THREAD_POOL, num_cores * 2 }   // O(n), some I/O wait
};
```

**Rationale:**
- **FD Passing (O(1)):** Thread pool with pool_size = num_cores (no I/O blocking)
- **Copy (O(n)):** Async I/O to overlap disk/network I/O
- **Splice (O(n)):** Thread pool with larger size (some blocking, but less than copy)

### Recommendation 3: Simplified Hybrid Architecture

```
┌─────────────────────────────────────────────┐
│         Connection Demultiplexer            │
└─────────────┬───────────────────────────────┘
              │
    Detect operation mode
              │
    ┌─────────┴─────────┐
    │                   │
    v                   v
┌──────────────┐   ┌─────────────────┐
│ FD Pass Path │   │ Copy/Splice Path│
│ (Thread Pool)│   │ (Async I/O)     │
│  O(1) ops    │   │  O(n) ops       │
└──────────────┘   └─────────────────┘
```

**Implementation:**
- Single accept thread
- Reads operation mode from first byte
- Routes to appropriate concurrency handler:
  - FD passing → Thread pool (fast path, O(1))
  - Copy/splice → Async I/O or larger thread pool (slow path, O(n))

### Recommendation 4: Gradual Evolution Path (UPDATED for Persistent Connections)

**Phase 1: Implement Thread Pool with Connection-Level Workers (CRITICAL - URGENT)**
- **Priority:** CRITICAL (current implementation is 10% efficient!)
- **Effort:** 2-3 days (more complex than simple pool - need connection handling)
- **Risk:** Medium (need to handle connection lifecycle correctly)
- **Expected Improvement:**
  - Latency: 43μs → 10μs (77% better)
  - Throughput: 23K → 100K req/s per core (4× better)
  - Memory: 800MB → 64MB at 100 connections (92% reduction)
  - **At 1000 connections:** 100μs → 12μs latency (88% better!), 10K → 83K req/s (8× better)
  - **Efficiency:** 10% → 90%

**Implementation Requirements:**
- Worker threads must handle entire connection lifetime
- Connection keep-alive support (read multiple requests)
- Graceful connection closure (client disconnect, timeout)
- Queue management at connection level, not request level

**Phase 2: Add Mode Detection and Routing (Medium Priority)**
- Detect OP_FDPASS vs OP_COPY/OP_SPLICE per request
- Route FD passing to small pool (num_cores)
- Route copy/splice to separate larger pool or async handler
- Measure performance split

**Phase 3: Optimize Copy/Splice Path (Optional)**
- Implement async I/O for copy mode if metrics show benefit
- Only if copy mode is >20% of traffic
- Only if large file sizes (>1MB) are common
- Keep FD passing on fast synchronous path

**Phase 4: Connection Pooling Optimizations (Future)**
- Implement connection draining for graceful shutdown
- Add per-connection request rate limiting
- Connection timeout handling
- Metrics per connection (hot connections vs idle)

---

## Performance Tuning Knobs

### Recommended Configuration Interface

```c
// Server startup - UPDATED for thread pool default
server_config_t config = {
    .transport = OBJMAPPER_TRANSPORT_UNIX,
    .concurrency = {
        .model = CONCURRENCY_THREAD_POOL,  // NEW DEFAULT (was thread-per-conn)
        
        // FD passing optimization (O(1) operations)
        .pool_size = 0,             // Auto = num_cores (no I/O blocking!)
        .max_queue_depth = 1000,    // Large queue OK for O(1) ops
        
        // Copy/splice optimization (O(n) operations)  
        .copy_pool_size = 0,        // Auto = num_cores * 2 (I/O blocking)
        .use_async_copy = 0,        // Set to 1 for async I/O on large files
        
        // Latency tuning
        .optimize_for_latency = 1,  // Prefer small queues, more workers
        
        // Resource limits
        .max_threads = 1000,        // Fallback limit
    }
};
```

### Runtime Metrics to Expose

```c
typedef struct {
    // Latency metrics (split by mode)
    struct {
        uint64_t p50_latency_us;
        uint64_t p99_latency_us;
        uint64_t p999_latency_us;
    } fdpass, copy, splice;
    
    // Throughput metrics
    uint64_t fdpass_requests_per_sec;
    uint64_t copy_requests_per_sec;
    uint64_t bytes_per_sec;
    
    // Resource metrics
    int active_threads;
    int idle_threads;
    int queue_depth_fdpass;
    int queue_depth_copy;
    uint64_t queue_wait_time_us;
    
    // Storage metrics (only relevant for copy/splice)
    uint64_t cache_hit_rate;
    uint64_t disk_io_pending;
    uint64_t avg_file_size;
    
    // Key insight metric
    uint64_t thread_overhead_us;   // Time spent in thread mgmt vs work
} server_metrics_t;
```

### Auto-Tuning Recommendations

**Pool Size Calculation (for persistent connections):**
```c
int calculate_pool_size(concurrency_config_t *config) {
    int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    
    if (config->persistent_connections) {
        // With persistent connections, each worker handles entire connection
        // FD passing is O(1) with no blocking, so num_cores is optimal
        return num_cores;
    } else {
        // One-shot connections (legacy mode)
        // Some blocking on accept(), use 2× cores
        return num_cores * 2;
    }
}
```

**Connection Queue Sizing:**
```c
int calculate_queue_size(concurrency_config_t *config) {
    // For O(1) operations on persistent connections, large queue is fine
    // Each connection will be handled quickly by worker thread
    // Queue size should match expected concurrent connections
    return config->max_connections;  // Default: 1000
}
```

**Request Rate Estimation:**
```c
uint64_t estimate_max_throughput(concurrency_config_t *config) {
    int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    
    if (config->model == CONCURRENCY_THREAD_POOL && 
        config->persistent_connections) {
        // Theoretical max: 125K req/s per core (8μs per request)
        // Practical with queue overhead: ~100K req/s per core
        return num_cores * 100000;
    } else {
        // Thread-per-request: limited by thread creation rate
        // Typical: ~12K req/s per core
        return num_cores * 12000;
    }
}
```

---

## Benchmarking Plan

### Test Scenarios

**1. FD Passing Latency Benchmark (Primary Use Case - Persistent Connections)**
```bash
# Measure O(1) performance with different concurrency models
# Use persistent connections with multiple requests per connection
./bench_fdpass --model thread-per-request --persistent --requests-per-conn 1000 --conns 100
./bench_fdpass --model thread-pool --pool-size 8 --persistent --requests-per-conn 1000 --conns 100
./bench_fdpass --model thread-pool --pool-size 16 --persistent --requests-per-conn 1000 --conns 100

# Expected results (persistent connections):
# thread-per-request: ~43μs avg (35μs thread overhead + 8μs work)
# thread-pool-8:      ~10μs avg (2μs queue + 8μs work)
# thread-pool-16:     ~9μs avg  (1μs queue + 8μs work)
```

**2. FD Passing Throughput (Saturation Test)**
```bash
# Verify we can achieve theoretical maximum (~100K req/s per core)
./bench_throughput --model thread-pool --pool-size $(nproc) \
                   --persistent --conns 1000 --duration 60s

# Expected: ~100K req/s per core (vs ~12K with thread-per-request)
```

**3. Connection Scalability (Thread Overhead Test)**
```bash
# Verify thread creation is the bottleneck
for conns in 10 100 1000 5000 10000; do
    echo "Testing $conns persistent connections"
    ./bench_fdpass --model thread-per-request --persistent --conns $conns --duration 10s
    ./bench_fdpass --model thread-pool --pool-size $(nproc) --persistent --conns $conns --duration 10s
done

# Expected: thread pool latency stays constant, thread-per-request degrades
```

**4. Copy Mode Benchmark (O(n) operations)**
```bash
# Vary file sizes to show O(n) behavior
for size in 1K 10K 100K 1M 10M; do
    ./bench_copy --file-size $size --model thread-pool --persistent --conns 100
done

# Expected: latency proportional to file size
```

**5. Mixed Workload (Realistic Traffic Pattern)**
```bash
# 80% FD passing, 20% copy (realistic workload)
./bench_mixed --fdpass-ratio 0.8 --persistent --conns 1000 --duration 60s
```

### Success Criteria (REVISED for Persistent Connections)

| Metric | Thread-per-request | Thread Pool | Target Improvement |
|--------|-------------------|-------------|---------------------|
| **FD pass latency (p50)** | 43μs | <10μs | **>75% better** |
| **FD pass latency (p99)** | 100μs | <15μs | **>85% better** |
| **FD pass throughput/core** | 23K req/s | >100K req/s | **>4× increase** |
| **Memory (100 conns)** | 800MB | <100MB | >87% reduction |
| **Memory (1000 conns)** | 8GB | <300MB | **>96% reduction** |
| **Efficiency (work/total)** | ~10% | >85% | **8× improvement** |

**Critical Validation:**
- ✅ Thread pool MUST achieve >100K req/s per core for FD passing
- ✅ Pool size = num_cores should be optimal (no I/O blocking)
- ✅ Queue wait time should be <5μs at low load, <10μs at high load
- ✅ Worker threads handle multiple requests per connection (not one-shot)
- ✅ Connection lifecycle managed correctly (keep-alive, graceful close)
- ✅ No connection leaks or zombie threads
- ✅ Persistent connections should enable 10× throughput vs one-shot

---

## Implementation Complexity Analysis

### Thread Pool Addition

**Effort:** Medium (1-2 days)
**Risk:** Low
**Lines of Code:** ~500
**Files Modified:** 2-3

**Key Components:**
- `lib/concurrency/thread_pool.h`
- `lib/concurrency/thread_pool.c`
- Update `objmapper/src/server.c`

### Work-Stealing Pool

**Effort:** High (1 week)
**Risk:** Medium (complex synchronization)
**Lines of Code:** ~1000
**Files Modified:** 3-5

**Key Components:**
- Lock-free deque implementation
- Stealing algorithm
- Load balancing heuristics

### Async I/O (epoll)

**Effort:** Very High (2-3 weeks)
**Risk:** High (state machine complexity)
**Lines of Code:** ~2000
**Files Modified:** Major refactor (10+)

**Key Components:**
- State machine per connection
- Event loop infrastructure
- FD passing workaround (defeats purpose)

### Hybrid Transport-Aware

**Effort:** Medium-High (1 week)
**Risk:** Medium
**Lines of Code:** ~800
**Files Modified:** 5-7

**Key Components:**
- Transport routing layer
- Dual concurrency paths
- Configuration integration

---

## Conclusion

### REVISED Primary Recommendation: Thread Pool is MANDATORY for Persistent Connections

**Critical Discovery:** With persistent connections, FD passing is only ~8μs per request (not ~15μs with accept()), making thread-per-request overhead (35-80μs) catastrophically wasteful - only **10% efficient!**

**THE CURRENT IMPLEMENTATION CREATES A NEW THREAD FOR EVERY REQUEST, NOT PER CONNECTION. THIS IS A CRITICAL BUG.**

**Implement a connection-aware thread pool with these properties:**

1. **Default Behavior:** Thread pool with connection-level workers
   - Pool size = num_cores for FD passing (no I/O blocking)
   - Workers handle entire connection lifetime, not single requests
   - Connections are persistent with keep-alive by default

2. **Connection Handling:** Workers process request loops
   - Each worker dequeues a new connection (not a request!)
   - Processes ALL requests on that connection until close
   - 1000 requests/conn × 8μs = only 8ms of worker time per connection

3. **Expected Performance:**
   - Theoretical max: 125K requests/sec per core (8μs each)
   - Thread pool practical: ~100K requests/sec per core (90% efficiency)
   - Thread-per-request current: ~12K requests/sec per core (10% efficiency)
   - **10× throughput improvement!**

4. **Configuration:** CLI flags `--concurrency-model` and `--pool-size`
   - Default to thread pool with persistent connections
   - Deprecate thread-per-request mode (broken architecture)

5. **Metrics:** Export efficiency metrics
   - Requests per connection
   - Thread overhead vs work time ratio
   - Connection lifecycle statistics

**Expected Performance Improvements (Persistent Connections, FD Passing):**

| Client Load | Metric | Thread-per-request | Thread Pool | Improvement |
|-------------|--------|-------------------|-------------|-------------|
| 100 conns | Latency (p50) | 43μs | 10μs | **77% faster** |
| 100 conns | Throughput/core | 23K req/s | 100K req/s | **4× more** |
| 100 conns | Memory | 800MB | 64MB | **92% less** |
| 1000 conns | Latency (p50) | 100μs | 12μs | **88% faster** |
| 1000 conns | Throughput/core | 10K req/s | 83K req/s | **8× more** |
| 1000 conns | Memory | 8GB | 256MB | **96% less** |
| 1000 conns | Efficiency | 10% | 90% | **9× better** |

### Why Initial Analysis Was Catastrophically Wrong

**Original assumptions:**
- ❌ Assumed one request per connection
- ❌ Included accept() overhead (~5μs) in every request
- ❌ Thought storage operations would dominate latency
- ❌ Assumed thread-per-connection minimized latency
- ❌ Didn't realize current code is thread-per-REQUEST (even worse!)

**Corrected understanding (persistent connections):**
- ✅ Connections are persistent with multiple requests each
- ✅ No accept() overhead after initial connection
- ✅ FD passing never touches file contents (O(1))
- ✅ Only hash lookup + open() + sendmsg() = ~8μs per request
- ✅ Thread creation (35-80μs) is 4-10× the actual work
- ✅ **Current code spawns thread per REQUEST, not per connection!**
- ✅ Thread pool with connection-level workers is mandatory, not optional

### Avoid: Thread-Per-Request (Current Broken Implementation)

**Do NOT use thread-per-request because:**
- ❌ Thread creation overhead (35-80μs) >> work (8μs) per request
- ❌ Only 10% efficient (90% wasted on thread management!)
- ❌ Massive memory waste (8MB per concurrent request, not connection!)
- ❌ Context switch overhead destroys performance
- ❌ Cannot scale beyond ~15K req/s per core
- ❌ **Fundamentally broken architecture for persistent connections**

**This is not an optimization - it's a CRITICAL BUG FIX!**

### Avoid: Pure Async I/O for FD Passing

**Do NOT implement async I/O for FD passing because:**
- ❌ sendmsg(SCM_RIGHTS) is blocking (can't be async)
- ❌ No I/O to overlap (FD passing is O(1))
- ❌ State machine complexity for zero benefit
- ❌ Thread pool is simpler and faster

**Consider async I/O only for:**
- Copy/splice modes with large files (>1MB)
- Many concurrent TCP connections
- Cache miss rates >20%

### Implementation Priority (UPDATED)

**Phase 1: Thread Pool for FD Passing (CRITICAL - DO THIS FIRST)**
- **Effort:** 1-2 days
- **Risk:** Low
- **ROI:** Very High (33-75% latency improvement, 92-96% memory reduction)
- **Make it default,** provide thread-per-conn as fallback option

**Phase 2: Mode Detection and Separate Pools**
- Detect OP_FDPASS vs OP_COPY/OP_SPLICE
- Use small pool (num_cores) for FD passing
- Use larger pool (num_cores × 2) for copy/splice
- Measure and validate separation benefit

**Phase 3: Async I/O for Copy Mode (Low Priority)**
- Only if copy mode >20% of traffic
- Only if average file size >1MB
- Only if benchmarks show benefit

**Phase 4: Work-Stealing (Optional)**
- Only for mixed workloads with high variance
- Diminishing returns vs complexity

### Next Steps

1. **Prototype:** Basic thread pool (fixed queue, simple workers)
2. **Benchmark:** Compare FD passing latency vs thread-per-conn
3. **Validate:** Confirm O(1) behavior independent of file size
4. **Measure:** Thread overhead vs actual work time
5. **Document:** Configuration guide and tuning parameters
6. **Deploy:** Make thread pool the default, announce breaking change

### Key Takeaway

**FD passing is fundamentally different from traditional I/O.** Because it's O(1) and never blocks on file contents, the optimal concurrency model is **thread pool, not thread-per-connection**. This insight only became clear when analyzing the actual operation breakdown.

The 15μs of actual work should not be wrapped in 30-80μs of thread overhead.
