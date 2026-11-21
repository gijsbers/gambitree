# Assets

## `pieces/classic` — Gambitree Classic

The twelve PNG files in `pieces/classic` are Gambitree's shipped `classic`
piece theme. They use this naming convention:

```text
{white,black}-{king,queen,rook,bishop,knight,pawn}.png
```

| Field | Value |
| --- | --- |
| Name | Gambitree Classic Pieces |
| Source record | The repository records a local OpenAI image-generation origin for this project and says no external chess-piece artwork was downloaded. |
| Author | No independently verifiable author record is included in this repository. |
| License record | The repository records a CC0 1.0 Universal claim for these files. |
| Modifications | Each named RGBA PNG is an individual, fully padded canvas for correct scaling and centring. |

`PieceAssetDirectory` maps a resolved directory and filenames, while
`AssetManager` transactionally loads the decoded images. The board renderer
selects from a central registry of piece-set descriptors. Board appearance is
independent of piece artwork.

## `pieces/meridian-shaded`

Meridian Shaded was created by Martin Sedlák and is included under the CC0
public-domain dedication in `meridian-shaded/source/LICENSE`. The upstream
project describes Meridian as based on a 1952 German newspaper design and the
shaded variant as adding linear gradients. Upstream states these sets may be
bundled with programs, including commercial programs.

Upstream project: <https://github.com/kmar/chess_svg_piece_sets>
Imported revision: `af40ea51e87eddee1ae7ee35ae312893a1271233`

The twelve original SVG masters and upstream license text are retained under
`pieces/meridian-shaded/source/`. Runtime PNG masters are rasterized directly
from those SVGs at 512x512 with librsvg. This resolution keeps large board
pieces sharp while reducing resize work; Gambitree applies its Lanczos3
runtime scaler. No SVG runtime dependency is used. This external artwork
dedication is separate from Gambitree's GPL-3.0-or-later software license.

## `pieces/cburnett`

Cburnett was designed by Colin M. L. Burnett (Wikimedia Commons User:Cburnett).
All twelve original Commons SVGs are retained unchanged in `cburnett/source/`
and verified against the pinned upstream SHA-1 values. Gambitree uses their
BSD 3-Clause license option, whose complete copyright notice, conditions and
disclaimer are in `cburnett/LICENSE`. This does not change the project license.

The twelve RGBA PNG masters are transparent 512x512 renders made in advance
with librsvg 2.57.4. The existing PNG loader, aspect-ratio-preserving Lanczos3
scaler, set/size cache and X11 compositor are reused. No SVG library is needed
at runtime. Cburnett is selectable with `P` or `--pieces cburnett`; Classic
remains the default and the board themes are independent.

[Per-file sources, verified versions and recipe](pieces/cburnett/README.md) and
[actual SVG/PNG SHA-256 hashes](pieces/cburnett/SHA256SUMS) accompany the assets
and remain in the source distribution. Installed `THIRD-PARTY-NOTICES` contains
the author credit and complete BSD copyright notice, conditions and disclaimer.

## Application icon

`icons/gambitree-{16,24,32,48,64,96,128,256}.png` are square RGBA application
icons cropped from the Gambitree Classic icon sheet supplied by the user. The
sheet contains separately rendered sizes; each crop uses its matching rendering
and excludes the printed size labels. Transparent padding is added only where
needed to make a crop square. No external provenance or licensing claim is made.

## Board appearance

`boards/wood-light.png` and `boards/wood-dark.png` are original AI-generated
wood veneer textures prepared for Gambitree's optional wood board. They are
sampled as per-square texture patches and color-adjusted by the renderer. The
user-provided `wooden1.jpg` and `wooden2.jpg` were used only as visual
references; their pixels were not copied into these assets. The classic flat
board remains code-defined and ships no texture asset.

`boards/wood-veneer-light.png` and `boards/wood-veneer-dark.png` are a separate
pair of 1024px original AI-generated textures for the optional `wood-veneer`
theme, distributed under the project's GPL-3.0-or-later terms where copyright
applies. They are not photographs or third-party CC0 scans. Prompts, source and
production hashes, licensing and preparation instructions are recorded in
[boards/wood-veneer/README.md](boards/wood-veneer/README.md). The existing Wood
textures, all pieces and the project license are unchanged.
