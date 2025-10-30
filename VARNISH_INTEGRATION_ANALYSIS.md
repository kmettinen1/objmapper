# Varnish Cache + objmapper Integration Analysis

## Executive Summary

This document analyzes how to integrate objmapper into Varnish Cache in two distinct ways:
1. **Storage Backend Integration**: Replace Varnish's storage stevedores with objmapper
2. **Protocol Integration**: Use objmapper protocol for upstream proxy communication (e.g., with H2O)

**Critical Architectural Mismatch Identified**: Varnish expects in-process memory (`ptr + len`), while objmapper provides out-of-process storage via file descriptors. This fundamental difference requires significant architectural modifications to Varnish.

---

## 1. Varnish Cache Architecture Overview

### 1.1 Storage Stevedore Interface

Varnish uses a pluggable storage backend system called "stevedores":

```c
struct stevedore {
    unsigned magic;
    const char *name;
    storage_init_f *init;           // MGT process initialization
    storage_open_f *open;           // Cache process open
    storage_allocobj_f *allocobj;   // REQUIRED: Allocate object
    storage_allocbuf_f *allocbuf;   // Optional: Allocate buffer
    storage_freebuf_f *freebuf;     // Optional: Free buffer
    sml_alloc_f *sml_alloc;         // SML backends only
    sml_free_f *sml_free;           // SML backends only
    const struct obj_methods *methods;
    struct lru *lru;                // LRU for eviction
    void *priv;                     // Private data
};
```

**Key Stevedore Implementations**:
- `sma_stevedore` (malloc): Default on Linux, uses `malloc()/free()`
- `smu_stevedore` (umem): Solaris umem library
- `smf_stevedore` (file): mmap'd file storage
- `smp_stevedore` (persistent): Deprecated
- `smd_stevedore` (debug): Testing backend

All current stevedores use the **Simple Memory Layer (SML)** framework.

### 1.2 Object Allocation Flow

```
Client Request
    ↓
VCL selects stevedore (beresp.storage = <name>)
    ↓
STV_NewObject(worker, objcore, stevedore, wsl)
    ↓
stevedore->allocobj(worker, stv, oc, wsl)
    ↓
SML_allocobj() [for SML backends]
    ↓
sml_stv_alloc(stv, size, flags)
    ↓
stv->sml_alloc(stv, size)  → malloc/umem/mmap
    ↓
SML_MkObject(stv, oc, ptr)
    ↓
Initialize object structure
Set oc->stobj->stevedore
    ↓
Return object pointer
```

### 1.3 Storage Structure

```c
struct storage {
    unsigned magic;
    unsigned flags;                 // STORAGE_F_BUFFER
    VTAILQ_ENTRY(storage) list;
    void *priv;                     // Backend private data
    unsigned char *ptr;             // *** IN-PROCESS POINTER ***
    unsigned len;                   // Used length
    unsigned space;                 // Total allocated space
};

struct object {
    unsigned magic;
    struct storage *objstore;       // Primary storage
    struct storagehead list;        // Linked list of storage segments
    // Fixed attributes (headers, etc.)
    // Variable attributes (body, etc.)
};
```

**CRITICAL**: Objects can span **multiple storage segments** in a VTAILQ linked list.

### 1.4 Object I/O Operations

#### Fetching (Backend → Storage)
```c
// bin/varnishd/cache/cache_fetch_proc.c
VFP_GetStorage(vfc, &sz, &ptr)
    ↓
ObjGetSpace(wrk, oc, &sz, &ptr)  // Get write pointer
    ↓
sml_getspace() → objallocwithnuke()
    ↓
Returns: uint8_t **ptr, ssize_t *sz
    ↓
VFP writes directly to ptr
    ↓
ObjExtend(wrk, oc, len, final)   // Mark data as written
```

#### Delivery (Storage → Client)
```c
// bin/varnishd/cache/cache_deliver.c
VDP_DeliverObj(vdc, objcore)
    ↓
ObjIterate(wrk, oc, priv, func, final)
    ↓
sml_iterator() / sml_ai_lease_boc()
    ↓
For each storage segment:
    viov->iov.iov_base = st->ptr  // *** DIRECT POINTER ***
    viov->iov.iov_len = st->len
    ↓
writev() to client socket
```

**Key Pattern**: Varnish uses `iovec` structures with **direct memory pointers** for zero-copy delivery via `writev()`.

### 1.5 HTTP Request/Response Flow

