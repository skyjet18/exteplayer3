#!/usr/bin/env python3
"""Release text of a release made by ipk-release.yml.

Fills the placeholders of release-notes.md (next to this file) with the
ipks of the run (and their .buildinfo.txt files) and the changes since the
previous version of the player. Runs in a checkout with the history.

usage: release_notes.py --dist DIR --sha COMMIT --version N --tag TAG
                        --repo OWNER/REPO --ffmpeg-repo OWNER/REPO
                        [--expected "file.ipk ..."] [--template FILE] OUT
"""
import argparse
import os
import re
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
MAX_COMMITS = 100


def git(*args):
    return subprocess.run(['git'] + list(args), check=True, capture_output=True,
                          text=True, encoding='utf-8', errors='replace').stdout


# where the version is: include/version.h; before that file existed, the
# number after the format string of the EPLAYER3_EXTENDED message
VERSION_FILES = ('include/version.h', 'main/exteplayer.c')


def version_of(rev):
    """EXTEPLAYER3_VERSION of include/version.h, or for older commits the
    number in the EPLAYER3_EXTENDED message of main/exteplayer.c."""
    try:
        src = git('show', rev + ':include/version.h').replace('\r', '')
    except subprocess.CalledProcessError:
        src = None
    if src is not None:
        m = re.search(r'^\s*#\s*define\s+EXTEPLAYER3_VERSION\s+([0-9]+)', src, re.M)
        return m.group(1) if m else None
    try:
        src = git('show', rev + ':main/exteplayer.c').replace('\r', '')
    except subprocess.CalledProcessError:
        return None
    m = re.search(r'EPLAYER3_EXTENDED[^\n]*version.*?;', src, re.S)
    if not m:
        return None
    m = re.search(r'.*",\s*([0-9]+)\s*[,)]', m.group(0).replace('\n', ''))
    return m.group(1) if m else None


def previous_version(sha, version):
    """(number, commit) of the previous version: the commit on the first-parent
    history before which the version file last carried another number."""
    commits = git('rev-list', '--first-parent', sha, '--', *VERSION_FILES).split()
    i = 0
    while i < len(commits) and version_of(commits[i]) == version:
        i += 1
    if i >= len(commits):
        return None, None
    prev = version_of(commits[i])
    start = commits[i]
    while i < len(commits) and version_of(commits[i]) == prev:
        start = commits[i]
        i += 1
    return prev, start


def changes(sha, version, repo):
    prev, start = previous_version(sha, version)
    out = []
    if start:
        rng = start + '..' + sha
        out.append('## Changes since %s' % prev)
    else:
        rng = sha
        out.append('## Latest changes')
    out.append('')
    prs = []
    for rec in git('log', '--first-parent', '--merges', '--format=%s%x1f%b%x1e', rng).split('\x1e'):
        if '\x1f' not in rec:
            continue
        subject, body = rec.strip('\n').split('\x1f', 1)
        m = re.match(r'Merge pull request #([0-9]+)', subject)
        if m:
            title = body.strip().splitlines()[0] if body.strip() else subject
            prs.append('- %s (#%s)' % (title, m.group(1)))
    if prs:
        out += ['Merged pull requests:', ''] + prs + ['', 'Commits:', '']
    limit = MAX_COMMITS if start else 30
    commits = git('log', '--no-merges', '--format=- %s (%h)', '-n', str(limit), rng).splitlines()
    out += commits
    total = int(git('rev-list', '--no-merges', '--count', rng).strip())
    if total > len(commits):
        if start:
            link = 'https://github.com/%s/compare/%s...%s' % (repo, start[:12], sha[:12])
        else:
            link = 'https://github.com/%s/commits/%s' % (repo, sha[:12])
        out.append('- ... %d more: %s' % (total - len(commits), link))
    return '\n'.join(out)


