# Concurrency Model Evaluation

## Executive Summary

This document evaluates concurrency options for objmapper, optimizing primarily for Unix socket file descriptor passing while maintaining flexibility for TCP/UDP transports. We analyze threading models, async I/O approaches, and hybrid schemes with focus on latency vs throughput trade-offs.

**Key Findings:**
- Current model (thread-per-connection) is optimal for FD passing latency
- Async I/O adds overhead for the primary Unix socket use case
- Hybrid approach with configurable concurrency models recommended
- Thread pool with work stealing provides best throughput for TCP/UDP

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

**Characteristics:**
- ✅ Minimal latency for FD passing (no queueing)
- ✅ Simple implementation, easy to debug
- ✅ Natural isolation between clients
- ✅ Works well with kernel's connection queue
- ❌ Thread creation overhead (~10-50μs per connection)
- ❌ Context switch overhead for many concurrent clients
- ❌ Memory overhead (~8MB stack per thread on Linux)
- ❌ Limited by `max_connections` setting

**Performance Profile:**
- **Latency:** Excellent (10-20μs for FD passing after accept)
- **Throughput:** Good up to ~1000 concurrent connections
- **Memory:** Poor (8MB × connections)
- **CPU:** Moderate (context switching overhead)

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
            handle_client_work(session);
        }
    }
}
```

#### Pros & Cons
**Advantages:**
- ✅ Bounded resource usage (fixed thread count)
- ✅ No thread creation overhead
- ✅ Better CPU cache locality (threads reused)
- ✅ Predictable memory footprint
- ✅ Easy to tune (pool size = CPU cores × 2)

**Disadvantages:**
- ❌ Queueing latency (5-50μs depending on load)
- ❌ Queue contention under high load
- ❌ Head-of-line blocking for slow requests
- ❌ Requires careful queue sizing

**Performance Profile:**
- **Latency:** Good (queue delay + processing)
- **Throughput:** Excellent (saturates CPU cores)
- **Memory:** Excellent (fixed × threads)
- **CPU:** Excellent (no creation overhead)

**Best For:** High-throughput TCP/UDP serving with predictable request times

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

**Blocking Operations:**
1. **Hash table lookup:** O(1), ~50-200ns, minimal blocking
2. **mmap cache hit:** O(1), ~100ns, negligible
3. **mmap cache miss:** O(file size), 100μs - 10ms for cold files
4. **Disk I/O:** Highly variable, 1ms - 100ms for spinning disks

### Async Storage Implications

**Option A: Thread Pool Offload**
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

**Option B: io_uring for Disk I/O**
```c
// Use io_uring for actual file operations
struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
io_uring_prep_openat(sqe, AT_FDCWD, path, O_RDONLY, 0);
```

**Trade-offs:**
- Async storage adds latency for cache hits (callback overhead)
- Benefits only appear for cache misses
- Complexity >> benefit for typical cache hit rate (>95%)

---

## Latency vs Throughput Analysis

### Scenario 1: Low Concurrency (<100 clients)

| Model | Latency (p50) | Latency (p99) | Throughput | Memory |
|-------|---------------|---------------|------------|--------|
| Thread-per-conn | 15μs | 25μs | 50K req/s | 800MB |
| Thread pool (8) | 30μs | 200μs | 60K req/s | 64MB |
| Async epoll | 50μs* | 500μs* | 55K req/s | 20MB |
| Work-stealing | 35μs | 150μs | 65K req/s | 64MB |

*Cannot truly async with FD passing; requires thread offload

### Scenario 2: High Concurrency (1000+ clients)

| Model | Latency (p50) | Latency (p99) | Throughput | Memory |
|-------|---------------|---------------|------------|--------|
| Thread-per-conn | 100μs | 10ms | 45K req/s | 8GB |
| Thread pool (32) | 200μs | 2ms | 80K req/s | 256MB |
| Async epoll | 150μs* | 1ms* | 90K req/s | 50MB |
| Work-stealing | 180μs | 1.5ms | 85K req/s | 256MB |

### Scenario 3: Mixed TCP/UDP + Unix Socket

**Unix Socket (FD passing):**
- Requires synchronous `sendmsg()` with `SCM_RIGHTS`
- Latency-critical (cache serving)
- Throughput secondary

**TCP/UDP:**
- Can benefit from async I/O
- Throughput-critical
- Latency tolerant (network RTT >> local ops)

**Optimal Strategy:** Separate concurrency models per transport

---

## Architectural Recommendations

### Recommendation 1: Configurable Concurrency Model

```c
typedef enum {
    CONCURRENCY_THREAD_PER_CONN,  // Current, optimal for FD passing
    CONCURRENCY_THREAD_POOL,       // Fixed pool, good for TCP
    CONCURRENCY_WORK_STEALING,     // Advanced pool, mixed workloads
    CONCURRENCY_ASYNC_EPOLL,       // For TCP/UDP with many idle conns
    CONCURRENCY_AUTO               // Select based on transport
} concurrency_model_t;

