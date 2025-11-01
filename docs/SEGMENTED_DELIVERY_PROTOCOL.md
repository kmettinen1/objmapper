# Segmented Delivery Protocol Extension

## Purpose

Define a backward-compatible extension to the objmapper wire protocol that allows a single response body to be streamed as an ordered sequence of heterogeneous segments. A segment can either copy inline bytes, splice from an on-disk file descriptor, or pass a descriptor for zero-copy delivery. The extension targets reuse by multiple front-ends (objmapper, Varnish, H2O) without divergent dialects.

## Design Goals

- Preserve existing OBJM protocol framing and message types.
- Negotiate support explicitly so legacy peers fall back to copy/FD modes.
- Allow responses to interleave small copied prefixes (headers) and large zero-copy ranges.
- Keep per-segment metadata fixed-size to simplify preallocation and validation.
- Permit future segment classes (e.g., cache hints) without redesigning the frame.

## Capability Negotiation

- Introduce capability flag `OBJM_CAP_SEGMENTED_DELIVERY` (bit 4 / `0x0010`).
- Introduce request mode `OBJM_MODE_SEGMENTED` (ASCII `'4'`).
- Clients advertise the capability during the V2 hello. Servers reply only if they can parse segmented responses.
- The negotiated capability governs both request and response processing. If either side lacks the flag the connection acts as today.
- A segmented request that reaches a server without negotiated support MUST elicit `OBJM_STATUS_INVALID_MODE`.

## Request Semantics

Clients issue requests exactly as in V2, changing only the `mode` byte to `OBJM_MODE_SEGMENTED` when they expect a segmented response. The request flags retain their current meaning.

Servers MAY choose segmented delivery even when the client sends copy/splice modes by enabling automatic fallback. For deterministic behaviour front-ends SHOULD set a `OBJM_REQ_ORDERED` flag if they need linear delivery and avoid segmented responses.

## Response Wire Format

Segmented responses reuse the existing V2 response envelope and add a segment table ahead of payload bytes.

```
Field                         Size   Notes
-----                         ----   -----
Message Type                  1      0x02 (RESPONSE)
Request ID                    4      Network byte order
Status                        1      0x00 for success
Segment Count                 2      Network byte order (0 disallowed)
Metadata Length               2      Network byte order
Metadata                      N      TLV as today (optional)
Segments[]                    M      `segment_count * sizeof(segment header)`
Inline Data Blocks            K      Concatenated copy segments
Ancillary Data (SCM_RIGHTS)   -      One FD per segment of type FD
```

### Segment Header

Each segment header is 32 bytes, aligned to simplify parsing:

```
Offset  Size  Field            Description
0       1     type             0=inline, 1=fd, 2=splice, 3=reserved
1       1     flags            Bitmask defined below
2       2     reserved         Must be zero; reserved for future use
4       4     copy_length      Inline data length in bytes (type=inline only)
8       8     logical_length   Bytes contributed to the response body
16      8     storage_offset   Start offset within the file descriptor
24      8     storage_length   Bytes readable from the descriptor
```

Field rules:
- Inline segments MUST set `copy_length == logical_length` and write `copy_length` bytes into the inline data area after the segment table.
- FD segments MUST attach one descriptor via SCM_RIGHTS in the same order they appear in the segment array. `logical_length` defines the final body size, `storage_offset`/`storage_length` describe the range of bytes accessible from the FD. `storage_length` MAY equal `logical_length`; if larger the receiver SHALL only read `logical_length` bytes starting at `storage_offset`.
- Splice segments reuse the descriptor table but indicate the sender will use `splice(2)` or `sendfile(2)` server-side. Receivers treat them like FD segments.

### Segment Flags

```
#define OBJM_SEG_FLAG_FIN      0x01  /* Segment is the final logical chunk */
#define OBJM_SEG_FLAG_REUSE_FD 0x02  /* Reuses most recently announced FD */
#define OBJM_SEG_FLAG_OPTIONAL 0x04  /* Receiver MAY drop segment on error */
```

`OBJM_SEG_FLAG_FIN` MUST be set on the last segment. All other segments clear it.

`OBJM_SEG_FLAG_REUSE_FD` indicates that no new SCM_RIGHTS message will be sent for the segment. Instead the consumer reuses the previous FD of type FD or splice. Inline segments MUST NOT set this flag.

`OBJM_SEG_FLAG_OPTIONAL` is intended for fallbacks (e.g., small preamble). Receivers dropping optional segments MUST still honor overall ordering.

### Inline Data Area

After all segment headers, the sender writes inline data blocks for segments with `type == 0`. Blocks appear in segment order, each exactly `copy_length` bytes. No additional length prefix is needed.

### File Descriptors

When the sender emits a segment with `type 1` (FD) and `OBJM_SEG_FLAG_REUSE_FD` unset it MUST attach exactly one descriptor through `SCM_RIGHTS`. Receivers track descriptors in arrival order and reference them when parsing segments. FDs are valid only for the duration of the response; the receiver closes them after use unless the application takes ownership.

## Metadata Interactions

- `OBJM_META_PAYLOAD` remains the authoritative manifest for variant selection.
- Introduce metadata type `OBJM_META_SEGMENT_HINTS (0x08)` containing an encoded array of `objm_segment_descriptor_t` suitable for proactive caching.
- Servers MAY omit the hints to minimize response headers; clients rely on the structured segment table within the frame.

## Ordering Guarantees

Segments appear strictly in transmission order and MUST be concatenated to form the HTTP body. Servers MAY pipeline future requests even while streaming segments for earlier responses provided they respect the per-request ordering rules.

`OBJM_REQ_ORDERED` still forces the server to serialize segment emission for that request relative to previous ordered requests.

## Error Handling

- Malformed segment tables (`segment_count == 0`, unexpected inline length, unknown types) MUST terminate the connection with `OBJM_STATUS_PROTOCOL_ERROR`.
- Receivers encountering `OBJM_SEG_FLAG_OPTIONAL` segments they cannot honor SHOULD log diagnostic metadata and continue.
- If a sender detects that the receiver lacks `OBJM_CAP_SEGMENTED_DELIVERY` it MUST revert to existing FD or copy responses.

## Implementation Notes

- The parser can allocate a vector sized by `segment_count` and reuse it for both metadata hints and live responses.
- Client libraries should surface segments to callers that can act on zero-copy ranges (e.g., hand the FD to sendfile) while also providing a simple fallback path that stitches the segments into a contiguous buffer.
- Servers that already synthesize HTTP headers can expose them as the first inline segment, followed by one or more FD ranges for the body.
- To limit memory pressure recommended `segment_count` maximum is 64; exceeding this should trigger `OBJM_STATUS_INVALID_REQUEST`.

## Testing Strategy

1. Extend `test_protocol.sh` to cover segmented negotiation (positive/negative).
2. Add unit tests for parsing tables, inline data handling, and FD reuse semantics.
3. Provide interop tests with Varnish/H2O harnesses once shared library APIs expose the segment iterator.

## Future Work

- Define additional segment types for checksum trailers, trailer metadata, or async completion notices.
- Consider compression flags per segment to pave way for mixed-encoding payloads.
- Explore transport hints so receivers can prefetch file ranges based on upcoming segments.
