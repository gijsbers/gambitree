# Installing Gambitree

Gambitree targets GNU/Linux with X11 and C++98.

## Dependencies

For building: a C++ compiler, GNU Make, `pkg-config`, X11 development files,
Xft, fontconfig, freetype, and libpng development files. The Makefile obtains
flags through `pkg-config` for X11, Xft/fontconfig/freetype, and libpng, with
linker fallbacks for the core libraries.

At runtime Gambitree needs an X display, fontconfig-accessible fonts, and its
installed PNG assets. Roboto and DejaVu Sans are recommended for text and chess
figurines; fontconfig supplies fallbacks when a requested font is unavailable.
The optional window icon is loaded from installed or executable-relative PNG
assets. `stockfish` is optional; without it the viewer works and reports analysis
as unavailable. Use `--engine PATH` for another UCI engine.

Piece SVGs have already been converted to PNG. The normal build and runtime do
not require an SVG renderer or the optional asset-preparation tools.

## Build

```sh
make clean
make PREFIX=/usr/local
```

`make debug` and `make release` clean and rebuild the program with the respective
compiler flags. Use the same `PREFIX` for every build and installation command:
the executable's installed asset path is compiled from `PREFIX`. Clean and
rebuild when changing it. `DESTDIR` only adds a packaging staging root; it is
not part of the compiled asset path.

## Tests

Run checks from the repository root. Each test program creates an isolated
scratch directory under `$TMPDIR`, or under `build/` when `TMPDIR` is unset or
empty. To choose a different location, set `TMPDIR` to an existing writable
directory. Relative paths and paths containing spaces are supported.

```sh
make test
make test-install
```

`make test-install` checks staged installation and removal, asset checksums,
license files, and the CPU regressions using installed piece assets. It needs
`sha1sum` and `sha256sum`. Rendered manpage checks additionally use `groff` and
`col` when available.
The X11 tests print the location of their captures and diagnostic files.

Sanitizer regression builds, when supported by the compiler, are:

```sh
make asan
make ubsan
```

The complete X11 rendering and interaction suite needs an X server without a
window manager: it checks exact geometry and simulates WM-owned window states.
A desktop window manager can override those requests and invalidate the tests.
Use an isolated display, for example with Xvfb and `xvfb-run`:

```sh
xvfb-run -a -s "-screen 0 1600x900x24" make test-x11
```

These graphical checks cannot run in an environment that prevents Xvfb from
opening its X11 sockets.

## Install and package staging

```sh
make install PREFIX=/usr/local
make uninstall PREFIX=/usr/local
```

When replacing an older installation, run `make uninstall` with its original
`PREFIX` before installing again. Uninstall also removes the former expanded
asset-documentation directories.

For packaging, use a staging root:

```sh
make clean
make PREFIX=/usr
make install PREFIX=/usr DESTDIR="$PWD/stage"
make uninstall PREFIX=/usr DESTDIR="$PWD/stage"
```

Installation places the executable, `gambitree(1)`, Gambitree Classic, Meridian
Shaded and Cburnett PNG pieces, PNG textures for Wood and Wood Veneer, the
multi-size hicolor application icons, a freedesktop desktop entry, and asset
compact license notices below the requested prefix. Classic remains
the default board and Gambitree Classic the default piece set.

The runtime data live in `PREFIX/share/gambitree`. Documentation below
`PREFIX/share/doc/gambitree` consists of the project `LICENSE` and
`THIRD-PARTY-NOTICES`, including Cburnett's complete BSD notice. Original SVGs,
source URLs, manifests, hashes and preparation scripts remain in the source
distribution and are not installed.
