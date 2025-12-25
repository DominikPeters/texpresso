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

    // Unique ID counter for generated elements (clips, masks, patterns, etc.)
    this.idCounter = 0;

    // Stack for tracking clip paths (for popClip)
    this.clipStack = [];

    // Current mask being defined (for beginMask/endMask)
    this.currentMaskId = null;
    this.maskGroup = null;

    // Current tile pattern being defined (for beginTile/endTile)
    this.currentPatternId = null;
    this.patternElement = null;

    // Layer stack
    this.layerStack = [];
  }

  /**
   * Generate a unique ID for SVG elements
   */
  generateId(prefix) {
    return `${prefix}_${++this.idCounter}`;
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
    // Reset ID counter
    this.idCounter = 0;
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

    if (cmd.cmd === 'beginPage') {
      // Initialize page rendering - headless always sends the current page
      this.viewer.beginPage(cmd.page, cmd.width, cmd.height);
      this.isRendering = true;
      this.currentGroup = this.viewer.getContentGroup();
      this.groupStack = [];
      this.clipStack = [];
      this.layerStack = [];
    } else if (cmd.cmd === 'endPage') {
      this.viewer.endPage(cmd.page);
      this.isRendering = false; // Stop rendering until next beginPage
    } else if (this.isRendering) {
      this.executeVisualCommand(cmd);
    }
  }

  /**
   * Execute a single visual render command (drawing, grouping)
   * @param {object} cmd
   */
  executeVisualCommand(cmd) {
    switch (cmd.cmd) {
      // Path commands
      case 'fillPath':
        this.fillPath(cmd);
        break;
      case 'strokePath':
        this.strokePath(cmd);
        break;
      case 'clipPath':
        this.clipPath(cmd);
        break;
      case 'clipStrokePath':
        this.clipStrokePath(cmd);
        break;

      // Text commands
      case 'fillText':
        this.fillText(cmd);
        break;
      case 'strokeText':
        this.strokeText(cmd);
        break;
      case 'clipText':
        this.clipText(cmd);
        break;
      case 'clipStrokeText':
        this.clipStrokeText(cmd);
        break;

      // Clip management
      case 'popClip':
        this.popClip();
        break;

      // Image commands
      case 'fillImage':
        this.fillImage(cmd);
        break;
      case 'fillImageMask':
        this.fillImageMask(cmd);
        break;
      case 'clipImageMask':
        this.clipImageMask(cmd);
        break;

      // Shading/gradients (stub)
      case 'fillShade':
        this.fillShade(cmd);
        break;

      // Transparency groups
      case 'beginGroup':
        this.beginGroup(cmd);
        break;
      case 'endGroup':
        this.endGroup();
        break;

      // Masks
      case 'beginMask':
        this.beginMask(cmd);
        break;
      case 'endMask':
        this.endMask();
        break;

      // Tiles/patterns
      case 'beginTile':
        this.beginTile(cmd);
        break;
      case 'endTile':
        this.endTile(cmd);
        break;

      // Layers
      case 'beginLayer':
        this.beginLayer(cmd);
        break;
      case 'endLayer':
        this.endLayer();
        break;

      // Legacy save/restore (if still used)
      case 'save':
        this.pushGroup();
        break;
      case 'restore':
        this.popGroup();
        break;

      default:
        // Unknown command
        if (cmd.cmd) {
          console.warn(`[Renderer] Unknown command: ${cmd.cmd}`);
        }
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
    // Re-initialize the view using beginPage (handles SVG setup and clearing)
    this.viewer.beginPage(page, dimensions.width, dimensions.height);
    this.currentGroup = this.viewer.getContentGroup();
    this.groupStack = [];
    this.clipStack = [];
    this.layerStack = [];

    // Replay all commands
    for (const cmd of commands) {
      this.executeVisualCommand(cmd);
    }

    // Note: We don't call endPage here as that triggers callbacks/logic for stream completion
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

  // ============================================================================
  // Path Commands
  // ============================================================================

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
   * Set clip region from a filled path
   */
  clipPath(cmd) {
    if (!cmd.path || cmd.path.length === 0) return;

    const clipId = this.generateId('clip');

    // Create clipPath element in defs
    const clipPathEl = document.createElementNS('http://www.w3.org/2000/svg', 'clipPath');
    clipPathEl.setAttribute('id', clipId);

    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    path.setAttribute('d', this.buildPathData(cmd.path));

    if (cmd.fillRule === 'evenodd') {
      clipPathEl.setAttribute('clip-rule', 'evenodd');
    }

    clipPathEl.appendChild(path);
    this.viewer.getDefs().appendChild(clipPathEl);

    // Create a new group with this clip applied
    const clippedGroup = document.createElementNS('http://www.w3.org/2000/svg', 'g');
    clippedGroup.setAttribute('clip-path', `url(#${clipId})`);

    // Push current group and switch to clipped group
    this.clipStack.push(this.currentGroup);
    this.currentGroup.appendChild(clippedGroup);
    this.currentGroup = clippedGroup;
  }

  /**
   * Set clip region from a stroked path
   *
   * Uses SVG mask with the stroked path to achieve clip effect.
   * This is visually correct but uses soft masking rather than hard clipping.
   */
  clipStrokePath(cmd) {
    if (!cmd.path || cmd.path.length === 0) return;

    const maskId = this.generateId('strokeclipmask');

    // Create mask with black background (transparent) and white stroke (opaque)
    const mask = document.createElementNS('http://www.w3.org/2000/svg', 'mask');
    mask.setAttribute('id', maskId);
    mask.setAttribute('maskUnits', 'userSpaceOnUse');

    // Black background covering the page (hides everything)
    const bg = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
    bg.setAttribute('x', '0');
    bg.setAttribute('y', '0');
    bg.setAttribute('width', this.viewer.pageWidth);
    bg.setAttribute('height', this.viewer.pageHeight);
    bg.setAttribute('fill', 'black');
    mask.appendChild(bg);

    // White stroke (reveals content)
    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    path.setAttribute('d', this.buildPathData(cmd.path));
    path.setAttribute('fill', 'none');
    path.setAttribute('stroke', 'white');
    path.setAttribute('stroke-width', cmd.lineWidth || 1.0);

    // Apply stroke properties
    if (cmd.lineCap !== undefined) {
      const caps = ['butt', 'round', 'square'];
      path.setAttribute('stroke-linecap', caps[cmd.lineCap] || 'butt');
    }
    if (cmd.lineJoin !== undefined) {
      const joins = ['miter', 'round', 'bevel'];
      path.setAttribute('stroke-linejoin', joins[cmd.lineJoin] || 'miter');
    }
    if (cmd.miterLimit) {
      path.setAttribute('stroke-miterlimit', cmd.miterLimit);
    }
    if (cmd.dashArray && cmd.dashArray.length > 0) {
      path.setAttribute('stroke-dasharray', cmd.dashArray.join(','));
      if (cmd.dashPhase) {
        path.setAttribute('stroke-dashoffset', cmd.dashPhase);
      }
    }

    if (cmd.matrix) {
      path.setAttribute('transform', this.matrixToString(cmd.matrix));
    }

    mask.appendChild(path);
    this.viewer.getDefs().appendChild(mask);

    // Create masked group
    const maskedGroup = document.createElementNS('http://www.w3.org/2000/svg', 'g');
    maskedGroup.setAttribute('mask', `url(#${maskId})`);

    this.clipStack.push(this.currentGroup);
    this.currentGroup.appendChild(maskedGroup);
    this.currentGroup = maskedGroup;
  }

  /**
   * Pop the current clip, returning to the previous group
   */
  popClip() {
    if (this.clipStack.length > 0) {
      this.currentGroup = this.clipStack.pop();
    }
  }

  // ============================================================================
  // Text Commands
  // ============================================================================

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
   * Build text glyphs as SVG elements (shared by fill/stroke/clip)
   * @returns {SVGGElement} Group containing the text glyphs
   */
  buildTextGroup(cmd) {
    const g = document.createElementNS('http://www.w3.org/2000/svg', 'g');

    for (const span of (cmd.spans || [])) {
      const spanMatrix = span.matrix || [1, 0, 0, 1, 0, 0];

      for (const glyph of (span.glyphs || [])) {
        if (glyph.cid) {
          this.ensureGlyph(glyph);

          const use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
          use.setAttribute('href', `#g_${glyph.cid}`);
          use.setAttribute('transform', `translate(${glyph.x},${glyph.y}) ${this.matrixToString(spanMatrix)}`);

          g.appendChild(use);
        } else if (glyph.path) {
          // Uncached glyph with inline path
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

          text.setAttribute('transform', `translate(${glyph.x},${glyph.y}) ${this.matrixToString(spanMatrix)}`);
          g.appendChild(text);
        }
      }
    }

    return g;
  }

  /**
   * Fill text glyphs
   */
  fillText(cmd) {
    if (!cmd.spans || cmd.spans.length === 0) return;

    const alpha = cmd.alpha !== undefined ? cmd.alpha : 1.0;
    const color = this.colorToCSS(cmd.color, alpha);

    const g = this.buildTextGroup(cmd);
    g.setAttribute('fill', color);

    this.currentGroup.appendChild(g);
  }

  /**
   * Stroke text outlines
   */
  strokeText(cmd) {
    if (!cmd.spans || cmd.spans.length === 0) return;

    const alpha = cmd.alpha !== undefined ? cmd.alpha : 1.0;
    const color = this.colorToCSS(cmd.color, alpha);

    const g = this.buildTextGroup(cmd);
    g.setAttribute('stroke', color);
    g.setAttribute('stroke-width', cmd.lineWidth || 1.0);
    g.setAttribute('fill', 'none');

    this.currentGroup.appendChild(g);
  }

  /**
   * Set clip region from filled text
   */
  clipText(cmd) {
    if (!cmd.spans || cmd.spans.length === 0) return;

    const clipId = this.generateId('clip');

    const clipPathEl = document.createElementNS('http://www.w3.org/2000/svg', 'clipPath');
    clipPathEl.setAttribute('id', clipId);

    const textGroup = this.buildTextGroup(cmd);
    clipPathEl.appendChild(textGroup);

    this.viewer.getDefs().appendChild(clipPathEl);

    // Create clipped group
    const clippedGroup = document.createElementNS('http://www.w3.org/2000/svg', 'g');
    clippedGroup.setAttribute('clip-path', `url(#${clipId})`);

    this.clipStack.push(this.currentGroup);
    this.currentGroup.appendChild(clippedGroup);
    this.currentGroup = clippedGroup;
  }

  /**
   * Set clip region from stroked text
   *
   * Uses SVG mask with stroked text to achieve clip effect.
   * This is visually correct but uses soft masking rather than hard clipping.
   */
  clipStrokeText(cmd) {
    if (!cmd.spans || cmd.spans.length === 0) return;

    const maskId = this.generateId('stroketextmask');

    // Create mask with black background and white stroked text
    const mask = document.createElementNS('http://www.w3.org/2000/svg', 'mask');
    mask.setAttribute('id', maskId);
    mask.setAttribute('maskUnits', 'userSpaceOnUse');

    // Black background covering the page
    const bg = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
    bg.setAttribute('x', '0');
    bg.setAttribute('y', '0');
    bg.setAttribute('width', this.viewer.pageWidth);
    bg.setAttribute('height', this.viewer.pageHeight);
    bg.setAttribute('fill', 'black');
    mask.appendChild(bg);

    // White stroked text
    const textGroup = this.buildTextGroup(cmd);
    textGroup.setAttribute('fill', 'none');
    textGroup.setAttribute('stroke', 'white');
    textGroup.setAttribute('stroke-width', cmd.lineWidth || 1.0);

    mask.appendChild(textGroup);
    this.viewer.getDefs().appendChild(mask);

    // Create masked group
    const maskedGroup = document.createElementNS('http://www.w3.org/2000/svg', 'g');
    maskedGroup.setAttribute('mask', `url(#${maskId})`);

    this.clipStack.push(this.currentGroup);
    this.currentGroup.appendChild(maskedGroup);
    this.currentGroup = maskedGroup;
  }

  // ============================================================================
  // Image Commands
  //
  // TODO: Image loading assumes the filename can be used directly as an href.
  // This works when the client has direct filesystem access (e.g., Electron)
  // but won't work in a pure browser environment. For web deployment, need to
  // either:
  // - Have the server serve images via HTTP endpoint
  // - Fetch images via WebSocket and use data: URLs
  // - Use a client-side file system abstraction
  // ============================================================================

  /**
   * Draw an image
   */
  fillImage(cmd) {
    const image = document.createElementNS('http://www.w3.org/2000/svg', 'image');

    // TODO: This assumes filename can be used directly as href.
    // For web deployment, may need to resolve through file manager.
    if (cmd.filename) {
      image.setAttribute('href', cmd.filename);
    } else {
      // TODO: Could implement fetch by imageId from server here
      console.warn(`[Renderer] fillImage: no filename, imageId=${cmd.imageId}`);
      return;
    }

    image.setAttribute('width', cmd.width || 1);
    image.setAttribute('height', cmd.height || 1);

    // Apply transform matrix
    if (cmd.matrix) {
      image.setAttribute('transform', this.matrixToString(cmd.matrix));
    }

    // Apply alpha if not 1.0
    if (cmd.alpha !== undefined && cmd.alpha < 1.0) {
      image.setAttribute('opacity', cmd.alpha);
    }

    // Preserve aspect ratio setting (usually none for exact placement)
    image.setAttribute('preserveAspectRatio', 'none');

    this.currentGroup.appendChild(image);
  }

  /**
   * Use image as mask to paint color
   *
   * TODO: SVG masks use luminance by default. PDF image masks may use different
   * rules (e.g., treating the image as 1-bit stencil). May need mask-type or
   * color-interpolation adjustments for accurate rendering.
   */
  fillImageMask(cmd) {
    const maskId = this.generateId('imgmask');

    // Create mask element
    const mask = document.createElementNS('http://www.w3.org/2000/svg', 'mask');
    mask.setAttribute('id', maskId);

    // Add image to mask
    const maskImage = document.createElementNS('http://www.w3.org/2000/svg', 'image');
    // TODO: Same filename assumption as fillImage
    if (cmd.filename) {
      maskImage.setAttribute('href', cmd.filename);
    } else {
      console.warn(`[Renderer] fillImageMask: no filename, imageId=${cmd.imageId}`);
      return;
    }
    maskImage.setAttribute('width', cmd.width || 1);
    maskImage.setAttribute('height', cmd.height || 1);
    maskImage.setAttribute('preserveAspectRatio', 'none');

    mask.appendChild(maskImage);
    this.viewer.getDefs().appendChild(mask);

    // Create a rect filled with the color, masked by the image
    const rect = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
    rect.setAttribute('x', '0');
    rect.setAttribute('y', '0');
    rect.setAttribute('width', cmd.width || 1);
    rect.setAttribute('height', cmd.height || 1);

    const alpha = cmd.alpha !== undefined ? cmd.alpha : 1.0;
    rect.setAttribute('fill', this.colorToCSS(cmd.color, alpha));
    rect.setAttribute('mask', `url(#${maskId})`);

    if (cmd.matrix) {
      rect.setAttribute('transform', this.matrixToString(cmd.matrix));
    }

    this.currentGroup.appendChild(rect);
  }

  /**
   * Set clip region from image mask
   *
   * TODO: SVG doesn't directly support image-based clip paths. We use a mask
   * instead, which is similar but uses alpha/luminance rather than hard clipping.
   * This may produce slightly different results (soft edges vs hard edges).
   * For true clip behavior, would need to convert image to a path.
   */
  clipImageMask(cmd) {
    const maskId = this.generateId('clipimgmask');

    const mask = document.createElementNS('http://www.w3.org/2000/svg', 'mask');
    mask.setAttribute('id', maskId);

    const maskImage = document.createElementNS('http://www.w3.org/2000/svg', 'image');
    // TODO: Same filename assumption as fillImage
    if (cmd.filename) {
      maskImage.setAttribute('href', cmd.filename);
    } else {
      console.warn(`[Renderer] clipImageMask: no filename, imageId=${cmd.imageId}`);
      return;
    }
    maskImage.setAttribute('width', cmd.width || 1);
    maskImage.setAttribute('height', cmd.height || 1);
    maskImage.setAttribute('preserveAspectRatio', 'none');

    if (cmd.matrix) {
      maskImage.setAttribute('transform', this.matrixToString(cmd.matrix));
    }

    mask.appendChild(maskImage);
    this.viewer.getDefs().appendChild(mask);

    // Create masked group
    const maskedGroup = document.createElementNS('http://www.w3.org/2000/svg', 'g');
    maskedGroup.setAttribute('mask', `url(#${maskId})`);

    this.clipStack.push(this.currentGroup);
    this.currentGroup.appendChild(maskedGroup);
    this.currentGroup = maskedGroup;
  }

  // ============================================================================
  // Shading/Gradients
  //
  // TODO: NOT IMPLEMENTED. This is a placeholder that shows a visual indicator.
  //
  // To implement properly:
  // - Linear gradients: Create <linearGradient> with stop colors from cmd
  // - Radial gradients: Create <radialGradient> with stop colors from cmd
  // - Mesh gradients: No direct SVG equivalent; would need to triangulate and
  //   approximate with many small gradient-filled paths, or use canvas fallback
  // ============================================================================

  /**
   * Fill with gradient (not implemented - logs error and shows placeholder)
   */
  fillShade(cmd) {
    const shadeType = cmd.shadeType || 'unknown';
    console.error(`[Renderer] fillShade not implemented (type: ${shadeType})`);

    // Draw a placeholder rect to show something is there
    if (cmd.bounds) {
      const rect = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
      rect.setAttribute('x', cmd.bounds.x0 || 0);
      rect.setAttribute('y', cmd.bounds.y0 || 0);
      rect.setAttribute('width', (cmd.bounds.x1 || 100) - (cmd.bounds.x0 || 0));
      rect.setAttribute('height', (cmd.bounds.y1 || 100) - (cmd.bounds.y0 || 0));
      rect.setAttribute('fill', 'rgba(200, 200, 200, 0.5)');
      rect.setAttribute('stroke', 'red');
      rect.setAttribute('stroke-width', '1');
      rect.setAttribute('stroke-dasharray', '4,4');
      this.currentGroup.appendChild(rect);
    }
  }

  // ============================================================================
  // Transparency Groups
  //
  // TODO: Basic implementation covers opacity and blend modes. More advanced
  // PDF transparency features not fully supported:
  // - Knockout groups (knockout flag) - affects how overlapping objects composite
  // - Non-isolated groups with backdrop interaction
  // - Alpha source flag
  // ============================================================================

  /**
   * Begin a transparency group
   */
  beginGroup(cmd) {
    const g = document.createElementNS('http://www.w3.org/2000/svg', 'g');

    // Apply alpha/opacity
    if (cmd.alpha !== undefined && cmd.alpha < 1.0) {
      g.setAttribute('opacity', cmd.alpha);
    }

    // Apply blend mode if specified
    if (cmd.blendMode && cmd.blendMode !== 'Normal') {
      // Map PDF blend modes to CSS mix-blend-mode
      const blendMap = {
        'Multiply': 'multiply',
        'Screen': 'screen',
        'Overlay': 'overlay',
        'Darken': 'darken',
        'Lighten': 'lighten',
        'ColorDodge': 'color-dodge',
        'ColorBurn': 'color-burn',
        'HardLight': 'hard-light',
        'SoftLight': 'soft-light',
        'Difference': 'difference',
        'Exclusion': 'exclusion',
        'Hue': 'hue',
        'Saturation': 'saturation',
        'Color': 'color',
        'Luminosity': 'luminosity'
      };
      const cssBlend = blendMap[cmd.blendMode] || 'normal';
      g.style.mixBlendMode = cssBlend;
    }

    // If isolated, use isolation CSS property
    if (cmd.isolated) {
      g.style.isolation = 'isolate';
    }

    this.groupStack.push(this.currentGroup);
    this.currentGroup.appendChild(g);
    this.currentGroup = g;
  }

  /**
   * End a transparency group
   */
  endGroup() {
    if (this.groupStack.length > 0) {
      this.currentGroup = this.groupStack.pop();
    }
  }

  // ============================================================================
  // Masks
  //
  // TODO: Basic implementation. PDF soft masks can have additional properties:
  // - Backdrop color (BC) - color to composite against
  // - Transfer function (TR) - remaps mask values
  // - Subtype (Alpha vs Luminosity) - how mask values are derived
  // Currently we just use SVG's default luminance-based masking.
  // ============================================================================

  /**
   * Begin soft mask definition
   */
  beginMask(cmd) {
    const maskId = this.generateId('softmask');

    const mask = document.createElementNS('http://www.w3.org/2000/svg', 'mask');
    mask.setAttribute('id', maskId);

    // TODO: Handle mask subtype (Alpha vs Luminosity) via mask-type CSS property
    // TODO: Handle backdrop color and transfer function if provided in cmd

    this.viewer.getDefs().appendChild(mask);

    // Save current state
    this.groupStack.push(this.currentGroup);
    this.currentMaskId = maskId;
    this.maskGroup = mask;

    // Subsequent drawing goes into the mask
    this.currentGroup = mask;
  }

  /**
   * End soft mask definition and apply to subsequent content
   */
  endMask() {
    if (!this.currentMaskId) {
      console.warn('[Renderer] endMask called without beginMask');
      return;
    }

    // Restore to previous group
    if (this.groupStack.length > 0) {
      this.currentGroup = this.groupStack.pop();
    }

    // Create a group with the mask applied for subsequent content
    const maskedGroup = document.createElementNS('http://www.w3.org/2000/svg', 'g');
    maskedGroup.setAttribute('mask', `url(#${this.currentMaskId})`);

    this.groupStack.push(this.currentGroup);
    this.currentGroup.appendChild(maskedGroup);
    this.currentGroup = maskedGroup;

    this.currentMaskId = null;
    this.maskGroup = null;
  }

  // ============================================================================
  // Tiles/Patterns
  //
  // TODO: Pattern support is incomplete. The pattern is defined in <defs> but
  // there's no mechanism to apply it to subsequent fills. In PDF, the pattern
  // is set as the current fill color, but our command protocol doesn't convey
  // this. Would need either:
  // - A way for fillPath commands to reference pattern IDs
  // - Auto-applying the pattern to the next fill after endTile
  // - Extending the protocol to include pattern references in fill commands
  // ============================================================================

  /**
   * Begin tile pattern definition
   */
  beginTile(cmd) {
    const patternId = this.generateId('pattern');

    const pattern = document.createElementNS('http://www.w3.org/2000/svg', 'pattern');
    pattern.setAttribute('id', patternId);
    pattern.setAttribute('patternUnits', 'userSpaceOnUse');

    // Set tile dimensions
    if (cmd.area) {
      pattern.setAttribute('x', cmd.area.x0 || 0);
      pattern.setAttribute('y', cmd.area.y0 || 0);
      pattern.setAttribute('width', cmd.xStep || (cmd.area.x1 - cmd.area.x0));
      pattern.setAttribute('height', cmd.yStep || (cmd.area.y1 - cmd.area.y0));
    }

    // Apply view box if we have view
    if (cmd.view) {
      const vbWidth = cmd.view.x1 - cmd.view.x0;
      const vbHeight = cmd.view.y1 - cmd.view.y0;
      pattern.setAttribute('viewBox', `${cmd.view.x0} ${cmd.view.y0} ${vbWidth} ${vbHeight}`);
    }

    // Apply transform if present
    if (cmd.matrix) {
      pattern.setAttribute('patternTransform', this.matrixToString(cmd.matrix));
    }

    this.viewer.getDefs().appendChild(pattern);

    // Save state
    this.groupStack.push(this.currentGroup);
    this.currentPatternId = patternId;
    this.patternElement = pattern;

    // Drawing now goes into the pattern
    this.currentGroup = pattern;
  }

  /**
   * End tile pattern definition
   *
   * TODO: Pattern is defined but not applied. See section comment above.
   */
  endTile(cmd) {
    if (!this.currentPatternId) {
      console.warn('[Renderer] endTile called without beginTile');
      return;
    }

    // Restore previous group
    if (this.groupStack.length > 0) {
      this.currentGroup = this.groupStack.pop();
    }

    // TODO: The pattern is now defined in <defs> as #${this.currentPatternId}
    // but we have no way to apply it. Would need protocol extension or
    // convention (e.g., next fillPath automatically uses this pattern).
    console.warn(`[Renderer] Pattern ${this.currentPatternId} defined but application not implemented`);

    this.currentPatternId = null;
    this.patternElement = null;
  }

  // ============================================================================
  // Layers (Optional Content Groups)
  //
  // Basic implementation - just creates named groups. For full PDF optional
  // content support would need:
  // TODO: Layer visibility state management (on/off/toggle)
  // TODO: Layer dependencies and mutual exclusion (radio button groups)
  // TODO: Default visibility based on zoom level, print vs screen, etc.
  // ============================================================================

  /**
   * Begin an optional content layer
   */
  beginLayer(cmd) {
    const g = document.createElementNS('http://www.w3.org/2000/svg', 'g');
    g.setAttribute('class', 'layer');

    if (cmd.name) {
      g.setAttribute('data-layer-name', cmd.name);
    }

    this.layerStack.push(this.currentGroup);
    this.currentGroup.appendChild(g);
    this.currentGroup = g;
  }

  /**
   * End the current layer
   */
  endLayer() {
    if (this.layerStack.length > 0) {
      this.currentGroup = this.layerStack.pop();
    }
  }
}
