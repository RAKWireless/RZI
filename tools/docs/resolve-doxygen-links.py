#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0
"""Resolve Markdown chapter links in generated Doxygen HTML."""

from __future__ import annotations

import argparse
from html import unescape
from html.parser import HTMLParser
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit


class Headings(HTMLParser):
    def __init__(self):
        super().__init__()
        self.depth = 0
        self.text = []
        self.anchor = None
        self.ids = set()
        self.by_slug = {}
        self.counts = {}

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        ident = attrs.get('id') or (attrs.get('name') if tag == 'a' else None)
        if ident:
            self.ids.add(ident)
        if re.fullmatch(r'h[1-6]', tag):
            self.depth = 1
            self.text = []
            self.anchor = ident
        elif self.depth:
            self.depth += 1
            if ident:
                self.anchor = ident

    def handle_endtag(self, tag):
        if not self.depth:
            return
        if re.fullmatch(r'h[1-6]', tag):
            title = ''.join(self.text).strip()
            slug = re.sub(r'[^\w\- ]', '', title.lower()).replace(' ', '-')
            number = self.counts.get(slug, 0)
            self.counts[slug] = number + 1
            if number:
                slug += '-' + str(number)
            if self.anchor:
                self.by_slug[slug] = self.anchor
            self.depth = 0
        else:
            self.depth = max(1, self.depth - 1)

    def handle_data(self, data):
        if self.depth:
            self.text.append(data)


def page_name(source: Path, repo: Path) -> str:
    if source == repo / 'doc/README.md':
        return 'index.html'
    stem = source.relative_to(repo).with_suffix('').as_posix()
    return 'md_' + stem.replace('_', '__').replace('/', '_2').replace('.', '_8') + '.html'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument('html', type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    html = args.html.resolve()
    sources = [repo / 'CHANGELOG.md', repo / 'CONTRIBUTING.md', repo / '.github/SECURITY.md']
    sources += [
        p
        for p in (repo / 'doc').rglob('*.md')
        if not any(x in p.relative_to(repo / 'doc').parts for x in ('book', 'doxygen', 'reqmgmt'))
        and p.name != 'SUMMARY.md'
    ]
    pages = {p.resolve(): page_name(p, repo) for p in sources}
    parsed = {}
    for source, page in pages.items():
        target = html / page
        if target.exists():
            headings = Headings()
            headings.feed(target.read_text(encoding='utf-8'))
            parsed[source] = headings
    failures = []
    changed = 0
    for source, headings in parsed.items():
        p = html / pages[source]
        text = p.read_text(encoding='utf-8')

        def rewrite(match):
            nonlocal changed
            url = urlsplit(unescape(match.group(1)))
            if url.scheme or url.netloc:
                return match.group(0)
            if url.path.endswith('.md'):
                destination = (source.parent / unquote(url.path)).resolve()
            elif not url.path and url.fragment:
                destination = source
            else:
                return match.group(0)
            if destination not in parsed:
                failures.append(f'{p.name}: missing generated chapter {url.path}')
                return match.group(0)
            fragment = unquote(url.fragment)
            info = parsed[destination]
            if fragment and fragment not in info.ids:
                fragment = info.by_slug.get(fragment, fragment)
            if fragment and fragment not in info.ids:
                failures.append(f'{p.name}: missing anchor {url.path}#{fragment}')
                return match.group(0)
            target = pages[destination] if destination != source else ''
            if fragment:
                target += '#' + fragment
            if target != match.group(1):
                changed += 1
            return 'href="' + target + '"'

        text = re.sub(r'href="([^"]+)"', rewrite, text)
        p.write_text(text, encoding='utf-8')
    for failure in sorted(set(failures)):
        print(failure)
    print(f'doxygen links: {changed} rewritten, {len(set(failures))} unresolved')
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
