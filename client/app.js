/**
 * TeXpresso Web Client
 * Simple frontend for live LaTeX editing with WebSocket server
 */

import { TeXpressoClient } from './client.js';
import { EditorManager } from './editor.js';
import { LogViewer } from './log.js';
import { Viewer } from './viewer.js';
import { Renderer } from './renderer.js';

// Configuration
const WS_URL = 'ws://localhost:8080';
const DOCUMENT_NAME = 'document.tex';

// UI Elements
const elements = {
  editor: document.getElementById('editor'),
  logOutput: document.getElementById('log-output'),
  btnConnect: document.getElementById('btn-connect'),
  btnCompile: document.getElementById('btn-compile'),
  btnClearLog: document.getElementById('btn-clear-log'),
  connectionStatus: document.getElementById('connection-status'),
  statusText: document.getElementById('status-text'),
  pageCount: document.getElementById('page-count'),
  charCount: document.getElementById('char-count'),
  cursorPos: document.getElementById('cursor-pos'),
  logLines: document.getElementById('log-lines'),
  wsUrl: document.getElementById('ws-url'),
  // Viewer elements
  viewerContainer: document.getElementById('viewer-container'),
  btnPrevPage: document.getElementById('btn-prev-page'),
  btnNextPage: document.getElementById('btn-next-page'),
  currentPage: document.getElementById('current-page'),
  totalPages: document.getElementById('total-pages'),
  btnZoomIn: document.getElementById('btn-zoom-in'),
  btnZoomOut: document.getElementById('btn-zoom-out'),
  zoomLevel: document.getElementById('zoom-level'),
};

// Initialize components
const client = new TeXpressoClient(WS_URL);
const editor = new EditorManager(elements.editor, DOCUMENT_NAME);
const log = new LogViewer(elements.logOutput);
const viewer = new Viewer(elements.viewerContainer);
const renderer = new Renderer(viewer);

// Set up viewer callbacks
viewer.onPageChange = (currentPage, pageCount) => {
  elements.currentPage.textContent = currentPage + 1;
  elements.totalPages.textContent = pageCount;
  elements.btnPrevPage.disabled = currentPage <= 0;
  elements.btnNextPage.disabled = currentPage >= pageCount - 1;
};

viewer.onZoomChange = (zoom) => {
  elements.zoomLevel.textContent = viewer.getZoomString();
};

viewer.onReRender = (page, commands, dimensions) => {
  renderer.replayCommands(page, commands, dimensions);
};

// Update UI with WebSocket URL
elements.wsUrl.textContent = WS_URL;

// ============================================================================
// WebSocket Event Handlers
// ============================================================================

client.on('connected', () => {
  console.log('Connected to server');
  updateConnectionStatus('connected', 'Connected');
  elements.btnConnect.textContent = 'Disconnect';
  elements.btnCompile.disabled = false;

  // Send initial document
  const content = elements.editor.value;
  client.init(DOCUMENT_NAME, content);

  // Initialize editor tracking with current content
  editor.setInitialContent(content);

  log.success('Connected to TeXpresso server');
});

client.on('disconnected', () => {
  console.log('Disconnected from server');
  updateConnectionStatus('disconnected', 'Disconnected');
  elements.btnConnect.textContent = 'Connect';
  elements.btnCompile.disabled = true;

  log.error('Disconnected from server');
});

client.on('error', (error) => {
  console.error('WebSocket error:', error);
  log.error(`Error: ${error.message}`);
});

client.on('status', (data) => {
  console.log('Status:', data.state);

  if (data.state === 'compiling') {
    updateConnectionStatus('compiling', 'Compiling...');
  } else if (data.state === 'ready') {
    updateConnectionStatus('connected', 'Ready');
  } else if (data.state === 'error') {
    updateConnectionStatus('disconnected', 'Error');
    log.error('Compilation error');
  }
});

client.on('log', (data) => {
  const prefix = data.level === 'error' ? '❌' :
                 data.level === 'warning' ? '⚠️' : 'ℹ️';

  const location = data.file && data.line ? ` (${data.file}:${data.line})` : '';
  log.append(`${prefix} ${data.message}${location}`, data.level);
});

client.on('output.append', (data) => {
  log.append(data.text, 'info');
});

client.on('doc.pageCount', (data) => {
  console.log('Page count:', data.count);
  elements.pageCount.textContent = `${data.count} page${data.count !== 1 ? 's' : ''}`;
  viewer.setPageCount(data.count);
  log.success(`Document compiled: ${data.count} page(s)`);
});

