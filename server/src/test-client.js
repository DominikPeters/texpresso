#!/usr/bin/env node

/**
 * @fileoverview Test client for TeXpresso WebSocket server
 *
 * This is a simple command-line client for testing the WebSocket server.
 * It can be used to manually test protocol messages and server behavior.
 *
 * Usage:
 *   node test-client.js [ws://localhost:8080]
 */

import WebSocket from 'ws';
import * as readline from 'readline';

const serverUrl = process.argv[2] || 'ws://localhost:8080';

console.log('TeXpresso WebSocket Test Client');
console.log('================================\n');
console.log(`Connecting to: ${serverUrl}`);

const ws = new WebSocket(serverUrl);

// Simple LaTeX document for testing
const testDocument = {
  name: 'test.tex',
  content: `\\documentclass{article}
\\begin{document}
Hello, World!

This is a test document for TeXpresso.

\\section{Introduction}
This is the introduction section.

\\end{document}
`
};

ws.on('open', () => {
  console.log('Connected!\n');

  // Send init message
  const initMsg = {
    type: 'init',
    document: testDocument
  };

  console.log('Sending init message...');
  ws.send(JSON.stringify(initMsg));

  // Start interactive mode
  startInteractive();
});

ws.on('message', (data) => {
  try {
    const msg = JSON.parse(data.toString());
    console.log('\n<< Received:', JSON.stringify(msg, null, 2));
  } catch (err) {
    console.log('\n<< Received (non-JSON):', data.toString());
  }
});

ws.on('close', (code, reason) => {
  console.log(`\nConnection closed: ${code} (${reason})`);
  process.exit(0);
});

ws.on('error', (err) => {
  console.error('WebSocket error:', err.message);
  process.exit(1);
});

// ============================================================================
// Interactive Mode
// ============================================================================

function startInteractive() {
  const rl = readline.createInterface({
    input: process.stdin,
    output: process.stdout,
    prompt: '\n> '
  });

  console.log('\nInteractive mode - Type a command or "help" for list of commands');
  rl.prompt();

  rl.on('line', (line) => {
    const trimmed = line.trim();

    if (!trimmed) {
      rl.prompt();
      return;
    }

    try {
      handleCommand(trimmed);
    } catch (err) {
      console.error('Error:', err.message);
    }

    rl.prompt();
  });

  rl.on('close', () => {
    console.log('\nClosing connection...');
    ws.close();
  });
}

/**
 * Handle interactive commands
 * @param {string} input
 */
function handleCommand(input) {
  const parts = input.split(/\s+/);
  const cmd = parts[0].toLowerCase();

  switch (cmd) {
    case 'help':
      showHelp();
      break;

    case 'change':
      sendFileChange();
      break;

    case 'goto':
      if (parts.length < 2) {
        console.log('Usage: goto <page>');
        break;
      }
      sendGotoPage(parseInt(parts[1]));
      break;

    case 'forward':
      if (parts.length < 2) {
        console.log('Usage: forward <line>');
        break;
      }
      sendSyncTexForward(parseInt(parts[1]));
      break;

    case 'backward':
      if (parts.length < 4) {
        console.log('Usage: backward <x> <y> <page>');
        break;
      }
      sendSyncTexBackward(
        parseFloat(parts[1]),
        parseFloat(parts[2]),
        parseInt(parts[3])
      );
      break;

    case 'render':
      if (parts.length < 2) {
        console.log('Usage: render <page>');
        break;
      }
      sendRenderRequest(parseInt(parts[1]));
      break;

    case 'json':
      // Send raw JSON
      const json = input.substring(4).trim();
      sendRaw(json);
      break;

    case 'quit':
    case 'exit':
      console.log('Closing connection...');
      ws.close();
      process.exit(0);
      break;

    default:
      console.log(`Unknown command: ${cmd}`);
      console.log('Type "help" for list of commands');
  }
}

function showHelp() {
  console.log(`
Available commands:
  help                    Show this help message
  change                  Send a test file change
  goto <page>             Navigate to page
  forward <line>          SyncTeX forward search
  backward <x> <y> <page> SyncTeX backward search
  render <page>           Request page render
  json <json>             Send raw JSON message
  quit, exit              Close connection and exit

Examples:
  > goto 2
  > forward 10
  > backward 150.5 300.2 1
  > json {"type":"nav.goto","page":1}
  `);
}

// ============================================================================
// Message Senders
// ============================================================================

function send(msg) {
  console.log('>> Sending:', JSON.stringify(msg, null, 2));
  ws.send(JSON.stringify(msg));
}

function sendFileChange() {
  send({
    type: 'file.change',
    path: testDocument.name,
    changes: [
      {
        offset: 50,
        length: 5,
        text: 'LaTeX'
      }
    ]
  });
}

function sendGotoPage(page) {
  send({
    type: 'nav.goto',
    page: page
  });
}

function sendSyncTexForward(line) {
  send({
    type: 'synctex.forward',
    path: testDocument.name,
    line: line,
    column: 0
  });
}

function sendSyncTexBackward(x, y, page) {
  send({
    type: 'synctex.backward',
    x: x,
    y: y,
    page: page
  });
}

function sendRenderRequest(page) {
  send({
    type: 'render.request',
    page: page,
    scale: 1.0
  });
}

function sendRaw(json) {
  try {
    const msg = JSON.parse(json);
    send(msg);
  } catch (err) {
    console.error('Invalid JSON:', err.message);
  }
}
