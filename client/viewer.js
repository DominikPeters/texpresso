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
    this.onSyncTexClick = null; // SyncTeX backward sync (PDF → source)
    
    // Page command cache with LRU eviction
    this.pageBuffers = new Map();
    this.pageDimensions = new Map();
    this.maxCachedPages = 5;
    this.previousBuffer = null; // Temporary storage during page render

    // Pending highlight for SyncTeX forward (applied after page renders)
    this.pendingHighlight = null;

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

    // SyncTeX backward sync - Cmd+Click (Mac) or Ctrl+Click (Windows/Linux) on PDF
    this.svg.addEventListener('click', (e) => {
      // Only trigger on Cmd/Ctrl+click
      if (!e.metaKey && !e.ctrlKey) return;

      // Get click coordinates relative to SVG
      const rect = this.svg.getBoundingClientRect();
      const clickX = e.clientX - rect.left;
      const clickY = e.clientY - rect.top;

      // Convert from screen coordinates to page coordinates (PDF points)
      // viewBox is "0 0 width height", display size is width*zoom x height*zoom
      const x = clickX / this.zoom;
      const y = clickY / this.zoom;

      // Emit synctex backward event
      if (this.onSyncTexClick) {
        this.onSyncTexClick(this.currentPage, x, y);
      }
    });
  }

  /**
   * Initialize SVG for a new page
   * @param {number} page - Page number (0-indexed)
   * @param {number} width - Page width in points
   * @param {number} height - Page height in points
   * @param {number} [replayPrefix] - Number of commands to replay from previous buffer
   * @returns {Object[]} Previous commands if replayPrefix > 0, empty array otherwise
   */
  beginPage(page, width, height, replayPrefix = 0) {
    this.currentPage = page;
    this.pageWidth = width;
    this.pageHeight = height;

    // Save previous buffer for replay, then reset
    const previousBuffer = this.pageBuffers.get(page) || [];
    this.pageBuffers.set(page, []);
    this.previousBuffer = previousBuffer; // Keep for suffix replay in endPage

    if (!this.pageDimensions) {
      this.pageDimensions = new Map();
    }
    this.pageDimensions.set(page, { width, height });

    // Update SVG dimensions and viewbox
    this.svg.setAttribute('viewBox', `0 0 ${width} ${height}`);
    this.updateZoom();

    // Clear content group for the new page
    while (this.contentGroup.firstChild) {
      this.contentGroup.removeChild(this.contentGroup.firstChild);
    }

    // Return prefix commands to replay
    if (replayPrefix > 0 && previousBuffer.length >= replayPrefix) {
      return previousBuffer.slice(0, replayPrefix);
    }
    return [];
  }

  /**
   * Called when page rendering is complete
   * @param {number} page
   * @param {number} [replaySuffix] - Number of commands to replay from previous buffer's end
   * @returns {Object[]} Suffix commands if replaySuffix > 0, empty array otherwise
   */
  endPage(page, replaySuffix = 0) {
    // Get suffix commands from previous buffer
    let suffixCommands = [];
    if (replaySuffix > 0 && this.previousBuffer && this.previousBuffer.length >= replaySuffix) {
      suffixCommands = this.previousBuffer.slice(-replaySuffix);
    }

    // Clear previous buffer reference
    this.previousBuffer = null;

    // Enforce LRU cache limit
    this.enforcePageCacheLimit(page);

    if (this.onPageChange) {
      this.onPageChange(page, this.pageCount);
    }

    // Apply pending SyncTeX highlight if this is the target page
    if (this.pendingHighlight && this.pendingHighlight.page === page) {
      const { x, y } = this.pendingHighlight;
      this.pendingHighlight = null;
      // Delay slightly to ensure rendering is complete
      setTimeout(() => {
        this.scrollToCoordinates(x, y);
        this.highlightPosition(x, y);
      }, 50);
    }

    return suffixCommands;
  }

  /**
   * Enforce LRU limit on page cache, keeping the current page
   * @param {number} currentPage - Page to never evict
   */
  enforcePageCacheLimit(currentPage) {
    // Move current page to end of Map (most recently used)
    if (this.pageBuffers.has(currentPage)) {
      const buffer = this.pageBuffers.get(currentPage);
      this.pageBuffers.delete(currentPage);
      this.pageBuffers.set(currentPage, buffer);
    }

    // Evict oldest pages until under limit
    while (this.pageBuffers.size > this.maxCachedPages) {
      const oldestPage = this.pageBuffers.keys().next().value;
      if (oldestPage === currentPage) break; // Safety: don't evict current
      this.pageBuffers.delete(oldestPage);
      this.pageDimensions.delete(oldestPage);
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

  /**
   * Scroll to a specific position in the document (for forward SyncTeX)
   * @param {number} page - Page number (0-indexed)
   * @param {number} x - X coordinate in PDF points
   * @param {number} y - Y coordinate in PDF points
   */
  scrollToPosition(page, x, y) {
    // If we need to change pages, set a pending highlight to apply after render
    if (page !== this.currentPage) {
      // Store pending highlight - will be applied in endPage()
      this.pendingHighlight = { page, x, y };

      // Request the page change - this will trigger a re-render
      this.currentPage = page;
      if (this.onPageChange) {
        this.onPageChange(page, this.pageCount);
      }
      // The actual scroll and highlight happen in endPage() after rendering
    } else {
      // Same page - scroll and highlight immediately
      this.scrollToCoordinates(x, y);
      this.highlightPosition(x, y);
    }
  }

  /**
   * Scroll the container to center on given coordinates
   * @param {number} x - X coordinate in PDF points
   * @param {number} y - Y coordinate in PDF points
   */
  scrollToCoordinates(x, y) {
    // Convert page coordinates to screen coordinates
    const screenX = x * this.zoom;
    const screenY = y * this.zoom;

    // Scroll to center on position (with some offset to show context)
    const targetScrollX = Math.max(0, screenX - this.container.clientWidth / 2);
    const targetScrollY = Math.max(0, screenY - this.container.clientHeight / 2);

    this.container.scrollLeft = targetScrollX;
    this.container.scrollTop = targetScrollY;
  }

  /**
   * Show a brief visual highlight at a position (for SyncTeX forward result)
   * @param {number} x - X coordinate in PDF points
   * @param {number} y - Y coordinate in PDF points
   */
  highlightPosition(x, y) {
    // Create a temporary highlight circle
    const highlight = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
    highlight.setAttribute('cx', x);
    highlight.setAttribute('cy', y);
    highlight.setAttribute('r', '10');
    highlight.setAttribute('fill', 'rgba(255, 200, 0, 0.6)');
    highlight.setAttribute('stroke', 'orange');
    highlight.setAttribute('stroke-width', '2');
    highlight.style.pointerEvents = 'none';

    this.contentGroup.appendChild(highlight);

    // Animate and remove after a short delay
    let opacity = 0.6;
    const fadeInterval = setInterval(() => {
      opacity -= 0.1;
      if (opacity <= 0) {
        clearInterval(fadeInterval);
        if (highlight.parentNode) {
          highlight.parentNode.removeChild(highlight);
        }
      } else {
        highlight.setAttribute('fill', `rgba(255, 200, 0, ${opacity})`);
      }
    }, 100);
  }
}