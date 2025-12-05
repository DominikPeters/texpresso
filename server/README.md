# TeXpresso WebSocket Server

WebSocket server for web-based TeXpresso, providing live LaTeX compilation and preview.

## Overview

This server manages WebSocket connections from browser clients and spawns headless TeXpresso processes for real-time LaTeX compilation. Each client session gets its own TeXpresso process, with communication via JSON over stdin/stdout.

### Architecture

```
Browser Client ←→ WebSocket ←→ Node.js Server ←→ stdin/stdout ←→ TeXpresso Process
                    (JSON)                        (JSON lines)      (LaTeX compilation)
```

### Features

- **Session Management**: One TeXpresso process per WebSocket connection
- **Virtual File System (VFS)**: Server-side file management with delta updates
- **Protocol Validation**: Comprehensive validation of incoming messages
- **Error Handling**: Graceful error handling and reporting
- **Resource Management**: Automatic cleanup on disconnect
- **Heartbeat**: Keep-alive pings to maintain connections
- **Configurable**: Environment-based configuration
- **Logging**: Structured logging with timestamps

## Installation

```bash
cd server
npm install
```

## Configuration

Configuration is done via environment variables. Create a `.env` file or export variables:

```bash
# Server port (default: 8080)
PORT=8080

# Path to TeXpresso executable (default: ../build/texpresso)
TEXPRESSO_PATH=../build/texpresso

# Working directory for TeXpresso processes (default: current directory)
WORKING_DIR=/path/to/working/dir

# Forward TeXpresso stderr to clients (default: false)
FORWARD_STDERR=true

# Maximum concurrent connections (default: 100)
MAX_CONNECTIONS=100

# Connection timeout in milliseconds, 0 = disabled (default: 0)
CONNECTION_TIMEOUT=300000

# Enable verbose logging (default: false)
VERBOSE=true
```

## Usage

### Start the server

```bash
npm start
```

Or with custom configuration:

```bash
PORT=3000 VERBOSE=true npm start
```

### Development mode (with auto-reload)

```bash
npm run dev
```

#### Testing with the test client

The included test client provides an interactive command-line interface:

```bash
node src/test-client.js [ws://localhost:8080]
```

Available commands:
- `help` - Show help message
- `change` - Send a test file change
- `goto <page>` - Navigate to page
- `forward <line>` - SyncTeX forward search
- `backward <x> <y> <page>` - SyncTeX backward search
- `render <page>` - Request page render
- `json <json>` - Send raw JSON message
- `quit`, `exit` - Close connection

### Running the automated tests

```bash
# Quick protocol translation test
node test-simple.js

# Full integration test with real TeXpresso compilation
node test-real.js

# Or use the manual test helper
./test-manual.sh
```

Expected output from `test-real.js`:
```
✅ TEST PASSED!
Received:
  - Status: ready
  - Page count: 1
  - Total messages: 32
```

## Protocol

### Client → Server Messages

#### Session Management

Initialize session:
```json
{
  "type": "init",
  "document": {
    "name": "main.tex",
    "content": "\\documentclass{article}..."
  }
}
```

Close session:
```json
{
  "type": "close"
}
```

#### File Operations

Open file:
```json
{
  "type": "file.open",
  "path": "chapter1.tex",
  "content": "..."
}
```

Update file (delta):
```json
{
  "type": "file.change",
  "path": "main.tex",
  "changes": [
    {
      "offset": 100,
      "length": 5,
      "text": "hello"
    }
  ]
}
```

Update file (line-based):
```json
{
  "type": "file.changeLine",
  "path": "main.tex",
  "startLine": 10,
  "endLine": 12,
  "text": "new content"
}
```

Close file:
```json
{
  "type": "file.close",
  "path": "chapter1.tex"
}
```

#### Navigation

Go to page:
```json
{
  "type": "nav.goto",
  "page": 5
}
```

SyncTeX forward (source → preview):
```json
{
  "type": "synctex.forward",
  "path": "main.tex",
  "line": 42,
  "column": 0
}
```

SyncTeX backward (preview → source):
```json
{
  "type": "synctex.backward",
  "x": 150.5,
  "y": 300.2,
  "page": 1
}
```

#### Configuration

Set theme:
```json
{
  "type": "config.theme",
  "background": [1.0, 1.0, 1.0],
  "foreground": [0.0, 0.0, 0.0]
}
```

Request render:
```json
{
  "type": "render.request",
  "page": 1,
  "scale": 1.5
}
```

### Server → Client Messages

#### Status

Compilation status:
```json
{
  "type": "status",
  "state": "compiling" | "ready" | "error",
  "progress": 0.5
}
```

#### Logs

