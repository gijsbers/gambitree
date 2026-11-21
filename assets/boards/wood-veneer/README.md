# Wood Veneer assets

`wood-veneer-light.png` and `wood-veneer-dark.png` are original AI-generated
surfaces made for Gambitree with the built-in OpenAI `image_gen` tool on
2026-11-14, without reference images. They are not photographs or third-party
CC0 scans; no ChessBase, Poly Haven, ambientCG or Commons pixels were imported.

The images and preparation files use the project's **GPL-3.0-or-later** terms
where copyright applies. No exclusive copyright in purely generated material
is claimed. OpenAI's [European terms](https://openai.com/policies/eu-terms-of-use/)
and [services agreement](https://openai.com/policies/services-agreement/) assign
any OpenAI rights in output to the user. The installed GPL copy is
`share/doc/gambitree/LICENSE`, with a brief artwork credit in
`share/doc/gambitree/THIRD-PARTY-NOTICES`. This README and the full preparation
records remain in the source distribution.

## Provenance and verification

In the source repository, `SOURCES.json` preserves the complete generation
prompts, date, original filenames, original SHA-256 hashes and dimensions,
output dimensions and colour corrections. `SHA256SUMS` identifies the final
production PNGs. From the repository root:

```sh
(cd assets/boards && sha256sum -c wood-veneer/SHA256SUMS)
```

## Reproduction

The source-only developer tool `prepare.py LIGHT_ORIGINAL.png DARK_ORIGINAL.png`
requires the two original images identified by `SOURCES.json`; those originals
are not included in the repository. It checks their hashes, converts each
1254×1254 RGB source to 1024×1024 with Pillow Lanczos, then applies fixed RGB
offsets: light `(-4,+9,+22)`, dark `(+7,+12,+13)`. No grain, noise or lighting
is added. The output is opaque RGB PNG, decoded as RGBA at runtime.
Preparation used Pillow 8.4.0; Pillow is not a build/runtime dependency.

Only the two PNGs are runtime assets. Installation also supplies this notice,
`SHA256SUMS` and the GPL; `SOURCES.json` and `prepare.py` stay in the source tree.
See `doc/wood-veneer.md` there for sampling, cache and regression details.
