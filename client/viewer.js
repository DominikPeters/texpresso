/**
 * TeXpresso Viewer Component
 * Manages the PDF preview canvas and page navigation
 */

export class Viewer {
  /**
   * @param {HTMLCanvasElement} canvas
   * @param {HTMLElement} container
   */
  constructor(canvas, container) {
    this.canvas = canvas;
    this.container = container;
    this.ctx = canvas.getContext('2d');

    // Page state
    this.currentPage = 0;
    this.pageCount = 0;

    // Page dimensions in points (will be set by beginPage)
    this.pageWidth = 612;   // Default US Letter
    this.pageHeight = 792;

    // Zoom level (1.0 = 100%)
    this.zoom = 1.0;

    // Device pixel ratio for sharp rendering on HiDPI displays
    this.dpr = window.devicePixelRatio || 1;

    // Page command buffers for re-rendering
    this.pageBuffers = new Map();

    // Callbacks
    this.onPageChange = null;
    this.onZoomChange = null;
  }

  /**
   * Initialize canvas for a new page
   * @param {number} page - Page number (0-indexed)
   * @param {number} width - Page width in points
   * @param {number} height - Page height in points
   */
  beginPage(page, width, height) {
    this.currentPage = page;
    this.pageWidth = width;
    this.pageHeight = height;

    // Store page dimensions in the buffer metadata
    if (!this.pageBuffers.has(page)) {
      this.pageBuffers.set(page, []);
    }

    // Store dimensions separately for each page
    if (!this.pageDimensions) {
      this.pageDimensions = new Map();
    }
    this.pageDimensions.set(page, { width, height });

    // Calculate canvas size with zoom and DPR
    const displayWidth = width * this.zoom;
    const displayHeight = height * this.zoom;

    // Set canvas size (actual pixels for rendering)
    this.canvas.width = displayWidth * this.dpr;
    this.canvas.height = displayHeight * this.dpr;

    // Set display size (CSS pixels)
    this.canvas.style.width = displayWidth + 'px';
    this.canvas.style.height = displayHeight + 'px';

    // Reset transform and scale for DPR and zoom
    this.ctx.setTransform(1, 0, 0, 1, 0, 0);
    this.ctx.scale(this.dpr * this.zoom, this.dpr * this.zoom);

    // Fill with white background
    this.ctx.fillStyle = 'white';
    this.ctx.fillRect(0, 0, width, height);
  }

  /**
   * Called when page rendering is complete
   * @param {number} page
   */
  endPage(page) {
    // Page rendering complete
    if (this.onPageChange) {
      this.onPageChange(page, this.pageCount);
    }
  }

  /**
   * Set the total page count
   * @param {number} count
   */
  setPageCount(count) {
    this.pageCount = count;
    if (this.onPageChange) {
      this.onPageChange(this.currentPage, count);
    }
  }

  /**
   * Set zoom level
   * @param {number} zoom - Zoom level (1.0 = 100%)
   */
  setZoom(zoom) {
    this.zoom = Math.max(0.25, Math.min(4.0, zoom));
    if (this.onZoomChange) {
      this.onZoomChange(this.zoom);
    }
    // Re-render current page with new zoom
    this.reRenderCurrentPage();
  }

  /**
   * Re-render the current page (after zoom or other changes)
   */
  reRenderCurrentPage() {
    const commands = this.pageBuffers.get(this.currentPage);
    const dimensions = this.pageDimensions && this.pageDimensions.get(this.currentPage);

    if (commands && commands.length > 0 && dimensions && this.onReRender) {
      this.onReRender(this.currentPage, commands, dimensions);
    }
  }

  /**
   * Store a command in the current page's buffer
   * @param {object} cmd
   */
  storeCommand(cmd) {
    const buffer = this.pageBuffers.get(this.currentPage);
    if (buffer) {
      buffer.push(cmd);
    }
  }

  /**
   * Zoom in by 25%
   */
  zoomIn() {
    this.setZoom(this.zoom + 0.25);
  }

  /**
   * Zoom out by 25%
   */
  zoomOut() {
    this.setZoom(this.zoom - 0.25);
  }

  /**
   * Get the 2D rendering context
   * @returns {CanvasRenderingContext2D}
   */
  getContext() {
    return this.ctx;
  }

  /**
   * Get current zoom level as percentage string
   * @returns {string}
   */
  getZoomString() {
    return Math.round(this.zoom * 100) + '%';
  }

  /**
   * Update device pixel ratio (call when window moves to different display)
   */
  updateDPR() {
    const newDPR = window.devicePixelRatio || 1;
    if (newDPR !== this.dpr) {
      this.dpr = newDPR;
      this.reRenderCurrentPage();
    }
  }
}
