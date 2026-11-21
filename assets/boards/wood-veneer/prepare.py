#!/usr/bin/env python3
"""Offline preparation only; Pillow is not a build/runtime dependency.

Usage: python3 prepare.py LIGHT_ORIGINAL.png DARK_ORIGINAL.png
The generated originals are retained outside the repository. SOURCES.json
records their hashes, complete generation prompts and the two color grades.
"""
import hashlib
import json
from pathlib import Path
import sys

from PIL import Image


def main():
    directory = Path(__file__).resolve().parent
    sources = json.loads((directory / 'SOURCES.json').read_text())
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    for filename, source in zip(sys.argv[1:], sources):
        original = Path(filename)
        if hashlib.sha256(original.read_bytes()).hexdigest() != source['source_sha256']:
            raise SystemExit('Original hash mismatch: ' + str(original))
        with Image.open(original) as image:
            if list(image.size) != source['source_size']:
                raise SystemExit('Original dimensions mismatch: ' + str(original))
            resampling = getattr(Image, 'Resampling', Image)
            image = image.convert('RGB').resize(tuple(source['size']), resampling.LANCZOS)
            channels = [channel.point([max(0, min(255, value + offset))
                                      for value in range(256)])
                        for channel, offset in zip(image.split(), source['rgb_offsets'])]
            image = Image.merge('RGB', channels)
            image.save(directory.parent / source['output'], optimize=True)


if __name__ == '__main__':
    main()
