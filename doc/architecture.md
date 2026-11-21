# Gambitree architecture

Gambitree is a C++98 Linux/X11 PGN viewer.  The dependency direction is kept
one-way:

```text
chess core <- PGN parser/index/library <- navigation <- X11 presentation
                                              ^
                                           UCI engine
```

## Core and PGN

`chess.{h,cpp}` owns legal chess state, FEN, move generation, make/unmake and
SAN. It stores an explicit Standard Chess or Chess960 ruleset and, for each
castling right, the king home square and participating rook home square.
Chess960 castling moves the king and rook to the normal c/g and d/f final
squares, including overlapping or stationary start/destination cases. This
core has no GUI, PGN, image or engine dependency.

`pgn.{h,cpp}` parses a single selected game strictly.  A `PgnGame` owns a root
`MoveNode`; a child at index zero is the main line and later children are RAV
alternatives.  Every node stores its resulting `Position`.  Comments before a
variation's first move are stored as that node's `leading_comments`; comments
after a move stay on that move.  The parser preserves tags, comments, NAGs,
results, SetUp/FEN and arbitrary nested RAVs.  Source locations allow a bad
game to be diagnosed and recovery to continue at a later safe tagged game.

PGN `Variant` is interpreted before `SetUp`/`FEN`: `Chess960`, `Fischerandom`,
and `Fischer Random` select Chess960, while standard games retain orthodox
rules. X-FEN `KQkq` and Shredder-FEN rook-file rights are accepted only for
Chess960; Chess960 FEN serialization uses Shredder file letters. Standard
SetUp/FEN permits non-standard material but validates any claimed orthodox
castling right against its required king and rook squares.

Import extensions also accept attached evaluation suffixes `+/-`, `-/+`, `+-`
and `-+` as NAGs 16 through 19. Comments and parenthesized hypothetical lines
immediately after a movetext result remain with that game. Those lines are
validated from its actual final position and do not change the played result
or final position. These tolerant import forms are not requirements for strict
PGN export.

`PgnLibrary` owns `PgnFile` objects and their non-owning UI-facing selection.
It materializes at most the selected `PgnGame` per file, replacing it safely
before a `MoveNavigator` is rebound.  A selected game range is guarded at
64 MiB; this is a materialization limit, not a database-size limit.

Control-Delete is limited to fully indexed, single-link regular PGN files up
to 32 MiB. `PgnFile` captures a SHA-256 fingerprint plus device/inode, size,
mode, and nanosecond mtime/ctime when loaded. The operation verifies that
baseline before showing confirmation and again before commit. It removes the
selected indexed byte range without serializing neighboring games, retaining
trailing boundary whitespace. A same-directory exclusive temporary file is
written, synced, and atomically renamed; deleting a file's only game unlinks
the source instead. After commit, the source is reindexed and the next game
(or previous game at end) is selected. A non-cooperating writer can still race
the final verification and rename/unlink, since POSIX does not offer an
identity-conditional rename or unlink. Mode bits are retained; filesystem
ACLs and extended attributes are not copied by this first version.

## Indexing and lazy loading

`PgnIndex` contains compact 16-byte entries: two unsigned 64-bit byte offsets
(begin and end).  It never owns movetext, tags or game trees.  Small, tagless,
or unusual input uses the general comment/RAV-aware scanner.  It recognizes
tag blocks and top-level results, and is bounded by a 1 MiB physical-line
buffer and a documented 16 million-entry safety limit.

Conventional databases of at least 128 MiB may use the provisional fast path.
After a tag-level probe, it scans fixed 1 MiB byte blocks for validated `[Event
"..."]` tag-block starts, including LF/CRLF and block boundaries.  This path
is deliberately provisional: materializing a selected range still uses the
strict parser, and unsuitable input falls back to the general scanner.  The
indexer runs in short single-threaded event-loop slices.  Found closed ranges
are immediately selectable while a `Game n / total+` count remains provisional.
Progress redraws are coalesced; they do not present independent partial frames.

## Navigation and presentation

`MoveNavigator` is non-owning and points only into the currently materialized
game.  It keeps selected-child choices at ancestors, so back/forward preserves
branch context.  Direct move selection records that context.  The X11
controller recreates the navigator before replacing a materialized game.

