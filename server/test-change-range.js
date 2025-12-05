#!/usr/bin/env node

/**
 * Test change-range recompilation via WebSocket server
 *
 * This test verifies that:
 * 1. Initial compilation works
 * 2. change-range commands trigger recompilation
 * 3. New rendering commands are sent after recompilation
 */

import WebSocket from 'ws';
import { spawn } from 'child_process';
import { readFileSync } from 'fs';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const rootDir = join(__dirname, '..');

const SERVER_PORT = 8484;
const SERVER_URL = `ws://localhost:${SERVER_PORT}`;

console.log('==========================================');
console.log('Change-Range Recompilation Test');
console.log('==========================================\n');

// Read the actual test document
const testDoc = readFileSync(join(rootDir, 'test/simple.tex'), 'utf8');
console.log(`Loaded test document (${testDoc.length} bytes)\n`);

// Start the server
console.log('Starting WebSocket server...');

const serverProcess = spawn('node', ['src/index.js'], {
  cwd: __dirname,
  env: {
    ...process.env,
    PORT: SERVER_PORT,
    TEXPRESSO_PATH: join(rootDir, 'build/texpresso'),
    WORKING_DIR: rootDir,
    VERBOSE: 'true'
  },
  stdio: ['ignore', 'pipe', 'pipe']
});

serverProcess.stdout.on('data', (data) => {
  if (process.env.VERBOSE === 'true') {
    console.log(`[server] ${data.toString().trim()}`);
  }
});

serverProcess.stderr.on('data', (data) => {
  console.error(`[server error] ${data.toString().trim()}`);
});

// Wait for server to start
await new Promise(resolve => setTimeout(resolve, 2000));

console.log('Connecting to server...\n');

const ws = new WebSocket(SERVER_URL);
const receivedMessages = [];
let testPhase = 'init'; // init, waiting_for_compilation, change_sent, checking_recompilation
let initialPageCount = 0;
let recompilationTriggered = false;
let testPassed = false;

ws.on('open', () => {
  console.log('[1] Connected to server');

  // Send init with the actual test document
  const initMsg = {
    type: 'init',
    document: {
      name: 'test/simple.tex',
      content: testDoc
    }
  };

  console.log('[2] Sending init message...');
  ws.send(JSON.stringify(initMsg));
  testPhase = 'waiting_for_compilation';
});

ws.on('message', (data) => {
  try {
    const msg = JSON.parse(data.toString());
    receivedMessages.push(msg);

    if (msg.type === 'doc.pageCount') {
      if (testPhase === 'waiting_for_compilation') {
        initialPageCount = msg.count;
        console.log(`[3] Initial compilation complete: ${msg.count} page(s)`);

        // Wait a bit then send change-range command
        setTimeout(() => {
          testPhase = 'change_sent';

          // Change "very simple" to "MODIFIED" on line 4
          // Line 4 (0-indexed): "  This is a very simple file..."
          const changeMsg = {
            type: 'file.changeRange',
            path: 'test/simple.tex',
            startLine: 4,
            startCol: 14,
            endLine: 4,
            endCol: 25,
            text: 'MODIFIED'
          };

          console.log('[4] Sending change-range command...');
          console.log(`    Replacing "very simple" with "MODIFIED"`);
          ws.send(JSON.stringify(changeMsg));
          testPhase = 'checking_recompilation';
        }, 1000);

      } else if (testPhase === 'checking_recompilation') {
        // Got another page count - recompilation happened!
        recompilationTriggered = true;
        console.log(`[5] Recompilation triggered! Page count: ${msg.count}`);

        // Success!
        testPassed = true;
        console.log('\n==========================================');
        console.log('TEST PASSED!');
        console.log('==========================================');
        console.log('- Initial compilation: OK');
        console.log('- Change-range command: Sent');
        console.log('- Recompilation: Triggered');
        console.log(`- Total messages received: ${receivedMessages.length}`);

        setTimeout(() => ws.close(), 500);
      }
    } else if (msg.type === 'status' && msg.state === 'compiling' && testPhase === 'checking_recompilation') {
      console.log(`    Compiling...`);
    }

  } catch (err) {
    console.error(`Failed to parse message: ${err.message}`);
  }
});

ws.on('error', (err) => {
  console.error(`WebSocket error: ${err.message}`);
  cleanup(false);
});

ws.on('close', () => {
  console.log('\nConnection closed');
  cleanup(testPassed);
});

function cleanup(success) {
  if (serverProcess) {
    serverProcess.kill('SIGTERM');
  }

  setTimeout(() => {
    process.exit(success ? 0 : 1);
  }, 500);
}

// Timeout
setTimeout(() => {
  if (!testPassed) {
    console.log('\nTest timeout');
    console.log(`Phase: ${testPhase}`);
    console.log(`Received ${receivedMessages.length} messages`);
    console.log(`Recompilation triggered: ${recompilationTriggered}`);
    cleanup(false);
  }
}, 30000);