```
┌─────────────┐
│ vcl_recv    │ Select backend_hint
└──────┬──────┘
       ↓
┌─────────────┐
│ vcl_backend │ Configure backend request
│  _fetch     │
└──────┬──────┘
       ↓
┌─────────────┐
│ VDI_GetHdr  │ Connect to backend, send request
│             │ Receive response headers
└──────┬──────┘
       ↓
┌─────────────┐
│ vcl_backend │ Process response headers
│  _response  │ Set caching policy
└──────┬──────┘
       ↓
┌─────────────┐
│ VFP_Pull    │ Fetch response body
│             │ Write to storage via ObjGetSpace/ObjExtend
└──────┬──────┘
       ↓
┌─────────────┐
│ vcl_deliver │ Process before delivery
└──────┬──────┘
       ↓
┌─────────────┐
│ VDP_Deliver │ Stream to client via ObjIterate
│             │ Uses writev() with storage pointers
└─────────────┘
```

### 1.6 Backend Connection Architecture

```c
// bin/varnishd/cache/cache_backend.c
struct backend {
    unsigned n_conn;
    struct vrt_endpoint *endpoint;
    struct vbp_target *probe;       // Health probes
    struct VSC_vbe *vsc;            // Statistics
    struct conn_pool *conn_pool;    // Connection pooling
    VCL_BACKEND director;
};

// Connection flow
vbe_dir_gethdrs(ctx, backend)
    ↓
vbe_get_pfd(wrk, ctx, dir, force_fresh)
    ↓
VCP_Get(conn_pool, timeout, wrk, force_fresh, &err)  // Pooled connections
    ↓
If needed: VTCP_connect(endpoint->ipv4, timeout)
    ↓
If proxy_header: VPX_Send_Proxy(fd, version, sp)     // PROXY protocol support
    ↓
HTTP/1 request sent via writev()
    ↓
Response read into htc (HTTP connection buffer)
```

**Key Features**:
- Connection pooling (`conn_pool`)
- Health probing (`vbp_target`)
- PROXY protocol support (v1, v2)
- HTTP/1 and HTTP/2 support
- TCP and Unix domain socket support

---

## 2. Architectural Mismatches with objmapper

### 2.1 Memory Model Mismatch

| Aspect | Varnish Expectation | objmapper Reality | Impact |
|--------|---------------------|-------------------|--------|
| **Storage Access** | In-process `uint8_t *ptr` | Out-of-process FD | **HIGH** |
| **I/O Pattern** | Direct memory access | FD passing + read/write | **HIGH** |
| **Zero-Copy** | `writev()` with pointers | `sendfile()` with FDs | **MEDIUM** |
| **Fragmentation** | Multiple `storage` segments | Single URI → Single FD | **MEDIUM** |
| **LRU Eviction** | Walk storage list, free | Send DELETE to objmapper | **LOW** |

### 2.2 Integration Approaches

#### Approach A: mmap Hybrid (Minimal Varnish Changes)
**Concept**: mmap the FDs returned by objmapper to get in-process pointers.

```c
// Hypothetical objmapper stevedore
int objm_allocobj(struct worker *wrk, const struct stevedore *stv,
                  struct objcore *oc, unsigned wsl)
{
    char uri[256];
    int fd;
    void *ptr;
    size_t size = compute_size(wsl);
    
    // Generate unique URI
    snprintf(uri, sizeof(uri), "varnish:obj:%ju", VXID(oc->vxid));
    
    // Request storage from objmapper
    fd = objmapper_get(uri, size);
    if (fd < 0) return 0;
    
    // mmap the FD to get in-process pointer
    ptr = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        return 0;
    }
    
    // Create storage structure with mmap'd pointer
    struct storage *st = create_storage(ptr, size);
    st->priv = (void *)(intptr_t)fd;  // Save FD for later
    
    // Standard SML object creation
    struct object *o = SML_MkObject(stv, oc, ptr);
    o->objstore = st;
    
    return 1;
}
```

**Pros**:
- Minimal changes to Varnish core
- Maintains existing I/O patterns (`writev`, direct access)
- Compatible with existing VCL

**Cons**:
- **Defeats objmapper's zero-copy design** (data is copied into mmap'd region)
- **Memory overhead** (mmap uses virtual address space)
- **FD lifecycle management** complexity (when to munmap/close?)
- **Synchronization issues** (when to `msync()`?)
- **Doesn't leverage objmapper's FD passing**

