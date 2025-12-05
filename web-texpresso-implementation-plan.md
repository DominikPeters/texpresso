# Web-Based TeXpresso Implementation Plan

## Executive Summary

This document provides a comprehensive implementation plan for building a web-based version of TeXpresso, a live LaTeX compilation and preview system. The architecture consists of a **Node.js WebSocket server** that manages browser connections and spawns **headless TeXpresso processes** (one per session), communicating via stdin/stdout. The frontend contains an editor and viewer based on mupdf.js.

---

## 0. Motivation and Goals

### 0.1 What We're Building

The goal is to create a **web-based live LaTeX editing and preview system** that brings TeXpresso's instant-feedback compilation to the browser. Users should be able to:

1. **Edit LaTeX documents** in a browser-based code editor with syntax highlighting
2. **See live preview updates** as they type, with sub-second latency for typical edits
3. **Navigate bidirectionally** between source and preview (SyncTeX support)
4. **Access from anywhere** without installing TeX distributions locally

### 0.2 Why a Web Version?

The native TeXpresso has significant advantages but also limitations:

| Native TeXpresso | Web TeXpresso |
|------------------|---------------|
| Requires local installation of Tectonic/XeTeX | No local installation needed |
| Native performance | Slightly higher latency (network) |
| Works offline | Requires server connection |
| Limited to desktop platforms | Works on any device with a browser |
| Single-user | Multi-user potential (collaboration) |

A web version democratizes access to live LaTeX compilation, making it available to:
- Students without admin rights to install software
- Users on Chromebooks, tablets, or restricted systems
- Teams wanting a shared LaTeX environment
- Educators providing a consistent experience for all students

### 0.3 Design Principles

1. **Preserve TeXpresso's Core Value**: Sub-second feedback loop from edit to preview
2. **Leverage Existing Code**: Reuse DVI interpreter, resource management, and engine coordination
3. **Efficient Communication**: Only send what's changed (incremental updates)
4. **Browser-Native Rendering**: Use mupdf.js for high-quality, zoomable output
5. **Stateless Frontend**: Server maintains compilation state; frontend is a thin renderer
6. **Clean Separation**: Node.js handles web concerns; TeXpresso stays focused on TeX

### 0.4 Expected User Experience

```
┌──────────────────────────────────────────────────────────────────┐
│  Web Browser                                                      │
│  ┌─────────────────────────────┬────────────────────────────────┐│
│  │  LaTeX Editor               │  PDF Preview                   ││
│  │                             │                                ││
│  │  \documentclass{article}    │  ┌────────────────────────┐   ││
│  │  \begin{document}           │  │                        │   ││
│  │  Hello, World!█             │  │    Hello, World!       │   ││
│  │  \end{document}             │  │                        │   ││
│  │                             │  └────────────────────────┘   ││
│  │                             │                                ││
│  │  [Ctrl+Click: Jump to PDF]  │  [Click: Jump to Source]      ││
│  └─────────────────────────────┴────────────────────────────────┘│
│  Status: Ready | Page 1/1 | Compiled in 45ms                     │
└──────────────────────────────────────────────────────────────────┘
```

**Key interactions:**
- Type in editor → Preview updates within ~100-200ms
- Click in preview → Editor jumps to corresponding source line
- Ctrl+click in editor → Preview scrolls to that content
- Zoom/pan in preview → Smooth, high-quality rendering at any scale

---

## 1. Architecture Overview

### 1.1 Current TeXpresso Architecture

TeXpresso's native architecture consists of:

```
┌─────────────────────────────────────────────────────────────────────┐
│                          TeXpresso Server                            │
│  ┌──────────┐  ┌───────────────┐  ┌──────────────┐  ┌────────────┐  │
│  │  Editor  │  │  File System  │  │ DVI Renderer │  │   MuPDF    │  │
│  │ Protocol │  │   (VFS)       │  │ (Incremental)│  │  Display   │  │
│  └────┬─────┘  └───────┬───────┘  └──────┬───────┘  └──────┬─────┘  │
│       │                │                 │                 │        │
│       ▼                ▼                 │                 ▼        │
│  ┌─────────────────────────────┐         │          ┌────────────┐  │
│  │      State Management       │◄────────┘          │   SDL2     │  │
│  │   (Backtracking/Forking)    │                    │  Renderer  │  │
│  └─────────────┬───────────────┘                    └────────────┘  │
│                │                                                     │
└────────────────┼─────────────────────────────────────────────────────┘
                 │
      ┌──────────▼──────────┐
      │  Tectonic/XeTeX     │
      │  (TeX Engine)       │
      │  - Produces XDV     │
      │  - Uses Server      │
      │    Protocol         │
      └─────────────────────┘
```

### 1.2 Proposed Web Architecture

The web version uses a **Node.js wrapper server** that spawns headless TeXpresso processes and communicates via stdin/stdout:

```
┌─────────────────────────────────────────────────────────────────────┐
│                           FRONTEND (Browser)                         │
│  ┌────────────────┐  ┌─────────────────┐  ┌──────────────────────┐  │
│  │   Code Editor  │  │  WebSocket      │  │   mupdf.js Viewer    │  │
│  │   (Monaco/CM)  │  │  Client         │  │   - Render commands  │  │
│  │   - Edit events│  │  - Send edits   │  │   - Font rendering   │  │
│  │   - Sync cursor│  │  - Receive draw │  │   - Text selection   │  │
│  └────────┬───────┘  └────────┬────────┘  └──────────┬───────────┘  │
│           │                   │                      │              │
│           └───────────────────┼──────────────────────┘              │
│                               │                                      │
└───────────────────────────────┼──────────────────────────────────────┘
                                │ WebSocket
                                │
┌───────────────────────────────┼──────────────────────────────────────┐
│                               │          BACKEND (Server)            │
│  ┌────────────────────────────▼────────────────────────────────────┐ │
│  │                    Node.js WebSocket Server                      │ │
│  │    - Session management  - JSON protocol translation            │ │
│  │    - Connection handling - Auth/rate limiting                   │ │
│  └────────────────────────────┬────────────────────────────────────┘ │
│                               │ stdin/stdout (JSON lines)            │
│                               │                                      │
│  ┌────────────────────────────▼────────────────────────────────────┐ │
│  │              TeXpresso Process (one per session)                 │ │
│  │                        --headless mode                           │ │
│  │  ┌──────────┐  ┌────────────────┐  ┌────────────────────────┐   │ │
│  │  │   VFS    │  │ TeX Engine Mgr │  │   DVI Interpreter      │   │ │
│  │  │          │  │ (Tectonic)     │  │   - Parse XDV          │   │ │
│  │  └──────────┘  └────────────────┘  │   - Generate Commands  │   │ │
│  │                                    └────────────────────────┘   │ │
│  └─────────────────────────────────────────────────────────────────┘ │
│                                                                      │
│  ┌─────────────────────────────────────────────────────────────────┐ │
│  │                    Resource Manager (shared)                     │ │
│  │    - Font cache (TFM, VF, OTF/TTF)                              │ │
│  │    - Encoding files                                              │ │
│  │    - Graphics cache                                              │ │
│  └─────────────────────────────────────────────────────────────────┘ │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

### 1.3 Why Node.js Wrapper + Headless TeXpresso?

Implementing WebSockets directly in C would require:
- Manual memory management around message framing
- Callback-heavy async patterns awkward in C
- SSL/TLS setup complexity
- More surface area for security bugs

The wrapper approach provides:

| Benefit | Description |
|---------|-------------|
| **Simpler WebSocket handling** | Node.js handles connections in ~50 lines vs. hundreds in C |
| **Clean separation** | Node.js handles web concerns; TeXpresso stays focused on TeX |
| **Easier debugging** | JSON over stdin/stdout is human-readable and easy to test |
| **Language consistency** | Frontend already uses JavaScript/TypeScript |
| **Negligible overhead** | ~0.1ms IPC latency is invisible compared to network latency |

### 1.4 Source Code Reference

This section maps key functionality in the current TeXpresso codebase to help the implementation team understand where to find relevant code.

#### Core Application (`src/`)

| File | Purpose | Key for Web Version |
|------|---------|---------------------|
| **`main.c`** | Main event loop, SDL2 window management, user input handling, coordinates all subsystems | Replace SDL2/UI parts with stdin/stdout JSON protocol; reuse engine coordination logic |
| **`engine.h`** | Abstract engine interface (step, render, changes) | Keep interface; implementations will output JSON instead of SDL |
| **`engine_tex.c`** | TeX engine implementation: process forking, VFS queries, backtracking state management | Core logic to reuse; ~1400 lines of critical state management |
| **`renderer.c`** | SDL2 texture rendering, zoom/pan, text selection | Replace entirely with command generation for mupdf.js |
| **`renderer.h`** | Renderer configuration (zoom, fit mode, colors) | Adapt config structures for JSON protocol |

#### Editor Protocol (`src/`)

| File | Purpose | Key for Web Version |
|------|---------|---------------------|
| **`editor.c`** | Parses editor commands (open, change, close), sends messages back | Adapt to JSON; core parsing logic reusable |
| **`editor.h`** | Editor command/message structures | Map directly to JSON message types |
| **`prot_parser.c`** | S-expression/JSON protocol parser | Replace with JSON-only parser for stdin |

#### Server Protocol (TeX ↔ TeXpresso)

| File | Purpose | Key for Web Version |
|------|---------|---------------------|
| **`sprotocol.c`** | Server↔TeX client communication (OPEN, READ, WRIT, etc.) | Keep unchanged; this is internal to backend |
| **`sprotocol.h`** | Protocol message definitions | Internal protocol; not exposed to web frontend |

#### DVI/XDV Rendering (`src/dvi/`)

| File | Purpose | Key for Web Version |
|------|---------|---------------------|
| **`mydvi.h`** | Core DVI structures: fonts, state, context | Essential; defines all rendering state |
| **`mydvi_opcodes.h`** | DVI/XDV opcode definitions (SET_CHAR, BOP, etc.) | Reference for understanding DVI stream |
| **`mydvi_interp.h`** | DVI interpreter interface | Interface to reuse |
| **`dvi_interp.c`** | DVI bytecode interpreter | Core logic; outputs to `fz_device` |
| **`dvi_prim.c`** | DVI primitive operations (char, rule, push/pop) | **Critical**: calls `fz_fill_text`, `fz_fill_path`, etc. |
| **`dvi_special.c`** (generated) | Handles DVI specials (colors, transforms, graphics) | **Critical**: ~5500 lines handling all special commands |
| **`dvi_special.re2c.c`** | Source for special parsing (re2c grammar) | Readable source; generates `dvi_special.c` |
| **`dvi_resmanager.c`** | Font and resource loading/caching | **Critical**: font discovery, format conversion |
| **`dvi_fonttable.c`** | Font table management | Font ID → font data mapping |
| **`dvi_context.c`** | DVI rendering context setup | Graphics state initialization |

#### Font Support (`src/dvi/`)

| File | Purpose | Key for Web Version |
|------|---------|---------------------|
| **`tex_tfm.c`** | TeX Font Metrics parsing | Character metrics for spacing |
| **`tex_vf.c`** | Virtual Font parsing | Composite character definitions |
| **`tex_fontmap.c`** | Font mapping (name → file) | Font discovery |
| **`tex_enc.c`** | Encoding file parsing | Character code → glyph name |

#### State Management (`src/`)

| File | Purpose | Key for Web Version |
|------|---------|---------------------|
| **`state.c`** | Filesystem state, file entries | VFS implementation |
| **`state.h`** | State structures (fileentry_t, mark_t) | Per-session state |
| **`incdvi.c`** | Incremental DVI parsing and page tracking | **Critical**: tracks page boundaries in DVI stream |
| **`incdvi.h`** | Incremental DVI interface | Page count, rendering entry point |

#### Other (`src/`)

| File | Purpose | Key for Web Version |
|------|---------|---------------------|
| **`synctex.c`** | SyncTeX parsing and lookup | Forward/backward sync |
| **`driver.c`** | Persistent state across binary reloads | May not be needed for web |
| **`vstack.c`** (`src/dvi/`) | Value stack for PDF operator parsing | Used by special handling |

#### Protocol Documentation

| File | Purpose |
|------|---------|
| **`EDITOR-PROTOCOL.md`** | Editor↔TeXpresso protocol specification |
| **`SERVER-PROTOCOL.md`** | TeXpresso↔TeX engine protocol specification |

### 1.5 Backend Implementation Strategy

This section describes the two-component backend: the Node.js WebSocket server and the headless TeXpresso process.

#### 1.5.1 Process Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         Node.js Server Process                           │
│                                                                          │
│  ┌────────────────────────────────────────────────────────────────────┐ │
│  │                    WebSocket Server (ws or uWebSockets.js)          │ │
│  │                                                                    │ │
│  │  - Accept connections                                              │ │
│  │  - Parse/validate JSON messages                                    │ │
│  │  - Route to appropriate TeXpresso process                          │ │
│  │  - Handle auth, rate limiting, connection lifecycle                │ │
│  └─────────────────────────────┬──────────────────────────────────────┘ │
│                                │                                        │
│  ┌─────────────────────────────▼──────────────────────────────────────┐ │
│  │                    Session Manager                                  │ │
│  │                                                                    │ │
│  │  Map<sessionId, {                                                  │ │
│  │    websocket: WebSocket,                                           │ │
│  │    process: ChildProcess,  // texpresso --headless                 │ │
│  │    stdin: Writable,                                                │ │
│  │    stdout: Readable                                                │ │
│  │  }>                                                                │ │
│  └────────────────────────────────────────────────────────────────────┘ │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
         │                    │                    │
         │ stdin/stdout       │ stdin/stdout       │ stdin/stdout
         ▼                    ▼                    ▼
┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
│ texpresso       │  │ texpresso       │  │ texpresso       │
│ --headless      │  │ --headless      │  │ --headless      │
│ (session 1)     │  │ (session 2)     │  │ (session 3)     │
└─────────────────┘  └─────────────────┘  └─────────────────┘
```