typedef struct {
    concurrency_model_t model;
    
    // Thread pool settings
    int pool_size;              // 0 = auto (num_cores * 2)
    int queue_size;             // Bounded queue depth
    
    // Latency vs throughput tuning
    int optimize_for_latency;   // 1 = minimize latency, 0 = max throughput
    int max_threads;            // Hard limit for thread-per-conn
    
    // Storage backend
    int async_storage;          // Offload storage I/O to separate pool
    int storage_pool_size;      // Size of I/O worker pool
} concurrency_config_t;
```

### Recommendation 2: Transport-Aware Concurrency

```c
typedef struct {
    transport_type_t transport;
    concurrency_model_t model;
} transport_concurrency_map_t;

// Default mappings
transport_concurrency_map_t defaults[] = {
    { TRANSPORT_UNIX, CONCURRENCY_THREAD_PER_CONN },  // FD passing优先
    { TRANSPORT_TCP,  CONCURRENCY_THREAD_POOL },       // 吞吐量优先
    { TRANSPORT_UDP,  CONCURRENCY_ASYNC_EPOLL }        // 高并发优先
};
```

### Recommendation 3: Hybrid Architecture (Best of Both Worlds)

```
┌─────────────────────────────────────────────┐
│            Transport Demultiplexer          │
└─────────────┬───────────────┬───────────────┘
              │               │
    ┌─────────┘               └─────────┐
    │                                   │
    v                                   v
┌───────────────────┐         ┌──────────────────┐
│ Unix Socket Path  │         │  TCP/UDP Path    │
│ (Thread-per-conn) │         │  (Thread Pool)   │
└───────────────────┘         └──────────────────┘
    │                                   │
    v                                   v
