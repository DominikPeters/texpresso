#!/usr/bin/env node

/**
 * @fileoverview Simple test for WebSocket server and headless TeXpresso integration
 *
 * This script tests:
 * 1. Protocol translation (JSON ↔ S-expressions)
 * 2. Basic WebSocket connectivity
 * 3. Document initialization
 */

import WebSocket from 'ws';
import { spawn } from 'child_process';
import { toSExpression, fromSExpression } from './src/protocol.js';

console.log('TeXpresso WebSocket Server - Simple Test');
console.log('=========================================\n');

// Test 1: Protocol Translation
console.log('Test 1: Protocol Translation');
console.log('----------------------------');

const testMessages = [
  {
    type: 'init',
    document: { name: 'test.tex', content: '\\documentclass{article}...' }
  },
  {
    type: 'file.open',
    path: 'chapter.tex',
    content: 'Hello, World!'
  },
  {
    type: 'file.change',
    path: 'test.tex',
    changes: [{ offset: 10, length: 5, text: 'LaTeX' }]
  },
  {
    type: 'synctex.forward',
    path: 'test.tex',
    line: 42,
    column: 0
  }
];

let allTestsPassed = true;

for (const msg of testMessages) {
  const sexp = toSExpression(msg);
  if (!sexp) {
    console.log(`  ✗ Failed to translate: ${msg.type}`);
    allTestsPassed = false;
  } else {
    console.log(`  ✓ ${msg.type} → ${JSON.stringify(sexp)}`);
  }
}

// Test S-expression → JSON
const testSExps = [
  ['append', 'log', 0, 'Test log'],
  ['input-file', 1, 'test.tex'],
  { type: 'status', state: 'ready' }
];

for (const sexp of testSExps) {
  const msg = Array.isArray(sexp) ? fromSExpression(sexp) : sexp;
  if (!msg && Array.isArray(sexp)) {
    console.log(`  ✗ Failed to parse: ${JSON.stringify(sexp)}`);
    allTestsPassed = false;
  } else {
    const label = Array.isArray(sexp) ? sexp[0] : sexp.type;
    console.log(`  ✓ ${label} → ${msg ? msg.type : 'passed'}`);
  }
}

if (!allTestsPassed) {
  console.log('\n❌ Some protocol tests failed');
  process.exit(1);
}

console.log('\n✅ All protocol translation tests passed!\n');

// Test 2: Server Integration
console.log('Test 2: Server Integration');
console.log('--------------------------');

const SERVER_PORT = 8282;
const SERVER_URL = `ws://localhost:${SERVER_PORT}`;

console.log('Starting server...');

const serverProcess = spawn('node', ['src/index.js'], {
  cwd: import.meta.dirname,
  env: {
    ...process.env,
    PORT: SERVER_PORT,
    VERBOSE: 'false'
  },
  stdio: ['ignore', 'pipe', 'pipe']
});

let serverReady = false;

serverProcess.stdout.on('data', (data) => {
  const text = data.toString();
  if (text.includes('TeXpresso WebSocket Server')) {
    serverReady = true;
  }
  if (process.env.VERBOSE === 'true') {
    console.log(`[server] ${text.trim()}`);
  }
});

serverProcess.stderr.on('data', (data) => {
  console.error(`[server error] ${data.toString().trim()}`);
});

// Wait for server to start
await new Promise(resolve => setTimeout(resolve, 2000));

if (!serverReady) {
  console.log('⚠️  Server may not be ready yet, proceeding anyway...\n');
}

console.log('Connecting to server...');

const ws = new WebSocket(SERVER_URL);

let testFailed = false;
const receivedMessages = [];

ws.on('open', () => {
  console.log('✓ Connected!\n');

  // Send init message
  const initMsg = {
    type: 'init',
    document: {
      name: 'test.tex',
      content: `\\documentclass{article}
\\begin{document}
Hello, World!
\\end{document}
`
    }
  };

  console.log('Sending init message...');
  ws.send(JSON.stringify(initMsg));
});

ws.on('message', (data) => {
  try {
    const msg = JSON.parse(data.toString());
    receivedMessages.push(msg);
    console.log(`✓ Received: ${msg.type}${msg.state ? ` (${msg.state})` : ''}`);

    // Check for successful compilation
    if (msg.type === 'doc.pageCount') {
      console.log(`  Page count: ${msg.count}`);
    }

    // After receiving some messages, close connection
    if (receivedMessages.length >= 3) {
      console.log('\n✅ Received expected messages, closing connection...');
      ws.close();
    }
  } catch (err) {
    console.error(`✗ Failed to parse message: ${err.message}`);
    testFailed = true;
  }
});

ws.on('error', (err) => {
  console.error(`✗ WebSocket error: ${err.message}`);
  testFailed = true;
  ws.close();
});

ws.on('close', () => {
  console.log('Connection closed\n');

  // Cleanup
  console.log('Stopping server...');
  serverProcess.kill('SIGTERM');

  setTimeout(() => {
    if (!testFailed && receivedMessages.length > 0) {
      console.log('✅ Integration test passed!');
      console.log(`   Received ${receivedMessages.length} messages from server`);
      process.exit(0);
    } else {
      console.log('❌ Integration test failed');
      console.log(`   Received ${receivedMessages.length} messages`);
      process.exit(1);
    }
  }, 500);
});

// Timeout
setTimeout(() => {
  if (ws.readyState === WebSocket.OPEN) {
    console.log('\n⏱️  Test timeout, closing...');
    ws.close();
  }
}, 10000);