#### 1.5.2 Node.js Server Implementation

```typescript
// server.ts - Node.js WebSocket server

import { WebSocketServer, WebSocket } from 'ws';
import { spawn, ChildProcess } from 'child_process';
import * as readline from 'readline';

interface Session {
  ws: WebSocket;
  process: ChildProcess;
  rl: readline.Interface;  // For reading stdout lines
}

const sessions = new Map<string, Session>();

const wss = new WebSocketServer({ port: 8080 });

wss.on('connection', (ws: WebSocket) => {
  const sessionId = crypto.randomUUID();
  
  // Spawn headless TeXpresso process
  const proc = spawn('texpresso', ['--headless'], {
    stdio: ['pipe', 'pipe', 'pipe']
  });
  
  // Read JSON lines from stdout
  const rl = readline.createInterface({ input: proc.stdout });
  rl.on('line', (line: string) => {
    // Forward to WebSocket client
    ws.send(line);
  });
  
  // Handle stderr (for debugging/logging)
  proc.stderr.on('data', (data: Buffer) => {
    console.error(`[${sessionId}] ${data.toString()}`);
  });
  
  // Handle process exit
  proc.on('exit', (code: number) => {
    console.log(`[${sessionId}] TeXpresso exited with code ${code}`);
    sessions.delete(sessionId);
    ws.close();
  });
  
  sessions.set(sessionId, { ws, process: proc, rl });
  
  // Forward WebSocket messages to stdin
  ws.on('message', (data: Buffer) => {
    const msg = data.toString();
    proc.stdin.write(msg + '\n');
  });
  
  ws.on('close', () => {
    const session = sessions.get(sessionId);
    if (session) {
      session.process.kill();
      sessions.delete(sessionId);
    }
  });
});

console.log('WebSocket server running on ws://localhost:8080');
```

#### 1.5.3 TeXpresso Headless Mode

Add a `--headless` mode to TeXpresso that:

1. **Skips SDL2 initialization** — no window, no renderer
2. **Reads JSON commands from stdin** — one JSON object per line
3. **Writes JSON events to stdout** — rendering commands, status updates, errors
4. **Uses a command-generating fz_device** — outputs JSON instead of drawing

```c
// Conceptual changes to main.c for --headless mode

int main(int argc, char *argv[]) {
    bool headless = has_arg(argc, argv, "--headless");
    
    fz_context *ctx = fz_new_context(...);
    
    if (!headless) {
        // Normal SDL2 initialization
        SDL_Init(...);
        SDL_CreateWindow(...);
        txp_renderer *renderer = txp_renderer_new(ctx, sdl_renderer);
    }
    
    txp_engine *eng = txp_create_tex_engine(ctx, ...);
    
    // Command generator for headless mode
    cmd_generator *cg = headless ? cmd_generator_new(ctx) : NULL;
    
    while (running) {
        if (headless) {
            // Read JSON from stdin (non-blocking)
            if (poll_stdin()) {
                char *line = read_line(stdin);
                json_value *cmd = json_parse(line);
                handle_json_command(eng, cmd);
                free(line);
            }
        } else {
            // Normal SDL event handling
            SDL_PollEvent(&event);
            handle_ui_event(&event);
        }
        
        // Step TeX engine (same for both modes)
        send(step, eng, ctx, restart_if_needed);
        
        // Check for document changes
        send(begin_changes, eng, ctx);
        send(detect_changes, eng, ctx);
        if (send(end_changes, eng, ctx)) {
            if (headless) {
                // Generate and output JSON commands
                fz_device *dev = cmd_generator_device(cg);
                fz_display_list *dl = send(render_page, eng, ctx, page);
                fz_run_display_list(ctx, dl, dev, fz_identity, fz_infinite_rect, NULL);
                
                fz_buffer *json = cmd_generator_get_commands(cg);
                fz_write_printf(ctx, fz_stdout(ctx), "%s\n", fz_string_from_buffer(ctx, json));
                fz_flush_output(ctx, fz_stdout(ctx));
                cmd_generator_reset(cg);
            } else {
                // Normal SDL rendering
                txp_renderer_set_contents(ctx, renderer, dl);
                render(ctx, &ui);
            }
        }
    }
}
```

#### 1.5.4 stdin/stdout JSON Protocol

Communication between Node.js and TeXpresso uses **newline-delimited JSON (NDJSON)**:

