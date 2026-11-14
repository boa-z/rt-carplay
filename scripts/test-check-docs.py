#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Mutation tests for documentation checks, using isolated temporary Git trees.

Reused from boa-z/cherryusb-ipheth scripts/test-check-docs.py (Apache-2.0).
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest


class DocumentationChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True)
        (self.root / 'scripts').mkdir()
        shutil.copyfile(Path(__file__).with_name('check-docs.py'),
                        self.root / 'scripts/check-docs.py')
        self.write('README.md', '[English](README.md) | [简体中文](README.zh-CN.md)\n\n'
                   '# Driver\n\nRead [guide](docs/guide.md#setup).\n')
        self.write('README.zh-CN.md', '[English](README.md) | [简体中文](README.zh-CN.md)\n\n'
                   '# 驱动\n\n阅读[指南](docs/zh-CN/guide.md#配置)。\n')
        self.write('docs/guide.md', '[English](guide.md) | [简体中文](zh-CN/guide.md)\n\n'
                   '# Setup\n\nUse `api()` with [source](../src.c).\n\n'
                   '```sh\nmake check\n```\n')
        self.write('docs/zh-CN/guide.md', '[English](../guide.md) | [简体中文](guide.md)\n\n'
                   '# 配置\n\n使用 `api()` 和[源码](../../src.c)。\n\n'
                   '```sh\nmake check\n```\n')
        self.write('src.c', '/* fixture */\n')

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')

    def append(self, name, text):
        path = self.root / name
        self.write(name, path.read_text(encoding='utf-8') + text)

    def run_check(self):
        environment = dict(os.environ, PYTHONIOENCODING='utf-8')
        return subprocess.run([sys.executable, str(self.root / 'scripts/check-docs.py')],
                              cwd=self.root, capture_output=True, text=True,
                              encoding='utf-8', env=environment)

    def rejects(self, message):
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(message, result.stdout)

    def test_valid_pairs_code_and_translated_heading_links(self):
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('2 complete pairs', result.stdout)

    def test_missing_translation(self):
        (self.root / 'README.zh-CN.md').unlink()
        self.rejects('missing translation README.zh-CN.md')

    def test_missing_primary(self):
        (self.root / 'README.md').unlink()
        self.rejects('orphan translation')

    def test_navigation(self):
        self.write('README.zh-CN.md', '# 驱动\n')
        self.rejects('first nonempty line must be')

    def test_file_link(self):
        (self.root / 'src.c').unlink()
        self.rejects('nonexistent local link')

    def test_heading_fragment(self):
        self.append('README.md', '\n[invalid](docs/guide.md#missing)\n')
        self.rejects('missing heading fragment')

    def test_wrapped_paragraph(self):
        self.append('README.md', '\nFirst line\nwrapped continuation.\n')
        self.rejects('prose is hard-wrapped')

    def test_wrapped_list(self):
        self.append('README.md', '\n- First line\n  wrapped continuation.\n')
        self.rejects('prose is hard-wrapped')

    def test_trailing_break(self):
        self.append('README.md', '\nArtificial break  \n')
        self.rejects('trailing-space break')

    def test_html_break(self):
        self.append('README.md', '\nArtificial<br>break\n')
        self.rejects('HTML paragraph break')

    def test_document_naming(self):
        self.write('Bad_Name.md', '# Invalid\n')
        self.rejects('lowercase kebab-case')

    def test_code_drift(self):
        self.append('docs/zh-CN/guide.md', '\n```sh\nmake changed\n```\n')
        self.rejects('fenced code/commands differ')

    def test_unclosed_fence(self):
        self.append('README.md', '\n```sh\nmake check\n')
        self.rejects('unclosed fenced code block')

    def test_literal_drift(self):
        self.append('README.md', '\nKeep `SYMBOL` unchanged.\n')
        self.rejects('inline technical literals differ')

    def test_external_reference_drift(self):
        self.append('README.md', '\n[reference](https://example.invalid/record)\n')
        self.rejects('external reference links differ')

    def test_mixed_primary_language(self):
        self.append('README.md', '\n不应混合正文。\n')
        self.rejects('Chinese prose in English primary')

    def test_untranslated_copy(self):
        self.write('README.zh-CN.md', '[English](README.md) | [简体中文](README.zh-CN.md)\n\n'
                   '# Driver\n\nUntranslated English.\n')
        self.rejects('translation has no Chinese prose')


if __name__ == '__main__':
    unittest.main()