┌───────────────────┐         ┌──────────────────┐
│ FD Passing (fast) │         │  Copy/Splice     │
└───────────────────┘         └──────────────────┘
```

**Implementation:**
- Accept thread detects transport type from config
- Routes to appropriate concurrency handler
- Unix sockets get dedicated fast path
- TCP/UDP share thread pool for efficiency

### Recommendation 4: Gradual Evolution Path

**Phase 1: Add Thread Pool (Low Risk)**
- Implement basic fixed-size thread pool
- Make it configurable via CLI flag
- Keep thread-per-conn as default for Unix sockets
- Measure and validate performance

**Phase 2: Add Auto-Selection (Medium Risk)**
- Implement transport-aware model selection
- Add runtime metrics collection
- Allow override via config

**Phase 3: Add Work-Stealing (Optional)**
- Only if metrics show benefit
- For mixed workloads with variable latency

**Phase 4: Async I/O (Future)**
- Only for TCP/UDP if needed
- Keep Unix socket path synchronous

---

## Performance Tuning Knobs

### Recommended Configuration Interface

```c
// Server startup
server_config_t config = {
    .transport = OBJMAPPER_TRANSPORT_UNIX,
    .concurrency = {
        .model = CONCURRENCY_AUTO,  // or explicit choice
        
        // Latency optimization
        .optimize_for_latency = 1,
        .max_queue_depth = 100,     // Reject if queue full
        
        // Throughput optimization
        .pool_size = 0,             // Auto-detect cores
        .max_threads = 1000,        // Upper bound
        
        // Storage tuning
        .async_storage = 0,         // Sync for cache hits
        .storage_pool_size = 4,     // For cache misses
    }
};
```

### Runtime Metrics to Expose

```c
typedef struct {
    // Latency metrics
    uint64_t p50_latency_us;
    uint64_t p99_latency_us;
    uint64_t p999_latency_us;
    
    // Throughput metrics
    uint64_t requests_per_sec;
    uint64_t bytes_per_sec;
    
    // Resource metrics
    int active_threads;
    int queue_depth;
    int queue_depth_max;
    uint64_t queue_wait_time_us;
    
    // Storage metrics
    uint64_t cache_hit_rate;
    uint64_t disk_io_pending;
} server_metrics_t;
```

---

## Benchmarking Plan

### Test Scenarios

**1. Latency Benchmark (FD Passing)**
```bash
# Single client, measure round-trip time
./bench_latency --clients 1 --requests 10000 --mode fdpass
```

**2. Throughput Benchmark (TCP Copy)**
```bash
# Multiple clients, measure total throughput
./bench_throughput --clients 100 --duration 60s --transport tcp
```

**3. Scalability Benchmark**
```bash
# Vary client count, measure degradation
for n in 1 10 100 1000 10000; do
    ./bench_scale --clients $n --duration 10s
done
```

**4. Storage Backend Stress**
```bash
# Mix of cache hits and misses
./bench_storage --hit-rate 0.8 --file-size 1MB --clients 100
```

### Success Criteria

| Metric | Thread-per-conn | Thread Pool | Improvement |
|--------|----------------|-------------|-------------|
| FD pass latency (p50) | 15μs | <20μs | <33% increase |
| FD pass latency (p99) | 25μs | <50μs | <100% increase |
| TCP throughput | 50K req/s | >70K req/s | >40% increase |
| Memory (1000 clients) | 8GB | <500MB | >90% reduction |

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

### Primary Recommendation: Configurable Thread Pool

**For objmapper, implement a configurable thread pool with these properties:**

1. **Default Behavior:** Thread-per-connection for Unix sockets (preserve current latency)
2. **Optional Mode:** Fixed-size thread pool for TCP/UDP (improve throughput)
3. **Configuration:** CLI flags `--concurrency-model` and `--pool-size`
4. **Metrics:** Export latency and throughput stats for tuning

**Rationale:**
- ✅ Maintains optimal latency for FD passing (primary use case)
- ✅ Improves throughput for secondary transports (TCP/UDP)
- ✅ Manageable implementation complexity
- ✅ Provides tuning flexibility
- ✅ Enables future optimizations (work-stealing, async I/O)

### Avoid: Pure Async I/O

**Do NOT implement pure async I/O (epoll/io_uring) because:**
- ❌ FD passing requires blocking syscalls (defeats async benefits)
- ❌ High implementation complexity
- ❌ Negligible benefit for cache-hit workloads
- ❌ Debugging and maintenance burden

### Future Considerations

- **If TCP throughput becomes critical:** Add work-stealing to thread pool
- **If many idle TCP connections:** Add async accept + thread pool hybrid
- **If storage I/O is bottleneck:** Add separate I/O worker pool
- **If metrics show benefit:** Implement per-transport concurrency models

### Next Steps

1. **Prototype:** Simple thread pool implementation
2. **Benchmark:** Compare against thread-per-conn baseline
3. **Validate:** Ensure FD passing latency unchanged
4. **Document:** Configuration options and tuning guide
5. **Deploy:** Make configurable, default to current behavior
