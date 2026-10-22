"""Check the Git index or a complete commit ancestry without printing matched values."""
import argparse
from pathlib import PurePosixPath
import re
import subprocess
import sys


PATTERNS = {
    'personal path': rb'(?:/(?:Users|home|Volumes)/[^\s/"\'<>`]+|/private/var/(?:folders)/|[A-Za-z]:\\Users\\(?!user\\)[^\s\\"\'<>`]+)',
    'private key': rb'-----BEGIN (?:RSA |OPENSSH |EC |DSA |PGP )?PRIVATE KEY(?: BLOCK)?-----',
    'access token': rb'(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}|(?:AKIA|ASIA)[A-Z0-9]{16}|sk-(?:proj-|ant-)?[A-Za-z0-9_\-]{24,}|xox[baprs]-[A-Za-z0-9-]{20,})',
    'credential URL': rb'(?:https?|ssh|smb)://[^\s/@]+:[^\s/@]+@',
    'private network address': rb'(?<![\d.])(?:192\.168\.\d{1,3}\.\d{1,3}|10\.\d{1,3}\.\d{1,3}\.\d{1,3}|172\.(?:1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3})(?![\d.])',
    'literal credential': rb'(?im)^\s*["\']?(?:password|passwd|api_key|secret_key|access_token|auth_token|master_key_\w+|titlekek_\w+|key_area_key_\w+)["\']?\s*[:=]\s*["\']?[A-Za-z0-9+/=_\-]{16,}',
    'title key': rb'(?im)^\s*[0-9a-f]{32}\s*=\s*[0-9a-f]{32}\s*$',
}
EMAIL = re.compile(rb'[A-Za-z0-9._%+\[\]\-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}')
NOREPLY = re.compile(rb'(?:\d+\+)?[A-Za-z0-9_\[\]\-]+@users\.noreply\.github\.com')
PRIVATE_DIRS = {'local', 'work', 'generated', 'build', '.venv', '__pycache__', '.ssh', '.aws'}
PRIVATE_SUFFIXES = {
    '.keys', '.key', '.pem', '.p12', '.pfx', '.nsz', '.nsp', '.nca', '.ncz', '.xci',
    '.xcz', '.nso', '.sav', '.bin', '.o', '.a', '.so', '.dylib', '.dll', '.exe',
    '.zip', '.7z', '.tar', '.gz', '.log', '.sqlite', '.db', '.pyc',
}


def git(*args):
    return subprocess.check_output(['git', '--no-replace-objects', *args], stderr=subprocess.PIPE)


def content_findings(data):
    findings = []
    for kind, pattern in PATTERNS.items():
        for match in re.finditer(pattern, data):
            findings.append((data.count(b'\n', 0, match.start()) + 1, kind))
    for match in EMAIL.finditer(data):
        address = match.group()
        if not NOREPLY.fullmatch(address) and address.rsplit(b'@', 1)[1] not in (
                b'example.com', b'example.org', b'example.net'):
            findings.append((data.count(b'\n', 0, match.start()) + 1, 'non-public email'))
    return sorted(set(findings))


def private_path(path):
    item = PurePosixPath(path)
    return (bool(set(item.parts) & PRIVATE_DIRS) or item.suffix.lower() in PRIVATE_SUFFIXES
            or item.name in {'.DS_Store', '.netrc', '.npmrc', 'id_rsa', 'id_ed25519'}
            or item.name == '.env' or item.name.startswith('.env.'))


def identity_is_public(line):
    match = re.fullmatch(rb'([^<>]+) <([^<>]+)> \d+ [+-]\d{4}', line)
    return bool(match and NOREPLY.fullmatch(match[2])
                and match[1].strip().lower() == match[2].split(b'@')[0].split(b'+')[-1].lower())


def check(*, staged=False, ref='HEAD'):
    objects = {}
    entries = set()
    failures = []
    if staged:
        for row in git('ls-files', '--stage', '-z').split(b'\0'):
            if not row:
                continue
            meta, path = row.split(b'\t', 1)
            mode, oid, stage = meta.split()
            if stage != b'0':
                failures.append('index: unresolved merge')
            entries.add((mode, oid, path))
        for role in ('AUTHOR', 'COMMITTER'):
            if not identity_is_public(git('var', f'GIT_{role}_IDENT').strip()):
                failures.append(f'index: {role.lower()} must use a GitHub handle and noreply email')
    else:
        if git('rev-parse', '--is-shallow-repository').strip() != b'false':
            raise ValueError('complete ancestry is required')
        head = git('rev-parse', '--verify', '--end-of-options', ref + '^{commit}').strip().decode()
        for commit in git('rev-list', head).splitlines():
            sha = commit.decode()
            objects[commit] = ('commit ' + sha[:12], True)
            for row in git('ls-tree', '-r', '-z', sha).split(b'\0'):
                if row:
                    meta, path = row.split(b'\t', 1)
                    mode, _, oid = meta.split()
                    entries.add((mode, oid, path))

    for mode, oid, raw_path in sorted(entries):
        path = raw_path.decode('utf-8', errors='replace')
        if private_path(path):
            failures.append(f'{path!r}: local data or build artifact')
        if mode not in (b'100644', b'100755'):
            failures.append(f'{path!r}: symlink or submodule requires a separate publication review')
        else:
            objects.setdefault(oid, (repr(path), False))

    # Read one object at a time so history size cannot become an aggregate memory spike.
    with subprocess.Popen(['git', '--no-replace-objects', 'cat-file', '--batch'], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
        for oid, (label, commit) in objects.items():
            process.stdin.write(oid + b'\n')
            process.stdin.flush()
            header = process.stdout.readline().split()
            if len(header) != 3 or header[1] not in (b'blob', b'commit'):
                process.kill()
                raise ValueError('unreadable Git object')
            size = int(header[2])
            if size > 2 * 1024 * 1024:
                failures.append(f'{label}: object over 2 MiB requires a separate publication review')
                process.kill()
                break
            data = process.stdout.read(size)
            if len(data) != size or process.stdout.read(1) != b'\n':
                process.kill()
                raise ValueError('incomplete Git object')
            if b'\0' in data:
                failures.append(f'{label}: binary content requires a separate publication review')
            if commit:
                for line in data.split(b'\n\n', 1)[0].splitlines():
                    if line.startswith((b'author ', b'committer ')) and not identity_is_public(line.split(b' ', 1)[1]):
                        failures.append(f'{label}: non-public author or committer identity')
            failures.extend(f'{label}:{line}: {kind}' for line, kind in content_findings(data))
        process.stdin.close()
        returncode = process.wait()
        if returncode and not failures:
            raise ValueError('Git object reader failed')

    for failure in sorted(set(failures)):
        print(failure, file=sys.stderr)
    print(f'Publication check: {len(objects)} objects, {len(set(failures))} findings')
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument('--staged', action='store_true', help='check the full Git index and next commit identity')
    scope.add_argument('--ref', default='HEAD', help='check this commit and every reachable ancestor')
    args = parser.parse_args()
    try:
        return check(staged=args.staged, ref=args.ref)
    except (OSError, ValueError, subprocess.CalledProcessError):
        print('Publication check failed: could not inspect the requested Git state', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