**Node.js → TeXpresso (stdin):**
```json
{"type":"init","document":{"name":"main.tex","content":"\\documentclass{article}..."}}
{"type":"file.change","path":"main.tex","changes":[{"offset":100,"length":5,"text":"hello"}]} // but actually we should use the change-range command
{"type":"synctex.forward","path":"main.tex","line":42}
```

**TeXpresso → Node.js (stdout):**
```json
{"type":"status","state":"compiling"}
{"type":"render.page","page":1,"commands":[...]}
{"type":"synctex.result","direction":"forward","locations":[...]}
{"type":"log","level":"error","file":"main.tex","line":42,"message":"Undefined control sequence"}
```

#### 1.5.5 Command Generator Design

The key new component is the **Command Generator**, which replaces direct rendering with command capture:

```c
// cmd_generator.c - New file

struct cmd_generator {
    fz_context *ctx;
    fz_buffer *commands;      // JSON buffer being built
    json_writer *writer;      // JSON serialization helper
    
    // State tracking
    int command_count;
    font_cache *fonts;        // Track which fonts have been sent
    image_cache *images;      // Track which images have been sent
};

// Custom fz_device implementation
static void cmd_fill_text(fz_context *ctx, fz_device *dev, 
                          const fz_text *text, fz_matrix ctm,
                          fz_colorspace *cs, const float *color, float alpha) {
    cmd_generator *cg = dev->user;
    
    // Extract glyphs from fz_text
    json_start_object(cg->writer);
    json_write_string(cg->writer, "cmd", "text");
    json_write_int(cg->writer, "font", get_font_id(cg, text->font));
    json_write_matrix(cg->writer, "matrix", ctm);
    
    json_start_array(cg->writer, "glyphs");
    for (fz_text_span *span = text->head; span; span = span->next) {
        for (int i = 0; i < span->len; i++) {
            json_start_object(cg->writer);
            json_write_int(cg->writer, "gid", span->items[i].gid);
            json_write_float(cg->writer, "x", span->items[i].x);
            json_write_float(cg->writer, "y", span->items[i].y);
            json_end_object(cg->writer);
        }
    }
    json_end_array(cg->writer);
    
    json_write_color(cg->writer, "color", cs, color);
    json_end_object(cg->writer);
    
    cg->command_count++;
}

// Similar implementations for:
// - cmd_fill_path()
// - cmd_stroke_path()
// - cmd_fill_image()
// - cmd_fill_shade()
// - cmd_clip_path()
// - cmd_begin_group() / cmd_end_group()

fz_device *cmd_generator_new_device(fz_context *ctx, cmd_generator *cg) {
    fz_device *dev = fz_new_device(ctx, sizeof(cmd_generator));
    dev->user = cg;
    dev->fill_text = cmd_fill_text;
    dev->fill_path = cmd_fill_path;
    // ... set other callbacks
    return dev;
}
```

#### 1.5.6 Reusable Code from Current Implementation

| Component | Files | Reusability |
|-----------|-------|-------------|
| TeX engine management | `engine_tex.c` | 90% - core logic unchanged |
| Server protocol | `sprotocol.c/h` | 100% - internal to TeXpresso |
| DVI interpretation | `src/dvi/*.c` | 95% - change output target only |
| VFS/State | `state.c/h` | 100% - unchanged |
| SyncTeX | `synctex.c/h` | 100% - unchanged |
| Editor parsing | `editor.c` | 70% - adapt to JSON |
| Renderer | `renderer.c` | 0% - replace entirely with cmd_generator |
| Main loop | `main.c` | 50% - refactor for headless mode |

---

## 2. Communication Protocols

### 2.1 Frontend ↔ Backend WebSocket Protocol

All messages are JSON-encoded. The protocol supports both request/response patterns and server-pushed updates. The Node.js server passes messages through to/from TeXpresso with minimal transformation.

#### 2.1.1 Client → Server Messages

##### Session Management
```json
// Initialize session
{
  "type": "init",
  "sessionId": "uuid-v4",
  "document": {
    "name": "main.tex",
    "content": "\\documentclass{article}..."
  }
}

// Close session
{
  "type": "close",
  "sessionId": "uuid-v4"
}
```

##### File Operations (VFS)
```json
// Open/create file in VFS
{
  "type": "file.open",
  "path": "chapter1.tex",
  "content": "..."
}

// Update file content (delta)
{
  "type": "file.change",
  "path": "main.tex",
  "changes": [
    {
      "offset": 100,      // byte offset in UTF-8 // but we should use the change-range command
      "length": 5,        // bytes to remove
      "text": "hello"     // replacement text
    }
  ]
}

// Alternative: line-based changes
{
  "type": "file.changeLine",
  "path": "main.tex",
  "startLine": 10,
  "endLine": 12,
  "text": "new line content\nanother line"
}

// Close file
{
  "type": "file.close",
  "path": "chapter1.tex"
}
```

##### Navigation Commands
```json
// Go to page
{
  "type": "nav.goto",
  "page": 5
}

// SyncTeX forward (source → preview)
{
  "type": "synctex.forward",
  "path": "main.tex",
  "line": 42,
  "column": 0
}

// SyncTeX backward (preview → source)
{
  "type": "synctex.backward",
  "x": 150.5,
  "y": 300.2,
  "page": 1
}
```

##### Configuration
```json
// Set theme colors
{
  "type": "config.theme",
  "background": [1.0, 1.0, 1.0],  // RGB floats 0-1
  "foreground": [0.0, 0.0, 0.0]
}

// Request full render
{
  "type": "render.request",
  "page": 1,
  "scale": 1.5
}
```

#### 2.1.2 Server → Client Messages

##### Status Messages
```json
// Compilation status
{
  "type": "status",
  "state": "compiling" | "ready" | "error",
  "progress": 0.5  // optional, 0-1
}

// Error/warning log
{
  "type": "log",
  "level": "error" | "warning" | "info",
  "file": "main.tex",
  "line": 42,
  "message": "Undefined control sequence"
}
```

##### Document Metadata
```json
// Page count updated
{
  "type": "doc.pageCount",
  "count": 15
}

// Page dimensions
{
  "type": "doc.pageDim",
  "page": 1,
  "width": 612.0,   // points
  "height": 792.0,
  "landscape": false
}

// Files used by document
{
  "type": "doc.inputFile",
  "index": 3,
  "path": "figures/fig1.pdf"
}
```

