[simple.tex](simple.tex) (`make simple`): sample .tex file to check that basic features are working as expected

[include.tex](include.tex) (`make include`): a .tex file to test include path support, it needs `-I incpath` to be passed to TeXpresso to build correctly.

`make test-synctex` checks inverse-search geometry without running a compiler:
text-line height/depth, compressed vertical coordinates, nearby whitespace,
negative kern widths, nested boxes, invisible struts, and glyph-only boxes.
It uses the same MuPDF/compiler configuration as the frontend.