The Xlib/Xft layer is isolated in `x11_window.cpp`, `board_renderer.cpp`, and
`x11_notation_renderer.cpp`.  Core headers do not include X11 or libpng.
`BoardLayout` calculates the left board region, fixed gutter, draggable,
clamped sidebar width and square board.  `SidebarLayout`, `NotationLayout` and
`MetadataLayout` are pure geometry/text helpers.  They use the same Xft
metrics supplied by the renderer for wrapping, line boxes, highlights,
scrolling and hit testing.  Full frames are rendered to a resize-aware Pixmap
backbuffer, then presented in one `XCopyArea`.

Notation figurines use DejaVu Sans Bold, slightly enlarged and lightly
emboldened by Xft. Missing glyphs fall back to a regular figurine font or piece
letters. Point sizes are persisted as `notation_points` and `engine_points`.
PGNs marked `[Event "Survey"]` with opening metadata are presented as opening
surveys. Recognizable legacy ChessBase piece references in their labels follow
the selected letters/figurines style without changing the original PGN tags.

The board renderer receives a `Position`, never chess rules. It draws either
the classic flat board or a wood board from cached, generated PNG veneer
textures, with Xft coordinates, logical-square highlights and cached alpha PNG
pieces. A central `PieceSetDescriptor` registry keeps stable
IDs, user-facing names, asset directories, crop parameters and per-type scale
and optical-offset metadata together. Board palette and piece set are separate.
The Gambitree Classic descriptor preserves its established geometry. Meridian
Shaded has independent metadata derived from the source artwork. A set is loaded
into a temporary asset collection and becomes active only after all twelve PNGs
load successfully; successful switching invalidates the scaled cache. `P`
cycles registry entries without changing game or engine state. The selected
stable ID is persisted as `pieces=` in session state; explicit `--pieces`
selection takes precedence. `--pieces` also accepts `classic` for Gambitree
Classic and names of locally installed piece folders.

`AssetLocator` first checks the compiled installed data root, then on Linux a
development tree relative to `/proc/self/exe`, then the legacy relative
development fallback. Runtime assets follow
`assets/pieces/<asset-directory>/{white,black}-{king,queen,rook,bishop,knight,pawn}.png`.
The X11 shell locates the multi-size application PNGs using the installed root
and executable-relative development tree, then publishes their ARGB records
through `_NET_WM_ICON` before mapping the window.

## UCI engine and desktop integration

`uci_engine.cpp` is a POSIX-only nonblocking fork/execvp UCI client. It owns
its pipes and child process, drives the `uci`/`isready` handshake, and is read
through the same `select()` loop as X11.  Each visible position has a search
generation; stale output cannot update the current snapshot.  Incremental
`info` fields merge only when present.  Bound progress cannot replace an exact
principal variation.  Scores are normalized to White's perspective, and PVs
are converted to SAN where possible with raw UCI retained for an invalid tail.
It switches `UCI_Chess960` before analysis when the active game's ruleset
changes, and interprets Chess960 castling PV coordinates as king-to-rook moves.
The display retains a dim pending prior snapshot only during normal position
changes and coalesces engine-only presentation to roughly 5 Hz.

The top-level window sets dynamic `WM_NAME`, `WM_CLASS`, `_NET_WM_PID`,
`WM_COMMAND`, `WM_CLIENT_MACHINE`, and a lifetime-scoped `STOCKFISH_PID`.
`--print-window-id`, and diagnostics, print one mapped window ID for tooling.

`session_state.cpp` owns the small XDG session-state file independently of the
X11 controller. Version 2 stores the visible outer-frame origin, bounded
dimensions, and the analysis preference, and publishes updates through a
same-directory temporary file and rename. The X11 controller reads
`_NET_FRAME_EXTENTS` after requesting them before mapping, then converts the
client root position captured at shutdown into the outer-frame origin. If the
EWMH property is unavailable, the mapped window's parent tree is used to locate
the root-level frame for shutdown capture; startup leaves placement to the
window manager because its decoration offset is unknown. Version 1 positions
are migrated from their former client-origin meaning when frame extents are
available; otherwise that saved position is ignored once. Startup keeps a
useful outer-frame strip visible when extents and screen geometry are known
and leaves final placement to the window manager. Restored analysis is
requested once the initial game position is ready. Duplicate keys use the
last occurrence; an invalid value resets only its field to the startup
default.
