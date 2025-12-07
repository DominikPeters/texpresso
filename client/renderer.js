/**
 * TeXpresso Render Command Interpreter
 * Processes JSON render commands and generates SVG
 */

export class Renderer {
  /**
   * @param {import('./viewer.js').Viewer} viewer
   */
  constructor(viewer) {
    this.viewer = viewer;
    this.groupStack = [];
    this.currentGroup = null;
    
    // Set of cached glyph CIDs to avoid querying DOM too often
    this.cachedGlyphs = new Set();
  }

  /**
   * Clear the glyph cache
   */
  clearGlyphCache() {
    this.cachedGlyphs.clear();
    // Clear defs
    const defs = this.viewer.getDefs();
    while (defs.firstChild) {
      defs.removeChild(defs.firstChild);
    }
  }

  /**
   * Process a render command from TeXpresso
   * @param {object} cmd - The render command object
   */
  processCommand(cmd) {
    // Store non-page commands in buffer for re-rendering if needed
    if (cmd.cmd !== 'beginPage' && cmd.cmd !== 'endPage') {
      this.viewer.storeCommand(cmd);
    }

    this.executeCommand(cmd);
  }

  /**
   * Execute a single render command
   * @param {object} cmd
   */
  executeCommand(cmd) {
    switch (cmd.cmd) {
      case 'beginPage':
        this.viewer.beginPage(cmd.page, cmd.width, cmd.height);
        this.currentGroup = this.viewer.getContentGroup();
        this.groupStack = [];
        break;

      case 'endPage':
        this.viewer.endPage(cmd.page);
        break;

      case 'fillPath':
        this.fillPath(cmd);
        break;

      case 'strokePath':
        this.strokePath(cmd);
        break;

      case 'fillText':
        this.fillText(cmd);
        break;

      case 'strokeText':
        this.strokeText(cmd);
        break;

      case 'save':
        this.pushGroup();
        break;

      case 'restore':
        this.popGroup();
        break;

      default:
        // Unknown command
        break;
    }
  }

  /**
   * Replay a list of commands
   * @param {number} page
   * @param {object[]} commands
   * @param {object} dimensions - {width, height}
   */
  replayCommands(page, commands, dimensions) {
    // Re-initialize the page
    this.viewer.beginPage(page, dimensions.width, dimensions.height);
    this.currentGroup = this.viewer.getContentGroup();
    this.groupStack = [];

    // Replay all commands
    for (const cmd of commands) {
      this.executeCommand(cmd);
    }

    this.viewer.endPage(page);
  }

  /**
   * Create a new group and push to stack (save state)
   */
  pushGroup() {
    const group = document.createElementNS('http://www.w3.org/2000/svg', 'g');
    if (this.currentGroup) {
      this.currentGroup.appendChild(group);
      this.groupStack.push(this.currentGroup);
      this.currentGroup = group;
    }
  }

  /**
   * Pop group from stack (restore state)
   */
  popGroup() {
    if (this.groupStack.length > 0) {
      this.currentGroup = this.groupStack.pop();
    }
  }

  /**
   * Convert RGB color array [r, g, b] (0-1 range) to CSS color string
   */
  colorToCSS(color, alpha = 1.0) {
    if (!color) return 'black';
    const r = Math.round(color[0] * 255);
    const g = Math.round(color[1] * 255);
    const b = Math.round(color[2] * 255);
    return `rgba(${r},${g},${b},${alpha})`;
  }

  /**
   * Convert matrix array to SVG transform string
   */
  matrixToString(m) {
    if (!m || m.length !== 6) return '';
    return `matrix(${m[0]},${m[1]},${m[2]},${m[3]},${m[4]},${m[5]})`;
  }

  /**
   * Build SVG path data string
   */
  buildPathData(pathOps) {
    if (!pathOps) return '';
    let d = '';
    for (const op of pathOps) {
      switch (op.op) {
        case 'M': d += `M ${op.x} ${op.y} `; break;
        case 'L': d += `L ${op.x} ${op.y} `; break;
        case 'C': d += `C ${op.x1} ${op.y1}, ${op.x2} ${op.y2}, ${op.x3} ${op.y3} `; break;
        case 'Z': d += `Z `; break;
      }
    }
    return d;
  }

  /**
   * Fill a path with color
   */
  fillPath(cmd) {
    if (!cmd.path || cmd.path.length === 0) return;

    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    path.setAttribute('d', this.buildPathData(cmd.path));
    
    const alpha = cmd.alpha !== undefined ? cmd.alpha : 1.0;
    path.setAttribute('fill', this.colorToCSS(cmd.color, alpha));
    
    if (cmd.fillRule === 'evenodd') {
      path.setAttribute('fill-rule', 'evenodd');
    }

    if (cmd.matrix) {
      path.setAttribute('transform', this.matrixToString(cmd.matrix));
    }

    // Default stroke is none
    path.setAttribute('stroke', 'none');

    this.currentGroup.appendChild(path);
  }

