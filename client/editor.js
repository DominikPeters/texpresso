/**
 * Editor Manager
 * Tracks changes and generates deltas for the TeXpresso protocol
 */

export class EditorManager {
  constructor(textarea, documentName) {
    this.textarea = textarea;
    this.documentName = documentName;
    this.content = textarea.value;
    this.listeners = new Map();

    // Debounce timer for change events
    this.changeTimeout = null;
    this.changeDelay = 500; // ms

    this.setupListeners();
  }

  setupListeners() {
    // Track all input changes
    this.textarea.addEventListener('input', () => {
      this.handleInput();
    });

    // Also track paste events
    this.textarea.addEventListener('paste', () => {
      // Input event will fire, so we just need to ensure it's tracked
      setTimeout(() => this.handleInput(), 0);
    });
  }

  handleInput() {
    // Clear existing timeout
    if (this.changeTimeout) {
      clearTimeout(this.changeTimeout);
    }

    // Debounce changes to avoid flooding the server
    this.changeTimeout = setTimeout(() => {
      this.processChanges();
    }, this.changeDelay);
  }

  processChanges() {
    const newContent = this.textarea.value;

    if (newContent === this.content) {
      return; // No changes
    }

    // Calculate the changes
    const changes = this.calculateChanges(this.content, newContent);

    // Update stored content
    this.content = newContent;

    // Emit change event
    if (changes.length > 0) {
      this.emit('change', changes);
    }
  }

  /**
   * Calculate changes between old and new content
   * Returns array of {startLine, startCol, endLine, endCol, text} changes
   * Uses line/column coordinates (change-range command)
   */
  calculateChanges(oldContent, newContent) {
    // Simple approach: find the common prefix and suffix
    const oldLen = oldContent.length;
    const newLen = newContent.length;

    // Find common prefix length
    let prefixLen = 0;
    while (prefixLen < oldLen && prefixLen < newLen &&
           oldContent[prefixLen] === newContent[prefixLen]) {
      prefixLen++;
    }

    // Find common suffix length
    let suffixLen = 0;
    while (suffixLen < (oldLen - prefixLen) &&
           suffixLen < (newLen - prefixLen) &&
           oldContent[oldLen - 1 - suffixLen] === newContent[newLen - 1 - suffixLen]) {
      suffixLen++;
    }

    // Calculate the replacement text
    const insertText = newContent.substring(prefixLen, newLen - suffixLen);

    // Convert byte offsets to line/column positions
    const startPos = this.offsetToLineCol(oldContent, prefixLen);
    const endPos = this.offsetToLineCol(oldContent, oldLen - suffixLen);

    // Return change-range format
    return [{
      startLine: startPos.line,
      startCol: startPos.col,
      endLine: endPos.line,
      endCol: endPos.col,
      text: insertText
    }];
  }

  /**
   * Convert byte offset to line/column position
   * Lines are 0-indexed, columns are 0-indexed
   */
  offsetToLineCol(text, offset) {
    let line = 0;
    let col = 0;

    for (let i = 0; i < offset && i < text.length; i++) {
      if (text[i] === '\n') {
        line++;
        col = 0;
      } else {
        col++;
      }
    }

    return { line, col };
  }

  /**
   * Set initial content (for initialization)
   */
  setInitialContent(content) {
    this.content = content;
  }

  /**
   * Get current content
   */
  getContent() {
    return this.textarea.value;
  }

  /**
   * Set content programmatically
   */
  setContent(content) {
    this.textarea.value = content;
    this.content = content;
  }

  /**
   * Get cursor position
   */
  getCursorPosition() {
    return this.textarea.selectionStart;
  }

  /**
   * Set cursor position
   */
  setCursorPosition(pos) {
    this.textarea.selectionStart = pos;
    this.textarea.selectionEnd = pos;
    this.textarea.focus();
  }

  /**
   * Insert text at cursor
   */
  insertAtCursor(text) {
    const start = this.textarea.selectionStart;
    const end = this.textarea.selectionEnd;
    const content = this.textarea.value;

    const newContent = content.substring(0, start) + text + content.substring(end);
    this.textarea.value = newContent;
    this.textarea.selectionStart = start + text.length;
    this.textarea.selectionEnd = start + text.length;

    // Trigger change processing
    this.handleInput();
  }

  /**
   * Event emitter
   */
  on(event, callback) {
    if (!this.listeners.has(event)) {
      this.listeners.set(event, []);
    }
    this.listeners.get(event).push(callback);
  }

  off(event, callback) {
    if (!this.listeners.has(event)) return;
    const callbacks = this.listeners.get(event);
    const index = callbacks.indexOf(callback);
    if (index !== -1) {
      callbacks.splice(index, 1);
    }
  }

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
