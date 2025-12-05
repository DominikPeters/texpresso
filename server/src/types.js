/**
 * @fileoverview Type definitions for TeXpresso WebSocket protocol
 * Using JSDoc for type checking without full TypeScript setup
 */

// ============================================================================
// Client → Server Messages
// ============================================================================

/**
 * @typedef {Object} InitMessage
 * @property {'init'} type
 * @property {string} [sessionId] - Optional session ID
 * @property {{name: string, content: string}} document - Initial document
 */

/**
 * @typedef {Object} CloseMessage
 * @property {'close'} type
 * @property {string} [sessionId]
 */

/**
 * @typedef {Object} FileOpenMessage
 * @property {'file.open'} type
 * @property {string} path
 * @property {string} content
 */

/**
 * @typedef {Object} FileChange
 * @property {number} offset - Byte offset in UTF-8
 * @property {number} length - Bytes to remove
 * @property {string} text - Replacement text
 */

/**
 * @typedef {Object} FileChangeMessage
 * @property {'file.change'} type
 * @property {string} path
 * @property {FileChange[]} changes
 */

/**
 * @typedef {Object} FileChangeRangeMessage
 * @property {'file.changeRange'} type
 * @property {string} path
 * @property {number} startLine
 * @property {number} startCol
 * @property {number} endLine
 * @property {number} endCol
 * @property {string} text
 */

/**
 * @typedef {Object} FileChangeLineMessage
 * @property {'file.changeLine'} type
 * @property {string} path
 * @property {number} startLine
 * @property {number} endLine
 * @property {string} text
 */

/**
 * @typedef {Object} FileCloseMessage
 * @property {'file.close'} type
 * @property {string} path
 */

/**
 * @typedef {Object} NavGotoMessage
 * @property {'nav.goto'} type
 * @property {number} page
 */

/**
 * @typedef {Object} SyncTexForwardMessage
 * @property {'synctex.forward'} type
 * @property {string} path
 * @property {number} line
 * @property {number} [column]
 */

/**
 * @typedef {Object} SyncTexBackwardMessage
 * @property {'synctex.backward'} type
 * @property {number} x
 * @property {number} y
 * @property {number} page
 */

/**
 * @typedef {Object} ConfigThemeMessage
 * @property {'config.theme'} type
 * @property {[number, number, number]} background - RGB floats 0-1
 * @property {[number, number, number]} foreground - RGB floats 0-1
 */

/**
 * @typedef {Object} RenderRequestMessage
 * @property {'render.request'} type
 * @property {number} page
 * @property {number} [scale]
 */

/**
 * @typedef {InitMessage | CloseMessage | FileOpenMessage | FileChangeMessage |
 *           FileChangeRangeMessage | FileChangeLineMessage | FileCloseMessage | NavGotoMessage |
 *           SyncTexForwardMessage | SyncTexBackwardMessage | ConfigThemeMessage |
 *           RenderRequestMessage} ClientMessage
 */

// ============================================================================
// Server → Client Messages
// ============================================================================

/**
 * @typedef {Object} StatusMessage
 * @property {'status'} type
 * @property {'compiling' | 'ready' | 'error'} state
 * @property {number} [progress] - Optional, 0-1
 */

/**
 * @typedef {Object} LogMessage
 * @property {'log'} type
 * @property {'error' | 'warning' | 'info'} level
 * @property {string} [file]
 * @property {number} [line]
 * @property {string} message
 */

/**
 * @typedef {Object} PageCountMessage
 * @property {'doc.pageCount'} type
 * @property {number} count
 */

/**
 * @typedef {Object} PageDimMessage
 * @property {'doc.pageDim'} type
 * @property {number} page
 * @property {number} width - Points
 * @property {number} height - Points
 * @property {boolean} landscape
 */

/**
 * @typedef {Object} InputFileMessage
 * @property {'doc.inputFile'} type
 * @property {number} index
 * @property {string} path
 */

/**
 * @typedef {Object} RenderPageMessage
 * @property {'render.page'} type
 * @property {number} page
 * @property {any[]} commands - Drawing commands
 */

/**
 * @typedef {Object} RenderUpdateMessage
 * @property {'render.update'} type
 * @property {number} page
 * @property {number} invalidateFrom - Command index
 * @property {any[]} commands - New commands
 */

/**
 * @typedef {Object} SyncTexLocation
 * @property {string} file
 * @property {number} line
 * @property {number} column
 * @property {number} x
 * @property {number} y
 * @property {number} page
 */

/**
 * @typedef {Object} SyncTexResultMessage
 * @property {'synctex.result'} type
 * @property {'forward' | 'backward'} direction
 * @property {SyncTexLocation[]} locations
 */

/**
 * @typedef {Object} OutputAppendMessage
 * @property {'output.append'} type
 * @property {'stdout' | 'log'} stream
 * @property {string} text
 */

/**
 * @typedef {Object} OutputTruncateMessage
 * @property {'output.truncate'} type
 * @property {'stdout' | 'log'} stream
 * @property {number} size
 */

/**
 * @typedef {Object} ErrorMessage
 * @property {'error'} type
 * @property {string} code
 * @property {string} message
 * @property {any} [details]
 */

/**
 * @typedef {StatusMessage | LogMessage | PageCountMessage | PageDimMessage |
 *           InputFileMessage | RenderPageMessage | RenderUpdateMessage |
 *           SyncTexResultMessage | OutputAppendMessage | OutputTruncateMessage |
 *           ErrorMessage} ServerMessage
 */

// ============================================================================
// Internal Types
// ============================================================================

/**
 * @typedef {Object} VFSFile
 * @property {string} path
 * @property {string} content
 * @property {boolean} dirty
 */

export {
  // Just export as empty to mark this as a module
  // Types are available via JSDoc
};
