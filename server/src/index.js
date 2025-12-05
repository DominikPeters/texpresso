#!/usr/bin/env node

/**
 * @fileoverview TeXpresso WebSocket Server
 *
 * This server manages WebSocket connections from browser clients and spawns
 * headless TeXpresso processes for live LaTeX compilation.
 *
 * Architecture:
 * - One WebSocket connection per client
 * - One TeXpresso process per session
 * - Communication via JSON over stdin/stdout
 * - VFS (Virtual File System) for document management
 */

import { WebSocketServer } from 'ws';
import { randomUUID } from 'crypto';
import { Session } from './session.js';

// ============================================================================
// Configuration
// ============================================================================

const config = {
  port: parseInt(process.env.PORT || '8080'),
  texpressoPath: process.env.TEXPRESSO_PATH || '../build/texpresso',
  workingDir: process.env.WORKING_DIR || process.cwd(),
  forwardStderr: process.env.FORWARD_STDERR === 'true',
  maxConnections: parseInt(process.env.MAX_CONNECTIONS || '100'),
  // Connection timeout in ms (0 = disabled)
  connectionTimeout: parseInt(process.env.CONNECTION_TIMEOUT || '0'),
  // Enable verbose logging
  verbose: process.env.VERBOSE === 'true'
};

// ============================================================================
// Session Management
// ============================================================================

/** @type {Map<string, Session>} */
const sessions = new Map();

/**
 * Create a new session
 * @param {string} sessionId
 * @param {import('ws').WebSocket} ws
 * @returns {Session}
 */
function createSession(sessionId, ws) {
  if (sessions.size >= config.maxConnections) {
    throw new Error(`Maximum connections (${config.maxConnections}) reached`);
  }

  const session = new Session(sessionId, ws, config);
  sessions.set(sessionId, session);

  log(`Session created (total: ${sessions.size})`);

  return session;
}

/**
 * Destroy a session
 * @param {string} sessionId
 */
function destroySession(sessionId) {
  const session = sessions.get(sessionId);
  if (session) {
    session.cleanup();
    sessions.delete(sessionId);
    log(`Session destroyed (total: ${sessions.size})`);
  }
}

// ============================================================================
// Message Validation
// ============================================================================

/**
 * Validate incoming client message
 * @param {any} msg
 * @returns {{valid: boolean, error?: string}}
 */
function validateMessage(msg) {
  if (!msg || typeof msg !== 'object') {
    return { valid: false, error: 'Message must be an object' };
  }

  if (!msg.type || typeof msg.type !== 'string') {
    return { valid: false, error: 'Message must have a type field' };
  }

  // Validate specific message types
  switch (msg.type) {
    case 'init':
      if (!msg.document || typeof msg.document !== 'object') {
        return { valid: false, error: 'init message requires document object' };
      }
      if (!msg.document.name || !msg.document.content) {
        return { valid: false, error: 'document must have name and content' };
      }
      break;

    case 'file.open':
      if (!msg.path || !msg.content) {
        return { valid: false, error: 'file.open requires path and content' };
      }
      break;

    case 'file.change':
      if (!msg.path || !Array.isArray(msg.changes)) {
        return { valid: false, error: 'file.change requires path and changes array' };
      }
      break;

    case 'file.changeLine':
      if (!msg.path || typeof msg.startLine !== 'number' || typeof msg.endLine !== 'number') {
        return { valid: false, error: 'file.changeLine requires path, startLine, endLine' };
      }
      break;

    case 'synctex.forward':
      if (!msg.path || typeof msg.line !== 'number') {
        return { valid: false, error: 'synctex.forward requires path and line' };
      }
      break;

    case 'synctex.backward':
      if (typeof msg.x !== 'number' || typeof msg.y !== 'number' || typeof msg.page !== 'number') {
        return { valid: false, error: 'synctex.backward requires x, y, and page' };
      }
      break;

    case 'nav.goto':
      if (typeof msg.page !== 'number') {
        return { valid: false, error: 'nav.goto requires page number' };
      }
      break;
  }

  return { valid: true };
}

// ============================================================================
// WebSocket Server
// ============================================================================

const wss = new WebSocketServer({
  port: config.port,
  // Increase payload size limit for large documents
  maxPayload: 10 * 1024 * 1024 // 10MB
});

log('='.repeat(70));
log('TeXpresso WebSocket Server');
log('='.repeat(70));
log(`Server:          ws://localhost:${config.port}`);
log(`TeXpresso:       ${config.texpressoPath}`);
log(`Working Dir:     ${config.workingDir}`);
log(`Max Connections: ${config.maxConnections}`);
log(`Verbose:         ${config.verbose}`);
log('='.repeat(70));

