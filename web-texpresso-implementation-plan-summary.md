# Web-Based TeXpresso Implementation Plan - Summary

This document provides a quick navigation reference to the full implementation plan.
Each section header shows the line number range in `web-texpresso-implementation-plan.md`.

---

## Document Structure

### Main Sections

- **Line 1-2:** # Web-Based TeXpresso Implementation Plan
- **Line 3-7:** ## Executive Summary
- **Line 9-73:** ## 0. Motivation and Goals
  - Line 11-18: ### 0.1 What We're Building
  - Line 20-36: ### 0.2 Why a Web Version?
  - Line 38-45: ### 0.3 Design Principles
  - Line 47-73: ### 0.4 Expected User Experience

- **Line 75-525:** ## 1. Architecture Overview
  - Line 77-104: ### 1.1 Current TeXpresso Architecture
  - Line 106-152: ### 1.2 Proposed Web Architecture
  - Line 154-170: ### 1.3 Why Node.js Wrapper + Headless TeXpresso?
  - Line 172-247: ### 1.4 Source Code Reference
    - Line 176-184: #### Core Application (`src/`)
    - Line 186-192: #### Editor Protocol (`src/`)
    - Line 194-199: #### Server Protocol (TeX ↔ TeXpresso)
    - Line 201-214: #### DVI/XDV Rendering (`src/dvi/`)
    - Line 216-223: #### Font Support (`src/dvi/`)
    - Line 225-232: #### State Management (`src/`)
    - Line 234-240: #### Other (`src/`)
    - Line 242-247: #### Protocol Documentation
  - Line 249-525: ### 1.5 Backend Implementation Strategy
    - Line 253-288: #### 1.5.1 Process Architecture
    - Line 290-354: #### 1.5.2 Node.js Server Implementation
    - Line 356-425: #### 1.5.3 TeXpresso Headless Mode
    - Line 427-444: #### 1.5.4 stdin/stdout JSON Protocol
    - Line 446-510: #### 1.5.5 Command Generator Design
    - Line 512-525: #### 1.5.6 Reusable Code from Current Implementation

- **Line 527-731:** ## 2. Communication Protocols
  - Line 529-695: ### 2.1 Frontend ↔ Backend WebSocket Protocol
    - Line 533-632: #### 2.1.1 Client → Server Messages
      - Line 535-552: ##### Session Management
      - Line 554-590: ##### File Operations (VFS)
      - Line 592-615: ##### Navigation Commands
      - Line 617-632: ##### Configuration
    - Line 634-731: #### 2.1.2 Server → Client Messages
      - Line 636-653: ##### Status Messages
      - Line 655-678: ##### Document Metadata
      - Line 680-695: ##### Rendering Commands (See Section 3)
      - Line 697-713: ##### SyncTeX Response
      - Line 715-731: ##### Log Output

- **Line 733-1011:** ## 3. Rendering Command Protocol
  - Line 737-743: ### 3.1 Design Philosophy
  - Line 745-971: ### 3.2 Command Types
    - Line 747-789: #### 3.2.1 Font Commands
    - Line 791-826: #### 3.2.2 Text Commands
    - Line 828-855: #### 3.2.3 Graphics State
    - Line 857-909: #### 3.2.4 Path Commands
    - Line 911-940: #### 3.2.5 Image Commands
    - Line 942-971: #### 3.2.6 Special Commands
  - Line 973-1011: ### 3.3 Complete Page Example

- **Line 1013-1091:** ## 4. Font Handling
  - Line 1017-1023: ### 4.1 Font Types in TeXpresso
  - Line 1025-1038: ### 4.2 Font Pipeline
  - Line 1040-1061: ### 4.3 Font Message Sequence
  - Line 1063-1091: ### 4.4 Glyph Mapping

- **Line 1093-1130:** ## 5. Resource Management
  - Line 1095-1110: ### 5.1 Server-Side Resources
  - Line 1112-1130: ### 5.2 Client-Side Caching

- **Line 1132-1216:** ## 6. Incremental Updates
  - Line 1134-1141: ### 6.1 Update Strategy
  - Line 1143-1173: ### 6.2 Incremental Render Protocol
  - Line 1175-1216: ### 6.3 Client-Side Command Buffer

- **Line 1218-1308:** ## 7. Implementation Phases
  - Line 1220-1244: ### Phase 1: Core Infrastructure (4-6 weeks)
  - Line 1246-1258: ### Phase 2: Font System (3-4 weeks)
  - Line 1260-1276: ### Phase 3: Complete Rendering (3-4 weeks)
  - Line 1278-1288: ### Phase 4: Editor Integration (2-3 weeks)
  - Line 1290-1308: ### Phase 5: Performance & Polish (2-3 weeks)

- **Line 1310-1382:** ## 8. Technical Considerations
  - Line 1312-1335: ### 8.1 mupdf.js Capabilities
  - Line 1337-1344: ### 8.2 Binary Data Handling
  - Line 1346-1357: ### 8.3 Error Handling
  - Line 1359-1365: ### 8.4 Security Considerations
  - Line 1367-1382: ### 8.5 Scaling Considerations

- **Line 1384-1480:** ## 9. Testing Strategy
  - Line 1386-1450: ### 9.0 Basic Test Scripts
    - Line 1390-1398: #### Test TeXpresso Headless Mode Directly
    - Line 1400-1416: #### Test WebSocket Server with websocat
    - Line 1418-1450: #### Python WebSocket Test
  - Line 1452-1458: ### 9.1 Unit Tests
  - Line 1460-1465: ### 9.2 Integration Tests
  - Line 1467-1471: ### 9.3 Visual Regression Tests
  - Line 1473-1480: ### 9.4 Performance Tests

- **Line 1482-1571:** ## 10. Appendices
  - Line 1484-1513: ### Appendix A: DVI Opcodes Reference
  - Line 1515-1527: ### Appendix B: DVI Specials Supported
  - Line 1529-1569: ### Appendix C: Project Structure
  - Line 1571+: ### Appendix D: References

---

## Quick Reference

To jump to a specific section in the full document:
1. Open `web-texpresso-implementation-plan.md`
2. Use the line numbers above to navigate directly to the section of interest
3. Example: "Line 356-425: TeXpresso Headless Mode" means the content spans from line 356 to 425