#### Approach B: Native FD Integration (Major Varnish Refactoring)
**Concept**: Modify Varnish to work directly with FDs throughout the pipeline.

```c
// New storage structure
struct storage_fd {
    unsigned magic;
    int fd;                    // File descriptor
    off_t offset;              // Offset within FD
    size_t len;                // Used length
    size_t space;              // Total space
    char uri[256];             // objmapper URI
};

// Modified object iteration
int objiter_fd(struct worker *wrk, struct objcore *oc,
               void *priv, objiterate_f *func, int final)
{
    struct storage_fd *st;
    
    VTAILQ_FOREACH(st, &oc->stobj->fd_list, list) {
        // Use sendfile() instead of writev()
        func(priv, OA_DATA, &st->fd, st->len);
    }
}

// Modified VDP delivery
int vdp_deliver_fd(vdc, objcore)
{
    // Instead of writev(iovec[])
    // Use sendfile(client_fd, storage_fd, offset, len)
}
```

**Changes Required**:
1. **cache/cache_obj.c**: New `obj_methods` for FD-based objects
2. **cache/cache_deliver.c**: Modify `ObjIterate` to return FDs
3. **cache/cache_vrt.c**: Update VDP pipeline for `sendfile()`
4. **cache/cache_fetch.c**: Modify fetch to write via FD
5. **storage/**: New `storage_objmapper.c` stevedore
6. **http1/cache_http1_deliver.c**: Replace `writev()` with `sendfile()`

**Pros**:
- **True zero-copy** delivery via `sendfile()`
- **Leverages objmapper's design**
- **Potential for cross-process object sharing**

**Cons**:
- **Massive refactoring** of Varnish internals
- **Breaks VCL compatibility** (body access patterns change)
- **ESI complexity** (inline processing requires reading FDs)
- **Gzip/Gunzip** (compression requires reading entire object)
- **Streaming** (partial object delivery becomes complex)
- **Testing burden** (entire delivery pipeline affected)

#### Approach C: Dual-Mode Stevedore (Pragmatic Hybrid)
**Concept**: Use objmapper for large, cacheable objects; keep malloc for small/dynamic objects.

```c
int objm_allocobj(struct worker *wrk, const struct stevedore *stv,
                  struct objcore *oc, unsigned wsl)
{
    size_t size = compute_size(wsl);
    
    // Small objects: use malloc (standard SML)
    if (size < 64*1024) {
        return SML_allocobj(wrk, stv, oc, wsl);
    }
    
    // Large objects: use objmapper with mmap
    return objm_allocobj_large(wrk, stv, oc, wsl);
}
```

**Pros**:
- **Gradual migration** path
- **Preserves compatibility** for small objects
- **Leverages objmapper** where it matters (large objects)

**Cons**:
- **Complexity** in stevedore logic
- **Mixed storage backends** in same cache
- **Still requires mmap** (not true zero-copy)

---

## 3. Protocol Integration for Upstream Proxies

### 3.1 Use Case: Varnish → objmapper → H2O

```
┌─────────────┐                           ┌─────────────┐
│   Client    │                           │     H2O     │
└──────┬──────┘                           └──────┬──────┘
       │                                         │
       │ HTTP request                            │
       ↓                                         │
┌─────────────┐                                  │
│   Varnish   │                                  │
│  (Modified) │                                  │
└──────┬──────┘                                  │
       │                                         │
       │ Cache miss                              │
       ↓                                         │
┌─────────────────────────────────┐              │
│       objmapper Protocol        │              │
│  1. GET uri://upstream/path     │              │
│  2. Receive FD (empty)          │              │
│  3. Forward request to H2O      │◄─────────────┘
│  4. H2O writes to FD            │
│  5. Return FD to Varnish        │
└─────────────────────────────────┘
```

### 3.2 Integration Points

#### Option 1: Custom Director
Create a new director type that speaks objmapper protocol:

```c
// vmod_objmapper_director.c
static VCL_BACKEND
objm_resolve(VRT_CTX, VCL_BACKEND dir)
{
    struct objm_director *od;
    char uri[512];
    int fd;
    
    CAST_OBJ_NOTNULL(od, dir->priv, OBJM_DIRECTOR_MAGIC);
    
    // Build objmapper URI from req
    build_uri(ctx->req, uri, sizeof(uri));
    
    // Try to GET from objmapper
    fd = objmapper_get(uri, 0);
    if (fd >= 0) {
        // Cache hit in objmapper
        // Attach FD to objcore
        attach_fd_to_objcore(ctx->bo->fetch_objcore, fd);
        return NULL;  // No need to fetch from backend
    }
    
    // Cache miss: fallback to real backend
    return od->backend;
}

static int
objm_gethdrs(VRT_CTX, VCL_BACKEND dir)
{
    struct objm_director *od;
    int fd;
    
    // GET empty FD from objmapper
    fd = objmapper_get(uri, expected_size);
    
    // Connect to H2O
    int h2o_fd = connect_to_h2o(od->h2o_backend);
    
    // Send HTTP request to H2O
    send_request(h2o_fd, ctx->bo->bereq);
    
    // Receive response, write to objmapper FD
    pump_response(h2o_fd, fd);
    
    // Parse headers from FD
    parse_headers_from_fd(fd, ctx->bo->beresp);
    
    return 0;
}
```

#### Option 2: Backend Filter
Intercept backend connections to inject objmapper protocol:

```c
// cache/cache_backend_objmapper.c
int
VDI_GetHdr_Objmapper(VRT_CTX, VCL_BACKEND d)
{
    char uri[512];
    int fd;
    
    // Try objmapper first
    build_uri_from_bereq(ctx->bo->bereq, uri);
    fd = objmapper_get(uri, 0);
    
    if (fd >= 0) {
        // Hit: attach FD, parse headers
        attach_fd(ctx->bo, fd);
        return 0;
    }
    
    // Miss: GET empty FD
    fd = objmapper_get(uri, 1*1024*1024 /* estimate */);
    
    // Forward to real backend (H2O)
    int ret = vbe_dir_gethdrs(ctx, d);  // Original function
    
    if (ret == 0) {
        // Success: copy response to objmapper FD
        copy_response_to_fd(ctx->bo, fd);
    }
    
    return ret;
}
```

### 3.3 Protocol Flow Example

```
Varnish                  objmapper              H2O
   │                         │                   │
   │ GET uri://h2o/path      │                   │
   ├────────────────────────►│                   │
   │                         │                   │
   │ FD (empty, 1MB)         │                   │
   │◄────────────────────────┤                   │
   │                         │                   │
   │ Connect to H2O          │                   │
   ├─────────────────────────┼──────────────────►│
   │                         │                   │
   │ GET /path HTTP/1.1      │                   │
   ├─────────────────────────┼──────────────────►│
   │                         │                   │
   │ HTTP/1.1 200 OK         │                   │
   │◄────────────────────────┼───────────────────┤
   │                         │                   │
   │ Body data (chunks)      │                   │
   │◄────────────────────────┼───────────────────┤
   │                         │                   │
   │ write(fd, data, len)    │                   │
   ├────────────────────────►│                   │
   │                         │ [writes to file]  │
   │                         │                   │
   │ EOF                     │                   │
   │◄────────────────────────┼───────────────────┤
   │                         │                   │
   │ Store FD in objcore     │                   │
   │                         │                   │
   │ Deliver to client       │                   │
   │ sendfile(client, fd)    │                   │
   │                         │                   │
