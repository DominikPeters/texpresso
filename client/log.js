/**
 * Log Viewer
 * Displays compilation logs and messages
 */

export class LogViewer {
  constructor(element) {
    this.element = element;
    this.container = element.parentElement; // The scrollable container
    this.lines = [];
    this.maxLines = 10000; // Limit to prevent memory issues
    this.renderedCount = 0; // Track how many lines have been rendered
  }

  /**
   * Append text to the log
   */
  append(text, level = 'info') {
    if (!text) return;

    // Split into lines if needed
    const newLines = text.split('\n');
    const linesToAdd = [];

    for (const line of newLines) {
      if (line || newLines.length === 1) { // Keep empty lines only if it's a single line
        linesToAdd.push({ text: line, level });
      }
    }

    this.lines.push(...linesToAdd);

    // Trim if too many lines
    if (this.lines.length > this.maxLines) {
      const removeCount = this.lines.length - this.maxLines;
      this.lines = this.lines.slice(removeCount);

      // Remove old DOM elements
      for (let i = 0; i < removeCount; i++) {
        if (this.element.firstChild) {
          this.element.removeChild(this.element.firstChild);
        }
      }
      this.renderedCount -= removeCount;
    }

    this.renderNew();
  }

  /**
   * Add an info message
   */
  info(text) {
    this.append(`[INFO] ${text}`, 'info');
  }

  /**
   * Add an error message
   */
  error(text) {
    this.append(`[ERROR] ${text}`, 'error');
  }

  /**
   * Add a warning message
   */
  warning(text) {
    this.append(`[WARNING] ${text}`, 'warning');
  }

  /**
   * Add a success message
   */
  success(text) {
    this.append(`[SUCCESS] ${text}`, 'success');
  }

  /**
   * Clear the log
   */
  clear() {
    this.lines = [];
    this.renderedCount = 0;
    this.element.textContent = ''; // More efficient than innerHTML = ''
  }

  /**
   * Get line count
   */
  getLineCount() {
    return this.lines.length;
  }

  /**
   * Render only new lines to the DOM (incremental)
   */
  renderNew() {
    const newLineCount = this.lines.length - this.renderedCount;
    if (newLineCount <= 0) return;

    // Create a document fragment for batch DOM insertion
    const fragment = document.createDocumentFragment();

    for (let i = this.renderedCount; i < this.lines.length; i++) {
      const { text, level } = this.lines[i];

      // Create span element for the line
      const span = document.createElement('span');
      span.textContent = text;
      if (level) {
        span.className = `log-${level}`;
      }

      fragment.appendChild(span);

      // Add newline after each line (except possibly the last)
      if (i < this.lines.length - 1) {
        fragment.appendChild(document.createTextNode('\n'));
      }
    }

    // Single DOM update
    this.element.appendChild(fragment);
    this.renderedCount = this.lines.length;

    // Auto-scroll to bottom
    this.scrollToBottom();
  }

  /**
   * Full render (used when we need to rebuild everything)
   */
  render() {
    this.renderedCount = 0;
    this.element.textContent = '';
    this.renderNew();
  }

  /**
   * Scroll to bottom of log
   */
  scrollToBottom() {
    if (this.container) {
      this.container.scrollTop = this.container.scrollHeight;
    }
  }

  /**
   * Escape HTML special characters
   */
  escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
  }

  /**
   * Export log as text
   */
  export() {
    return this.lines.map(({ text }) => text).join('\n');
  }
}
