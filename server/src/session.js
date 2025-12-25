/**
 * @fileoverview Session management for TeXpresso WebSocket server
 */

import { spawn } from 'child_process';
import { createInterface } from 'readline';
import { join, isAbsolute } from 'path';
import { toSExpression, translateFileChanges, fromSExpression } from './protocol.js';

/**
 * Manages a single TeXpresso session with VFS
 */
export class Session {
  /**
   * @param {string} sessionId - Unique session identifier
   * @param {import('ws').WebSocket} ws - WebSocket connection
   * @param {Object} config - Server configuration
   */
  constructor(sessionId, ws, config = {}) {
    this.sessionId = sessionId;
    this.ws = ws;
    this.config = config;

    // Process management
    this.process = null;
    this.rl = null;
    this.isRunning = false;

    // Virtual File System
    /** @type {Map<string, {content: string, dirty: boolean}>} */
    this.vfs = new Map();

    // State
    this.currentPage = 0;  // 0-indexed to match headless backend and client
    this.pageCount = 0;
    this.state = 'initializing'; // initializing, ready, compiling, error

    // Buffering for partial JSON lines
    this.stderrBuffer = '';

    // Working directory for path resolution
    this.workingDir = null;

    // Initialization tracking
    this.initializationState = 'not_started'; // not_started, waiting_first_compile, waiting_open, done
    this.pendingInitDocument = null;
  }

  /**
   * Convert a path to absolute, relative to the working directory
   * TeXpresso's relative_path() function requires absolute paths
   * @param {string} path - The path to convert
   * @returns {string} Absolute path
   */
  toAbsolutePath(path) {
    if (isAbsolute(path)) {
      return path;
    }
    return join(this.workingDir || process.cwd(), path);
  }

  /**
   * Start the TeXpresso process
   * @param {{name: string, content: string}} document - Initial document
   */
  async start(document) {
    this.log('Starting TeXpresso process');

    // Add initial document to VFS
    this.vfs.set(document.name, { content: document.content, dirty: true });

    const texpressoPath = this.config.texpressoPath || '../build/texpresso';

    // Use the document name directly - TeXpresso will look for it in VFS after we send 'open'
    // For the initial spawn, we pass the document name and TeXpresso will wait for VFS commands
    const args = ['--headless', '-json', document.name];

    // Determine working directory - if a specific document path includes directories,
    // we need to ensure TeXpresso can find it
    this.workingDir = this.config.workingDir || process.cwd();

    try {
      // Spawn headless TeXpresso process
      this.process = spawn(texpressoPath, args, {
        stdio: ['pipe', 'pipe', 'pipe'],
        cwd: this.workingDir
      });

      this.isRunning = true;

      // Set up stdout reader for JSON lines
      this.rl = createInterface({
        input: this.process.stdout,
        crlfDelay: Infinity
      });

      this.rl.on('line', (line) => this.handleStdout(line));

      // Handle stderr (logging/debugging)
      this.process.stderr.on('data', (data) => this.handleStderr(data));

      // Handle process exit
      this.process.on('exit', (code, signal) => {
        this.log(`Process exited with code ${code}, signal ${signal}`);
        this.isRunning = false;
        this.sendToClient({
          type: 'status',
          state: 'error',
          message: `Process exited unexpectedly: code=${code}, signal=${signal}`
        });
        this.cleanup();
      });

      this.process.on('error', (err) => {
        this.log(`Process error: ${err.message}`);
        this.sendToClient({
          type: 'error',
          code: 'PROCESS_ERROR',
          message: err.message
        });
      });

      // Store the document for later - we'll send it after the first compilation
      this.pendingInitDocument = document;
      this.initializationState = 'waiting_first_compile';

      this.log('Process started successfully, waiting for initial compilation');

    } catch (err) {
      this.log(`Failed to start process: ${err.message}`);
      throw err;
    }
  }

