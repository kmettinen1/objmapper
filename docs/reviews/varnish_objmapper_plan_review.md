# Review of objmapper ↔ Varnish Integration Plan

## Scope

Assessment covers the design proposals in `VARNISH_INTEGRATION_ANALYSIS.md`, with emphasis on storage semantics, delivery plumbing, and operational risk when marrying objmapper with Varnish Cache.

## Key Findings

- **Approach A duplicates existing Varnish capabilities** – Varnish already relies on virtual memory and lazy paging ("ptr + len" backed by mmapped files) to avoid double buffering.[1] An objmapper stevedore that simply mmaps the FD replays the built-in `storage_file` design, while adding user-space metadata tracking and coordination with a remote service. The plan understates the extra complexity (FD lifecycle, URI bookkeeping, index coherency) for negligible behaviour change on the Varnish side.

- **Approach B underestimates VDP/VFP entanglement** – Delivery processors expect contiguous memory presented through `ObjIterate` so that filters such as gzip, range truncation, and ESI can transform bytes before they reach HTTP/1 writers.[2][3] Replacing that interface with raw FDs (sendfile or `splice`) would bypass filter hooks or force expensive temporary buffers, negating the zero-copy goal and risking regressions for any VCL that toggles `beresp.do_gzip`, `beresp.do_esi`, or `resp.filters`.

- **Sendfile-only delivery conflicts with feature defaults** – Even in cache-hit paths, Varnish routinely assembles `struct iovec` chains and may coalesce synthetic headers, gunzip bodies, or apply conditional delivery based on `Range` requests.[2][3] The proposal does not map those cases to sendfile-friendly code paths, so fallback copies would remain mandatory; without quantified hit ratios, the expected throughput gains are speculative.

- **Protocol integration lacks failure-domain analysis** – The director/VMOD sketches push every miss through objmapper first, but there is no plan for partial availability (objmapper outage, backend success), retry storms, or FD exhaustion. Coordinating Varnish retries, grace handling, and objmapper's own eviction policy requires explicit SLIs/SLOs and backpressure design.

- **Operational knobs & observability are missing** – The document calls for hybrid policies (small objects in malloc, large in objmapper) yet defers eviction, telemetry, and CLI controls. Without parity with existing `storage.*` statistics and `varnishadm storage.list`, operators would lose visibility into cache residency and resource pressure.

## Opportunities / Suggested Refinements

- Prioritise the **protocol/director VMOD** track. Front-loading FD prefetch (objmapper → backend) delivers value without disturbing Varnish's storage internals. Use this as a proving ground for request routing, health checks, and observability before touching stevedores.

- Treat objmapper storage as an **augmenting fetch filter** instead of a stevedore replacement. For example, populate an objmapper FD during backend fetch, then hand Varnish the usual SML buffers. This preserves VDP compatibility while letting downstream consumers reuse the FD outside of varnishd.

- If a new stevedore is still desired, model it after `storage_file` and **focus on metadata reuse** (e.g., letting objmapper own the mmap backing file) rather than restructuring `obj_methods`. This aligns with the kernel's paging model and reduces the number of moving parts that must stay consistent across restarts.[1]

- Build **concrete compatibility matrices** (gzip, ESI, streaming, conditional requests) with acceptance criteria and fallback strategies, so stakeholders can weigh zero-copy gains against feature loss explicitly.

- Add **failure-mode playbooks** for director integration: timeouts, objmapper quorum loss, FD leaks, response truncation, and cache invalidation races. Borrow Varnish's existing `probe`, `connect_timeout`, and `max_retries` semantics to avoid surprising operators.

## References

1. Poul-Henning Kamp, *Notes from the Architect* – discussion of Varnish using virtual memory and mmapped storage to avoid redundant copies. <https://varnish-cache.org/docs/trunk/phk/notes.html>
2. `workspace_thread` runtime parameter description – highlights Varnish's reliance on `writev()`/IOV sequences in delivery. <https://varnish-cache.org/docs/trunk/reference/varnishd.html#workspace-thread>
3. `http_gzip_support` runtime parameter – documents default transformation features that consume and mutate object bodies before delivery. <https://varnish-cache.org/docs/trunk/reference/varnishd.html#http-gzip-support>
