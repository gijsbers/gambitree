# Cburnett integration

The Cburnett set is now bundled alongside Gambitree Classic and Meridian Shaded.
Gambitree Classic remains the default; no existing artwork, wooden-board colors,
chess parser, chess rules or navigation behavior was changed. No staging, commit
or push is part of this work. Cburnett should be reviewed visually before any
future decision to change the default or commit the work.

## Source verification and license

All twelve user-supplied originals in `assets/pieces/cburnett/source/` were checked
against `SOURCES.sha1` before conversion. Every source matched exactly. The SVGs
remain byte-identical. The previously verified Commons pages identify Cburnett
(Colin M. L. Burnett), offer BSD with three conditions and allow choosing that
license. The artwork uses that BSD 3-Clause option. The complete notice and
license are retained separately from the unchanged project LICENSE.

The [asset README](../assets/pieces/cburnett/README.md) lists the original source
URLs, sizes, verified SHA-1 values, converter version and regeneration procedure.
[SHA256SUMS](../assets/pieces/cburnett/SHA256SUMS) records actual imported SVG and
generated PNG bytes. Install/uninstall includes PNG assets and the complete BSD
notice in `share/doc/gambitree/THIRD-PARTY-NOTICES`. Original SVGs, hashes and the
rendering recipe remain in the source distribution.

## Existing architecture reused

- `src/piece_set.cpp` adds one immutable descriptor after the two existing sets.
  Existing descriptors and the Classic default are unchanged.
- `PieceAssetDirectory` maps the twelve existing PNG filenames. `AssetLocator`
  searches installed data and existing executable-relative/legacy fallbacks.
- `AssetManager` transactionally decodes all twelve PNGs with libpng, crops
  transparent margins and swaps the set only when the whole load succeeds.
- `BoardRenderer::load_piece_set()` invalidates piece caches only after a
  successful load. The existing fallback handles unavailable/corrupt sets.
- The existing scaler preserves aspect ratio and uses premultiplied alpha and
  Lanczos3. Its high-quality cache depends on set ID and final square size.
  Redraws reuse cached rasters; interactive resizing uses temporary rasters and
  the existing deferred high-quality rebuild. No rendering/cache code changed.
- The existing batched XImage compositor combines RGBA pieces with the actual
  background. Wood has a separate size/orientation cache and needs no recoloring.
- `BoardLayout` continues to own square geometry, board rotation and hitboxes.
  Game parsing, chess rules and navigation source files are untouched.

SVG conversion is performed once during artwork preparation, using librsvg 2.57.4
at 512x512 with transparency. The normal build/runtime needs no SVG dependency.
Measurements of the original alpha bounds, common canvas scale and baseline
rounding determine the descriptor's heights and offsets, rather than distortion
or redraw-time SVG work.

## Selecting and reviewing

```sh
./gambitree --pieces cburnett your-game.pgn
```

Use `P` to cycle Classic, Meridian Shaded and Cburnett, `B` to select wood and
`F` to rotate. The existing session mechanism remembers the piece and board
choices independently. `--pieces` overrides a saved piece choice. No shortcut
was introduced and Cburnett was not made the default.

## Regression coverage and visual captures

CPU tests cover registry order and default, cycling, command-line selection,
session persistence/override, installed asset discovery, all twelve PNG masters,
transparent borders and antialiasing, aspect-ratio-preserving scaling at six
square sizes, fitted alpha bounds, compositing on light/dark backgrounds and
transactional load failure. Existing parser/rules/navigation tests also run.

The X11 tests additionally cover actual piece drawing on classic/wood at 320,
600 and 1000 board pixels, unchanged redraw cache reuse, board-theme changes,
rotation, all 64 rotated hitboxes, interactive/deferred resizing and switching
between Cburnett and Meridian without discarding the wooden background cache.
Two gallery positions put each of the twelve distinct pieces on both square
colors. These are rendering fixtures, not replacements for the original artwork.

When an X11 display is available, run:

```sh
xvfb-run -a -s "-screen 0 1600x900x24" make test-x11
```

This captures actual renderer PPM images under
`cburnett/screenshots/` below the scratch directory printed by the X11 tests,
including Classic/classic,
Cburnett/classic, Cburnett/wood, small/large sizes, rotation and the galleries.
If the sandbox prevents Xvfb from creating its sockets, X11 tests and captures
remain unverified; no permission changes or synthetic GUI screenshots are used.
Build/test logs and captures are local development artifacts, not part of the
source distribution. Scratch paths use `TMPDIR`, with `build/` as the default.

## Validated results (2026-11-13)

With an explicit scratch directory, `make clean`, `make`, `make test`, `make debug`,
`make release`, `make ubsan`, `make test-install` and `git diff --check` passed.
UBSan reported no runtime errors. A normal release executable is left for local
review after the sanitizer run. The X11 test program also compiled successfully.
The requested Xvfb invocation could not run the graphical checks: Xvfb failed
to open local/UNIX listening sockets and the test executable reported
`X11 display unavailable` (77). No system permissions were changed. Actual X11
screenshots and visual verification on wood therefore remain pending a working
display; the X11 test assertions are not reported as passed.

All 24 imported/generated SHA-256 hashes match, and repeating the conversion
with the recorded converter version produced byte-identical PNGs. Negative
checks on scratch copies also confirmed that missing or hash-mismatched SVGs
stop the recipe before it generates any PNG. SHA-256 snapshots confirm the
original SVGs, existing Classic/Meridian PNGs, wooden textures and project
LICENSE are unchanged.

A local asset preview showed the actual PNG artwork on flat light/dark
backgrounds. It was explicitly labeled as an asset preview, not an X11
screenshot or wooden-board assessment, and is not included in the distribution.
Use the running application on the host to make that assessment before deciding
about any commit or default change.

## Changed files

- `src/piece_set.cpp`, `src/command_line.cpp`, `src/main.cpp`,
  `src/keyboard_help.cpp`: register/select Cburnett and update help.
- `assets/pieces/cburnett/`: twelve new PNG masters, `render.sh`, `SHA256SUMS`,
  updated README and LICENSE. `SOURCES.sha1` and all twelve SVGs are unchanged.
- `Makefile`: install/uninstall and staged-install coverage for the new set and
  full third-party notices.
- `README.md`, `assets/README.md`, `doc/gambitree.1`, this document:
  selection instructions, source/license/rendering records and validation.
- `tests/test_main.cpp`, `tests/test_manpage.sh`, `tests/test_x11_notation.cpp`:
  CPU regression tests, help checks, and X11/cache/resize/rotation/capture tests.
