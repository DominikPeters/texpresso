/**
 * TeXpresso Viewer Component
 * Manages the PDF preview SVG and page navigation
 */

export class Viewer {
  /**
   * @param {HTMLElement} container
   */
  constructor(container) {
    this.container = container;
    
    // Create SVG element
    this.svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    this.svg.style.display = 'block';
    this.svg.style.backgroundColor = 'white';
    this.container.appendChild(this.svg);

    // Create defs for reusable glyphs
    this.defs = document.createElementNS('http://www.w3.org/2000/svg', 'defs');
    this.svg.appendChild(this.defs);

    // Main content group
    this.contentGroup = document.createElementNS('http://www.w3.org/2000/svg', 'g');
    this.svg.appendChild(this.contentGroup);

    // Page state
    this.currentPage = 0;
    this.pageCount = 0;

    // Page dimensions in points
    this.pageWidth = 612;   // Default US Letter
    this.pageHeight = 792;

    // Zoom level (1.0 = 100%)
    this.zoom = 1.0;

    // Callbacks
    this.onPageChange = null;
    this.onZoomChange = null;
    this.onReRender = null; // Still useful if we need to completely rebuild
    
    // We still store commands to support page switching/re-rendering if needed
    this.pageBuffers = new Map();
    this.pageDimensions = new Map();

    // Trackpad zoom handler
    this.container.addEventListener('wheel', (e) => {
      if (e.ctrlKey) {
        e.preventDefault();
        
        // Get mouse position relative to SVG *before* zoom
        const rect = this.svg.getBoundingClientRect();
        const mouseX = e.clientX - rect.left;
        const mouseY = e.clientY - rect.top;
        
        // Determine zoom direction
        const delta = -e.deltaY;
        const zoomFactor = 1.028; 
        
        const oldZoom = this.zoom;
        const newZoom = Math.max(0.25, Math.min(4.0, oldZoom * (delta > 0 ? zoomFactor : 1/zoomFactor)));
        
        if (newZoom !== oldZoom) {
          this.setZoom(newZoom);
          
          // Calculate where the mouse point is now *after* zoom
          // The point on the SVG is the same proportion or just scaled
          const newMouseX = mouseX * (newZoom / oldZoom);
          const newMouseY = mouseY * (newZoom / oldZoom);
          
          // Adjust scroll to keep that point stationary relative to viewport
          // We want: newScreenPos = oldScreenPos
          // (svgLeft_new + newMouseX) = (svgLeft_old + mouseX)
          // svgLeft_new = svgLeft_old + mouseX - newMouseX
          // Since svgLeft is controlled by scroll (-scrollLeft), we adjust scrollLeft by the difference
          
          this.container.scrollLeft += (newMouseX - mouseX);
          this.container.scrollTop += (newMouseY - mouseY);
        }
      }
    }, { passive: false });
  }

  /**
   * Initialize SVG for a new page
   * @param {number} page - Page number (0-indexed)
   * @param {number} width - Page width in points
   * @param {number} height - Page height in points
   */
  beginPage(page, width, height) {
    this.currentPage = page;
    this.pageWidth = width;
    this.pageHeight = height;

    // Store metadata
    if (!this.pageBuffers.has(page)) {
      this.pageBuffers.set(page, []);
    }
    if (!this.pageDimensions) {
      this.pageDimensions = new Map();
    }
    this.pageDimensions.set(page, { width, height });

    // Update SVG dimensions and viewbox
    this.svg.setAttribute('viewBox', `0 0 ${width} ${height}`);
    this.updateZoom();

    // Clear content group for the new page
    // Note: We keep <defs> populated as glyphs might be shared across pages? 
    // Actually, usually glyphs are defined per page or per document. 
    // If the server sends them incrementally, we should keep them.
    // But if 'beginPage' implies a full refresh, we might want to clear content.
    // For now, let's clear the content group.
    while (this.contentGroup.firstChild) {
      this.contentGroup.removeChild(this.contentGroup.firstChild);
    }
    
    // Reset any state if necessary
  }

  /**
   * Called when page rendering is complete
   * @param {number} page
   */
  endPage(page) {
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
    this.updateZoom();
    
    if (this.onZoomChange) {
      this.onZoomChange(this.zoom);
    }
  }

  updateZoom() {
    const width = this.pageWidth * this.zoom;
    const height = this.pageHeight * this.zoom;
    
    this.svg.style.width = `${width}px`;
    this.svg.style.height = `${height}px`;
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
   * Replay commands - useful if we switched pages and need to redraw
   * The renderer will call this.
   */
  reRenderCurrentPage() {
    const commands = this.pageBuffers.get(this.currentPage);
    const dimensions = this.pageDimensions && this.pageDimensions.get(this.currentPage);

    if (commands && commands.length > 0 && dimensions && this.onReRender) {
      // Clear content before replaying
      while (this.contentGroup.firstChild) {
        this.contentGroup.removeChild(this.contentGroup.firstChild);
      }
      this.onReRender(this.currentPage, commands, dimensions);
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
   * Get current zoom level as percentage string
   * @returns {string}
   */
  getZoomString() {
    return Math.round(this.zoom * 100) + '%';
  }

  /**
   * Update device pixel ratio - No-op for SVG but kept for interface compatibility
   */
  updateDPR() {
    // SVG handles scaling automatically
  }
  
  // Accessors for Renderer
  getDefs() { return this.defs; }
  getContentGroup() { return this.contentGroup; }
}