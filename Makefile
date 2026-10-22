all: | Makefile.config
	$(MAKE) common texpresso texpresso-xetex
	@echo "# Build succeeded."
	@echo "# TeXpresso detects package providers (TeXlive or Tectonic) by looking in PATH:"
	@echo "# - it defaults to Tectonic if the 'tectonic' command is available"
	@echo "# - it falls back to TeXlive if the 'kpsewhich' command is available"
	@echo "# A provider can be selected manually by passing the '-texlive' or '-tectonic' flags."
	@echo "#"
	@echo "# When using tectonic, first time launch needs to download many files and can be slow."
	@echo "# You can speed-up this process using:"
	@echo "#   make fill-tectonic-cache"
	@echo "#"
	@echo "# After, you can try texpresso by running:"
	@echo "#   build/texpresso test/simple.tex"
	@echo "#"
	@echo "# Or:"
	@echo "#   build/texpresso -texlive test/simple.tex"
	@echo "#   build/texpresso -tectonic test/simple.tex"

common: | Makefile.config
	$(MAKE) -C src/common

texpresso: | Makefile.config
	$(MAKE) -C src/frontend texpresso

dev: | Makefile.config
	$(MAKE) -C src texpresso-dev

debug: | Makefile.config
	$(MAKE) -C src texpresso-debug texpresso-debug-proxy

