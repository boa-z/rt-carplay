#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check maintained Markdown pairs, local links and single-line prose.

Reused from boa-z/cherryusb-ipheth scripts/check-docs.py (Apache-2.0).

No network requests or third-party Python packages are required. Exemptions
are explicit path/reason entries in docs/documentation-exceptions.json.
Content checks are mechanical drift indicators, not semantic translation proof.
"""
import collections
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
LINK = re.compile(r'!?\[[^\]\n]*\]\((<[^>]+>|[^\s)]+)(?:\s+"[^"]*")?\)')
INLINE = re.compile(r'`([^`\n]+)`')
HEADING = re.compile(r'^(#{1,6})\s+(.+?)\s*#*\s*$')
LIST = re.compile(r'^\s*(?:[-+*]|\d+[.)])\s+')
CJK = re.compile(r'[\u3400-\u9fff]')


def parts(text):
    """Return prose lines, heading levels, fenced blocks and inline literals."""
    prose, headings, blocks, literals = [], [], [], set()
    fence, code = None, []
    for number, line in enumerate(text.splitlines(), 1):
        match = re.match(r'^\s{0,3}(`{3,}|~{3,})(.*)$', line)
        if fence is None and match:
            fence = match.group(1)
            code = [line]
        elif fence is not None:
            code.append(line)
            if re.match(r'^\s{0,3}' + re.escape(fence[0]) + r'{'
                        + str(len(fence)) + r',}\s*$', line):
                blocks.append('\n'.join(code))
                fence = None
        else:
            prose.append((number, line))
            heading = HEADING.match(line)
            if heading:
                headings.append(len(heading.group(1)))
            literals.update(INLINE.findall(line))
    if fence is not None:
        blocks.append('\n'.join(code))
    return prose, headings, blocks, literals, fence is not None


def slugs(text):
    found, counts = set(), collections.Counter()
    for _, line in parts(text)[0]:
        heading = HEADING.match(line)
        if not heading:
            continue
        title = re.sub(r'\[([^]]+)\]\([^)]*\)', r'\1', heading.group(2))
        title = re.sub(r'<[^>]*>', '', title).lower()
        slug = re.sub(r'[^\w\-\s]', '', title).replace(' ', '-')
        found.add(slug + ('-' + str(counts[slug]) if counts[slug] else ''))
        counts[slug] += 1
    return found


def counterpart(path):
    if path.name == 'README.md':
        return path.with_name('README.zh-CN.md')
    return path.parent / 'zh-CN' / path.name


def relative(source, target):
    return Path(os.path.relpath(target, source.parent)).as_posix()


def main():
    result = subprocess.run(['git', 'ls-files', '-z', '--cached', '--others',
                             '--exclude-standard', '--', '*.md'],
                            cwd=ROOT, check=True, capture_output=True)
    documents = {Path(p) for p in result.stdout.decode().split('\0') if p}
    manifest = ROOT / 'docs/documentation-exceptions.json'
    exceptions = json.loads(manifest.read_text(encoding='utf-8')) if manifest.exists() else {}
    errors = []
    for name, reason in exceptions.items():
        if not reason or Path(name) not in documents:
            errors.append(f'{name}: exemption must name an existing document and a reason')
    documents -= {Path(p) for p in exceptions}
    primary = sorted(p for p in documents if 'zh-CN' not in p.parts
                     and not p.name.endswith('.zh-CN.md'))
    expected = set(primary)
    texts = {}
    for path in documents:
        if not (ROOT / path).is_file():
            errors.append(f'{path}: tracked document is missing from working tree')
        else:
            texts[path] = (ROOT / path).read_text(encoding='utf-8-sig')
    for en in primary:
        if en.name != 'README.md' and not re.fullmatch(r'[a-z0-9]+(?:-[a-z0-9]+)*\.md', en.name):
            errors.append(f'{en}: use lowercase kebab-case for formal documents')
        zh = counterpart(en)
        expected.add(zh)
        if zh not in texts:
            errors.append(f'{en}: missing translation {zh}')
            continue
        if en not in texts:
            continue
        for path in (en, zh):
            nav = f'[English]({relative(path, en)}) | [简体中文]({relative(path, zh)})'
            first = next((line for line in texts[path].splitlines() if line.strip()), '')
            if first != nav:
                errors.append(f'{path}: first nonempty line must be {nav}')
        a, b = parts(texts[en]), parts(texts[zh])
        english_body = '\n'.join(line for _, line in a[0]
                                 if not line.startswith('[English]('))
        chinese_body = '\n'.join(line for _, line in b[0]
                                 if not line.startswith('[English]('))
        if CJK.search(english_body):
            errors.append(f'{en}: Chinese prose in English primary document')
        if not CJK.search(chinese_body):
            errors.append(f'{zh}: translation has no Chinese prose')
        if a[1] != b[1]:
            errors.append(f'{zh}: heading hierarchy differs from English')
        if a[2] != b[2]:
            errors.append(f'{zh}: fenced code/commands differ from English')
        if a[3] != b[3]:
            errors.append(f'{zh}: inline technical literals differ: '
                          f'{sorted(a[3] ^ b[3])}')
        external = lambda text: {m.group(1).strip('<>') for m in LINK.finditer(text)
                                 if urlsplit(m.group(1).strip('<>')).scheme in ('http', 'https')}
        if external(texts[en]) != external(texts[zh]):
            errors.append(f'{zh}: external reference links differ from English')
    for path in sorted(documents - expected):
        errors.append(f'{path}: orphan translation without an English primary')
    if Path('README.md') not in documents:
        errors.append('README.md: missing primary README')
    for path, text in sorted(texts.items()):
        lines, _, _, _, unclosed = parts(text)
        if unclosed:
            errors.append(f'{path}: unclosed fenced code block')
        previous = None
        for number, line in lines:
            stripped = line.strip()
            structural = not stripped or bool(HEADING.match(line)) or bool(LIST.match(line)) \
                or stripped.startswith(('|', '<!--', '-->', '[English](')) \
                or bool(re.fullmatch(r'[-*_]{3,}', stripped)) or line.startswith('    ')
            if line.endswith('  '):
                errors.append(f'{path}:{number}: artificial trailing-space break')
            if re.search(r'<br\s*/?>', line, re.I):
                errors.append(f'{path}:{number}: artificial HTML paragraph break')
            if previous is not None and previous[0] + 1 == number and not structural:
                errors.append(f'{path}:{number}: paragraph/list prose is hard-wrapped')
            # A list's following indented prose must also remain on its item line.
            previous = (number, line) if (not structural or LIST.match(line)) else None
        visible = '\n'.join(line for _, line in lines)
        for link in LINK.finditer(visible):
            target = link.group(1).strip('<>')
            parsed = urlsplit(target)
            if parsed.scheme or parsed.netloc:
                continue  # External sites never block this local CI check.
            destination = (ROOT / path.parent / unquote(parsed.path)).resolve() if parsed.path else ROOT / path
            if not destination.exists():
                errors.append(f'{path}: nonexistent local link {target}')
            elif parsed.fragment and destination.is_file() and destination.suffix == '.md':
                if unquote(parsed.fragment) not in slugs(destination.read_text(encoding='utf-8-sig')):
                    errors.append(f'{path}: missing heading fragment {target}')
    if errors:
        print('\n'.join(errors))
        print(f'documentation: FAIL ({len(errors)} findings)', file=sys.stderr)
        return 1
    print(f'documentation: PASS ({len(primary)} complete pairs; '
          f'{len(exceptions)} explicit exemptions; local links and layout checked)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
