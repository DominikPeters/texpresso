# Headless editor preview (protocol 1)

`texpresso -capabilities` prints a JSON object and exits before initializing SDL
or opening a document. Current features are `previewProtocol: 1`, `codecs:
["png", "svg"]`, `synctex: true`, `regions: true`, `links: true`, `adaptiveRendering: true`, `pngPatches: true`, and `backgroundRendering: true`. Existing editor clients and the default native
window are unchanged. This protocol is enabled only with `-headless -json`.
Headless mode initializes SDL events/timers, never video/window/texture APIs.

All existing JSON input commands and output notifications remain available.
Headless input additions are positional JSON arrays:

* `["preview-barrier", sequence]`: acknowledge the source commands preceding
  it, including buffered edits. Sequence numbers are nonnegative integers.
* `["preview-render", requestId, page, pixelWidth, revision, sequence]`:
  request the zero-based page at the expected revision/source sequence. The
  newest pending request replaces an older one. Width is 1–4096; actual output
  is reduced to at most 4,000,000 pixels and 8192 pixels on either axis.
* `["preview-png", requestId, page, pixelWidth, revision, sequence, baseRasterId]`:
  opt into PNG deltas after checking `pngPatches`. Request IDs must be positive.
  Pass the last successfully displayed `rasterId`, or 0 to force a full frame.
  `preview-auto` also accepts an optional final `baseRasterId` argument when
  this capability is available. Legacy commands keep their full-frame behavior.
* `["preview-svg", requestId, page, pixelWidth, revision, sequence]`:
  export an outlined SVG, with the same request slot and source guards. Width
  is used only if export falls back to PNG; SVG geometry is independent of it.
  Check the `svg` codec capability before using this command.
* `["preview-auto", requestId, page, pixelWidth, revision, sequence, vectorLimit]`:
  choose one codec after the cheap display-list preflight. Check
  `adaptiveRendering` before using this command. `vectorLimit` is an integer
  from 0 to 200000; the client initially uses 16000, then adjusts it from measured
  SVG costs. Glyph count (weighted by 20) and packed path bytes form the score.
  Eligible pages within that limit use SVG; other pages use bounded PNG. Pages
  containing raster images, unsupported SVG graphics, or a score above 200000
  stay PNG. The response includes `vectorCost` and `svgEligible` so the client
  can request `preview-svg` later without recompiling the same snapshot.
  Ordinary PNG choices in Auto do not carry a `fallback` warning.
* `["preview-background", requestId, page, pixelWidth, revision, sequence, codec, vectorLimit]`:
  speculative full-page export after checking `backgroundRendering`. `codec` is
  0 for PNG, 1 for SVG, or 2 for Auto; `vectorLimit` has the same bounds as Auto.
  The frame never carries `patch` or `rasterId` and does not replace or release
  the native foreground PNG comparison raster, including when SVG is selected.
  It uses one temporary display list, without another retained raster. Foreground
  renderer contents remain available and `preview-state.currentPage` remains the
  foreground page. Completion restores foreground compiler demand automatically.
* `["preview-cancel", requestId, foregroundPage]`: cancel the matching background
  request and retarget compiler demand without exporting another image. Request
  ID 0 explicitly clears any pending export and retargets a cached foreground
  navigation. A nonzero ID cannot cancel an unrelated request. Source changes,
  normal render requests, forward/inverse SyncTeX, and destination lookups preempt
  speculation. Direct source/SyncTeX preemption emits `preview-retry` for the
  abandoned background request; explicit cancellation needs no response.
* `["preview-destination", revision, sequence, name]`: resolve a DVI named
  destination. Check `links` first. Compiles only until the destination's page
  is complete, without exporting intermediate pages. Stale requests are ignored.
* `["preview-region", requestId, page, pixelWidth, revision, sequence, x0, y0, x1, y1]`:
  render a page-space rectangle, clipped to the page. Uses the same image response
  and limits as `preview-render`; `bounds` describes the clipped rectangle.
  Allocates only its pixel region and clips display-list replay to that area.
  Clients must check the optional `regions` capability before using this command.
* `["preview-inverse", revision, sequence, page, x, y]`: run native SyncTeX
  against the current snapshot. Coordinates are MuPDF page-space points.
  Stale snapshots are ignored. The existing `synctex` notification returns
  the source location; a column of -1 means unavailable. Headless inverse lookup
  may append a fifth string field containing the clicked word, recovered from
  the display list. Clients may use this to refine line-only locations. Unknown
  glyph mappings or clicks away from text leave the field absent.

Rendering demand uses the existing compiler stepping and convergence loop.
Page count is provisional until the engine terminates its current pass. These
notifications describe availability and navigation:

* `["preview-state", revision, sequence, completedPageCount, complete, currentPage]`
* `["preview-invalidated", revision, sequence]`
* `["preview-forward", revision, sequence, page, x, y]`, responding to the
  existing `synctex-forward` command without moving a native window.
* `["preview-destination", revision, sequence, name, page]`, where `page` is
  zero-based or -1 when the current pass completed without that destination.
* `["preview-retry", requestId]` if the requested snapshot became obsolete.
* `["preview-error", requestId, message]` if a page cannot be exported.