def buildinfo(path):
    info = {}
    try:
        text = open(path, encoding='utf-8', errors='replace').read()
    except OSError:
        return info
    m = re.search(r'^ffmpeg: .*\(([^()]*)\)\s*$', text, re.M)
    if m:
        info['ffmpeg'] = m.group(1)
    m = re.search(r'highest GLIBC_ version required: ([0-9.]+)', text)
    if m:
        info['glibc'] = m.group(1)
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dist', required=True)
    ap.add_argument('--sha', required=True)
    ap.add_argument('--version', required=True)
    ap.add_argument('--tag', required=True)
    ap.add_argument('--repo', required=True)
    ap.add_argument('--ffmpeg-repo', required=True)
    ap.add_argument('--expected', default='')
    ap.add_argument('--template', default=os.path.join(HERE, 'release-notes.md'))
    ap.add_argument('out')
    a = ap.parse_args()

    ipks = sorted(f for f in os.listdir(a.dist) if f.endswith('.ipk'))
    ff = {'6': '6.1', '9': '9'}
    built = []
    rows = ['| File | Images | FFmpeg | needs glibc |', '|---|---|---|---|']
    for f in ipks:
        m = re.match(r'exteplayer3_.*_ffmpeg([0-9]+)_([a-z0-9]+)\.ipk$', f)
        major, label = (m.group(1), m.group(2)) if m else ('?', '?')
        images = {'armv7ahf': 'ARM', 'mips32el': 'MIPS',
                  'aarch64': '64-bit ARM (Dreambox One/Two), untested'}.get(label, label)
        info = buildinfo(os.path.join(a.dist, f[:-4] + '.buildinfo.txt'))
        if 'ffmpeg' in info and major in ff:
            ff[major] = info['ffmpeg']
        if major in ff and major not in built:
            built.append(major)
        url = 'https://github.com/%s/releases/download/%s/%s' % (a.repo, a.tag, f)
        rows.append('| [`%s`](%s) | %s | %s | %s |' % (f, url, images, info.get('ffmpeg', major),
                                                     info.get('glibc', '?')))

    # the Dreambox OS debs: the files of the ipk of that architecture
    debs = sorted(f for f in os.listdir(a.dist) if f.endswith('_dreamos.deb'))
    for f in debs:
        m = re.match(r'(exteplayer3_.*_ffmpeg([0-9]+))_(armhf|mipsel)_dreamos\.deb$', f)
        if not m:
            continue
        label = {'armhf': 'armv7ahf', 'mipsel': 'mips32el'}[m.group(3)]
        images = {'armhf': 'Dreambox OS: DM900, DM920', 'mipsel': 'Dreambox OS: DM520, DM525'}[m.group(3)]
        info = buildinfo(os.path.join(a.dist, '%s_%s.buildinfo.txt' % (m.group(1), label)))
        url = 'https://github.com/%s/releases/download/%s/%s' % (a.repo, a.tag, f)
        rows.append('| [`%s`](%s) | %s, untested | %s | %s |' % (f, url, images, info.get('ffmpeg', m.group(2)),
                                                               info.get('glibc', '?')))

    missing = [f for f in a.expected.split() if f not in ipks]
    note = ''
    if missing:
        variants = sorted(set(re.sub(r'^exteplayer3_[^_]*_(ffmpeg[0-9]+)_([a-z0-9]+)\.ipk$', r'\1 \2', f)
                              for f in missing))
        note = ('\n> **Note:** no package for %s: this version of the player does not build '
                'with that FFmpeg or for that architecture (or did not pass the tests of the '
                'build); support for it came with a later version.\n' %', '.join(v.replace('ffmpeg6', 'FFmpeg 6.1')
                                                      .replace('ffmpeg9', 'FFmpeg 9') for v in variants))

    text = open(a.template, encoding='utf-8').read()
    text = re.sub(r'\A\s*<!--.*?-->\s*', '', text, flags=re.S)
    values = {
        'VERSION': a.version,
        'REPOSITORY': a.repo,
        'TAG': a.tag,
        'FFMPEG_REPO': a.ffmpeg_repo,
        'FFMPEG6': ff['6'],
        'FFMPEG9': ff['9'],
        'FFMPEG_VERSIONS': ' or '.join(ff[m] for m in sorted(built)) or '?',
        'PACKAGES': '\n'.join(rows),
        'MISSING': note,
        'CHANGES': changes(a.sha, a.version, a.repo),
    }
    text = re.sub(r'\{\{([A-Z0-9_]+)\}\}', lambda m: values.get(m.group(1), m.group(0)), text)
    with open(a.out, 'w', encoding='utf-8') as f:
        f.write(text.rstrip() + '\n')


if __name__ == '__main__':
    main()
