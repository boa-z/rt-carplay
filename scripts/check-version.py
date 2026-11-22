import argparse
from pathlib import Path
import re
import runpy
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tag')
    parser.add_argument('--binary', type=Path)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    header = (root / 'include' / 'rtcp' / 'version.h').read_text(encoding='ascii')
    parts = []
    for name in ('MAJOR', 'MINOR', 'PATCH'):
        values = re.findall(r'^#define RTCP_VERSION_' + name + r'[ \t]+([0-9]+)$', header, re.M)
        if len(values) != 1 or str(int(values[0])) != values[0]:
            parser.error('expected one canonical SemVer ' + name + ' number in version.h')
        parts.append(values[0])
    suffixes = re.findall(r'^#define RTCP_VERSION_SUFFIX[ \t]+"(-dev|)"$', header, re.M)
    if len(suffixes) != 1:
        parser.error('expected one RTCP_VERSION_SUFFIX of "-dev" or "" in version.h')

    version = '.'.join(parts) + suffixes[0]
    if args.tag is not None and (suffixes[0] or args.tag != 'v' + version):
        parser.error('tag %s does not match a formal SDK version (%s)' % (args.tag, version))

    if args.binary is not None:
        metadata = runpy.run_path(str(root / 'scripts' / 'build-git-sha.py'))
        sha = metadata['build_git_sha'](str(root))
        actual = subprocess.check_output([str(args.binary.resolve())], text=True).splitlines()
        if actual != [version, sha]:
            parser.error('expected %r, got %r from the SDK version API' % ([version, sha], actual))
    print('SDK version: ' + version)


if __name__ == '__main__':
    main()
