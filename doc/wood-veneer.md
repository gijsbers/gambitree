# Wood Veneer maintenance

`B` cycles **Classic → Wood → Wood Veneer → Classic**; Classic remains the
default. The existing session mechanism stores the board choice independently
of pieces. `F` rotates the board; `P` selects pieces. Classic and Wood retain
their existing appearance.

Asset provenance, licensing, complete generation prompts, pinned source/output
hashes and preparation instructions live in
[the asset dossier](../assets/boards/wood-veneer/README.md). The two surfaces are
original AI-generated images, not photographs or third-party CC0 textures.

## Rendering contract

`BoardVeneer` loads the two opaque PNG sheets together and prepares a CPU raster.
The existing PNG decoder and Lanczos3 scaler filter each entire sheet once to
the board size. Each logical square then takes one fixed normalized 8×8 window.
Filtering before cutting avoids artificial filter boundaries around the crops.

For square `0=a1` through `63=h8`, `index = square/2` and
`slot = (17*index + seed) % 64`, with seed 11 for light and 37 for dark squares.
This assigns 32 distinct 128×128 source windows per colour in the 1024px assets.
Resizing preserves the source window. Rotation turns the complete material
180° while pieces stay upright. Do not change the seeds, filtering or mapping
as a cleanup: they define the approved appearance.

The composed RGBA board passes through the existing XImage conversion into the
shared background Pixmap. Theme changes invalidate it; size/orientation changes
rebuild it. Moves and ordinary redraws reuse it. Pieces, coordinates, overlays,
frame geometry and their caches remain in the existing renderer. Veneer uses
the existing flat frame colour; no second renderer or cache is needed.

The CPU module caps squares at 511px, keeping its unframed raster below 4096px
(`8*511=4088`). The renderer also reserves the 5px frame on both sides, so its
maximum is 510px (`8*510+10=4090`, whereas `8*511+10=4098`). Bounds are checked
before multiplication. Missing/invalid sheets leave the renderer's Classic
fallback available and log once; restarting retries asset lookup.

The decoded Veneer sources occupy 8 MiB and remain loaded to avoid repeat PNG
decoding on theme changes. Temporary scaled sheets, the composed raster and
Lanczos working buffers exist only during background reconstruction.

## Regression references

CPU tests require no X11. Before simplification, the approved renderer at
commit `78ff703` captured complete row-major RGBA boards without frame, pieces
or coordinates. Python independently hashed those byte streams; the fixed
FNV-1a-64 references are embedded in `tests/test_main.cpp`, not calculated from
`window()` or another expected sampling algorithm at test time:

| Board pixels | Normal | Rotated 180° |
| --- | --- | --- |
| 320 | `bcb58c60a2e540f1` | `6727e101a462a4a9` |
| 600 | `d076c8541f824349` | `8cb45792c7b75ce9` |
| 1000 | `169777092dea211e` | `c523607fb947801e` |

Tests also retain repeatability, full-pixel rotation, crop uniqueness, internal
grain, asset-failure/transaction checks, overflow bounds and theme/session
round-trips. Reference updates require an intentional visual review; do not
regenerate expected hashes just to make an unexpected failure pass.

The separate X11 tests check uploaded pixels, all logical hitboxes, overlays,
cache reuse/invalidation and piece-cache independence. Existing Classic/Wood
captures remain in their tests; Veneer captures its three positions at 320,
600 and 1000px without repeating the old Wood capture matrix.

## Installation

`make install` puts the two PNGs in `share/gambitree/boards`, with a brief artwork
credit and the GPL in the shared `share/doc/gambitree/THIRD-PARTY-NOTICES` and
`LICENSE`. Production hashes, preparation code and source-generation metadata
remain in the source distribution. `make test-install` checks staged asset
hashes against the source manifests, compact notices and cleanup.
No new runtime or build library is required.
