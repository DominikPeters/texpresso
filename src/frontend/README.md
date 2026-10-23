# TeXpresso binaries

The binary can be compiled in two ways:

1. `texpresso-dev` is a frontend that supports _hot-loading_ by dynamically linking `texpresso.so`.
After updating `texpresso.so`, send a USR1 to reload it (`killall -SIGUSR1 texpresso-dev`).

2. `texpresso` is a standalone version.

Produce them using `make texpresso` or `make texpresso-dev`.

## Important source files

### Entrypoints

[driver.c](driver.c) implements the entrypoint (the `main` function). It is the common
code between the hot-loaded and standalone version. [driver.h](driver.h) defines the
interface that should be implemented by the different versions.
`main` initializes the shared state and resources (mupdf and SDL2), and passes the
control to `texpresso_main`.

[loader.c](loader.c) implements the hot-loader, loading (and reloading) `texpresso.so`

[main.c](main.c) implements the main TeXpresso interface

### Engine

[engine.h](engine.h) defines the common interface between different engines. The engine
is a component that takes the input file provided to `texpresso` and turns it
into something that can be displayed:

- [engine_dvi.c](engine_dvi.c) renders a DVI and XDV files.
- [engine_pdf.c](engine_pdf.c) renders a PDF file (using MuPDF).
- [engine_tex.c](engine_tex.c) renders a .tex file, by turning it into an XDV stream using
  texpresso-xetex (TeXpresso flavor of XeTeX, forked from [tectonic](https://github.com/tectonic-typesetting/tectonic))

[dvi/](dvi/) is a generic interpreter for DVI format,with support for TeX
TFM, VF, enc, PDF graphic stream, etc.

[fs.c](fs.c) fakes a minimalist file-system. It keeps an in-memory copy of files
read from the real file system by LaTeX (to detect changes or to patch them) and
it stores files written by LaTeX.

[state.c](state.c), [state.h](state.h) tracks the state of running LaTeX processes (somewhat
like the unix "U structure", it keeps the list of opened file descriptors),
while supporting backtracking.

[incdvi.c](incdvi.c), [incdvi.h](incdvi.h) is an incremental viewer for DVI files, implemented
on top of <dvi/> library.

[renderer.c](renderer.c), [renderer.h](renderer.h) renders the contents of TeXpresso window,
with support for scrolling, cropping, remapping colors, etc.

### Misc files

[sexp_parser.c](sexp_parser.c), [sexp_parser.h](sexp_parser.h) is a simple S-expression parser, compatible
enough with Emacs.

[sprotocol.c](sprotocol.c), [sprotocol.h](sprotocol.h) is an implementation of the protocol used by
TeXpresso to communicate with TeXpresso-enabled LaTeX processes.

[myabort.c](myabort.c), [myabort.h](myabort.h) is an helper to print backtraces before aborting.

[proxy.c](proxy.c) is a small C tool (compiled using `make texpresso-debug-proxy`) to
proxy TeXpresso communication from the editor to an instance running through a
debugger (launched using <../scripts/texpresso-debug>).

[synctex.c](synctex.c), [synctex.h](synctex.h) is a quick'n'dirty SyncTeX parser (not used in
current version).

[logo.c](logo.c), [logo.h](logo.h) is the TeXpresso logo, represented as a [qoi.h](qoi.h) image
and serialized as a C string.

### Native activity attribution

`TEXPRESSO_PROFILE=1 TEXPRESSO_PROFILE_ACTIVITY=1` enables detailed wall/CPU
attribution on stderr. Leave the second flag unset for a same-binary stage-timing
control. No C02 wire fields change. `nativeAt`, `beginAt`, and `maxAt` use shared
`CLOCK_MONOTONIC` milliseconds; CPU deltas use the current process's
`CLOCK_PROCESS_CPUTIME_ID`. Wall minus CPU includes external work, I/O, parking
and descheduling; it is not a measurement of OS scheduling alone.

The worker's `work` interval ends at the next query, with that query's end token
count. `transaction` includes all request writes, buffered flushes, response tags
and response payload reads. Diagnostic mode makes the existing implicit stdio
write-to-read flush explicit, so request flushing and response reading can be
separated. One-way SEEN ends after its buffered write. `fork` measures the parent's
fork syscall; `park` includes CHLD delivery, waiting for the child and the DONE
resume response. Both child return and parent resume reset the work baseline.
Workers killed mid-interval cannot report their unfinished work; process sampling
is still useful for those intervals.

The frontend reports socket reads/writes, poll, complete query read, answer and
response flush. Its `peerPid` and per-worker `query` ordinal correlate with worker
`pid` and `query`; raw CHLD counts as a parent query and children restart at one.
A poll before a tag is available has an unknown tag. Query-read/answer contain
socket operations; worker transactions contain their I/O. These are nested
measurements: do not sum their durations or CPU totals. The main loop additionally
reports `input` (stdin parsing through end_changes/restart), SDL `idle`, and whole
`loop` intervals, which contain compiler/rendering/input/idle work. Loop peer PIDs
are sampled at the end, rather than assigning the whole iteration to one worker.

Each process/context aggregates fixed stage counters for 250 ms windows and emits
at most four individual spans of at least 10 ms per window. Fork/finish/shutdown
also flush summaries. Each JSON line uses a single write of fewer than 512 bytes.
Summary `query`, `tag`, `tokens`, `peerPid`, `maxAt`, and `cpuAtMaxMs` describe the
maximum-wall span; `maxCpuMs` is an independent CPU maximum. `dropped` includes
suppressed slow spans and failed/truncated records; it is repeated on summary
rows, so take a window's maximum rather than adding the rows. Detail mode adds
clock and output overhead; compare both flags on/off in the same binary before
interpreting latency changes.

`make test-activity` runs controlled real C02 exchanges without TeX or rendering:
CPU versus sleep, a delayed response payload, fork parking and resume, local query
numbering, default-off parity, and bounded-output accounting.
