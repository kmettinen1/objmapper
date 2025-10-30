# Implementation Roadmap: Varnish FD Delivery

The roadmap follows an outside-in approach: stabilize shared interfaces first, wire up
zero-touch delivery, and then iterate on performance-related variants. Each phase ends
with verifiable tests.

## Phase 0 – Groundwork

1. **Finalize objmapper metadata schema**
   - Define payload structure describing body variants, length, content encoding,
     and capability flags (gzip/identity, ESI-flattened, Range ready).
   - Add schema validation unit tests in objmapper (`tests/test_metadata_schema.py`).

2. **Baseline metrics**
   - Instrument existing delivery path with counters for `ObjIterate` invocations,
     response sizes, gzip usage, and ESI toggles so we can track regressions.
   - Add varnishd unit/varnishtest cases to assert counter availability.

## Phase 1 – Interface Extensions

1. **obj_fd_payload in objcore**
   - Extend Varnish’s `struct objcore`/`struct object` with optional FD payload.
   - Provide accessor helpers (`ObjSetFdPayload`, `ObjGetFdPayload`).
   - Add varnishtest verifying ABI stability (`tests/fd_payload_abi.vtc`).

2. **VDP capability negotiation**
   - Introduce VDP flag bitmask (`VDP_CAP_BODY_OPAQUE`, `VDP_CAP_REQUIRES_BODY`).
   - Update core VDPs (gzip, esi, range) to declare requirements.
   - Add unit tests to ensure capability flags reflect configuration changes.

3. **objmapper stevedore FD plumbing**
   - Modify objmapper stevedore to attach FD payload metadata when allocating
     objects; fallback to legacy storage if FD unavailable.
   - Write Varnish API test that fetches via objmapper and asserts FD presence.

## Phase 2 – Dual-Path Delivery (Option A)

1. **VBF_Sendfile implementation**
   - Add helper that executes `sendfile()` or `splice()` depending on transport.
   - Integrate with KTLS-capable sockets and expose runtime flag `delivery_zero_copy`.
   - varnishtest: simulate HTTP/1 and HTTP/2 clients verifying identical payloads.

2. **Delivery switch logic**
   - Update `VDP_DeliverObj` to choose between sendfile and legacy pipeline based on
     capability negotiation.
   - Add tests for fallback triggers (gzip enabled, ESI on, Range requested).

3. **Telemetry and logging**
   - Export stats (`fd_delivery.hits`, `fd_delivery.fallback_gzip`, etc.) and log
     entries for fallback reasons.
   - varnishtest asserting stats increments during fallback scenarios.

## Phase 3 – Precomputed Variant Support (Option B subset)

1. **objmapper variant generation hooks**
   - Extend PUT/fetch pipeline to allow precompression (gzip) and metadata tagging.
   - Unit tests in objmapper ensuring both identity and gzip variants available.

2. **Varnish variant selection**
   - Extend VCL/VDP to request specific variant (identity/gzip) based on client
     Accept-Encoding.
   - varnishtest verifying Accept-Encoding negotiation selects correct FD without
     fallback.

3. **Range-friendly metadata**
   - Mark FDs as range capable; ensure sendfile path handles HTTP Range by issuing
     correct offsets.
   - varnishtest validating partial content responses via FD path.

## Phase 4 – PUT / CoW Updates

1. **Update workflow**
   - Enhance objmapper PUT to reserve new FDs while keeping old ones live until
     transaction completes.
   - Integration test ensuring concurrent readers survive PUT updates.

2. **Varnish CoW fallback**
   - Document and implement explicit `ObjMapAndModify` API that mmaps FD, applies
     changes, and reattaches payload (with zero-copy invalidated).
   - varnishtest covering synthetic response or body modification case.

## Phase 5 – Performance & Hardening

1. **Benchmark suites**
   - Update `benchmark/` tools to measure throughput of sendfile vs legacy path.
   - Automate results comparison (`make bench-fd`) with thresholds.

2. **Edge cases**
   - Validate HTTP/2, TLS renegotiation, long-lived streaming responses.
   - Add tests for connection drops mid-sendfile, verifying cleanup of FDs.

3. **Documentation and ops guide**
   - Produce admin documentation describing configuration knobs, metrics, and
     troubleshooting for zero-touch delivery.

## Test Strategy Summary

- **Unit tests**: metadata schema, capability assertions, FD attachment helpers.
- **varnishtest suites**: delivery selection, fallback reasons, Range, gzip/ESI,
  PUT updates, stat counters.
- **Benchmark harness**: repeatable measurement of latency/throughput improvements.

By approaching implementation in these phases, we keep interfaces stable and
observable before wiring in zero-touch optimizations, with test coverage growing at
each step.
