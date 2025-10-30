# Varnish ↔ objmapper Zero-Touch Delivery Options

This note outlines design candidates for a full file-descriptor delivery path while
preserving Varnish/H2O functionality and keeping interfaces explicit. The focus is on
streaming cached objects with “zero CPU touch” in the hot path, while providing
predictable fallbacks whenever VCL or built-in filters need to inspect or mutate the
body.

## Shared Constraints

- **Functional parity**: headers, cookies, variants, gzip/ESI toggles, Range support,
  and PUT updates must keep working. Zero-copy is an acceleration, not a behavioral
  change.
- **objmapper as system of record**: the backing store remains objmapper-owned.
  Varnish acquires read handles (FDs) and uses PUT to update. CoW/mmap fallback is
  acceptable for modifications.
- **Existing syscalls only**: delivery relies on today’s primitives (`sendfile`,
  `splice`, `mmap`, `madvise`, `mprotect`). The proposed `mapm` syscall is considered
  optional and only for ingestion if it appears.
- **KTLS-friendly**: the fast path should dovetail with the kernel TLS layer so
  encryption/compression can stay in-kernel when NIC hardware supports it.

## Option A – Dual-Path Delivery (Preferred)

### Summary

Keep the current Varnish object API untouched, but attach optional FD payloads to
`objcore`. A new delivery processor chooses between:

1. **FD fast lane** – call `sendfile()`/`splice()` (with KTLS if enabled) when the
   object is marked “transformation complete”.
2. **Legacy path** – fall back to `ObjIterate()` and existing VDPs when the body must
   be inspected or altered.

### Interfaces

- Extend `struct objcore` with an opaque `struct obj_fd_payload { int fd; off_t len;
  uint32_t capabilities; }`.
- New VDP flag `VDP_CTX_FD_READY` signals the availability of an FD.
- Delivery loop chooses path: if all active VDPs declare `vdx->needs_body == false`,
  call a new `VBF_Sendfile()` helper; otherwise call existing iterator.
- objmapper stevedore supplies FD capabilities (`OBJM_CAP_GZIP`, `OBJM_CAP_ESI_FLAT`,
  etc.) so filters can veto the fast lane deterministically.

### Pros

- Minimal disruption to VCL and existing stevedores.
- Zero-copy hits automatically drop into the fast lane.
- Fallback remains battle-tested.

### Cons

- Requires VDP capability handshake to be reliable.
- Mixed responses (e.g., body mapped mid-stream) always pay legacy cost.

## Option B – Precomputed Variants in objmapper

### Summary

Have objmapper materialize transformation variants at ingest time. For example, store
both a compressed and uncompressed FD, ESI-flattened FD, or Range-friendly chunked FD.
Varnish selects the variant based on VCL and client headers, eliminating run-time
body manipulation.

### Interfaces

- Extend objmapper metadata with `variant_id` tags (gzip, identity, esi_flat, …).
- During backend fetch, objmapper filters the stream and emits one FD per variant.
- Varnish stores variant metadata in `objcore` and maps cache keys to specific FDs.

### Pros

- Keeps delivery always zero-touch once the variant choice is known.
- Moves heavy processing out of varnishd worker threads.

### Cons

- Increases storage consumption (multiple bodies per object).
- Adds complexity to objmapper ingestion pipelines.
- Harder to support on-demand transformation when feature toggles change.

## Option C – Filter-as-Process

### Summary

Transformations (gzip, ESI) run in helper processes that read from the source FD and
write to a target FD owned by objmapper. Varnish never touches the body; it only
switches the FD pointer once the helper confirms completion.

### Interfaces

- Define a filter ABI (probably over Unix sockets) where helpers advertise supported
  transformations.
- Backend fetch stream is piped into helpers; helpers write final data back via
  `sendmsg(SCM_RIGHTS)`.
- Varnish waits on helper completion events before marking the cache entry usable.

### Pros

- Main varnish process remains I/O-only, preserving zero-touch goals.
- Clear isolation for complex transformations.

### Cons

- Higher latency for cold fetches (extra IPC round-trips).
- Requires lifecycle management of helper processes and failure handling.

## Option D – Direct mmap Readers

### Summary

When filters need body access, varnishd `mmap`s the objmapper FD, uses
`madvise(MADV_SEQUENTIAL|WILLNEED)` to hint kernel prefetch, performs the required
work, and then relies on FD delivery for the final send.

### Interfaces

- objmapper stevedore exposes `fd` for mapping. No extra metadata needed.
- After filters are done, varnish flushes any changes (if modifications were made) and
  marks the FD “clean”.

### Pros

- Uses only existing syscalls; no object duplication.
- Simple fallback story for rarely-used features.

### Cons

- CPU touches the data ↔ zero-touch promise broken during fallback.
- Requires strict discipline to unmap and avoid double writes.

## Compatibility Matrix

| Feature / VCL Need   | Option A | Option B | Option C | Option D |
|----------------------|----------|----------|----------|----------|
| Header/cookie logic  | ✅       | ✅       | ✅       | ✅       |
| gzip on/off          | ⚠️ (needs metadata) | ✅ (store both) | ✅ (helper) | ⚠️ (touch data) |
| ESI processing       | ⚠️ (preprocessed?) | ✅ | ✅ | ⚠️ |
| Range requests       | ✅ (kernel handled) | ✅ (chunk variants) | ✅ | ✅ |
| Synthetic responses  | ❌ (body generated) | ❌ | ❌ | ❌ |
| PUT/updates          | ✅ (PUT to objmapper) | ✅ | ✅ | ✅ |

## Operational Observability

Regardless of option, expose metrics:

- `OBJM.delivery.fd_hits`, `OBJM.delivery.fd_fallbacks`
- `OBJM.delivery.ktls_enabled`
- `OBJM.delivery.variant_miss`
- Fallback reasons (gzip requested, ESI required, synthetic response, Range mismatch)

These counters let operators verify that the fast lane remains effective when VCL
changes roll out.

## Recommendation & Next Steps

1. **Prototype Option A** with capability flags and automatic fallback – least risky
   path that preserves existing invariants.
2. Incrementally add **Option B** mechanics in objmapper for high-value variants
   (e.g., precompressed assets) so CDN workloads can stay zero-touch even with gzip.
3. Keep Option C as a future enhancement if transformation workloads become heavier
   or need isolation.

Follow-up tasks:

- Specify the `obj_fd_payload` structure and VDP capability bits.
- Define objmapper metadata schema for body variants and delivery capabilities.
- Add logging and metrics for fast-lane vs. fallback decisions.
- Validate KTLS + `sendfile()` with FD delivery under load to confirm zero-copy goals.