```

---

## 4. VCL Processing Impact

### 4.1 VCL Access Patterns

Current VCL can access response bodies:
```vcl
sub vcl_backend_response {
    # This requires reading the entire body into memory
    if (beresp.http.Content-Type ~ "text/html") {
        set beresp.do_esi = true;
    }
}

sub vcl_deliver {
    # Body modification (synthetic)
    synthetic("Error: " + resp.status);
}
```

**With FD-based storage**:
- **ESI processing** requires reading FD, parsing, potentially re-writing
- **Synthetic responses** need separate code path (can't use objmapper)
- **Body inspection** (e.g., for virus scanning) requires FD reads

### 4.2 Compatibility Matrix

| VCL Feature | Approach A (mmap) | Approach B (native FD) | Impact |
|-------------|-------------------|------------------------|--------|
| Header manipulation | ✓ Compatible | ✓ Compatible | None |
| ESI processing | ✓ Compatible | ⚠ Requires changes | High |
| Gzip/Gunzip | ✓ Compatible | ⚠ Requires changes | High |
| Streaming delivery | ✓ Compatible | ⚠ Complex | Medium |
| Synthetic responses | ✓ Compatible | ✓ Compatible | None |
| Hit-for-pass | ✓ Compatible | ✓ Compatible | None |
| Grace/stale | ✓ Compatible | ⚠ FD lifecycle | Medium |

### 4.3 Features That Could Be Removed

If we're willing to break compatibility, these Varnish features could be simplified or removed:

1. **ESI (Edge Side Includes)**: Complex inline processing
2. **Compression** (do_gzip/do_gunzip): Requires full body read
3. **Multiple storage segments**: objmapper uses single FD per URI
4. **Partial object updates**: objmapper FDs are immutable once written

**Trade-off**: Removing these features makes Varnish a simpler **caching proxy** rather than a **content transformation engine**.

---

## 5. Implementation Recommendations

### 5.1 Phase 1: Proof of Concept (Approach A - mmap)

**Goal**: Validate integration with minimal Varnish changes.

**Implementation**:
1. Create `storage_objmapper.c` stevedore
2. Implement `allocobj` with mmap of objmapper FDs
3. Implement `objfree` with munmap/close
4. Test basic GET/PUT/DELETE flows
5. Measure performance vs malloc stevedore

**Deliverables**:
- Working objmapper stevedore (POC quality)
- Performance benchmarks
- Identified integration issues

**Timeline**: 2-3 weeks

### 5.2 Phase 2: Protocol Integration (Custom Director)

**Goal**: Enable Varnish → objmapper → H2O communication.

**Implementation**:
1. Create `vmod_objmapper` VMOD
2. Implement director with objmapper protocol client
3. Add VCL hooks for URI mapping
4. Test with H2O backend
5. Measure cache hit rates and latency

**Deliverables**:
- VMOD for objmapper protocol
- VCL examples
- Integration guide for H2O

**Timeline**: 3-4 weeks

### 5.3 Phase 3: Native FD Integration (If POC shows benefit)

**Goal**: True zero-copy delivery with FD-based storage.

**Implementation** (high-level):
1. Design new `obj_methods` for FD storage
2. Modify `ObjIterate` to return FDs
3. Update delivery pipeline for `sendfile()`
4. Handle ESI/compression as special cases
5. Extensive testing

**Deliverables**:
- Modified Varnish with FD-native storage
- Performance comparison vs mmap approach
- Migration guide

**Timeline**: 3-6 months (major undertaking)

### 5.4 Alternative: Fork Varnish

Given the architectural mismatches, consider forking Varnish as **"Varnish-objmapper"**:

**Removed Features**:
- ESI processing
- Compression (do_gzip/do_gunzip)
- Multiple storage backends (only objmapper)
- Synthetic responses (or limited)

**Added Features**:
- Native FD-based storage
- objmapper protocol for upstream
- Simplified configuration
- Better performance for large objects

**Pros**:
- **Freedom to break compatibility**
- **Cleaner architecture**
- **Focused use case** (caching proxy, not transformation engine)

**Cons**:
- **Maintenance burden** (tracking upstream Varnish)
- **Smaller ecosystem** (VCL modules may not work)
- **User migration** (existing Varnish users can't easily switch)

---

## 6. Performance Considerations

### 6.1 Expected Performance Impact

| Metric | Approach A (mmap) | Approach B (native FD) |
|--------|-------------------|------------------------|
| **Memory usage** | +50-100% (mmap overhead) | -20% (no in-process storage) |
| **Cache hit latency** | +5-10% (mmap overhead) | -10-20% (sendfile gains) |
| **Cache miss latency** | +10-15% (objmapper protocol) | Same |
| **Throughput (large objects)** | -5% (memory copies) | +15-25% (zero-copy) |
| **Throughput (small objects)** | -10% (overhead not amortized) | -5% (syscall overhead) |

### 6.2 Optimization Opportunities

1. **Connection pooling**: Reuse objmapper Unix socket connections
2. **Batch operations**: Group multiple GET/PUT requests
3. **Mmap caching**: Keep frequently accessed mmap regions
4. **FD caching**: Don't close FDs for hot objects
5. **Hybrid approach**: Use malloc for small objects (<64KB), objmapper for large

---

## 7. Risk Analysis

### 7.1 Technical Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| **mmap overhead negates benefits** | HIGH | HIGH | Benchmark early; pivot to Approach B |
| **FD lifecycle bugs** (leaks, double-free) | MEDIUM | HIGH | Extensive testing, valgrind |
| **Compatibility breaks** | HIGH (Approach B) | MEDIUM | Clear documentation, migration guide |
| **Performance regression** | MEDIUM | HIGH | Continuous benchmarking |
| **objmapper single point of failure** | LOW | HIGH | objmapper redundancy design |

### 7.2 Operational Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| **User migration resistance** | HIGH | MEDIUM | Gradual rollout, compatibility mode |
| **VCL incompatibility** | HIGH (Approach B) | HIGH | VCL validation tools |
| **Debugging difficulty** | MEDIUM | MEDIUM | Enhanced logging, monitoring |
| **objmapper not mature** | MEDIUM | HIGH | Harden objmapper first |

---

## 8. Conclusions and Recommendations

### 8.1 Key Findings

1. **Fundamental Mismatch**: Varnish's in-process pointer model and objmapper's FD-based model are architecturally incompatible for true zero-copy.

2. **mmap Hybrid is Pragmatic**: Approach A (mmap) allows quick POC with minimal Varnish changes, but defeats objmapper's core design.

3. **Native FD Integration is Ideal**: Approach B would fully leverage objmapper, but requires massive Varnish refactoring (3-6 months).

4. **Protocol Integration is Separable**: Using objmapper protocol for upstream communication (H2O) can be implemented independently via VMOD.

5. **Feature Trade-offs Required**: ESI, compression, and some VCL features become problematic with FD-based storage.

### 8.2 Recommended Path Forward

**Short-term (0-3 months)**:
1. ✅ **Implement Approach A** (mmap stevedore) as POC
2. ✅ **Implement Protocol VMOD** for H2O integration
3. ✅ **Benchmark** both against baseline Varnish
4. ⚠ **Identify breaking points** (when does mmap overhead dominate?)

**Decision Point** (3 months):
- **If mmap overhead < 15%**: Ship Approach A, iterate on optimizations
- **If mmap overhead > 15%**: Evaluate Approach B or forking

**Long-term (6-12 months)** (if pursuing native FD):
1. 🔄 Design FD-native object model
2. 🔄 Prototype modified delivery pipeline
3. 🔄 Handle ESI/compression edge cases
4. 🔄 Extensive testing and migration tooling

### 8.3 Alternative Strategy: Targeted Use Case

Instead of replacing Varnish's storage entirely, **use objmapper for specific workloads**:

**Use Case**: Large media files (video, images) that benefit most from zero-copy.

**Implementation**:
- VCL selects objmapper stevedore based on Content-Type or URL pattern
- Small dynamic content uses malloc stevedore
- objmapper protocol used for select upstream services (media CDN, transcoding)

**Benefits**:
- **Focused benefits** where they matter most
- **Gradual migration** from existing Varnish setups
- **Preserves compatibility** for existing workloads

---

## 9. Next Steps

1. **Review this analysis** with stakeholders
2. **Decide on approach** (A, B, or targeted use case)
3. **Implement POC** of chosen approach
4. **Create benchmark suite** for apples-to-apples comparison
5. **Design objmapper protocol** specification for upstream communication
6. **Plan migration strategy** if pursuing production deployment

**Questions to resolve**:
- Is objmapper mature enough for production integration?
- What is the target workload? (Large files? Many small objects? Mixed?)
- Is breaking VCL compatibility acceptable?
- What is the timeline for production deployment?
- Is forking Varnish an acceptable option?

---

## Appendix A: Code Snippets

### A.1 Hypothetical objmapper Stevedore (Approach A)

```c
// storage_objmapper.c
#include "cache/cache_varnishd.h"
#include "storage/storage.h"
#include "storage/storage_simple.h"
#include <sys/mman.h>

