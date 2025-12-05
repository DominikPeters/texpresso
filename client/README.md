# TeXpresso Web Client

Simple web-based frontend for live LaTeX editing with TeXpresso.

## Features

- **Live Editor**: Edit LaTeX code in a textarea with change tracking
- **Real-time Compilation**: Changes are sent to TeXpresso server via WebSocket
- **Compilation Log**: View TeX output and compilation status
- **Simple UI**: Clean, responsive interface without unnecessary complexity

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Browser                                                 │
│  ┌─────────────┐         ┌──────────────────────────┐   │
│  │   Editor    │◄───────►│   WebSocket Client       │   │
│  │  (textarea) │         │   - Connection mgmt      │   │
│  │             │         │   - Protocol handling    │   │
│  └─────────────┘         └──────────┬───────────────┘   │
│  ┌─────────────┐                    │                   │
│  │  Log Viewer │                    │                   │
│  │  (pre)      │                    │                   │
│  └─────────────┘                    │                   │
└─────────────────────────────────────┼───────────────────┘
                                      │ WebSocket
                                      ▼
                            ws://localhost:8080
                        (TeXpresso WebSocket Server)
```

## Files

```
client/
├── index.html      # Main HTML structure
├── style.css       # Styling
├── app.js          # Main application logic
├── client.js       # WebSocket client
├── editor.js       # Editor change tracking
├── log.js          # Log viewer
├── serve.js        # Simple HTTP server
├── package.json    # NPM configuration
└── README.md       # This file
```

## Usage

### 1. Start the WebSocket server

In a terminal:
```bash
cd ../server
npm start
```

The server should be running on `ws://localhost:8080`.

### 2. Start the web client

In another terminal:
```bash
cd client
npm start
```

This starts a simple HTTP server on `http://localhost:3000`.

### 3. Open in browser

Navigate to: **http://localhost:3000**

The client will automatically connect to the WebSocket server.

## How It Works

### Editor Change Tracking

The editor tracks changes using a simple diff algorithm:
1. User types in the textarea
2. Changes are debounced (500ms delay)
3. Diff is calculated (prefix/suffix matching)
4. Delta is sent to server: `{offset, length, text}`

Example:
```
Old: "Hello World"
New: "Hello LaTeX World"
Delta: {offset: 6, length: 0, text: "LaTeX "}
```

### WebSocket Protocol

The client sends JSON messages:
```json
{
  "type": "file.change",
  "path": "document.tex",
  "changes": [
    {"offset": 100, "length": 5, "text": "LaTeX"}
  ]
}
```

And receives status updates:
```json
{"type": "status", "state": "ready"}
{"type": "doc.pageCount", "count": 1}
{"type": "output.append", "stream": "log", "text": "..."}
```

### Components

**client.js** - WebSocket client
- Connection management
- Message serialization
- Event emitter for incoming messages

**editor.js** - Editor manager
- Change tracking
- Delta calculation
- Debouncing

**log.js** - Log viewer
- Syntax highlighting for errors/warnings
- Auto-scroll
- Line limiting (10k lines max)

**app.js** - Main application
- Wires everything together
- UI updates
- Event handling

## Configuration

Edit `app.js` to change:
- WebSocket URL (default: `ws://localhost:8080`)
- Document name (default: `document.tex`)
- Change debounce delay (default: 500ms)

## Keyboard Shortcuts

Currently, there are no special keyboard shortcuts. The editor is a standard textarea with:
- Tab key (inserts tab character)
- Standard text editing shortcuts
- Copy/paste support

## Limitations

- No syntax highlighting (yet)
- No line numbers (yet)
- No PDF preview (coming soon)
- No SyncTeX integration (yet)
- Single document only

## Future Improvements

1. **Monaco Editor** - Better editing experience with syntax highlighting
2. **PDF Viewer** - Render mupdf.js output
3. **SyncTeX** - Click-to-source and source-to-preview
4. **Multiple Files** - Support for multi-file projects
5. **Toolbar** - Quick actions (bold, italic, sections, etc.)
6. **Themes** - Dark mode support

## Testing

Open the browser console (F12) to see:
- Connection events
- Incoming/outgoing messages
- Change deltas
- Errors and warnings

## Troubleshooting

**"Disconnected from server"**
- Make sure the WebSocket server is running on port 8080
- Check the browser console for errors

**Changes not being sent**
- Check that you're connected (status indicator should be green)
- Try clicking "Compile" button to force recompilation

**No compilation output**
- Check that headless TeXpresso is working
- Look at server logs for errors
- Verify the document is valid LaTeX

## Development

To modify the client:
1. Edit the files (HTML, CSS, JS)
2. Refresh the browser (no build step needed)
3. Check browser console for errors

The client uses ES6 modules, so it requires a modern browser (Chrome 61+, Firefox 60+, Safari 11+).

## License

Same as TeXpresso parent project