clean:
	rm -rf build/*/*

distclean:
	rm -rf build Makefile.config

re2c:
	$(MAKE) -C src/dvi $@

test-utfmapping:
	mkdir -p build
	gcc -g -o build/test_utf_mapping test/test_utf_mapping.c
	build/test_utf_mapping &> test/test_utf_mapping.output
	git diff --exit-code test/test_utf_mapping.output

UNAME := $(shell uname)

Makefile.config: Makefile
	$(MAKE) config

ifeq ($(UNAME), Linux)
config:
	mkdir -p build/objects
# LDCC: some Linux distribution build mupdf with C++ dependencies,
	echo >Makefile.config "CFLAGS=-O2 -ggdb -I. -fPIC"
	echo >>Makefile.config 'CC=gcc $$(CFLAGS)'
	echo >>Makefile.config 'LDCC=g++ $$(CFLAGS)'
	echo >>Makefile.config "LIBS=-lmupdf -lm `CC=gcc ./mupdf-config.sh` -lz -ljpeg -lharfbuzz -lfreetype -lSDL2"
endif

ifeq ($(UNAME), Darwin)
BREW=$(shell brew --prefix)
BREW_ICU4C=$(shell brew --prefix icu4c)
config:
	mkdir -p build/objects
	echo >Makefile.config "CFLAGS=-O2 -ggdb -I. -fPIC -I$(BREW)/include"
	echo >>Makefile.config 'CC=gcc $$(CFLAGS)'
	echo >>Makefile.config 'LDCC=g++ $$(CFLAGS)'
	echo >>Makefile.config "LIBS=-L$(BREW)/lib -lmupdf -lm `CC=gcc ./mupdf-config.sh -L$(BREW)/lib` -lz -ljpeg -lharfbuzz -lfreetype -lSDL2"
endif

texpresso-xetex:
	$(MAKE) -C src/engine

compile_commands.json:
	bear -- $(MAKE) -B -k all

fill-tectonic-cache:
	tectonic --outfmt fmt test/format.tex
	tectonic --outfmt xdv test/simple.tex

test-texlive:
	build/texpresso-xetex -texlive test/simple.tex
	rm simple.aux simple.log simple.xdv

test-tectonic:
	build/texpresso-xetex -tectonic test/simple.tex
	rm simple.aux simple.log simple.xdv

test-open-base64:
	printf '(open-base64 "test/simple.tex" "%s")\n' "$$(base64 < test/simple.tex | tr -d '\n')" | \
	env SDL_VIDEODRIVER=dummy build/texpresso -test-initialize test/simple.tex

test-texpresso:
	env SDL_VIDEODRIVER=dummy build/texpresso -test-initialize test/simple.tex

test-texpresso-texlive:
	env SDL_VIDEODRIVER=dummy build/texpresso -texlive -test-initialize test/simple.tex

test-texpresso-tectonic:
	env SDL_VIDEODRIVER=dummy build/texpresso -tectonic -test-initialize test/simple.tex

test-stream:
	bash test/test_stream.sh

test-register:
	bash test/test-register.sh

test-lookup-file:
	bash test/test-lookup-file.sh

test-rerun:
	bash test/test-rerun.sh

macos-app: texpresso
	@[ "$$(uname)" = "Darwin" ] || { echo "macos-app requires macOS"; exit 1; }
	bash scripts/build-macos-app.sh

.PHONY: all dev clean config texpresso common texpresso-xetex re2c compile_commands.json fill-tectonic-cache test-texlive test-tectonic test-texpresso test-stream test-open-base64 test-register test-lookup-file test-rerun macos-app

.PHONY: test-synctex
test-synctex: | Makefile.config
	$(MAKE) -f Makefile.config -f Makefile build/test-synctex
	./build/test-synctex

build/test-synctex: test/test_synctex.c src/frontend/synctex.c src/frontend/synctex.h
	mkdir -p build
	$(CC) -Isrc/include -Isrc/dvi -c -o build/test-synctex.o test/test_synctex.c
	$(CC) -Isrc/include -Isrc/dvi -c -o build/test-synctex-scanner.o src/frontend/synctex.c
	$(LDCC) -o $@ build/test-synctex.o build/test-synctex-scanner.o $(LIBS)

.PHONY: test-preview-svg
test-preview-svg: texpresso
	$(MAKE) -f Makefile.config -f Makefile build/test-preview-svg
	build/test-preview-svg
build/test-preview-svg: test/test_preview_svg.c build/frontend/preview.o build/frontend/renderer.o
	$(CC) -Isrc/frontend -Isrc/dvi -c -o build/test-preview-svg.o test/test_preview_svg.c
	$(LDCC) -o $@ build/test-preview-svg.o build/frontend/preview.o build/frontend/renderer.o $(LIBS)

.PHONY: test-texlive-provider
test-texlive-provider:
	$(MAKE) -f Makefile.config -f Makefile build/test-texlive-provider
	build/test-texlive-provider
build/test-texlive-provider: test/test_texlive_provider.c src/common/texlive_provider.c
	$(CC) -Isrc/include -o $@ test/test_texlive_provider.c

.PHONY: test-dvi-annotations
test-dvi-annotations: texpresso
	$(MAKE) -f Makefile.config -f Makefile build/test-dvi-annotations
	build/test-dvi-annotations
build/test-dvi-annotations: test/test_dvi_annotations.c build/frontend/incdvi.o build/frontend/libmydvi.a build/common/libcommon.a
	$(CC) -Isrc/frontend -Isrc/dvi -c -o build/test-dvi-annotations.o test/test_dvi_annotations.c
	$(LDCC) -o $@ build/test-dvi-annotations.o build/frontend/incdvi.o build/frontend/libmydvi.a build/common/libcommon.a $(LIBS)

.PHONY: test-preview-png
test-preview-png: texpresso
	$(MAKE) -f Makefile.config -f Makefile build/test-preview-png
	build/test-preview-png
build/test-preview-png: test/test_preview_png.c build/frontend/preview.o build/frontend/renderer.o
	$(CC) -Isrc/frontend -Isrc/dvi -c -o build/test-preview-png.o test/test_preview_png.c
	$(LDCC) -o $@ build/test-preview-png.o build/frontend/preview.o build/frontend/renderer.o $(LIBS)

.PHONY: test-dvi-strokes
test-dvi-strokes: texpresso
	$(MAKE) -f Makefile.config -f Makefile build/test-dvi-strokes
	build/test-dvi-strokes
build/test-dvi-strokes: test/test_dvi_strokes.c build/frontend/libmydvi.a build/common/libcommon.a
	$(CC) -Isrc/dvi -c -o build/test-dvi-strokes.o test/test_dvi_strokes.c
	$(LDCC) -o $@ build/test-dvi-strokes.o build/frontend/libmydvi.a build/common/libcommon.a $(LIBS)