The revision is deliberately conservative: edits, rescans, reruns, and detected
dependency changes invalidate the current display-list snapshot. Keep any old
image visible if desired, but do not use it for hit testing after invalidation.
One foreground display list is retained and reused for repeated zoom exports;
speculation may temporarily retain one additional display list until completion
or cancellation. Theme
commands recolor the PNG with the same mapping as the native renderer.

## Binary image framing

Every notification is a UTF-8 JSON line except an image response, which has a
JSON header line followed immediately by exactly `byteLength` image bytes:

```json
["preview-image",{"requestId":1,"revision":2,"sequence":1,"page":0,"bounds":[0,0,612,792],"width":1200,"height":1553,"mime":"image/png","byteLength":12345}]
```

The next JSON line starts immediately after the image body; there is no extra
delimiter. For PNG the integer width/height describe the actual raster; for SVG they
describe intrinsic page-point dimensions (not a render resolution). The MIME
is `image/png` or `image/svg+xml`.
The image covers `bounds` in page space, top-left origin, with an opaque page
background. The native frontend serializes header/body writes on its main
thread; stderr remains independent text. A receiver must parse arbitrary byte
chunks and must not attach a text decoder/readline reader to stdout in this
mode. Image bodies are limited to 16 MiB. A partial frame is a terminal protocol
error; searching image bytes for a subsequent newline cannot resynchronize it.

Receivers should allow only bounded outstanding image requests and always drain
stdout, discarding obsolete images when necessary. Source updates can arrive
while a previous export is being delivered: compare the frame's sequence with
the latest source sequence sent by the client. On process replacement, discard
all prior state and identify the new session outside this per-process protocol.

Full-page PNG/SVG and clipped-region PNGs share one replaceable request slot.
SVG uses outlined glyphs with within-page definition reuse, embedded image data,
and an explicit white page background. Clients should load it as an isolated
blob-backed image rather than injecting markup. Shared cross-page glyph stores
remain a future optimization.

Full-page headers may include `annotations: {links, destinations}`. A link is
`{bounds: [x0,y0,x1,y1], uri}`; `#name` denotes a DVI named destination. A
page-local destination is `{name, x, y}`. Coordinates use the same page space
as SyncTeX and do not depend on the image codec. Wrapped links have separate
text-run rectangles. Link actions come from Dvipdfmx `bann`/`eann` and `dest`
specials; image and rule bounds are collected too. There are at most 4096
entries of each kind per page, names/URIs are at most 4096 bytes, and metadata
has a 512 KiB budget. The incremental destination index retains at most
100,000 entries and 4 MiB of names; rollback removes superseded entries.
Clients must reject stale hits and apply their own policy to external URIs.

When SVG cannot preserve the page, the response is PNG with a short `fallback`
reason. The current policy rejects non-isolated/knockout/non-normal blend groups,
non-RGB/Gray group blending spaces, soft masks, image masks/decode arrays/color
keys or non-device RGB/Gray image color spaces, Type 3 text, overprint, shadings, and tiling patterns. It also falls back
for preview color remapping, excessive vector complexity or embedded image size,
failed export, or more than 16 MiB of SVG. Regions always use bounded PNGs.
A client-side SVG decode failure can retry the same snapshot with PNG.

## PNG patches

Opted-in full-page PNGs include `rasterId`. When the named base matches the
engine's retained raster, page, bounds and dimensions, headers may also include
`patch: {base, rect: [x, y, width, height]}`. The body is a PNG of that pixel
rectangle; header `width`, `height` and `bounds` still describe the complete page.
The rectangle replaces old pixels, including white pixels that erase old ink.
A zero rectangle `[0,0,0,0]` with `byteLength: 0` means unchanged pixels with
fresh revision/source/annotation metadata. Emit/consume this frame immediately;
it has no binary body. Other frames must have nonzero body length.

Differences are computed in RGB before compression. A rectangle covering more
than 40% of the raster sends a full PNG. A mismatched or unknown base also sends
a full PNG. Only one previous RGB raster is retained (at most 12,000,000 sample
bytes under the four-megapixel budget). Source invalidation deliberately keeps
it for the next comparison. Region renders do not replace it. A successful SVG
or legacy full-page render releases it, and session teardown frees it.

The client must acknowledge successful application before requesting dependent
patches. A skipped frame, canvas eviction or webview reload can use base 0 to
recover. The server never requires a client to retain a chain of patch PNGs.
Existing compiler/display-list rendering and region exports are unchanged.

## Local profiling

Set `TEXPRESSO_PROFILE=1` to emit `[profile] { ... }` JSON records on stderr.
Records report DVI display-list creation, annotations, SVG preflight/export,
PNG drawing/color/encoding, and pipe-write wall time in milliseconds. Export
records include the request ID, page and encoded bytes. These diagnostics are
disabled by default and do not add messages to stdout or change framing.

Opaque device-RGB preview pixmaps use a streaming, lossless PNG writer with
unfiltered scanlines and fast deflate. Scratch storage is one scanline, a 64 KiB
output block and zlib state, rather than whole-image compression buffers.
After checking every RGB pixel, neutral pages use 8-bit grayscale PNG; one colored pixel keeps RGB. Other pixmap formats retain MuPDF's general PNG writer. Rasterization, color
conversion, pixel limits and SVG fallback decisions are independent of encoding.
