# Headless editor preview (protocol 1)

`texpresso -capabilities` prints a JSON object and exits before initializing SDL
or opening a document. Current features are `previewProtocol: 1`, `codecs:
["png"]`, `synctex: true`, and `regions: true`. Existing editor clients and the default native
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
* `["preview-retry", requestId]` if the requested snapshot became obsolete.
* `["preview-error", requestId, message]` if a page cannot be exported.

The revision is deliberately conservative: edits, rescans, reruns, and detected
dependency changes invalidate the current display-list snapshot. Keep any old
image visible if desired, but do not use it for hit testing after invalidation.
One display list is retained and reused for repeated zoom exports. Theme
commands recolor the PNG with the same mapping as the native renderer.

## Binary image framing

Every notification is a UTF-8 JSON line except an image response, which has a
JSON header line followed immediately by exactly `byteLength` PNG bytes:

```json
["preview-image",{"requestId":1,"revision":2,"sequence":1,"page":0,"bounds":[0,0,612,792],"width":1200,"height":1553,"mime":"image/png","byteLength":12345}]
```

The next JSON line starts immediately after the PNG body; there is no extra
delimiter. The header's integer pixel width/height describe the actual raster.
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

Full-page and clipped-region PNGs share one replaceable request slot. SVG,
hyperlink annotations, and shared glyph resources remain future extensions.
