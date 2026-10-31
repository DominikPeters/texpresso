[simple.tex](simple.tex) (`make simple`): sample .tex file to check that basic features are working as expected

[include.tex](include.tex) (`make include`): a .tex file to test include path support, it needs `-I incpath` to be passed to TeXpresso to build correctly.

`make test-synctex` checks inverse-search geometry without running a compiler:
text-line height/depth, compressed vertical coordinates, nearby whitespace,
negative kern widths, nested boxes, invisible struts, and glyph-only boxes.
It uses the same MuPDF/compiler configuration as the frontend.

`make test-preview-png` checks exact pixels with an independent system-zlib
decoder, chunk CRCs, large incompressible streams, grayscale/RGB detection,
resolution metadata and dirty-patch erasure. Run it with the default frontend
configuration and again with `-DTXP_ZLIB_NG`, the generated native zlib-ng
include path and `libz-ng.a` added to `Makefile.config`. That backend streams
standard PNG at level 2 and uses distinct `zng_` symbols so other zlib users
keep their system ABI. `-capabilities` reports the chosen backend and level.
