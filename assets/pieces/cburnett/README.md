# Cburnett artwork provenance audit

Cburnett by Colin M. L. Burnett is bundled as `cburnett`. Select it with
`--pieces cburnett`, or use the existing `P` cycle: Gambitree Classic,
Meridian Shaded, Cburnett. The existing session mechanism remembers the choice.
`B` selects the wooden board independently and `F` rotates it. Gambitree Classic
remains the default; the wooden board artwork and colors have not been changed.

The twelve original SVGs were supplied locally by the user and verified against
SOURCES.sha1 on 2026-11-13 **before any conversion or integration**. Every file
matched exactly; none was substituted, reformatted, recolored or otherwise edited.

## Author and license

All twelve original Wikimedia Commons file-description pages were inspected on
2026-11-12. Each identifies its source as the uploader's own work and its author
as Cburnett (Colin M. L. Burnett). Each offers BSD with three conditions alongside
other license choices and explicitly allows selecting one license. BSD-3-Clause
is the selected license for the imported SVGs and their derived PNG renders.
The full BSD notice and disclaimer are preserved in LICENSE in this directory.
This choice does not replace or change Gambitree's project LICENSE.

Author: <https://commons.wikimedia.org/wiki/User:Cburnett>

Later file revisions also credit other Commons contributors. Their attribution
and revision history remain available from each file-description page; an import
must retain the actual source files unchanged, including any embedded notices.

## Verified upstream files

The upstream hashes below were read from each Commons page's structured data
(SHA-1, not Git object IDs). All twelve actual imported files match these hashes
and byte sizes exactly. All canvases are 45 by 45. `source/` retains the original
bytes, including original XML formatting, line endings and any embedded notices.

[SHA256SUMS](SHA256SUMS) records the actual SHA-256 digests of all twelve imported
SVGs and all twelve generated PNGs. The source/PNG mapping is listed below.

| Commons source and file history | Runtime PNG name | Bytes | Upstream SHA-1 |
| --- | --- | ---: | --- |
| [Chess_klt45.svg](https://commons.wikimedia.org/wiki/File:Chess_klt45.svg) | `white-king.png` | 700 | `2c7569b837971207e40f7148e2b2086aaf8e4bbd` |
| [Chess_qlt45.svg](https://commons.wikimedia.org/wiki/File:Chess_qlt45.svg) | `white-queen.png` | 1087 | `e638eb28ec25007b9f8fac8476bdf6ae1fc5a0ee` |
| [Chess_rlt45.svg](https://commons.wikimedia.org/wiki/File:Chess_rlt45.svg) | `white-rook.png` | 1198 | `126b7779885b87e87acc713474587732711bc8d3` |
| [Chess_blt45.svg](https://commons.wikimedia.org/wiki/File:Chess_blt45.svg) | `white-bishop.png` | 1282 | `35dd477dc22636bfcc559a01f75b00369205ffda` |
| [Chess_nlt45.svg](https://commons.wikimedia.org/wiki/File:Chess_nlt45.svg) | `white-knight.png` | 1325 | `3a2253429c0e39863b3f5ecf447209dccecdc337` |
| [Chess_plt45.svg](https://commons.wikimedia.org/wiki/File:Chess_plt45.svg) | `white-pawn.png` | 766 | `09d59e2770fcee23722ac53b26e875f76d2c1eb1` |
| [Chess_kdt45.svg](https://commons.wikimedia.org/wiki/File:Chess_kdt45.svg) | `black-king.png` | 1456 | `b1165ef85a3df6f1af2549ab0af78ab21b540e8a` |
| [Chess_qdt45.svg](https://commons.wikimedia.org/wiki/File:Chess_qdt45.svg) | `black-queen.png` | 1422 | `ed72e75b7bdbf880a3c9bee053c8786cfab8bdb9` |
| [Chess_rdt45.svg](https://commons.wikimedia.org/wiki/File:Chess_rdt45.svg) | `black-rook.png` | 1818 | `bd0e866f1e6da8e3d6f9c0b356b60fc58391aff6` |
| [Chess_bdt45.svg](https://commons.wikimedia.org/wiki/File:Chess_bdt45.svg) | `black-bishop.png` | 1282 | `da6dd1b5ef629bacebbd2ce26c7d81ba8a205587` |
| [Chess_ndt45.svg](https://commons.wikimedia.org/wiki/File:Chess_ndt45.svg) | `black-knight.png` | 1620 | `3c79cbbda76bcf1d4e4062cef6dd3b58a250a562` |
| [Chess_pdt45.svg](https://commons.wikimedia.org/wiki/File:Chess_pdt45.svg) | `black-pawn.png` | 766 | `0a6d2f3dc6327a02ca591bc489d701fbff228138` |

## Reproducible development-time rasterization

The shipped masters were generated with `rsvg-convert version 2.57.4`, rendering
the original square canvas directly at 512 by 512 with `--keep-aspect-ratio` and
no background-color option. Every PNG is RGBA with transparent outer borders,
opaque artwork and intermediate alpha values along antialiased edges. No
shadows, outlines, background rectangles or color changes were added.

To regenerate from the source checkout:

```sh
sh assets/pieces/cburnett/render.sh
(cd assets/pieces/cburnett && sha256sum -c SHA256SUMS)
```

The script verifies **all twelve** SHA-1 hashes before generating any image,
then writes the twelve PNGs and refreshes their SHA-256 manifest. Converter-version
changes can change PNG bytes; review such changes before updating a release.
The script is optional developer tooling, not a normal build or runtime step.
A normal `make` and ordinary redraws never invoke SVG conversion.

Gambitree uses its existing libpng loader, transparent alpha crop, premultiplied-
alpha Lanczos3 scaler and X11 compositor. No SVG runtime library is introduced.
The set descriptor uses the common 512px source canvas scaled to 80px at an
84px square, with 8px crop padding in the master. Measured threshold-4 bounds
produce type-indexed reference heights `{60,62,65,59,66,65}` and vertical offsets
`{3,1,0,3,1,1}` in pawn/knight/bishop/rook/queen/king order. Queen values average
the slightly different white/black source bounds. Each image retains its own
aspect ratio; the existing scaler, cache and deferred resize path are unchanged.

Install/uninstall includes the twelve runtime PNGs. The complete BSD copyright
notice, conditions and disclaimer are included in
`share/doc/gambitree/THIRD-PARTY-NOTICES`. Original SVGs, the asset LICENSE,
README, manifests and rendering recipe remain in the source distribution.

The developer audit is `doc/cburnett-integration.md` in the source checkout.