struct objm_priv {
    char uri[256];
    int fd;
    void *mmap_addr;
    size_t mmap_size;
};

static struct storage *
objm_alloc(const struct stevedore *stv, size_t size)
{
    struct objm_priv *priv;
    struct storage *st;
    char uri[256];
    int fd;
    void *addr;
    
    // Generate unique URI
    snprintf(uri, sizeof(uri), "varnish:%ju", (uintmax_t)random());
    
    // GET from objmapper (creates if not exists)
    fd = objmapper_get(uri, size);
    if (fd < 0)
        return (NULL);
    
    // ftruncate to requested size
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return (NULL);
    }
    
    // mmap the FD
    addr = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        return (NULL);
    }
    
    // Create storage structure
    st = calloc(1, sizeof(*st));
    AN(st);
    st->magic = STORAGE_MAGIC;
    st->ptr = addr;
    st->space = size;
    st->len = 0;
    
    // Create private data
    priv = calloc(1, sizeof(*priv));
    AN(priv);
    strcpy(priv->uri, uri);
    priv->fd = fd;
    priv->mmap_addr = addr;
    priv->mmap_size = size;
    
    st->priv = priv;
    
    return (st);
}

static void
objm_free(struct storage *st)
{
    struct objm_priv *priv;
    
    CHECK_OBJ_NOTNULL(st, STORAGE_MAGIC);
    priv = st->priv;
    AN(priv);
    
    // munmap
    if (priv->mmap_addr != NULL)
        AZ(munmap(priv->mmap_addr, priv->mmap_size));
    
    // Close FD (objmapper may cache it)
    if (priv->fd >= 0)
        close(priv->fd);
    
    // Delete from objmapper
    objmapper_delete(priv->uri);
    
    free(priv);
    free(st);
}

