/**
 * TeXpresso WebSocket Client
 * Handles communication with the TeXpresso server
 */

export class TeXpressoClient {
  constructor(url) {
    this.url = url;
    this.ws = null;
    this.connected = false;
    this.listeners = new Map();
  }

  /**
   * Connect to the WebSocket server
   */
  connect() {
    if (this.ws) {
      console.warn('Already connected or connecting');
      return;
    }

    console.log(`Connecting to ${this.url}...`);
    this.ws = new WebSocket(this.url);

    this.ws.onopen = () => {
      console.log('WebSocket connected');
      this.connected = true;
      this.emit('connected');
    };

    this.ws.onclose = (event) => {
      console.log('WebSocket closed:', event.code, event.reason);
      this.connected = false;
      this.ws = null;
      this.emit('disconnected', { code: event.code, reason: event.reason });
    };

    this.ws.onerror = (error) => {
      console.error('WebSocket error:', error);
      this.emit('error', error);
    };

    this.ws.onmessage = (event) => {
      try {
        const message = JSON.parse(event.data);
        this.handleMessage(message);
      } catch (err) {
        console.error('Failed to parse message:', err, event.data);
      }
    };
  }

  /**
   * Disconnect from the server
   */
  disconnect() {
    if (this.ws) {
      this.ws.close();
      this.ws = null;
      this.connected = false;
    }
  }

  /**
   * Check if connected
   */
  isConnected() {
    return this.connected && this.ws && this.ws.readyState === WebSocket.OPEN;
  }

  /**
   * Send a message to the server
   */
  send(message) {
    if (!this.isConnected()) {
      console.warn('Not connected, cannot send message:', message);
      return false;
    }

    try {
      this.ws.send(JSON.stringify(message));
      return true;
    } catch (err) {
      console.error('Failed to send message:', err);
      return false;
    }
  }

  /**
   * Initialize session with a document
   */
  init(name, content) {
    return this.send({
      type: 'init',
      document: {
        name,
        content
      }
    });
  }

  /**
   * Send a file change command using change-range
   */
  sendChangeRange(path, startLine, startCol, endLine, endCol, text) {
    return this.send({
      type: 'file.changeRange',
      path,
      startLine,
      startCol,
      endLine,
      endCol,
      text
    });
  }

  /**
   * Send a file change command (legacy byte-offset based)
   */
  sendChange(path, offset, length, text) {
    return this.send({
      type: 'file.change',
      path,
      changes: [{ offset, length, text }]
    });
  }

  /**
   * Open a file in the VFS
   */
  openFile(path, content) {
    return this.send({
      type: 'file.open',
      path,
      content
    });
  }

  /**
   * Close a file in the VFS
   */
  closeFile(path) {
    return this.send({
      type: 'file.close',
      path
    });
  }

  /**
   * Go to next page
   */
  nextPage() {
    console.log('[Client] Sending nav.next');
    return this.send({ type: 'nav.next' });
  }

  /**
   * Go to previous page
   */
  prevPage() {
    console.log('[Client] Sending nav.prev');
    return this.send({ type: 'nav.prev' });
  }

  /**
   * SyncTeX forward search
   */
  syncTexForward(path, line, column = 0) {
    return this.send({
      type: 'synctex.forward',
      path,
      line,
      column
    });
  }

  /**
   * SyncTeX backward search
   */
  syncTexBackward(x, y, page) {
    return this.send({
      type: 'synctex.backward',
      x,
      y,
      page
    });
  }

  /**
   * Handle incoming messages
   */
  handleMessage(message) {
    const { type } = message;

    // Emit specific event for this message type
    this.emit(type, message);

    // Also emit a generic 'message' event
    this.emit('message', message);
  }

  /**
   * Register an event listener
   */
  on(event, callback) {
    if (!this.listeners.has(event)) {
      this.listeners.set(event, []);
    }
    this.listeners.get(event).push(callback);
  }

  /**
   * Remove an event listener
   */
  off(event, callback) {
    if (!this.listeners.has(event)) return;

    const callbacks = this.listeners.get(event);
    const index = callbacks.indexOf(callback);
    if (index !== -1) {
      callbacks.splice(index, 1);
    }
  }

  /**
   * Emit an event
   */
  emit(event, data) {
    if (!this.listeners.has(event)) return;

    const callbacks = this.listeners.get(event);
    for (const callback of callbacks) {
      try {
        callback(data);
      } catch (err) {
        console.error(`Error in ${event} listener:`, err);
      }
    }
  }
}
