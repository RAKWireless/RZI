#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0
"""Build a chapter-based mdBook without copying old build artifacts."""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def main() -> int:
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument('--out-dir', type=Path, default=repo / 'doc/book')
    args = parser.parse_args()
    output = args.out_dir.resolve()
    binary = shutil.which('mdbook')
    if binary is None:
        local = Path.home() / '.local/bin/mdbook'
        if local.is_file():
            binary = str(local)
        else:
            parser.error('mdbook is required on PATH or in ~/.local/bin')
    with tempfile.TemporaryDirectory(prefix='rzi-mdbook-') as directory:
        stage = Path(directory)
        for name in ('README.md', 'SUMMARY.md', 'book.toml'):
            shutil.copy2(repo / 'doc' / name, stage / name)
        for name in ('architecture', 'api-design', 'api', 'development', 'compatibility'):
            for source in (repo / 'doc' / name).rglob('*.md'):
                target = stage / source.relative_to(repo / 'doc')
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
        subprocess.run([binary, 'build', str(stage), '--dest-dir', str(output)], check=True)
    # mdBook names README pages index.html but can retain README.html in
    # cross-directory links with fragments. Normalize those generated links.
    for page in output.rglob('*.html'):
        text = page.read_text(encoding='utf-8')
        text = re.sub(r'(href="(?:[^"#]*?/)?)(?:README)(\.html(?:#[^"]*)?")', r'\1index\2', text)
        page.write_text(text, encoding='utf-8')
    print(f'mdbook: HTML at {output / "index.html"}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
