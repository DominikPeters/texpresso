#!/usr/bin/env node

import { WebSocketServer } from 'ws';
import { spawn } from 'child_process';
import { createInterface } from 'readline';
import { randomUUID } from 'crypto';

const PORT = process.env.PORT || 8080;
const TEXPRESSO_PATH = process.env.TEXPRESSO_PATH || '../build/texpresso';

// Session management
const sessions = new Map();

class Session {
  constructor(sessionId, ws, docPath) {
    this.sessionId = sessionId;
    this.ws = ws;
    this.docPath = docPath;
    this.process = null;
    this.rl = null;
  }

  start() {
    console.log(`[${this.sessionId}] Starting TeXpresso process for ${this.docPath}`);

    // Spawn headless TeXpresso process
    this.process = spawn(TEXPRESSO_PATH, ['--headless', this.docPath], {
      stdio: ['pipe', 'pipe', 'pipe']
    });

    // Read JSON lines from stdout
    this.rl = createInterface({
      input: this.process.stdout,
      crlfDelay: Infinity
    });

    this.rl.on('line', (line) => {
      try {
        // Validate JSON
        JSON.parse(line);
        // Forward to WebSocket client
        this.ws.send(line);
      } catch (err) {
        console.error(`[${this.sessionId}] Invalid JSON from texpresso: ${line}`);
      }
    });

    // Handle stderr (for debugging/logging)
    this.process.stderr.on('data', (data) => {
      console.log(`[${this.sessionId}] ${data.toString().trim()}`);
    });

    // Handle process exit
    this.process.on('exit', (code) => {
      console.log(`[${this.sessionId}] TeXpresso exited with code ${code}`);
      this.cleanup();
    });
  }

  send(message) {
    if (this.process && this.process.stdin.writable) {
      this.process.stdin.write(message + '\n');
    }
  }

  cleanup() {
    if (this.rl) {
      this.rl.close();
    }
    if (this.process) {
      this.process.kill();
    }
    sessions.delete(this.sessionId);
  }
}

// Create WebSocket server
const wss = new WebSocketServer({ port: PORT });

console.log(`TeXpresso WebSocket server running on ws://localhost:${PORT}`);
console.log(`Using TeXpresso at: ${TEXPRESSO_PATH}`);

wss.on('connection', (ws) => {
  const sessionId = randomUUID();
  console.log(`[${sessionId}] New connection`);

  let session = null;

  ws.on('message', (data) => {
    try {
      const msg = JSON.parse(data.toString());
      console.log(`[${sessionId}] Received:`, msg.type);

      if (msg.type === 'init') {
        // Initialize session with document
        const docPath = msg.document?.path || 'test/simple.tex';
        session = new Session(sessionId, ws, docPath);
        sessions.set(sessionId, session);
        session.start();
      } else if (session) {
        // Forward message to TeXpresso process
        session.send(JSON.stringify(msg));
      } else {
        ws.send(JSON.stringify({
          type: 'error',
          code: 'NOT_INITIALIZED',
          message: 'Session not initialized. Send init message first.'
        }));
      }
    } catch (err) {
      console.error(`[${sessionId}] Error processing message:`, err);
      ws.send(JSON.stringify({
        type: 'error',
        code: 'INVALID_MESSAGE',
        message: err.message
      }));
    }
  });

  ws.on('close', () => {
    console.log(`[${sessionId}] Connection closed`);
    if (session) {
      session.cleanup();
    }
  });

  ws.on('error', (err) => {
    console.error(`[${sessionId}] WebSocket error:`, err);
    if (session) {
      session.cleanup();
    }
  });
});

// Graceful shutdown
process.on('SIGINT', () => {
  console.log('\nShutting down...');
  for (const session of sessions.values()) {
    session.cleanup();
  }
  wss.close(() => {
    console.log('Server closed');
    process.exit(0);
  });
});
