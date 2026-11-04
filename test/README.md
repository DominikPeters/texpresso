[simple.tex](simple.tex) (`make simple`): sample .tex file to check that basic features are working as expected

[include.tex](include.tex) (`make include`): a .tex file to test include path support, it needs `-I incpath` to be passed to TeXpresso to build correctly.

`make test-editing-lifecycle` replays deterministic DVI output edits
(byte-by-byte arrival, rollback to every prefix, same-length compiler restarts)
under AddressSanitizer and UndefinedBehaviorSanitizer, comparing the incremental
index with a fresh one. Fuzzers live in [fuzz/](fuzz/README.md).

`make test-sprotocol` covers the frontend side of the worker channel when a
worker dies, discards a checkpoint descriptor, or sends malformed messages.

`make test-synctex` checks inverse-search geometry without running a compiler:
text-line height/depth, compressed vertical coordinates, nearby whitespace,
negative kern widths, nested boxes, invisible struts, and glyph-only boxes.
It uses the same MuPDF/compiler configuration as the frontend.

`make test-dvi-annotations` also checks preview figure-special geometry, source
metadata, malformed inputs and page reset. Live XeLaTeX workers install a small
built-in TikZ hook (ordinary CLI compilation is unchanged). Headless image
annotations can optionally contain `figures: [{bounds, file, line, endLine}]`;
`tikzFigures: true` advertises support. Bounds are page points, top-left origin,
with the renderer's real transformation applied. Source lines are one-based
**execution locations**, with `endLine` exclusive. TeX collects macro arguments
before execution: a multiline `scalebox` can report its closing-argument line for
both endpoints. Clients must resolve that location against a unique literal
source picture, not assume it is the picture's opening line. Repeated/ambiguous
macro expansions are not exact editable source ranges. Nested TikZ pictures
inside node labels are suppressed. Format namespace 1002 is required for the
hex-UTF8 `\texpressosourcefile` primitive; ordinary job names and source line
numbering remain intact.

`make test-preview-png` checks exact pixels with an independent system-zlib
decoder, chunk CRCs, large incompressible streams, grayscale/RGB detection,
resolution metadata and dirty-patch erasure. Run it with the default frontend
configuration and again with `-DTXP_ZLIB_NG`, the generated native zlib-ng
include path and `libz-ng.a` added to `Makefile.config`. That backend streams
standard PNG at level 2 and uses distinct `zng_` symbols so other zlib users
keep their system ABI. `-capabilities` reports the chosen backend and level.

`make test-parent-watch` builds a small fixture around the production parent
watcher. It verifies isolated-group enforcement and cleanup after frontend
crashes, stdin EOF, and a launcher crash. It creates only disposable processes
and does not require TeX Live or MuPDF.

Headless clients can opt into `-watch-parent` when `parentWatch` is advertised.
The launcher must create an isolated process group with the engine as its
leader (for example, `setsid` before exec). A small pre-initialization supervisor
keeps that PID/group alive while the actual frontend and checkpoint workers run.
If the launcher disappears or the frontend exits, the supervisor releases its
entire group, including itself. Consequently the supervised process reports a
signal exit; clients must already distinguish requested shutdown from failures.
Unwatched commands, including capability probes, retain their existing behavior.