static int v_matchproto_(storage_init_f)
objm_init(struct stevedore *parent, int ac, char * const *av)
{
    // Parse arguments (objmapper socket path, etc.)
    // Initialize connection to objmapper server
    return (0);
}

static void v_matchproto_(storage_open_f)
objm_open(struct stevedore *stv)
{
    // Open connection pool to objmapper
}

static void v_matchproto_(storage_close_f)
objm_close(const struct stevedore *stv, int pass)
{
    // Close connections to objmapper
}

const struct stevedore smo_stevedore = {
    .magic = STEVEDORE_MAGIC,
    .name = "objmapper",
    .init = objm_init,
    .open = objm_open,
    .close = objm_close,
    .sml_alloc = objm_alloc,
    .sml_free = objm_free,
};
```

### A.2 objmapper Protocol VMOD

```c
// vmod_objmapper.c
$Module objmapper 3 "objmapper protocol integration"

$Object director(STRING socket_path="/tmp/objmapper.sock")

Create an objmapper director that uses objmapper protocol
for backend communication.

Example::

    sub vcl_init {
        new objm = objmapper.director("/tmp/objmapper.sock");
        objm.set_backend(h2o_backend);
    }

$Method VOID .set_backend(BACKEND)

Set the real backend to fetch from on cache miss.