  /**
   * Handle JSON line from TeXpresso stdout
   * @param {string} line
   */
  handleStdout(line) {
    try {
      const parsed = JSON.parse(line);

      // Log timing for render commands
      if (parsed.cmd === 'beginPage') {
        this.log(`[TIMING] Received beginPage from TeXpresso at ${Date.now()}`);
      } else if (parsed.cmd === 'endPage') {
        this.log(`[TIMING] Received endPage from TeXpresso at ${Date.now()}`);
      } else if (parsed.type === 'status') {
        this.log(`[TIMING] Received status=${parsed.state} from TeXpresso at ${Date.now()}`);
      } else if (parsed.type === 'render.version') {
        this.log(`[TIMING] Received render.version=${parsed.version} from TeXpresso at ${Date.now()}`);
      }

      // Translate S-expression to WebSocket JSON (if needed)
      let msg;
      if (Array.isArray(parsed)) {
        // S-expression, needs translation
        msg = fromSExpression(parsed);
        if (!msg) {
          // Unknown S-expression, log and skip
          if (this.config.verbose) {
            this.log(`Unknown S-expression: ${line.substring(0, 100)}`);
          }
          return;
        }
      } else if (parsed.type) {
        // Already WebSocket JSON format
        msg = parsed;
      } else if (parsed.cmd) {
        // Rendering command from TeXpresso (beginPage, fillText, etc.)
        // Wrap it in a render message type for the client
        msg = {
          type: 'render',
          command: parsed
        };
      } else {
        this.log(`Unknown message format: ${line.substring(0, 100)}`);
        return;
      }

      // Update internal state based on message type
      if (msg.type === 'doc.pageCount') {
        this.pageCount = msg.count;
      } else if (msg.type === 'status') {
        this.state = msg.state;

        // Handle initialization sequence
        this.handleInitializationState(msg);
      }

      // Forward to WebSocket client
      this.sendToClient(msg);

    } catch (err) {
      this.log(`Invalid JSON from texpresso: ${line.substring(0, 100)}`);
      this.log(`Parse error: ${err.message}`);
    }
  }

  /**
   * Handle stderr output (logging)
   * @param {Buffer} data
   */
  handleStderr(data) {
    const text = data.toString();
    this.stderrBuffer += text;

    // Split into lines
    const lines = this.stderrBuffer.split('\n');
    this.stderrBuffer = lines.pop() || ''; // Keep incomplete line

    for (const line of lines) {
      if (line.trim()) {
        this.log(`[texpresso] ${line}`);

        // Optionally forward to client
        if (this.config.forwardStderr) {
          this.sendToClient({
            type: 'log',
            level: 'info',
            message: line
          });
        }
      }
    }
  }

  /**
   * Handle message from WebSocket client
   * @param {import('./types.js').ClientMessage} msg
   */
  async handleClientMessage(msg) {
    try {
      switch (msg.type) {
        case 'file.open':
          this.handleFileOpen(msg);
          break;

        case 'file.changeRange':
          this.handleFileChangeRange(msg);
          break;

        case 'file.change':
          this.handleFileChange(msg);
          break;

        case 'file.changeLine':
          this.handleFileChangeLine(msg);
          break;

        case 'file.close':
          this.handleFileClose(msg);
          break;

        case 'nav.next':
        case 'nav.prev':
          const navSexp = toSExpression(msg);
          if (navSexp) {
            this.sendSExpression(navSexp);
          }
          break;

        case 'synctex.forward':
        case 'synctex.backward':
        case 'config.theme':
        case 'render.request':
          // Translate and forward to TeXpresso
          const sexp = toSExpression(msg);
          if (sexp) {
            this.sendSExpression(sexp);
          }
          break;

        case 'close':
          this.cleanup();
          break;

        default:
          this.sendToClient({
            type: 'error',
            code: 'UNKNOWN_MESSAGE_TYPE',
            message: `Unknown message type: ${msg.type}`
          });
      }
    } catch (err) {
      this.log(`Error handling message: ${err.message}`);
      this.sendToClient({
        type: 'error',
        code: 'MESSAGE_HANDLER_ERROR',
        message: err.message
      });
    }
  }

