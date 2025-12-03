# TeXpresso Headless Mode

## Overview

TeXpresso now supports a **headless mode** for running without a GUI. This is the foundation for the web-based TeXpresso implementation, where the TeX engine runs on a server and communicates with a web frontend.

## Usage

```bash
texpresso --headless [-json] document.tex
```

The `--headless` flag automatically enables JSON protocol mode for stdin/stdout communication.

## What Works

### ✅ Implemented Features

1. **Basic Initialization**
   - Skips SDL2/GUI initialization
   - Sets up TeX engine (Tectonic/XeTeX)
   - Performs initial compilation
   - Outputs JSON status messages

2. **Command Processing**
   - Accepts commands via stdin using JSON-encoded S-expressions
   - Processes `open`, `change`, and `close` commands
   - Triggers recompilation when files change

3. **JSON Output**
   - `{"type":"status","state":"ready"}` - Initial ready state
   - `{"type":"doc.pageCount","count":N}` - Document page count
   - `{"type":"error",...}` - Error messages

### Example Session

```bash
$ build/texpresso --headless test/simple.tex
{"type":"status","state":"ready"}
{"type":"doc.pageCount","count":1}

# Send commands (one per line):
["open","main.tex","\\documentclass{article}..."]
["change","main.tex",100,5,"text"]
^D
```

## Protocol

Commands follow the [EDITOR-PROTOCOL.md](EDITOR-PROTOCOL.md) specification. In JSON mode, S-expressions are represented as JSON arrays:

- `(open "path" "contents")` → `["open","path","contents"]`
- `(change "path" offset length "data")` → `["change","path",offset,length,"data"]`
- `(close "path")` → `["close","path"]`

## Testing

Run the test script to verify headless mode:

```bash
./scripts/test-headless.sh
./scripts/test-headless-commands.sh
```

## Implementation Status

### Phase 1: Core Infrastructure ✅ COMPLETE

- [x] Add `--headless` command-line flag
- [x] Skip SDL2 initialization in headless mode
- [x] Create JSON writer utility (`json_writer.c/h`)
- [x] Implement basic headless main loop
- [x] Process file operation commands (open, change, close)
- [x] Output JSON status messages

### Phase 2: Next Steps (TODO)

- [ ] **Rendering Output**: Implement command generator to output rendering commands instead of displaying via SDL
  - Create `cmd_generator.c/h` - fz_device that generates JSON drawing commands
  - Output page rendering as JSON command stream
  - Support fonts, text, paths, images

- [ ] **Status Updates**: Output compilation progress and errors as JSON
  - `{"type":"status","state":"compiling","progress":0.5}`
  - `{"type":"log","level":"error","file":"main.tex","line":42,"message":"..."}`

- [ ] **SyncTeX Support**: Implement forward/backward sync in headless mode
  - Process `synctex-forward` commands
  - Output `{"type":"synctex.result",...}` messages

## Architecture

```
┌─────────────────────────────────────┐
│  texpresso --headless document.tex │
│                                     │
│  ┌────────────┐  ┌──────────────┐  │
│  │  stdin     │  │   stdout     │  │
│  │  (JSON     │  │   (JSON      │  │
│  │  commands) │  │   messages)  │  │
│  └─────┬──────┘  └──────▲───────┘  │
│        │                │          │
│        ▼                │          │
│  ┌────────────────────────┐        │
│  │  Command Parser        │        │
│  │  (JSON S-expressions)  │        │
│  └──────┬─────────────────┘        │
│         │                          │
│         ▼                          │
│  ┌────────────────────────┐        │
│  │  TeX Engine            │        │
│  │  (Tectonic/XeTeX)      │        │
│  │  - VFS management      │        │
│  │  - Incremental compile │        │
│  └──────┬─────────────────┘        │
│         │                          │
│         ▼                          │
│  ┌────────────────────────┐        │
│  │  DVI Interpreter       │────────┘
│  │  (Future: cmd_generator)│
│  └────────────────────────┘
└─────────────────────────────────────┘
```

## Files Modified/Created

### Modified Files
- `src/driver.h` - Added `headless` field to `persistent_state`
- `src/driver.c` - Parse `--headless` flag, skip SDL initialization
- `src/main.c` - Added `texpresso_main_headless()` function
- `src/Makefile` - Added `json_writer.o` to build

### New Files
- `src/json_writer.h` - JSON serialization API
- `src/json_writer.c` - JSON serialization implementation
- `scripts/test-headless.sh` - Basic headless test
- `scripts/test-headless-commands.sh` - Command processing test
- `HEADLESS-MODE.md` - This documentation

## Future: Web Architecture

The headless mode is designed to be wrapped by a Node.js WebSocket server:

```
Browser ←→ WebSocket ←→ Node.js Server ←→ stdin/stdout ←→ texpresso --headless
```

This allows multiple users to compile LaTeX documents through a web interface, with the TeX engine running server-side in headless mode.

## Notes

- The headless mode currently outputs `{"type":"doc.pageCount","count":0}` due to bundle loading issues in some environments. This will be resolved with proper Tectonic bundle configuration.
- Rendering command generation (the most complex part) is not yet implemented. The current version focuses on the communication infrastructure.
- The headless implementation is designed to be non-intrusive - it doesn't modify the GUI code path and routes to a separate `texpresso_main_headless()` function.