$Method BACKEND .backend()

Return this director for use in req.backend_hint.

$Function VOID set_uri_prefix(STRING prefix)

Set URI prefix for objmapper storage.

Example::

    objmapper.set_uri_prefix("cdn://example.com");
```

---

## Appendix B: References

### Varnish Cache Source Files Analyzed

**Storage System**:
- `bin/varnishd/storage/storage.h` - Stevedore interface
- `bin/varnishd/storage/stevedore.c` - Core stevedore logic
- `bin/varnishd/storage/storage_simple.c` - SML implementation
- `bin/varnishd/storage/storage_simple.h` - Object structure
- `bin/varnishd/storage/storage_malloc.c` - malloc backend
- `bin/varnishd/storage/storage_lru.c` - LRU eviction

**Object Management**:
- `bin/varnishd/cache/cache_obj.c` - Object lifecycle
- `bin/varnishd/cache/cache_obj.h` - Object methods
- `bin/varnishd/cache/cache.h` - Core structures

**HTTP Fetch/Delivery**:
- `bin/varnishd/cache/cache_fetch.c` - Backend fetching
- `bin/varnishd/cache/cache_fetch_proc.c` - VFP pipeline
- `bin/varnishd/cache/cache_deliver.c` - VDP delivery
- `bin/varnishd/http1/cache_http1_fetch.c` - HTTP/1 fetch
- `bin/varnishd/http1/cache_http1_deliver.c` - HTTP/1 delivery

**Backend System**:
- `bin/varnishd/cache/cache_backend.c` - Backend implementation
- `bin/varnishd/cache/cache_backend.h` - Backend structures
- `bin/varnishd/cache/cache_director.c` - Director API

**PROXY Protocol**:
- `bin/varnishd/proxy/cache_proxy_proto.c` - PROXY v1/v2

### External References

- Varnish Cache: https://github.com/varnishcache/varnish-cache
- objmapper: /home/dagge/src/objmapper
- H2O: https://github.com/h2o/h2o
- PROXY Protocol: https://www.haproxy.org/download/1.8/doc/proxy-protocol.txt

---

**Document Version**: 1.0  
**Date**: 2024  
**Author**: Analysis based on Varnish Cache (latest main) and objmapper v0.1
