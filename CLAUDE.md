The task is to produce a headless version of TeXpresso, a live LaTeX compilation and preview system.
This headless version will be used as part of a web application, where the TeX engine runs on a server and communicates with a web frontend through a node-run WebSocket server.

Claude should use good git commit practices, including clear commit messages and logical commit structure.

For background information, this repo contains a copy of the mupdf repository in the folder mupdf-master-copy, for Claude to look for API definitions, e.g. in mupdf-master-copy/include/mupdf/fitz/text.h for the text rendering API.

Note that we should use the `change-range` command for file edits, not the legacy `change` command (because `change` uses bytes / UTF-8 offsets which are incompatible with modern editors). From EDITOR-PROTOCOL.md:

<EDITOR-PROTOCOL.md>
```scheme
(change-range "path" start-line start-column end-line end-column "replacement-text")
```

Update file at "path" in VFS (it should have been `open`ed before), by replacing the characters in range starting from line `start-line` at column `start-column` (implemented by counting the number of UTF-16 code units, with 0 being the beginning of the line) up to line `end-line` at column `end-column`. This is designed to be compatible with [LSP position encoding](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#positionEncodingKind) using the default 'utf-16' encoding.
</EDITOR-PROTOCOL.md>

Note that paths should always be absolute paths within the TeXpresso VFS.

A detailed implementation plan is provided in the file web-texpresso-implementation-plan.md.
Here we will only provide a short summary, in particular including just headings and section line numbers to look at in the full plan.

- **Line 1-2:** # Web-Based TeXpresso Implementation Plan
- **Line 3-7:** ## Executive Summary
- **Line 9-73:** ## 0. Motivation and Goals
  - Line 11-18: ### 0.1 What We're Building
  - Line 20-36: ### 0.2 Why a Web Version?
  - Line 38-45: ### 0.3 Design Principles
  - Line 47-73: ### 0.4 Expected User Experience

- **Line 75-525:** ## 1. Architecture Overview
  - Line 77-104: ### 1.1 Current TeXpresso Architecture
  - Line 106-152: ### 1.2 Proposed Web Architecture
  - Line 154-170: ### 1.3 Why Node.js Wrapper + Headless TeXpresso?
  - Line 172-247: ### 1.4 Source Code Reference
    - Line 176-184: #### Core Application (`src/`)
    - Line 186-192: #### Editor Protocol (`src/`)
    - Line 194-199: #### Server Protocol (TeX ↔ TeXpresso)
    - Line 201-214: #### DVI/XDV Rendering (`src/dvi/`)
    - Line 216-223: #### Font Support (`src/dvi/`)
    - Line 225-232: #### State Management (`src/`)
    - Line 234-240: #### Other (`src/`)
    - Line 242-247: #### Protocol Documentation
  - Line 249-525: ### 1.5 Backend Implementation Strategy
    - Line 253-288: #### 1.5.1 Process Architecture
    - Line 290-354: #### 1.5.2 Node.js Server Implementation
    - Line 356-425: #### 1.5.3 TeXpresso Headless Mode
    - Line 427-444: #### 1.5.4 stdin/stdout JSON Protocol
    - Line 446-510: #### 1.5.5 Command Generator Design
    - Line 512-525: #### 1.5.6 Reusable Code from Current Implementation

- **Line 527-731:** ## 2. Communication Protocols
  - Line 529-695: ### 2.1 Frontend ↔ Backend WebSocket Protocol
    - Line 533-632: #### 2.1.1 Client → Server Messages
      - Line 535-552: ##### Session Management
      - Line 554-590: ##### File Operations (VFS)
      - Line 592-615: ##### Navigation Commands
      - Line 617-632: ##### Configuration
    - Line 634-731: #### 2.1.2 Server → Client Messages
      - Line 636-653: ##### Status Messages
      - Line 655-678: ##### Document Metadata
      - Line 680-695: ##### Rendering Commands (See Section 3)
      - Line 697-713: ##### SyncTeX Response
      - Line 715-731: ##### Log Output

- **Line 733-1011:** ## 3. Rendering Command Protocol
  - Line 737-743: ### 3.1 Design Philosophy
  - Line 745-971: ### 3.2 Command Types
    - Line 747-789: #### 3.2.1 Font Commands
    - Line 791-826: #### 3.2.2 Text Commands
    - Line 828-855: #### 3.2.3 Graphics State
    - Line 857-909: #### 3.2.4 Path Commands
    - Line 911-940: #### 3.2.5 Image Commands
    - Line 942-971: #### 3.2.6 Special Commands
  - Line 973-1011: ### 3.3 Complete Page Example

- **Line 1013-1091:** ## 4. Font Handling
  - Line 1017-1023: ### 4.1 Font Types in TeXpresso
  - Line 1025-1038: ### 4.2 Font Pipeline
  - Line 1040-1061: ### 4.3 Font Message Sequence
  - Line 1063-1091: ### 4.4 Glyph Mapping

- **Line 1093-1130:** ## 5. Resource Management
  - Line 1095-1110: ### 5.1 Server-Side Resources
  - Line 1112-1130: ### 5.2 Client-Side Caching

- **Line 1132-1216:** ## 6. Incremental Updates
  - Line 1134-1141: ### 6.1 Update Strategy
  - Line 1143-1173: ### 6.2 Incremental Render Protocol
  - Line 1175-1216: ### 6.3 Client-Side Command Buffer

- **Line 1218-1308:** ## 7. Implementation Phases
  - Line 1220-1244: ### Phase 1: Core Infrastructure (4-6 weeks)
  - Line 1246-1258: ### Phase 2: Font System (3-4 weeks)
  - Line 1260-1276: ### Phase 3: Complete Rendering (3-4 weeks)
  - Line 1278-1288: ### Phase 4: Editor Integration (2-3 weeks)
  - Line 1290-1308: ### Phase 5: Performance & Polish (2-3 weeks)

- **Line 1310-1382:** ## 8. Technical Considerations
  - Line 1312-1335: ### 8.1 mupdf.js Capabilities
  - Line 1337-1344: ### 8.2 Binary Data Handling
  - Line 1346-1357: ### 8.3 Error Handling
  - Line 1359-1365: ### 8.4 Security Considerations
  - Line 1367-1382: ### 8.5 Scaling Considerations

- **Line 1384-1480:** ## 9. Testing Strategy
  - Line 1386-1450: ### 9.0 Basic Test Scripts
    - Line 1390-1398: #### Test TeXpresso Headless Mode Directly
    - Line 1400-1416: #### Test WebSocket Server with websocat
    - Line 1418-1450: #### Python WebSocket Test
  - Line 1452-1458: ### 9.1 Unit Tests
  - Line 1460-1465: ### 9.2 Integration Tests
  - Line 1467-1471: ### 9.3 Visual Regression Tests
  - Line 1473-1480: ### 9.4 Performance Tests

- **Line 1482-1571:** ## 10. Appendices
  - Line 1484-1513: ### Appendix A: DVI Opcodes Reference
  - Line 1515-1527: ### Appendix B: DVI Specials Supported
  - Line 1529-1569: ### Appendix C: Project Structure
  - Line 1571+: ### Appendix D: References


Extracts:

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