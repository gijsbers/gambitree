# Gambitree

Gambitree is a chess-game viewer for GNU/Linux with X11. Open PGN files,
replay moves, read annotations and explore variations beside a chessboard.

## Highlights

- **Explore games and variations.** Click through moves and alternative lines
  while the board and notation stay in step.
- **Try Stockfish's ideas.** Preview suggested continuations directly on the
  board while keeping the original game intact.
- **Browse large PGN files smoothly.** Start exploring games while a collection
  is still loading, and keep using the viewer during engine analysis.
- **Find your way quickly.** Choose games from searchable lists and search
  through moves and comments.
- **Arrange your workspace.** Resize the window and give either the board or
  notation more room to suit the way you study.
- **Choose a comfortable look.** Pick classic or wooden boards, your preferred
  pieces and a readable text size. Gambitree remembers your preferences.
- **Study more than standard chess.** Explore Chess960, odds games and games
  with custom starting positions.

## Getting started

See [INSTALL.md](INSTALL.md) for dependencies, installation and testing.
Build from the source directory and install under `/usr`:

```sh
make release PREFIX=/usr && sudo make install PREFIX=/usr
```

Then open a game file. Replace `game.pgn` with the name or path of your PGN file:

```sh
gambitree game.pgn
```

Click a move or use the Left and Right arrow keys to explore the game.
Press `?` for the keyboard guide inside the window. Stockfish is optional;
Gambitree works without it.

## User manual

The [manpage](doc/gambitree.1) describes all keyboard and mouse controls,
command-line options, analysis, remembered settings and troubleshooting.
After installation, open it with:

```sh
man gambitree
```

From the source directory, use `man ./doc/gambitree.1`.

## Further reading

- [Technical overview](doc/architecture.md)
- [Artwork sources and preparation](assets/README.md)

## License

Gambitree is licensed under [GPL-3.0-or-later](LICENSE).
Artwork has its own terms; see [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES).
