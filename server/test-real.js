#!/usr/bin/env node

/**
 * Real integration test with actual TeXpresso headless mode
 * This test actually compiles a document and verifies the full flow
 */

import WebSocket from 'ws';
import { spawn } from 'child_process';
import { readFileSync } from 'fs';

const SERVER_PORT = 8383;
const SERVER_URL = `ws://localhost:${SERVER_PORT}`;

console.log('========================================');
console.log('Real Integration Test');
console.log('========================================\n');

// Read the actual test document
const testDoc = readFileSync('../test/simple.tex', 'utf8');
console.log(`✓ Loaded test document (${testDoc.length} bytes)\n`);

// Start the server
console.log('Starting WebSocket server...');
const serverProcess = spawn('node', ['src/index.js'], {
  cwd: import.meta.dirname,
  env: {
    ...process.env,
    PORT: SERVER_PORT,
    TEXPRESSO_PATH: '/Users/dominik/GitHub/texpresso/build/texpresso',
    WORKING_DIR: '/Users/dominik/GitHub/texpresso',
    VERBOSE: 'false'
  },
  stdio: ['ignore', 'pipe', 'pipe']
});

let serverOutput = '';
serverProcess.stdout.on('data', (data) => {
  serverOutput += data.toString();
  console.log(`[server] ${data.toString().trim()}`);
});

serverProcess.stderr.on('data', (data) => {
  console.error(`[server error] ${data.toString().trim()}`);
});

// Wait for server to start
await new Promise(resolve => setTimeout(resolve, 2000));

console.log('\nConnecting to server...');

const ws = new WebSocket(SERVER_URL);
const receivedMessages = [];
let testPassed = false;

ws.on('open', () => {
  console.log('✓ Connected to server\n');

  // Send init with the actual test document
  // Note: document name should be relative to working dir
  const initMsg = {
    type: 'init',
    document: {
      name: 'test/simple.tex',  // Relative to working dir (..)
      content: testDoc
    }
  };

  console.log('Sending init message with simple.tex...');
  ws.send(JSON.stringify(initMsg));
});

ws.on('message', (data) => {
  try {
    const msg = JSON.parse(data.toString());
    receivedMessages.push(msg);

    console.log(`\n<< Received: ${msg.type}`);

    if (msg.type === 'status') {
      console.log(`   State: ${msg.state}`);
    } else if (msg.type === 'doc.pageCount') {
      console.log(`   Page count: ${msg.count}`);
      if (msg.count === 1) {
        console.log('   ✓ Correct page count!');
      }
    } else if (msg.type === 'output.append') {
      console.log(`   Stream: ${msg.stream}`);
      console.log(`   Text: ${msg.text.substring(0, 50)}...`);
    } else if (msg.type === 'error') {
      console.log(`   ERROR: ${msg.code} - ${msg.message}`);
    } else {
      console.log(`   Data: ${JSON.stringify(msg).substring(0, 100)}...`);
    }

    // Check if we got the expected messages
    const hasStatus = receivedMessages.some(m => m.type === 'status' && m.state === 'ready');
    const hasPageCount = receivedMessages.some(m => m.type === 'doc.pageCount' && m.count === 1);

    if (hasStatus && hasPageCount) {
      console.log('\n========================================');
      console.log('✅ TEST PASSED!');
      console.log('========================================');
      console.log('Received:');
      console.log('  - Status: ready');
      console.log('  - Page count: 1');
      console.log('  - Total messages:', receivedMessages.length);
      testPassed = true;

      // Wait a bit then close
      setTimeout(() => {
        console.log('\nClosing connection...');
        ws.close();
      }, 1000);
    }

  } catch (err) {
    console.error(`\n✗ Failed to parse message: ${err.message}`);
    console.error(`  Raw data: ${data.toString().substring(0, 200)}`);
  }
});

ws.on('error', (err) => {
  console.error(`\n✗ WebSocket error: ${err.message}`);
  cleanup(false);
});

ws.on('close', () => {
  console.log('\nConnection closed');
  cleanup(testPassed);
});

// Cleanup and exit
function cleanup(success) {
  console.log('\nCleaning up...');

  // Kill server
  if (serverProcess) {
    serverProcess.kill('SIGTERM');
  }

  setTimeout(() => {
    console.log('\n========================================');
    if (success) {
      console.log('✅ All tests passed!');
      console.log(`Received ${receivedMessages.length} messages`);
      console.log('========================================\n');
      process.exit(0);
    } else {
      console.log('❌ Tests failed');
      console.log(`Received ${receivedMessages.length} messages`);
      console.log('========================================\n');
      process.exit(1);
    }
  }, 500);
}

// Timeout
setTimeout(() => {
  if (!testPassed) {
    console.log('\n⏱️  Test timeout');
    console.log(`Received ${receivedMessages.length} messages so far:`);
    receivedMessages.forEach((msg, i) => {
      console.log(`  ${i + 1}. ${msg.type}${msg.state ? ` (${msg.state})` : ''}`);
    });
    cleanup(false);
  }
}, 15000);