##### Rendering Commands (See Section 3)
```json
{
  "type": "render.page",
  "page": 1,
  "commands": [ /* drawing commands */ ]
}

// Incremental update
{
  "type": "render.update",
  "page": 1,
  "invalidateFrom": 150,  // command index
  "commands": [ /* new commands */ ]
}
```

##### SyncTeX Response
```json
{
  "type": "synctex.result",
  "direction": "forward" | "backward",
  "locations": [
    {
      "file": "main.tex",
      "line": 42,
      "column": 0,
      "x": 150.5,
      "y": 300.2,
      "page": 1
    }
  ]
}
```

##### Log Output
```json
// TeX output streams
{
  "type": "output.append",
  "stream": "stdout" | "log",
  "text": "..."
}

{
  "type": "output.truncate",
  "stream": "stdout" | "log",
  "size": 1000  // keep first N bytes
}
```

---

## 3. Rendering Command Protocol

This is the core of the system—translating DVI/XDV rendering into commands that mupdf.js can execute.

### 3.1 Design Philosophy

Rather than sending raw XDV bytes or pre-rasterized images, we send high-level drawing commands that:
1. Map directly to mupdf.js canvas/device operations
2. Support efficient incremental updates
3. Enable text selection and search
4. Allow zooming without quality loss

### 3.2 Command Types

#### 3.2.1 Font Commands

```json
// Define a font for later use
{
  "cmd": "fontDef",
  "id": 1,
  "type": "tex" | "xdv",
  "name": "cmr10",
  
  // For TeX fonts
  "tfmChecksum": 123456789,
  "scaleFactor": 65536,
  "designSize": 655360,
  
  // For XDV fonts  
  "flags": 4608,
  "size": 655360,
  "rgba": 0x000000FF,
  "extend": 1000,
  "slant": 0,
  "bold": 0
}

// Font data (sent separately, cached)
{
  "cmd": "fontData",
  "id": 1,
  "format": "otf" | "ttf" | "pfb" | "woff2",
  "data": "<base64-encoded-font-data>"
}

// Glyph mapping for TeX fonts
{
  "cmd": "fontGlyphMap",
  "id": 1,
  "encoding": "T1",  // or null for direct mapping
  "map": {
    "65": "A",      // char code → glyph name
    "66": "B"
  }
}
```

#### 3.2.2 Text Commands

```json
// Show text using font
{
  "cmd": "text",
  "font": 1,          // font id
  "matrix": [1,0,0,1,72,720],  // 6-element transform matrix
  "glyphs": [
    {
      "gid": 65,      // glyph ID
      "x": 0,         // x offset from origin
      "y": 0          // y offset from origin
    },
    {
      "gid": 66,
      "x": 7.2,
      "y": 0
    }
  ],
  "color": [0,0,0]    // RGB 0-1
}

// XDV-style text with explicit positions
{
  "cmd": "xdvGlyphs",
  "font": 2,
  "matrix": [1,0,0,1,72,720],
  "width": 50000,     // total advance width (scaled)
  "glyphs": [
    {"gid": 100, "dx": 0, "dy": 0},
    {"gid": 101, "dx": 7200, "dy": 0}
  ],
  "color": [0,0,0]
}
```

#### 3.2.3 Graphics State

```json
// Save graphics state
{ "cmd": "save" }

// Restore graphics state  
{ "cmd": "restore" }

// Set transform matrix
{
  "cmd": "setMatrix",
  "matrix": [1,0,0,1,0,0]
}

// Concatenate transform
{
  "cmd": "transform",
  "matrix": [1,0,0,1,100,200]
}

// Set clip path
{
  "cmd": "clip",
  "path": [ /* path commands */ ],
  "evenOdd": false
}
```

#### 3.2.4 Path Commands

```json
// Fill path
{
  "cmd": "fill",
  "path": [
    {"op": "M", "x": 0, "y": 0},           // moveto
    {"op": "L", "x": 100, "y": 0},         // lineto
    {"op": "L", "x": 100, "y": 100},
    {"op": "L", "x": 0, "y": 100},
    {"op": "Z"}                             // closepath
  ],
  "color": [0.5, 0.5, 0.5],
  "evenOdd": false
}

// Stroke path
{
  "cmd": "stroke",
  "path": [
    {"op": "M", "x": 0, "y": 0},
    {"op": "C", "x1": 10, "y1": 20, "x2": 30, "y2": 40, "x3": 50, "y3": 50}  // curveto
  ],
  "color": [0, 0, 0],
  "lineWidth": 1.0,
  "lineCap": "butt" | "round" | "square",
  "lineJoin": "miter" | "round" | "bevel",
  "miterLimit": 10.0,
  "dashArray": [3, 2],
  "dashPhase": 0
}

// Fill and stroke
{
  "cmd": "fillStroke",
  "path": [ /* path commands */ ],
  "fillColor": [1, 0, 0],
  "strokeColor": [0, 0, 0],
  "lineWidth": 0.5
}

// Rectangle (common case, optimized)
{
  "cmd": "rect",
  "x": 72,
  "y": 720,
  "width": 100,
  "height": 50,
  "fill": [0, 0, 0],
  "stroke": null
}
```

#### 3.2.5 Image Commands

```json
// Define image for later use
{
  "cmd": "imageDef",
  "id": "img-001",
  "width": 640,
  "height": 480,
  "format": "png" | "jpeg" | "pdf-page",
  "data": "<base64-encoded-image>"
}

// For PDF pages embedded as images
{
  "cmd": "imageDef",
  "id": "pdf-001-p3",
  "format": "pdf-page",
  "pdfData": "<base64-encoded-pdf>",
  "page": 3,
  "cropBox": [0, 0, 612, 792]
}

// Draw image
{
  "cmd": "image",
  "id": "img-001",
  "matrix": [100, 0, 0, 100, 72, 620]  // scale and position
}
```

#### 3.2.6 Special Commands

