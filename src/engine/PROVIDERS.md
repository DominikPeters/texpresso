# TeXpresso Providers

TeXpresso embeds its own version of the XeTeX engine but needs a TeX
distribution to run it. It currently supports distribution "providers"
which are used to load files from existing distributions: `tectonic`, `texlive` and `miktex`.

A provider fills two roles:
- The primary one is to return the contents of a file from its name (e.g., when
  one does `\usepackage{foo}`, the engine will search for `foo.sty`; if it is
  not contained in the input directory, the provider will be asked for this
  file).
- The second one is to provide a way to detect upgrades in order to invalidate
  files derived from the distribution (this is relevant for _format_ files).

## Design of the Tectonic Provider

Interactions with a Tectonic installation happen through the `tectonic` binary.
TeXpresso will consider that Tectonic is available only if `tectonic` can be
executed.

Tectonic has not really been designed with the idea of being used as a provider
of TeX dependencies, but two functions are offered that enable this use:
- `tectonic -X bundle search` lists the names of all files it can provide.
- `tectonic -X bundle cat <name>` outputs the contents of file `<name>`
  (if it has it).

Bundle is the name given by Tectonic to a snapshot of all TeX packages it
provides. `tectonic -X bundle cat SHA256SUM` returns a hash of the bundle,
which we use as a version identifier.

By default, files are fetched from the internet on demand and cached on disk.
The cache directory can be found using `tectonic -X show user-cache-dir`.

### Failure Paths

Commands can fail if the internet connection is not available:
1. If Tectonic has never been used, it has to fetch a bundle index first. In
   this case, `bundle search` can fail. `bundle search` also checks for updates
   from time to time by fetching upstream `SHA256SUM`. This can also fail if
   internet is not available.
2. If Tectonic has been properly initialized, `bundle search` can succeed, and
   we could think a specific file is available, yet `bundle cat` can fail
   because the file is not in cache and cannot be downloaded.

### Integration with TeXpresso

The main challenge for TeXpresso is that using `tectonic -X bundle cat` is too
slow. A process has to be spawned for each request, and the initialization costs
add up quickly.

So TeXpresso maintains its own cache in the user-wide cache directory,
`CACHE_DIR=${XDG_CACHE_HOME:-$HOME/.cache}`. TeXpresso-specific files go into
`$CACHE_DIR/texpresso`, and Tectonic ones to `$CACHE_DIR/texpresso/tectonic`.
The latter only stores a copy of `bundle cat` contents: if
`$CACHE_DIR/texpresso/tectonic/foo` exists, it should have the same contents as
`tectonic -X bundle cat foo`.

#### Initialization

The provider starts by registering all names in `tectonic -X bundle search` in a
hash set. Then it creates the cache directory if necessary and checks if
`SHA256SUM` is the same as `tectonic -X bundle cat SHA256SUM`. If not, it clears
the contents of the cache.

#### Requesting a File

Whenever a file is requested:

1. **Check Hash Set**: If the file is not in the hash set, the request is
   declined immediately.
2. **Check Cache**: If it is in the set and in cache, its contents are returned.
3. **Fetch and Cache**: If not, `bundle cat` is used to fetch the file:
   - The fetched file is copied to the cache.
   - The contents are sent to the XeTeX engine.

## Design of the TeXlive Provider

TeXpresso uses the `kpsewhich` binary to interact with a TeXlive installation.
TeXpresso will consider that TeXlive is available only if the command
`kpsewhich -engine=xetex --all ls-R` succeeds.

When successful, it outputs a list of absolute file paths of files that contain
lists of relative file paths in the format of `ls -R`.

### Integration with TeXpresso

The `ls-R` lists are read by TeXpresso and used to populate a map from file
names to absolute paths. File requests are answered by looking up this map.

To detect version changes, requests can be recorded:
- Whether they succeeded or failed.
- If they succeeded, the mtime and size of the target are recorded too.

### Limitations
- FIXME: Error handling and logging.
- BY DESIGN: Approximation of actual visibility and file ranking for a given
  engine

## Generation of Formats


## Engine compatibility and format caches

The provider supplies packages and fonts, while the compiler is the embedded
XeTeX fork. Updating TeX Live does not update its built-in primitives.

The core includes the Web2C `partoken.ch` behavior (public domain, Petr Olsak,
2021): `partokenname` globally selects the automatically inserted paragraph
control sequence, and the scoped integer `partokencontext` controls insertion
at ordinary boundaries (0), vertical boxes/centers (1), and insertion/output/
alignment contexts (2). It also includes `showstream.ch` behavior (public domain,
Marcel Krüger, 2021) for redirecting show diagnostics to an open write stream.
As in XeTeX 0.999998, bit 1 of `ignoreprimitiveerror` suppresses the recoverable
infinite-shrinkage error in `vsplit`, recording an `ignored:` line only in the
log. Other primitive errors retain their normal handling.

