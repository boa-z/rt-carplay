import os
import re
import subprocess


def build_git_sha(source):
    if not os.path.exists(os.path.join(source, '.git')):
        return 'unknown'
    try:
        sha = subprocess.check_output(['git', 'rev-parse', '--verify', 'HEAD'],
                                      cwd=source, stderr=subprocess.STDOUT)
        sha = sha.strip().decode('ascii')
    except (OSError, subprocess.CalledProcessError, UnicodeError):
        return 'unknown'
    return sha if re.match(r'\A(?:[0-9a-f]{40}|[0-9a-f]{64})\Z', sha) else 'unknown'


if __name__ == '__main__':
    import argparse
    from pathlib import Path

    parser = argparse.ArgumentParser()
    parser.add_argument('source')
    parser.add_argument('--header', type=Path)
    args = parser.parse_args()
    sha = build_git_sha(args.source)
    if args.header is None:
        print(sha)
    else:
        content = ('#define RTCP_BUILD_GIT_SHA %s\n' % sha).encode('ascii')
        if not args.header.exists() or args.header.read_bytes() != content:
            args.header.parent.mkdir(parents=True, exist_ok=True)
            staged = args.header.with_suffix('.tmp')
            staged.write_bytes(content)
            staged.replace(args.header)
