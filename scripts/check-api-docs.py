#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Require attached Doxygen comments on public functions and callback typedefs.

The public headers use file-scope, unindented declarations. This check
deliberately leaves field and macro contracts to review and Doxygen.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
TOKEN = re.compile(
    r'(?P<comment>/\*[\s\S]*?\*/|//[^\n]*)'
    r'|(?P<directive>^\s*\#(?:[^\n\\]|\\[\s\S])*)'
    r'|^(?:typedef\s+\w[\w\s*]*?\(\s*\*\s*(?P<callback>(?:rtcp|iap2_session)_\w+)\s*\)'
    r'|\w[\w \t*]*?\b(?P<function>(?:rtcp|iap2_session)_\w+)\s*)'
    r'\([^;{}]*\)\s*;'
    r'|(?P<other>\S)',
    re.MULTILINE,
)


def check(paths):
    total = 0
    missing = []
    for path in paths:
        source = path.read_text(encoding='utf-8-sig')
        documented = False
        for match in TOKEN.finditer(source):
            name = match['function'] or match['callback']
            if name:
                total += 1
                if not documented:
                    line = source.count('\n', 0, match.start()) + 1
                    missing.append(f'{path.relative_to(ROOT)}:{line}: add Doxygen for {name}')
            comment = match['comment']
            documented = bool(comment and comment.startswith(('/**', '/*!', '///', '//!'))
                              and not re.match(r'/\*[*!]\s*[@\\]file\b', comment))
    return total, missing


if __name__ == '__main__':
    headers = sorted((ROOT / 'include/rtcp').glob('*.h'))
    headers.append(ROOT / 'core/iap2/include/iap2_session.h')
    total, missing = check(headers)
    if not total:
        sys.exit('API documentation: no declarations found')
    if missing:
        print('\n'.join(missing))
    print(f'API documentation: {total - len(missing)}/{total} functions and callback typedefs')
    sys.exit(bool(missing))