  /**
   * Stroke a path outline
   */
  strokePath(cmd) {
    if (!cmd.path || cmd.path.length === 0) return;

    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    path.setAttribute('d', this.buildPathData(cmd.path));
    
    const alpha = cmd.alpha !== undefined ? cmd.alpha : 1.0;
    path.setAttribute('stroke', this.colorToCSS(cmd.color, alpha));
    path.setAttribute('stroke-width', cmd.lineWidth || 1.0);
    path.setAttribute('fill', 'none');

    if (cmd.lineCap) path.setAttribute('stroke-linecap', cmd.lineCap);
    if (cmd.lineJoin) path.setAttribute('stroke-linejoin', cmd.lineJoin);
    if (cmd.miterLimit) path.setAttribute('stroke-miterlimit', cmd.miterLimit);

    if (cmd.matrix) {
      path.setAttribute('transform', this.matrixToString(cmd.matrix));
    }

    this.currentGroup.appendChild(path);
  }

  /**
   * Ensure a glyph is defined in <defs>
   */
  ensureGlyph(glyph) {
    if (glyph.cid && this.cachedGlyphs.has(glyph.cid)) return;

    if (glyph.cid && glyph.path) {
      const id = `g_${glyph.cid}`;
      const symbol = document.createElementNS('http://www.w3.org/2000/svg', 'symbol');
      symbol.setAttribute('id', id);
      symbol.setAttribute('overflow', 'visible'); // Allow glyphs to extend beyond bbox if needed
      
      const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
      path.setAttribute('d', this.buildPathData(glyph.path));
      // No fill/stroke on definition usually, as it inherits from <use>
      // However, PDF glyphs are shapes. <use> fills them if we set fill on <use>.
      
      symbol.appendChild(path);
      this.viewer.getDefs().appendChild(symbol);
      this.cachedGlyphs.add(glyph.cid);
    }
  }

  /**
   * Fill text glyphs
   */
  fillText(cmd) {
    if (!cmd.spans || cmd.spans.length === 0) return;

    const alpha = cmd.alpha !== undefined ? cmd.alpha : 1.0;
    const color = this.colorToCSS(cmd.color, alpha);

    // Group for this text command
    const g = document.createElementNS('http://www.w3.org/2000/svg', 'g');
    g.setAttribute('fill', color);
    
    for (const span of cmd.spans) {
      const spanMatrix = span.matrix || [1, 0, 0, 1, 0, 0];
      
      for (const glyph of span.glyphs) {
        if (glyph.cid) {
          this.ensureGlyph(glyph);
          
          const use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
          use.setAttribute('href', `#g_${glyph.cid}`);
          
          // Transform: translate(x,y) then matrix(...)
          // Note: SVG transform list applies right-to-left in terms of coordinate systems
          // But in string "translate(...) matrix(...)" they apply left-to-right.
          // PDF: T = translate(x,y). Text Matrix: Tm. Result: Tm * T? 
          // Usually position (x,y) is separate from the text matrix (scaling/rotation).
          // span.matrix includes font scaling.
          
          // We can construct a combined matrix or just list them.
          // translate(glyph.x, glyph.y) matrix(spanMatrix)
          use.setAttribute('transform', `translate(${glyph.x},${glyph.y}) ${this.matrixToString(spanMatrix)}`);
          
          g.appendChild(use);
        } else if (glyph.path) {
          // Uncached glyph
          const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
          path.setAttribute('d', this.buildPathData(glyph.path));
          path.setAttribute('transform', `translate(${glyph.x},${glyph.y}) ${this.matrixToString(spanMatrix)}`);
          g.appendChild(path);
        } else if (glyph.ucs > 0) {
          // Fallback to text element
          const text = document.createElementNS('http://www.w3.org/2000/svg', 'text');
          const fontSize = Math.sqrt(spanMatrix[0] * spanMatrix[0] + spanMatrix[1] * spanMatrix[1]);
          
          text.setAttribute('x', '0');
          text.setAttribute('y', '0');
          text.setAttribute('font-family', `"${span.font}", serif`);
          text.setAttribute('font-size', fontSize);
          text.textContent = String.fromCodePoint(glyph.ucs);
          
          // Position and transform
          text.setAttribute('transform', `translate(${glyph.x},${glyph.y}) ${this.matrixToString(spanMatrix)}`);
          g.appendChild(text);
        }
      }
    }
    
    this.currentGroup.appendChild(g);
  }

  /**
   * Stroke text outlines
   */
  strokeText(cmd) {
    if (!cmd.spans || cmd.spans.length === 0) return;

    const alpha = cmd.alpha !== undefined ? cmd.alpha : 1.0;
    const color = this.colorToCSS(cmd.color, alpha);

    const g = document.createElementNS('http://www.w3.org/2000/svg', 'g');
    g.setAttribute('stroke', color);
    g.setAttribute('stroke-width', cmd.lineWidth || 1.0);
    g.setAttribute('fill', 'none');

    for (const span of cmd.spans) {
      const spanMatrix = span.matrix || [1, 0, 0, 1, 0, 0];
      
      for (const glyph of span.glyphs) {
        if (glyph.cid) {
          this.ensureGlyph(glyph);
          const use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
          use.setAttribute('href', `#g_${glyph.cid}`);
          use.setAttribute('transform', `translate(${glyph.x},${glyph.y}) ${this.matrixToString(spanMatrix)}`);
          g.appendChild(use);
        } else if (glyph.path) {
          const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
          path.setAttribute('d', this.buildPathData(glyph.path));
          path.setAttribute('transform', `translate(${glyph.x},${glyph.y}) ${this.matrixToString(spanMatrix)}`);
          g.appendChild(path);
        }
      }
    }
    this.currentGroup.appendChild(g);
  }
}