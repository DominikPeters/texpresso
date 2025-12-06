/**
 * TeXpresso Render Command Interpreter
 * Processes JSON render commands and draws to Canvas 2D
 */

export class Renderer {
  /**
   * @param {import('./viewer.js').Viewer} viewer
   */
  constructor(viewer) {
    this.viewer = viewer;
    this.ctx = viewer.getContext();

    // Graphics state stack for save/restore
    this.stateStack = [];

    // Glyph path cache: maps cache_id (cid) to Path2D object
    this.glyphCache = new Map();
  }

  /**
   * Clear the glyph cache (call when server resets its cache)
   */
  clearGlyphCache() {
    this.glyphCache.clear();
  }

  /**
   * Process a render command from TeXpresso
   * @param {object} cmd - The render command object
   */
  processCommand(cmd) {
    // Update context reference (may change on beginPage)
    this.ctx = this.viewer.getContext();

    switch (cmd.cmd) {
      case 'beginPage':
        this.viewer.beginPage(cmd.page, cmd.width, cmd.height);
        this.ctx = this.viewer.getContext();
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
        this.ctx.save();
        break;

      case 'restore':
        this.ctx.restore();
        break;

      default:
        // Unknown command - ignore silently for now
        break;
    }
  }

  /**
   * Convert RGB color array [r, g, b] (0-1 range) to CSS color string
   * @param {number[]} color - RGB values in 0-1 range
   * @param {number} alpha - Alpha value (0-1)
   * @returns {string} CSS rgba color
   */
  colorToCSS(color, alpha = 1.0) {
    const r = Math.round(color[0] * 255);
    const g = Math.round(color[1] * 255);
    const b = Math.round(color[2] * 255);
    return `rgba(${r},${g},${b},${alpha})`;
  }

  /**
   * Build a Path2D from path operations
   * @param {object[]} pathOps - Array of path operations
   * @returns {Path2D}
   */
  buildPath(pathOps) {
    const path = new Path2D();

    for (const op of pathOps) {
      switch (op.op) {
        case 'M':
          path.moveTo(op.x, op.y);
          break;
        case 'L':
          path.lineTo(op.x, op.y);
          break;
        case 'C':
          path.bezierCurveTo(op.x1, op.y1, op.x2, op.y2, op.x3, op.y3);
          break;
        case 'Z':
          path.closePath();
          break;
        default:
          console.warn('Unknown path operation:', op.op);
      }
    }

    return path;
  }