```json
// Begin page
{
  "cmd": "beginPage",
  "page": 1,
  "width": 612,
  "height": 792,
  "background": [1, 1, 1]
}

// End page
{
  "cmd": "endPage",
  "page": 1
}

// SyncTeX anchor (for click-to-source)
{
  "cmd": "syncAnchor",
  "file": "main.tex",
  "line": 42,
  "column": 0,
  "h": 72.0,
  "v": 700.0,
  "width": 50.0,
  "height": 12.0
}
```

### 3.3 Complete Page Example

```json
{
  "type": "render.page",
  "page": 1,
  "commands": [
    {"cmd": "beginPage", "page": 1, "width": 612, "height": 792, "background": [1,1,1]},
    
    // Font definitions (cached, may be omitted if already sent)
    {"cmd": "fontDef", "id": 1, "type": "xdv", "name": "CMU Serif", "size": 655360},
    
    // Title
    {"cmd": "save"},
    {"cmd": "text", "font": 1, "matrix": [1,0,0,1,72,720], 
     "glyphs": [{"gid": 72, "x": 0, "y": 0}, {"gid": 101, "x": 8, "y": 0}],
     "color": [0,0,0]},
    {"cmd": "syncAnchor", "file": "main.tex", "line": 5, "column": 0, 
     "h": 72, "v": 720, "width": 50, "height": 12},
    {"cmd": "restore"},
    
    // Horizontal rule
    {"cmd": "rect", "x": 72, "y": 700, "width": 468, "height": 0.5, "fill": [0,0,0]},
    
    // Body text...
    {"cmd": "text", "font": 1, "matrix": [1,0,0,1,72,680], /* ... */},
    
    // Included figure
    {"cmd": "save"},
    {"cmd": "transform", "matrix": [200, 0, 0, 150, 206, 400]},
    {"cmd": "image", "id": "fig1"},
    {"cmd": "restore"},
    
    {"cmd": "endPage", "page": 1}
  ]
}
```

---

## 4. Font Handling

Fonts are critical for correct rendering. The system must handle multiple font formats and mappings.

### 4.1 Font Types in TeXpresso

| Type | Format | Usage | Handling |
|------|--------|-------|----------|
| TeX fonts | TFM + VF + PFB | Traditional LaTeX | Convert PFB→WOFF2, use TFM for metrics |
| OpenType | OTF | XeLaTeX, modern fonts | Direct use, convert to WOFF2 for browser |
| TrueType | TTF | XeLaTeX, system fonts | Direct use, convert to WOFF2 for browser |

### 4.2 Font Pipeline

```
TeXpresso (headless):                     Frontend:
┌────────────────────────────────────┐    ┌────────────────────────────┐
│ 1. TeX engine requests font        │    │ 5. Receive font data       │
│ 2. Resource manager loads:         │    │ 6. Register with mupdf.js  │
│    - TFM (metrics)                 │    │ 7. Cache in IndexedDB      │
│    - VF (virtual font, if any)     │    │                            │
│    - Font file (OTF/TTF/PFB)       │    │                            │
│ 3. Convert to WOFF2 if needed      │───►│                            │
│ 4. Send fontDef + fontData (JSON)  │    │                            │
└────────────────────────────────────┘    └────────────────────────────┘
```

### 4.3 Font Message Sequence

```
TeXpresso                  Node.js                  Browser
   │                          │                        │
   │  {"cmd":"fontDef",...}   │                        │
   │─────────────────────────►│  (forward)             │
   │                          │───────────────────────►│
   │                          │                        │ (check cache)
   │                          │  {"type":"fontReq"...} │ (cache miss)
   │                          │◄───────────────────────│
   │  {"type":"fontReq"...}   │                        │
   │◄─────────────────────────│                        │
   │                          │                        │
   │  {"cmd":"fontData",...}  │                        │
   │─────────────────────────►│  (forward)             │
   │                          │───────────────────────►│
   │                          │                        │ (cache font)
   │  {"cmd":"text",...}      │                        │
   │─────────────────────────►│───────────────────────►│
   │                          │                        │ (render)
```

### 4.4 Glyph Mapping

For TeX fonts, character codes must be mapped to glyph IDs:

```json
// TeXpresso sends encoding information
{
  "cmd": "fontGlyphMap",
  "id": 1,
  "map": {
    "0": "Gamma",      // char 0 → glyph name "Gamma"
    "1": "Delta",
    "65": "A",
    "66": "B",
    // ... full encoding
  }
}
```

The frontend uses this to look up glyph IDs when rendering:
```javascript
function getGlyphId(fontId, charCode) {
  const font = fonts.get(fontId);
  const glyphName = font.glyphMap[charCode];
  return mupdf.encodeCharacterByGlyphName(font.fzFont, glyphName);
}
```

---

## 5. Resource Management

### 5.1 Server-Side Resources

```
/resources/
├── fonts/
│   ├── cache/           # Converted WOFF2 fonts
│   │   ├── cmr10.woff2
│   │   └── ...
│   ├── tfm/             # TeX Font Metrics
│   ├── vf/              # Virtual Fonts
│   └── enc/             # Encoding files
├── graphics/
│   └── cache/           # Converted/processed images
└── tectonic/
    └── bundle/          # Tectonic bundle files
```

### 5.2 Client-Side Caching

```javascript
// IndexedDB structure
const db = {
  fonts: {
    // key: fontId, value: {format, data, timestamp}
  },
  images: {
    // key: imageId, value: {format, data, timestamp}  
  }
};

// Cache management
const CACHE_MAX_SIZE = 100 * 1024 * 1024; // 100MB
const CACHE_MAX_AGE = 7 * 24 * 60 * 60 * 1000; // 7 days
```

---

## 6. Incremental Updates

### 6.1 Update Strategy

TeXpresso's key feature is incremental compilation. The web version preserves this:

1. **File Change Detection**: Backend tracks which files/positions have changed
2. **Process Forking**: TeX engine forks at safe points for fast rollback
3. **DVI Diffing**: Only changed portions of DVI output are reprocessed
4. **Command Streaming**: Only affected rendering commands are resent

### 6.2 Incremental Render Protocol

