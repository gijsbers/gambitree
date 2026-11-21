#!/bin/sh
# Optional artwork preparation; never invoked by the normal build or redraws.
set -eu
export LC_ALL=C
asset_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Validate every source before generating or replacing any PNG.
(cd "$asset_root/source" && sha1sum -c ../SOURCES.sha1)
for side in l d; do
    case "$side" in l) color=white ;; d) color=black ;; esac
    for type in k q r b n p; do
        case "$type" in
            k) name=king ;; q) name=queen ;; r) name=rook ;;
            b) name=bishop ;; n) name=knight ;; p) name=pawn ;;
        esac
        rsvg-convert --keep-aspect-ratio --width 512 --height 512 \
            --output "$asset_root/$color-$name.png" \
            "$asset_root/source/Chess_${type}${side}t45.svg"
    done
done
(cd "$asset_root" && sha256sum source/*.svg black-*.png white-*.png > SHA256SUMS)