Error/warning/info:
```json
{
  "type": "log",
  "level": "error" | "warning" | "info",
  "file": "main.tex",
  "line": 42,
  "message": "Undefined control sequence"
}
```

#### Document Metadata

Page count:
```json
{
  "type": "doc.pageCount",
  "count": 15
}
```

Page dimensions:
```json
{
  "type": "doc.pageDim",
  "page": 1,
  "width": 612.0,
  "height": 792.0,
  "landscape": false
}
```

#### Rendering

Page render commands:
```json
{
  "type": "render.page",
  "page": 1,
  "commands": [ /* drawing commands */ ]
}
```

Incremental update:
```json
{
  "type": "render.update",
  "page": 1,
  "invalidateFrom": 150,
  "commands": [ /* new commands */ ]
}
```

#### SyncTeX Results

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

#### Errors

```json
{
  "type": "error",
  "code": "ERROR_CODE",
  "message": "Human-readable error message",
  "details": { /* optional */ }
}
```

Error codes:
- `INVALID_MESSAGE` - Message validation failed
- `INVALID_JSON` - Message is not valid JSON
- `NOT_INITIALIZED` - Session not initialized
- `ALREADY_INITIALIZED` - Session already initialized
- `INIT_FAILED` - Failed to initialize session
- `FILE_NOT_FOUND` - File not found in VFS
- `PROCESS_ERROR` - TeXpresso process error
- `INTERNAL_ERROR` - Internal server error

## File Structure

```
server/
├── package.json           # Dependencies and scripts
├── README.md             # This file
├── .env.example          # Configuration example
├── .gitignore            # Git ignore rules
├── test-simple.js        # Automated integration test
├── test-manual.sh        # Manual test helper
├── test-integration.sh   # Full integration test
└── src/
    ├── index.js          # Main server entry point
    ├── session.js        # Session management with VFS
    ├── protocol.js       # Protocol translation (JSON ↔ S-expressions)
    ├── types.js          # JSDoc type definitions
    └── test-client.js    # Interactive test client
```

## Development

### Code Organization

- **index.js**: Main WebSocket server, connection handling, message routing, validation
- **session.js**: Session class managing individual TeXpresso processes, VFS, and protocol handling
- **protocol.js**: Protocol translator between WebSocket JSON and TeXpresso S-expressions
- **types.js**: JSDoc type definitions for the protocol (enables type checking without TypeScript)
- **test-client.js**: Interactive CLI for testing

### Protocol Translation

The server acts as a protocol bridge:

**Browser → Server**: Structured JSON objects
```json
{"type": "file.change", "path": "main.tex", "changes": [...]}
```

**Server → TeXpresso**: S-expressions as JSON arrays
```json
["change", "main.tex", 100, 5, "new text"]
```

**TeXpresso → Server**: S-expressions or JSON objects
```json
["append", "log", 0, "LaTeX log output"]
{"type": "status", "state": "ready"}
```

**Server → Browser**: Structured JSON objects
```json
{"type": "output.append", "stream": "log", "text": "LaTeX log output"}
{"type": "status", "state": "ready"}
```

This design allows browser clients to use a modern JSON API while maintaining compatibility with TeXpresso's S-expression protocol.

### Logging

The server uses structured logging with ISO timestamps:

```
[2025-12-03T10:30:45.123Z] Server started on port 8080
[2025-12-03T10:30:50.456Z] [abc123] New connection from ::1
[2025-12-03T10:30:50.789Z] [abc123] Starting TeXpresso process
```

Enable verbose logging with `VERBOSE=true` for detailed message tracing.

### Session Lifecycle

1. **Connection**: Client connects via WebSocket
2. **Init**: Client sends `init` message with document
3. **Processing**: Server spawns TeXpresso process, sets up stdio pipes
4. **Active**: Bidirectional message passing between client and TeXpresso
5. **Cleanup**: On disconnect or error, process is killed and resources freed

### Virtual File System

The VFS maintains document state server-side:

- Files are stored as `{content: string, dirty: boolean}`
- Changes are applied incrementally via offset-based or line-based updates
- VFS state is used for crash recovery and process restarts
- All files are cleaned up when session ends

## Troubleshooting

### Connection refused

Ensure TeXpresso is built and the path is correct:
```bash
ls -la ../build/texpresso
```

### Process spawn errors

Check TeXpresso can run in headless mode:
```bash
../build/texpresso --headless test/simple.tex
```

### Memory issues with many connections

Reduce `MAX_CONNECTIONS` or increase system limits:
```bash
ulimit -n 4096  # Increase file descriptor limit
```

### JSON parse errors

Enable verbose logging to see raw messages:
```bash
VERBOSE=true npm start
```

## License

Same as TeXpresso (see parent directory)