  /**
   * Apply a transformation matrix [a, b, c, d, e, f]
   * @param {number[]} matrix
   */
  applyMatrix(matrix) {
    if (matrix && matrix.length === 6) {
      this.ctx.transform(matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5]);
    }
  }

  /**
   * Fill a path with color
   * @param {object} cmd - fillPath command
   */
  fillPath(cmd) {
    if (!cmd.path || cmd.path.length === 0) return;

    this.ctx.save();

    // Apply transformation matrix if present
    if (cmd.matrix) {
      this.applyMatrix(cmd.matrix);
    }

    // Build the path
    const path = this.buildPath(cmd.path);

    // Set fill style
    const alpha = cmd.alpha !== undefined ? cmd.alpha : 1.0;
    this.ctx.fillStyle = this.colorToCSS(cmd.color, alpha);

    // Fill with appropriate winding rule
    if (cmd.fillRule === 'evenodd') {
      this.ctx.fill(path, 'evenodd');
    } else {
      this.ctx.fill(path, 'nonzero');
    }

    this.ctx.restore();
  }

  /**
   * Stroke a path outline
   * @param {object} cmd - strokePath command
   */
  strokePath(cmd) {
    if (!cmd.path || cmd.path.length === 0) return;

    this.ctx.save();

    // Apply transformation matrix if present
    if (cmd.matrix) {
      this.applyMatrix(cmd.matrix);
    }

    // Build the path
    const path = this.buildPath(cmd.path);

    // Set stroke style
    const alpha = cmd.alpha !== undefined ? cmd.alpha : 1.0;
    this.ctx.strokeStyle = this.colorToCSS(cmd.color, alpha);
    this.ctx.lineWidth = cmd.lineWidth || 1.0;

    // Apply line cap and join if specified
    if (cmd.lineCap) {
      this.ctx.lineCap = cmd.lineCap;
    }
    if (cmd.lineJoin) {
      this.ctx.lineJoin = cmd.lineJoin;
    }
    if (cmd.miterLimit) {
      this.ctx.miterLimit = cmd.miterLimit;
    }

    this.ctx.stroke(path);

    this.ctx.restore();
  }

  /**
   * Fill text glyphs
   * For now, this is a stub that will be enhanced in Phase 4
   * @param {object} cmd - fillText command
   */
  fillText(cmd) {
    if (!cmd.spans || cmd.spans.length === 0) return;

    const alpha = cmd.alpha !== undefined ? cmd.alpha : 1.0;
    this.ctx.fillStyle = this.colorToCSS(cmd.color, alpha);

    for (const span of cmd.spans) {
      // Extract font size from matrix (for font rendering fallback)
      const m = span.matrix || [1, 0, 0, 1, 0, 0];
      const fontSize = Math.sqrt(m[0] * m[0] + m[1] * m[1]);

      for (const glyph of span.glyphs) {
        this.ctx.save();

        // Translate to glyph position in document coordinates
        this.ctx.translate(glyph.x, glyph.y);

        // Try to get path from cache or from glyph data
        let path = null;
        if (glyph.cid) {
          // Check if we have this glyph cached
          path = this.glyphCache.get(glyph.cid);
          if (!path && glyph.path) {
            // First time seeing this glyph - build and cache it
            path = this.buildPath(glyph.path);
            this.glyphCache.set(glyph.cid, path);
          }
        } else if (glyph.path) {
          // No cache ID - just build the path
          path = this.buildPath(glyph.path);
        }

        if (path) {
          // Apply span transformation matrix for the glyph shape
          // The matrix scales (font size) and may flip Y axis
          if (span.matrix) {
            this.applyMatrix(span.matrix);
          }
          this.ctx.fill(path);
        } else if (glyph.ucs > 0) {
          // Has valid Unicode - try to render with canvas text
          // Use the font name from span
          this.ctx.font = `${fontSize}px "${span.font}", serif`;
          this.ctx.fillText(String.fromCodePoint(glyph.ucs), 0, 0);
        }
        // If neither path nor valid ucs, skip the glyph

        this.ctx.restore();
      }
    }
  }

  /**
   * Stroke text outlines
   * @param {object} cmd - strokeText command
   */
  strokeText(cmd) {
    if (!cmd.spans || cmd.spans.length === 0) return;

    const alpha = cmd.alpha !== undefined ? cmd.alpha : 1.0;
    this.ctx.strokeStyle = this.colorToCSS(cmd.color, alpha);
    this.ctx.lineWidth = cmd.lineWidth || 1.0;

    for (const span of cmd.spans) {
      // Extract font size from matrix (for font rendering fallback)
      const m = span.matrix || [1, 0, 0, 1, 0, 0];
      const fontSize = Math.sqrt(m[0] * m[0] + m[1] * m[1]);

      for (const glyph of span.glyphs) {
        this.ctx.save();

        // Translate to glyph position in document coordinates
        this.ctx.translate(glyph.x, glyph.y);

        // Try to get path from cache or from glyph data
        let path = null;
        if (glyph.cid) {
          // Check if we have this glyph cached
          path = this.glyphCache.get(glyph.cid);
          if (!path && glyph.path) {
            // First time seeing this glyph - build and cache it
            path = this.buildPath(glyph.path);
            this.glyphCache.set(glyph.cid, path);
          }
        } else if (glyph.path) {
          // No cache ID - just build the path
          path = this.buildPath(glyph.path);
        }

        if (path) {
          // Apply span transformation matrix for the glyph shape
          if (span.matrix) {
            this.applyMatrix(span.matrix);
          }
          this.ctx.stroke(path);
        } else if (glyph.ucs > 0) {
          // Has valid Unicode - try to render with canvas text
          this.ctx.font = `${fontSize}px "${span.font}", serif`;
          this.ctx.strokeText(String.fromCodePoint(glyph.ucs), 0, 0);
        }

        this.ctx.restore();
      }
    }
  }
}
