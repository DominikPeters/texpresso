/**
 * @fileoverview Protocol translator between WebSocket JSON and TeXpresso S-expressions
 *
 * The WebSocket protocol uses structured JSON objects (e.g., {"type": "file.change", ...})
 * while TeXpresso expects S-expressions encoded as JSON arrays (e.g., ["change", ...])
 *
 * This module translates between these two formats.
 */

/**
 * Translate WebSocket JSON message to TeXpresso S-expression
 * @param {import('./types.js').ClientMessage} msg - WebSocket message
 * @returns {any[] | null} S-expression as JSON array, or null if not translatable
 */
export function toSExpression(msg) {
  switch (msg.type) {
    case 'init':
      // TeXpresso doesn't need an init command, document is passed via CLI
      // We use 'open' to set the initial content
      return ['open', msg.document.name, msg.document.content];

    case 'file.open':
      // (open "path" "contents")
      return ['open', msg.path, msg.content];

    case 'file.close':
      // (close "path")
      return ['close', msg.path];

    case 'file.change':
      // (change "path" offset length "data")
      // WebSocket protocol sends an array of changes, TeXpresso expects one at a time
      // We'll return multiple S-expressions
      if (!msg.changes || msg.changes.length === 0) {
        return null;
      }
      // For simplicity, we'll return just the first change here
      // The session should handle multiple changes by calling this multiple times
      const change = msg.changes[0];
      return ['change', msg.path, change.offset, change.length, change.text];

    case 'file.changeRange':
      // (change-range "path" start-line start-column end-line end-column "replacement-text")
      return ['change-range', msg.path, msg.startLine, msg.startCol, msg.endLine, msg.endCol, msg.text];

    case 'file.changeLine':
      // (change-lines "path" offset count "data")
      const lineCount = msg.endLine - msg.startLine + 1;
      return ['change-lines', msg.path, msg.startLine, lineCount, msg.text];

    case 'synctex.forward':
      // (synctex-forward "path" line column)
      return ['synctex-forward', msg.path, msg.line, msg.column || 0];

    case 'synctex.backward':
      // (synctex-backward page x y)
      return ['synctex-backward', msg.page, msg.x, msg.y];

    case 'nav.goto':
      // (goto-page page)
      return ['goto-page', msg.page];

    case 'config.theme':
      // (set-theme bg-r bg-g bg-b fg-r fg-g fg-b)
      if (msg.background && msg.foreground) {
        return ['set-theme',
          msg.background[0], msg.background[1], msg.background[2],
          msg.foreground[0], msg.foreground[1], msg.foreground[2]
        ];
      }
      return null;

    case 'render.request':
      // (render-page page [scale])
      return ['render-page', msg.page, msg.scale || 1.0];

    default:
      return null;
  }
}

/**
 * Translate multiple file changes into multiple S-expressions
 * @param {string} path
 * @param {Array<{offset: number, length: number, text: string}>} changes
 * @returns {any[][]}
 */
export function translateFileChanges(path, changes) {
  return changes.map(change => ['change', path, change.offset, change.length, change.text]);
}

/**
 * Parse TeXpresso S-expression message to WebSocket JSON
 * @param {any} sexp - S-expression (JSON array or object)
 * @returns {import('./types.js').ServerMessage | null}
 */
export function fromSExpression(sexp) {
  // Handle both JSON objects (already parsed) and arrays (S-expressions)
  if (!Array.isArray(sexp)) {
    // Already a JSON object, probably from newer TeXpresso
    if (sexp.type) {
      return sexp;
    }
    return null;
  }

  // Parse S-expression array
  const [cmd, ...args] = sexp;

  switch (cmd) {
    case 'append':
      // ["append", "log", position, "text"]
      // ["append", "stdout", position, "text"]
      if (args[0] === 'log' || args[0] === 'stdout') {
        return {
          type: 'output.append',
          stream: args[0],
          text: args[2] || ''
        };
      }
      return null;

    case 'truncate':
      // ["truncate", "log", size]
      // ["truncate", "stdout", size]
      if (args[0] === 'log' || args[0] === 'stdout') {
        return {
          type: 'output.truncate',
          stream: args[0],
          size: args[1] || 0
        };
      }
      return null;

    case 'append-lines':
      // ["append-lines", "log", line, "text"]
      if (args[0] === 'log' || args[0] === 'stdout') {
        return {
          type: 'output.append',
          stream: args[0],
          text: args[2] || ''
        };
      }
      return null;

    case 'truncate-lines':
      // ["truncate-lines", "log", lines]
      if (args[0] === 'log' || args[0] === 'stdout') {
        return {
          type: 'output.truncate',
          stream: args[0],
          size: args[1] || 0
        };
      }
      return null;

    case 'input-file':
      // ["input-file", index, "path"]
      return {
        type: 'doc.inputFile',
        index: args[0],
        path: args[1]
      };

    case 'page-count':
      // ["page-count", count]
      return {
        type: 'doc.pageCount',
        count: args[0]
      };

    case 'page-dim':
      // ["page-dim", page, width, height]
      return {
        type: 'doc.pageDim',
        page: args[0],
        width: args[1],
        height: args[2],
        landscape: args[1] > args[2]
      };

    case 'synctex-result':
      // ["synctex-result", "forward"|"backward", [[file, line, col, x, y, page], ...]]
      return {
        type: 'synctex.result',
        direction: args[0],
        locations: args[1].map(loc => ({
          file: loc[0],
          line: loc[1],
          column: loc[2],
          x: loc[3],
          y: loc[4],
          page: loc[5]
        }))
      };

    case 'error':
      // ["error", "code", "message"]
      return {
        type: 'error',
        code: args[0],
        message: args[1]
      };

    case 'log':
      // ["log", "error"|"warning"|"info", "file", line, "message"]
      return {
        type: 'log',
        level: args[0],
        file: args[1],
        line: args[2],
        message: args[3]
      };

    default:
      // Unknown S-expression, return null (caller should handle)
      return null;
  }
}

/**
 * Check if a line is a valid JSON
 * @param {string} line
 * @returns {boolean}
 */
export function isJSON(line) {
  try {
    JSON.parse(line);
    return true;
  } catch {
    return false;
  }
}