```json
// Full page render (initial or major change)
{
  "type": "render.page",
  "page": 1,
  "version": 42,
  "commands": [ /* all commands */ ]
}

// Incremental update (minor change)
{
  "type": "render.update",
  "page": 1,
  "version": 43,
  "baseVersion": 42,
  "operations": [
    {
      "op": "delete",
      "start": 150,      // command index
      "count": 10
    },
    {
      "op": "insert", 
      "at": 150,
      "commands": [ /* new commands */ ]
    }
  ]
}
```

### 6.3 Client-Side Command Buffer

```javascript
class PageCommandBuffer {
  constructor(page) {
    this.page = page;
    this.version = 0;
    this.commands = [];
  }
  
  applyFullRender(version, commands) {
    this.version = version;
    this.commands = commands;
    this.render();
  }
  
  applyUpdate(version, baseVersion, operations) {
    if (baseVersion !== this.version) {
      // Request full render
      return false;
    }
    
    for (const op of operations) {
      if (op.op === 'delete') {
        this.commands.splice(op.start, op.count);
      } else if (op.op === 'insert') {
        this.commands.splice(op.at, 0, ...op.commands);
      }
    }
    
    this.version = version;
    this.render();
    return true;
  }
  
  render() {
    // Execute commands via mupdf.js
  }
}
```

---

## 7. Implementation Phases

### Phase 1: Core Infrastructure (4-6 weeks)

1. **TeXpresso Headless Mode**
   - Add `--headless` command-line flag
   - Skip SDL2 initialization when headless
   - Implement stdin JSON command reader (non-blocking)
   - Implement stdout JSON event writer
   - Create command generator (`cmd_generator.c`) as fz_device

2. **Node.js WebSocket Server**
   - WebSocket server with `ws` or `uWebSockets.js`
   - Session management (spawn/kill TeXpresso processes)
   - stdin/stdout piping to/from TeXpresso
   - Basic error handling and logging

3. **Basic Rendering Pipeline**
   - DVI interpreter outputting render commands via cmd_generator
   - JSON serialization of commands
   - Simple command types: text, rect, basic paths

4. **Frontend Skeleton**
   - WebSocket client
   - Basic mupdf.js integration
   - Command interpreter
   - Simple canvas rendering

### Phase 2: Font System (3-4 weeks)

1. **Server-Side Font Processing**
   - Font format detection and conversion
   - TFM/VF parsing and glyph mapping
   - Encoding file support
   - Font caching

2. **Client-Side Font Handling**
   - WOFF2 loading into mupdf.js
   - Glyph mapping tables
   - Font caching (IndexedDB)
   - Fallback handling

### Phase 3: Complete Rendering (3-4 weeks)

1. **Graphics Support**
   - PDF page embedding
   - Image formats (PNG, JPEG)
   - SVG (if needed)

2. **Advanced Drawing**
   - Full path operations
   - Clipping
   - Transformations
   - Color spaces (RGB, CMYK, grayscale)

3. **Special Commands**
   - All DVI specials (colors, transforms)
   - PDF operators
   - dvipdfmx compatibility

### Phase 4: Editor Integration (2-3 weeks)

1. **Code Editor**
   - Monaco or CodeMirror integration
   - LaTeX syntax highlighting
   - Real-time sync with backend VFS

2. **SyncTeX**
   - Forward sync (source → preview)
   - Backward sync (preview → source)
   - Visual indicators

### Phase 5: Performance & Polish (2-3 weeks)

1. **Incremental Updates**
   - Efficient delta transmission
   - Command buffer management
   - Optimized re-rendering

2. **Caching & Optimization**
   - Resource caching strategy
   - Lazy loading
   - Memory management

3. **UI/UX**
   - Zoom/pan controls
   - Page navigation
   - Split view
   - Error display

---

## 8. Technical Considerations

### 8.1 mupdf.js Capabilities

mupdf.js (based on MuPDF compiled to WASM) provides:
- PDF rendering
- Text extraction
- Font handling
- Path drawing
- Image rendering

Key APIs to use:
```javascript
// Document operations
mupdf.Document.openDocument(data, magic);

// Drawing operations  
device.fillText(text, ctm, colorspace, color, alpha);
device.fillPath(path, evenOdd, ctm, colorspace, color, alpha);
device.fillImage(image, ctm, alpha);

// Font operations
mupdf.Font.create(name);
font.encodeCharacter(unicode);
font.encodeCharacterByGlyphName(name);
```

### 8.2 Binary Data Handling

For efficiency, large binary data (fonts, images) should use:
- Base64 encoding in JSON (simple, ~33% overhead)
- Binary WebSocket frames (efficient, more complex)
- Separate HTTP endpoints (cacheable, requires CORS)

Recommendation: Start with Base64, optimize later if needed.

### 8.3 Error Handling

```json
// Server error response
{
  "type": "error",
  "code": "COMPILE_ERROR" | "RESOURCE_NOT_FOUND" | "INTERNAL_ERROR",
  "message": "Detailed error description",
  "file": "main.tex",  // optional
  "line": 42           // optional
}
```

### 8.4 Security Considerations

1. **Input Validation**: Sanitize all file paths and content
2. **Resource Limits**: Limit file sizes, compilation time, memory
3. **Sandboxing**: Run TeX in isolated environment
4. **Authentication**: Session tokens, rate limiting
5. **Process Isolation**: Each session runs in separate TeXpresso process

### 8.5 Scaling Considerations

The process-per-session architecture provides good isolation but has limits:

| Sessions | Memory (approx) | Recommendation |
|----------|-----------------|----------------|
| 1-50 | 2-10 GB | Single server, no changes needed |
| 50-200 | 10-40 GB | Larger server or horizontal scaling |
| 200+ | 40+ GB | Process pooling, load balancing |

Future optimizations if needed:
- **Process pooling**: Reuse TeXpresso processes across sessions
- **Horizontal scaling**: Multiple servers behind load balancer
- **Multiplexing**: Single TeXpresso handling multiple sessions (requires more work)

---

## 9. Testing Strategy

### 9.0 Basic Test Scripts

Simple scripts to test the system at various levels:

#### Test TeXpresso Headless Mode Directly
```bash
#!/bin/bash
# test-headless.sh - Test TeXpresso stdin/stdout directly

echo '{"type":"init","document":{"name":"test.tex","content":"\\documentclass{article}\\begin{document}Hello\\end{document}"}}' | \
  texpresso --headless | \
  head -20
```