  /**
   * Handle file.open message
   * @param {import('./types.js').FileOpenMessage} msg
   */
  handleFileOpen(msg) {
    this.vfs.set(msg.path, { content: msg.content, dirty: false });
    // Convert to absolute path for TeXpresso
    const sexp = toSExpression({
      ...msg,
      path: this.toAbsolutePath(msg.path)
    });
    if (sexp) {
      this.sendSExpression(sexp);
    }
    this.log(`File opened: ${msg.path} (${msg.content.length} bytes)`);
  }

  /**
   * Handle file.changeRange message (line/column based)
   * @param {{type: string, path: string, startLine: number, startCol: number, endLine: number, endCol: number, text: string}} msg
   */
  handleFileChangeRange(msg) {
    const file = this.vfs.get(msg.path);
    if (!file) {
      this.sendToClient({
        type: 'error',
        code: 'FILE_NOT_FOUND',
        message: `File not found in VFS: ${msg.path}`
      });
      return;
    }

    this.log(`[TIMING] Received change-range from client at ${Date.now()}`);
    this.log(`File changed (change-range): ${msg.path} (${msg.startLine}:${msg.startCol} - ${msg.endLine}:${msg.endCol}) text="${msg.text}"`);

    // Apply change to VFS
    // Convert line/col to byte offsets
    const lines = file.content.split('\n');

    // Calculate start offset
    let startOffset = 0;
    for (let i = 0; i < msg.startLine && i < lines.length; i++) {
      startOffset += lines[i].length + 1; // +1 for newline
    }
    startOffset += msg.startCol;

    // Calculate end offset
    let endOffset = 0;
    for (let i = 0; i < msg.endLine && i < lines.length; i++) {
      endOffset += lines[i].length + 1;
    }
    endOffset += msg.endCol;

    // Apply the change
    const before = file.content.substring(0, startOffset);
    const after = file.content.substring(endOffset);
    file.content = before + msg.text + after;
    file.dirty = true;

    // Translate and send to TeXpresso (use absolute path to match open command)
    const sexp = toSExpression({
      ...msg,
      path: this.toAbsolutePath(msg.path)
    });
    if (sexp) {
      this.sendSExpression(sexp);
    }
  }

  /**
   * Handle file.change message (byte-offset based, legacy)
   * @param {import('./types.js').FileChangeMessage} msg
   */
  handleFileChange(msg) {
    const file = this.vfs.get(msg.path);
    if (!file) {
      this.sendToClient({
        type: 'error',
        code: 'FILE_NOT_FOUND',
        message: `File not found in VFS: ${msg.path}`
      });
      return;
    }

    // Apply changes to VFS
    let content = file.content;
    // Sort changes by offset (descending) to apply from end to start
    const sortedChanges = [...msg.changes].sort((a, b) => b.offset - a.offset);

    for (const change of sortedChanges) {
      const before = content.substring(0, change.offset);
      const after = content.substring(change.offset + change.length);
      content = before + change.text + after;
    }

    file.content = content;
    file.dirty = true;

    // Translate to S-expressions and send to TeXpresso with absolute path
    const sexps = translateFileChanges(this.toAbsolutePath(msg.path), msg.changes);
    for (const sexp of sexps) {
      this.sendSExpression(sexp);
    }

    this.log(`File changed: ${msg.path} (${msg.changes.length} changes)`);
  }

