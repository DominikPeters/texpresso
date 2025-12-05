/**
 * Log Viewer
 * Displays compilation logs and messages
 */

export class LogViewer {
  constructor(element) {
    this.element = element;
    this.lines = [];
    this.maxLines = 10000; // Limit to prevent memory issues
  }

  /**
   * Append text to the log
   */
  append(text, level = 'info') {
    if (!text) return;

    // Split into lines if needed
    const newLines = text.split('\n');

    for (const line of newLines) {
      if (line || newLines.length === 1) { // Keep empty lines only if it's a single line
        this.lines.push({ text: line, level });
      }
    }

    // Trim if too many lines
    if (this.lines.length > this.maxLines) {
      this.lines = this.lines.slice(-this.maxLines);
    }

    this.render();
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
    this.render();
  }

  /**
   * Get line count
   */
  getLineCount() {
    return this.lines.length;
  }

  /**
   * Render the log to the DOM
   */
  render() {
    // Build HTML with styled lines
    const html = this.lines
      .map(({ text, level }) => {
        const className = level ? `log-${level}` : '';
        // Escape HTML
        const escaped = this.escapeHtml(text);
        return className ? `<span class="${className}">${escaped}</span>` : escaped;
      })
      .join('\n');

    this.element.innerHTML = html;

    // Auto-scroll to bottom
    this.scrollToBottom();
  }

  /**
   * Scroll to bottom of log
   */
  scrollToBottom() {
    this.element.scrollTop = this.element.scrollHeight;
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