#### Test WebSocket Server with websocat
```bash
#!/bin/bash
# test-websocket.sh - Test the Node.js WebSocket server

# Start server in background
node server.js &
SERVER_PID=$!
sleep 1

# Send init message and capture response
echo '{"type":"init","document":{"name":"test.tex","content":"\\documentclass{article}\\begin{document}Hello\\end{document}"}}' | \
  websocat ws://localhost:8080 | \
  head -20

kill $SERVER_PID
```

#### Python WebSocket Test
```python
#!/usr/bin/env python3
# test_websocket.py - More comprehensive WebSocket test

import asyncio
import websockets
import json

async def test():
    async with websockets.connect('ws://localhost:8080') as ws:
        # Send init
        await ws.send(json.dumps({
            "type": "init",
            "document": {
                "name": "test.tex",
                "content": r"\documentclass{article}\begin{document}Hello\end{document}"
            }
        }))
        
        # Collect responses for a few seconds
        try:
            while True:
                msg = await asyncio.wait_for(ws.recv(), timeout=5.0)
                data = json.loads(msg)
                print(f"Received: {data['type']}")
                if data['type'] == 'render.page':
                    print(f"  Page {data['page']}: {len(data['commands'])} commands")
        except asyncio.TimeoutError:
            print("Done (timeout)")

asyncio.run(test())
```

### 9.1 Unit Tests

- Protocol serialization/deserialization (JSON parsing in C and TypeScript)
- Font conversion
- Rendering command generation
- Incremental update logic
- Node.js session management

### 9.2 Integration Tests

- Full compilation pipeline (edit → TeXpresso → render commands)
- WebSocket communication (browser ↔ Node.js ↔ TeXpresso)
- mupdf.js rendering accuracy
- Process lifecycle (spawn, communicate, kill)

### 9.3 Visual Regression Tests

- Render known documents
- Compare output to reference images
- Automated diff detection

### 9.4 Performance Tests

- Latency measurement (edit → render)
- Memory usage monitoring (per-process and total)
- Large document handling
- Concurrent session stress testing

---

## 10. Appendices

### Appendix A: DVI Opcodes Reference

| Opcode | Name | Description |
|--------|------|-------------|
| 0-127 | SET_CHAR_n | Typeset character n |
| 128-131 | SET1-4 | Typeset character (1-4 byte arg) |
| 132 | SET_RULE | Draw filled rectangle |
| 133-136 | PUT1-4 | Typeset without advancing |
| 137 | PUT_RULE | Draw rule without advancing |
| 138 | NOP | No operation |
| 139 | BOP | Begin page |
| 140 | EOP | End page |
| 141 | PUSH | Push position stack |
| 142 | POP | Pop position stack |
| 143-146 | RIGHT1-4 | Move right |
| 147-151 | W0-4 | Move right (with state) |
| 152-156 | X0-4 | Move right (with state) |
| 157-160 | DOWN1-4 | Move down |
| 161-165 | Y0-4 | Move down (with state) |
| 166-170 | Z0-4 | Move down (with state) |
| 171-234 | FNT_NUM_n | Select font n |
| 235-238 | FNT1-4 | Select font (1-4 byte arg) |
| 239-242 | XXX1-4 | Special (1-4 byte length) |
| 243-246 | FNT_DEF1-4 | Define font |
| 247 | PRE | Preamble |
| 248 | POST | Postamble |
| 249 | POST_POST | End of postamble |
| 252 | XDV_NATIVE_FONT_DEF | XDV font definition |
| 253 | XDV_GLYPHS | XDV glyph array |
| 254 | XDV_TEXT_GLYPHS | XDV text with glyphs |

### Appendix B: DVI Specials Supported

| Prefix | Type | Description |
|--------|------|-------------|
| `color push` | Color | Push color onto stack |
| `color pop` | Color | Pop color from stack |
| `pdf:` | PDF | PDF-specific commands |
| `pdf: image` | Graphics | Include image |
| `pdf: btrans` | Transform | Begin transformation |
| `pdf: etrans` | Transform | End transformation |
| `pdf: code` | Raw PDF | Inject PDF operators |
| `pdfcolorstack` | Color | PDF color stack operations |
| `x:` | Transform | Apply transformation |

### Appendix C: Project Structure

```
web-texpresso/
├── server/                    # Node.js WebSocket server
│   ├── package.json
│   ├── tsconfig.json
│   ├── src/
│   │   ├── index.ts          # Entry point
│   │   ├── session.ts        # Session management
│   │   └── types.ts          # TypeScript types for protocol
│   └── test/
│       └── *.test.ts
│
├── client/                    # Frontend (React/Vite recommended)
│   ├── package.json
│   ├── src/
│   │   ├── App.tsx
│   │   ├── components/
│   │   │   ├── Editor.tsx    # Monaco/CodeMirror wrapper
│   │   │   └── Viewer.tsx    # mupdf.js wrapper
│   │   ├── lib/
│   │   │   ├── protocol.ts   # WebSocket client
│   │   │   └── renderer.ts   # Command interpreter
│   │   └── hooks/
│   │       └── useTeXpresso.ts
│   └── public/
│       └── mupdf.wasm
│
├── texpresso/                 # Modified TeXpresso (or as submodule)
│   └── src/
│       ├── main.c            # Modified for --headless
│       ├── cmd_generator.c   # NEW: JSON command output
│       ├── cmd_generator.h
│       └── json_writer.c     # NEW: JSON serialization
│
└── scripts/
    ├── test-headless.sh
    ├── test-websocket.sh
    └── test_websocket.py
```

### Appendix D: References

- [TeXpresso GitHub Repository](https://github.com/let-def/texpresso)
- [mupdf.js Documentation](https://mupdf.readthedocs.io/)
- [DVI File Format Specification](https://www.tug.org/TUGboat/tb38-3/tb120mills.pdf)
- [XDV Extensions Documentation](https://tug.org/xetex/)
- [SyncTeX Protocol](https://www.tug.org/TUGboat/tb29-3/tb93laurens.pdf)
- [ws (Node.js WebSocket library)](https://github.com/websockets/ws)
- [uWebSockets.js (high-performance alternative)](https://github.com/uNetworking/uWebSockets.js)