client.on('doc.inputFile', (data) => {
  console.log('Input file:', data.path);
});

client.on('render', (data) => {
  // Pass rendering commands to the renderer
  renderer.processCommand(data.command);
});

client.on('message', (data) => {
  // Catch-all for other message types
  console.log('Received message:', data.type, data);
});

// ============================================================================
// Editor Event Handlers
// ============================================================================

editor.on('change', (changes) => {
  console.log('Editor changes:', changes);

  // Send changes to server using change-range
  if (client.isConnected()) {
    for (const change of changes) {
      client.sendChangeRange(
        DOCUMENT_NAME,
        change.startLine,
        change.startCol,
        change.endLine,
        change.endCol,
        change.text
      );
    }
  }

  updateStats();
});

// Update stats on cursor movement
elements.editor.addEventListener('selectionchange', updateCursorPosition);
elements.editor.addEventListener('keyup', updateCursorPosition);
elements.editor.addEventListener('click', updateCursorPosition);

// ============================================================================
// Button Handlers
// ============================================================================

elements.btnConnect.addEventListener('click', () => {
  if (client.isConnected()) {
    client.disconnect();
  } else {
    client.connect();
  }
});

elements.btnCompile.addEventListener('click', () => {
  log.info('Requesting recompilation...');
  // Force recompilation by sending the whole document again
  const content = elements.editor.value;
  client.sendChange(DOCUMENT_NAME, 0, content.length, content);
});

elements.btnClearLog.addEventListener('click', () => {
  log.clear();
});

// Viewer controls
elements.btnZoomIn.addEventListener('click', () => {
  viewer.zoomIn();
});

elements.btnZoomOut.addEventListener('click', () => {
  viewer.zoomOut();
});

// ============================================================================
// Panel Resizing
// ============================================================================

function initResizers() {
  const resizers = document.querySelectorAll('.resizer');

  resizers.forEach(resizer => {
    let isResizing = false;
    let startX = 0;
    let startWidth = 0;
    let pane = null;

    resizer.addEventListener('mousedown', (e) => {
      isResizing = true;
      pane = resizer.parentElement;
      startX = e.clientX;
      startWidth = pane.offsetWidth;
      resizer.classList.add('resizing');
      document.body.style.cursor = 'col-resize';
      document.body.style.userSelect = 'none';
      e.preventDefault();
    });

    document.addEventListener('mousemove', (e) => {
      if (!isResizing) return;

      const dx = e.clientX - startX;
      const newWidth = startWidth + dx;
      const minWidth = parseInt(getComputedStyle(pane).minWidth) || 200;

      if (newWidth >= minWidth) {
        // Calculate flex-grow based on new width relative to container
        const mainWidth = pane.parentElement.offsetWidth;
        const flexGrow = newWidth / (mainWidth / 3.5); // Approximate flex distribution
        pane.style.flex = `${flexGrow} 1 0`;
      }
    });

    document.addEventListener('mouseup', () => {
      if (isResizing) {
        isResizing = false;
        resizer.classList.remove('resizing');
        document.body.style.cursor = '';
        document.body.style.userSelect = '';
      }
    });
  });
}

// ============================================================================
// UI Update Functions
// ============================================================================

function updateConnectionStatus(status, text) {
  elements.connectionStatus.className = `status-indicator ${status}`;
  elements.statusText.textContent = text;
}

function updateStats() {
  const content = elements.editor.value;
  elements.charCount.textContent = `${content.length} characters`;
  elements.logLines.textContent = `${log.getLineCount()} lines`;
}

function updateCursorPosition() {
  const editor = elements.editor;
  const pos = editor.selectionStart;
  const text = editor.value.substring(0, pos);
  const lines = text.split('\n');
  const line = lines.length;
  const col = lines[lines.length - 1].length + 1;

  elements.cursorPos.textContent = `Line ${line}, Col ${col}`;
}

// ============================================================================
// Initialization
// ============================================================================

function init() {
  console.log('TeXpresso Web Client initialized');
  updateStats();
  updateCursorPosition();
  initResizers();

  // Listen for DPR changes (when window moves between displays)
  if (window.matchMedia) {
    const updateDPR = () => viewer.updateDPR();
    window.matchMedia(`(resolution: ${window.devicePixelRatio}dppx)`).addEventListener('change', updateDPR);
  }

  // Auto-connect on load
  setTimeout(() => {
    client.connect();
  }, 500);
}

// Start the application
init();