`xetex_format.h` began as the generated Tectonic format-33 header. TeXpresso now
maintains its own layout: three appended integer parameters shift the following
equivalence-table regions. The derived bases are expressions so that storage,
scoping, and serialization agree. `partokenname` uses a non-node extension opcode
internally, preserving the original command and mode numbering. The format serial
is 1001, checked against `xetex_bindings.h`; bump it when changing stored layout.
Cache filenames contain this serial, so an old format is neither reused nor
replaced during migration. Incremental builds track the layout headers.

Run `python3 test/test-engine-compat.py --worker build/texpresso-xetex
--reference /path/to/xelatex`, optionally adding `--old-worker /path/to/old/worker`
to check cache migration. The tests use temporary documents and private caches,
compare all paragraph contexts and redirected diagnostics against XeLaTeX, and
cover scoping, control-symbol paragraph names, primitive identity/redefinition,
error suppression, `microtype` titles/footnotes, and repeated format loads.

## MiKTeX

Select `-miktex` to use `findtexmf` on PATH for package and renderer font/resource
lookup. Auto selection tries TeX Live first, then MiKTeX, then Tectonic.
`-capabilities` advertises `packageProviders` and `miktexAutoInstall` (support
for the opt-in flag, not the current policy).

By default the provider omits `--must-exist`; findtexmf explicitly disables its
installer in this mode, overriding an interactive Ask policy. Add
`-miktex-auto-install` together with `-miktex` to permit noninteractive downloads.
Lookups use argument vectors, never a shell, and do not inherit protocol stdin.
Diagnostics go to stderr. Installed-file lookups have a 15-second bound;
installation-enabled lookups have a 180-second bound. Session process-group
cleanup also owns lookup subprocesses. Downloads happen on the engine host,
including inside the selected WSL distribution.

Successful paths are cached for the process lifetime and checked for existence;
misses are retried because packages may be installed later. MiKTeX formats use
a separate `miktex-v<SERIAL>-` cache namespace. Their successful source inputs
are recorded by absolute path, size and modification/change time and checked
before reuse. The compiler is still TeXpresso's embedded XeTeX, not MiKTeX's
compiler executable; MiKTeX package/kernel upgrades require compatibility tests.

Run `python3 test/test-miktex-provider.py` for the small command/cache regression
suite, or `python3 test/test-miktex-real.py build/texpresso` with an initialized
MiKTeX installation to exercise compilation, downloads, PNG/SVG rendering and
provider-separated cache reuse.

Text Unicode tables use the TeX input search type, TECkit maps use miscellaneous
fonts, and PDF inputs use graphics. When a renderer font is absent from the
previously loaded map, MiKTeX font-map configuration is refreshed and reloaded
under the same explicit installer policy. This covers metrics/packages installed
after the renderer was created, without assuming TeX Live's directory layout.


### Experimental Windows MiKTeX from WSL

Use `-miktex-windows` instead of `-miktex` to resolve packages through an
existing Windows MiKTeX installation. The compiler and format cache remain in
Linux; Windows `findtexmf.exe` locates package files and `wslpath` translates
returned paths. No Linux MiKTeX installation is needed. Windows system fonts
are not automatically added to Linux fontconfig.

Windows PowerShell and WSL Windows interoperability must be available. By
default the helper finds MiKTeX on the Windows PATH. For a specific installation,
pass `-miktex-windows-bin 'C:\Program Files\MiKTeX\miktex\bin\x64'`, or set
`TEXPRESSO_MIKTEX_WINDOWS_BIN` to that Windows directory. Workers inherit the
setting. Downloads remain disabled unless `-miktex-auto-install` is explicit;
when enabled they run in the Windows user's MiKTeX installation.

Arguments travel as encoded data to the fixed `src/common/miktex_windows.ps1`
persistent helper (embedded in `miktex_windows.h`); never as interpolated shell commands.
One helper serves sequential requests for each Linux engine process, avoiding
a PowerShell startup for every package file.
The helper caches its small process-job assembly under Windows LocalAppData,
bounds lookup duration, and owns descendants in a kill-on-close Windows job.
A dedicated stdin keepalive releases the job when the Linux caller disappears.
Windows format caches have a separate `miktex-windows` namespace, further
qualified by the selected binary directory when supplied. First worker startup
has a ten-minute bound for this experimental bridge, including when downloads
are disabled: building a fresh format can exceed the ordinary two-minute limit.

The ordinary provider fixture checks encoding, paths and installer policy.
Windows/WSL CI runs the real MiKTeX compile/render/cold-download/warm-cache
acceptance test with `TEXPRESSO_TEST_MIKTEX_PROVIDER=miktex-windows`, plus
`test/test-miktex-windows-cancel.py` against an owned Windows process fixture.