  /**
   * Handle file.changeLine message
   * @param {import('./types.js').FileChangeLineMessage} msg
   */
  handleFileChangeLine(msg) {
    const file = this.vfs.get(msg.path);
    if (!file) {
      this.sendToClient({
        type: 'error',
        code: 'FILE_NOT_FOUND',
        message: `File not found in VFS: ${msg.path}`
      });
      return;
    }

    // Split into lines and replace
    const lines = file.content.split('\n');
    const newLines = msg.text.split('\n');
    lines.splice(msg.startLine, msg.endLine - msg.startLine + 1, ...newLines);
    file.content = lines.join('\n');
    file.dirty = true;

    // Translate and forward to TeXpresso with absolute path
    const sexp = toSExpression({
      ...msg,
      path: this.toAbsolutePath(msg.path)
    });
    if (sexp) {
      this.sendSExpression(sexp);
    }
    this.log(`File changed (lines): ${msg.path} (${msg.startLine}-${msg.endLine})`);
  }

  /**
   * Handle file.close message
   * @param {import('./types.js').FileCloseMessage} msg
   */
  handleFileClose(msg) {
    this.vfs.delete(msg.path);
    // Convert to absolute path for TeXpresso
    const sexp = toSExpression({
      ...msg,
      path: this.toAbsolutePath(msg.path)
    });
    if (sexp) {
      this.sendSExpression(sexp);
    }
    this.log(`File closed: ${msg.path}`);
  }

  /**
   * Send S-expression to TeXpresso process via stdin
   * @param {any[]} sexp - S-expression as array
   */
  sendSExpression(sexp) {
    if (this.process && this.process.stdin.writable) {
      const json = JSON.stringify(sexp);
      if (this.config.verbose) {
        this.log(`Sending to TeXpresso: ${json}`);
      }
      this.process.stdin.write(json + '\n');
    } else {
      this.log('Cannot send to process: stdin not writable');
    }
  }

  /**
   * Send message to WebSocket client
   * @param {import('./types.js').ServerMessage} msg
   */
  sendToClient(msg) {
    if (this.ws.readyState === 1) { // WebSocket.OPEN
      this.ws.send(JSON.stringify(msg));
    } else {
      this.log(`Cannot send to client: WebSocket not open (state=${this.ws.readyState})`);
    }
  }

  /**
   * Handle initialization state machine
   * @param {Object} msg - Status message
   */
  handleInitializationState(msg) {
    if (this.initializationState === 'done') {
      return; // Already initialized
    }

    if (this.initializationState === 'waiting_first_compile' && msg.state === 'ready') {
      // First compilation finished, now send the 'open' command with VFS content
      this.log('Initial compilation complete, sending open command with VFS content');

      const initSexp = toSExpression({
        type: 'init',
        document: {
          name: this.toAbsolutePath(this.pendingInitDocument.name),
          content: this.pendingInitDocument.content
        }
      });

      if (initSexp) {
        this.sendSExpression(initSexp);
      }

      this.initializationState = 'waiting_open';
    } else if (this.initializationState === 'waiting_open' && msg.state === 'ready') {
      // Second compilation finished, now we're truly ready
      this.log('VFS open compilation complete, initialization done');
      this.initializationState = 'done';
      this.pendingInitDocument = null;
    }
  }

  /**
   * Log message with session ID prefix
   * @param {string} message
   */
  log(message) {
    const shortId = this.sessionId.split('-')[0];
    console.log(`[${shortId}] ${message}`);
  }

  /**
   * Clean up resources
   */
  cleanup() {
    this.log('Cleaning up session');

    if (this.rl) {
      this.rl.close();
      this.rl = null;
    }

    if (this.process) {
      if (this.isRunning) {
        this.process.kill('SIGTERM');

        // Force kill after timeout
        setTimeout(() => {
          if (this.isRunning) {
            this.log('Force killing process');
            this.process.kill('SIGKILL');
          }
        }, 5000);
      }
      this.process = null;
    }

    this.isRunning = false;
    this.vfs.clear();
  }

  /**
   * Get session statistics
   * @returns {Object}
   */
  getStats() {
    return {
      sessionId: this.sessionId,
      state: this.state,
      isRunning: this.isRunning,
      pageCount: this.pageCount,
      currentPage: this.currentPage,
      filesInVFS: this.vfs.size,
      dirtyFiles: Array.from(this.vfs.values()).filter(f => f.dirty).length
    };
  }
}