wss.on('connection', (ws, request) => {
  const sessionId = randomUUID();
  const clientIp = request.socket.remoteAddress;

  log(`[${sessionId.split('-')[0]}] New connection from ${clientIp}`);

  /** @type {Session | null} */
  let session = null;

  /** @type {NodeJS.Timeout | null} */
  let timeoutHandle = null;

  // Set connection timeout if configured
  if (config.connectionTimeout > 0) {
    timeoutHandle = setTimeout(() => {
      log(`[${sessionId.split('-')[0]}] Connection timeout`);
      ws.close(1000, 'Connection timeout');
    }, config.connectionTimeout);
  }

  // Handle incoming messages
  ws.on('message', async (data) => {
    // Reset timeout on activity
    if (timeoutHandle) {
      clearTimeout(timeoutHandle);
      if (config.connectionTimeout > 0) {
        timeoutHandle = setTimeout(() => {
          log(`[${sessionId.split('-')[0]}] Connection timeout`);
          ws.close(1000, 'Connection timeout');
        }, config.connectionTimeout);
      }
    }

    try {
      const msg = JSON.parse(data.toString());

      if (config.verbose) {
        log(`[${sessionId.split('-')[0]}] Received: ${msg.type}`);
      }

      // Validate message
      const validation = validateMessage(msg);
      if (!validation.valid) {
        ws.send(JSON.stringify({
          type: 'error',
          code: 'INVALID_MESSAGE',
          message: validation.error
        }));
        return;
      }

      // Handle init message
      if (msg.type === 'init') {
        if (session) {
          ws.send(JSON.stringify({
            type: 'error',
            code: 'ALREADY_INITIALIZED',
            message: 'Session already initialized'
          }));
          return;
        }

        try {
          session = createSession(sessionId, ws);
          await session.start(msg.document);

          // Send acknowledgment
          ws.send(JSON.stringify({
            type: 'status',
            state: 'ready',
            sessionId: sessionId
          }));

        } catch (err) {
          log(`[${sessionId.split('-')[0]}] Init failed: ${err.message}`);
          ws.send(JSON.stringify({
            type: 'error',
            code: 'INIT_FAILED',
            message: err.message
          }));
          ws.close(1011, 'Initialization failed');
        }
        return;
      }

      // Other messages require initialized session
      if (!session) {
        ws.send(JSON.stringify({
          type: 'error',
          code: 'NOT_INITIALIZED',
          message: 'Session not initialized. Send init message first.'
        }));
        return;
      }

      // Forward to session
      await session.handleClientMessage(msg);

    } catch (err) {
      log(`[${sessionId.split('-')[0]}] Error processing message: ${err.message}`);
      if (err instanceof SyntaxError) {
        ws.send(JSON.stringify({
          type: 'error',
          code: 'INVALID_JSON',
          message: 'Message is not valid JSON'
        }));
      } else {
        ws.send(JSON.stringify({
          type: 'error',
          code: 'INTERNAL_ERROR',
          message: err.message
        }));
      }
    }
  });

  // Handle connection close
  ws.on('close', (code, reason) => {
    if (timeoutHandle) {
      clearTimeout(timeoutHandle);
    }

    const reasonStr = reason.toString() || 'no reason';
    log(`[${sessionId.split('-')[0]}] Connection closed: ${code} (${reasonStr})`);

    if (session) {
      destroySession(sessionId);
    }
  });

  // Handle WebSocket errors
  ws.on('error', (err) => {
    log(`[${sessionId.split('-')[0]}] WebSocket error: ${err.message}`);
    if (session) {
      destroySession(sessionId);
    }
  });

  // Send ping to keep connection alive
  const pingInterval = setInterval(() => {
    if (ws.readyState === 1) { // WebSocket.OPEN
      ws.ping();
    } else {
      clearInterval(pingInterval);
    }
  }, 30000); // Every 30 seconds

  ws.on('close', () => clearInterval(pingInterval));
});

// Handle WebSocket server errors
wss.on('error', (err) => {
  log(`WebSocket server error: ${err.message}`);
});

// ============================================================================
// Graceful Shutdown
// ============================================================================

function shutdown() {
  log('\nShutting down gracefully...');

  // Close WebSocket server (stops accepting new connections)
  wss.close(() => {
    log('WebSocket server closed');
  });

  // Clean up all sessions
  let cleanedUp = 0;
  for (const session of sessions.values()) {
    session.cleanup();
    cleanedUp++;
  }
  sessions.clear();

  log(`Cleaned up ${cleanedUp} session(s)`);
  log('Shutdown complete');

  process.exit(0);
}

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);

// ============================================================================
// Status Reporting
// ============================================================================

// Log stats periodically
setInterval(() => {
  if (sessions.size > 0) {
    log(`Active sessions: ${sessions.size}`);
    if (config.verbose) {
      for (const session of sessions.values()) {
        const stats = session.getStats();
        log(`  ${stats.sessionId.split('-')[0]}: ${stats.state}, ` +
            `${stats.pageCount} pages, ${stats.filesInVFS} files in VFS`);
      }
    }
  }
}, 60000); // Every minute

// ============================================================================
// Utilities
// ============================================================================

/**
 * Log message with timestamp
 * @param {string} message
 */
function log(message) {
  const timestamp = new Date().toISOString();
  console.log(`[${timestamp}] ${message}`);
}
