"""Publishes an IPA built by the "iOS distribution" workflow (in the public AltStore repository,
src/ filled by mirror_build.py; older tags: in this private repository)
to the public AltStore repository and rebuilds its source.json. Run on a PC with
git credentials for GitHub (the token comes from `git credential fill` and is
never printed). Python stdlib only; reuses distribute.py's IPA checks.

usage:
  python ios-distribution/scripts/publish_public.py v0.3.0 [--notes "what changed"] [--dry-run]
  python ios-distribution/scripts/publish_public.py --source-only     # rebuild source.json from the releases

Steps: find the successful "iOS distribution" run of the tag (same commit) ->
download its ios-package artifact -> check IPA / manifest / tag / build number
against the published history -> draft release with app.ipa, icon.png,
manifest.json in the public repository -> publish it -> rebuild source.json
(all published versions, newest first) and commit it -> make sure GitHub Pages
serves the repository. Published releases are never replaced.
"""
import argparse
import hashlib
import io
import json
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import distribute  # noqa: E402

PRIVATE = 'tera-works/tera-synth'
PUBLIC = 'tera-works/tera-synth-ios'
BUILD = PUBLIC  # the IPA is built here too (src/, mirror_build.py): free minutes on a public repository
WORKFLOW = 'ios.yml'
# commits in the public repository: never the account's e-mail address
IDENTITY = {'name': 'tera-works', 'email': '240604790+tera-works@users.noreply.github.com'}
API = 'https://api.github.com'

_token = None


def token():
    global _token
    if _token is None:
        out = subprocess.run(['git', 'credential', 'fill'], input='protocol=https\nhost=github.com\n\n',
                             capture_output=True, text=True, cwd=ROOT).stdout
        _token = next((l[9:] for l in out.splitlines() if l.startswith('password=')), None)
        distribute.require(bool(_token), 'no GitHub credential (git credential fill)')
    return _token


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None


def api(method, path, body=None, raw=None, content_type=None, base=API, ok=(200, 201, 204)):
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    headers = {'Authorization': 'Bearer ' + token(), 'Accept': 'application/vnd.github+json',
               'X-GitHub-Api-Version': '2022-11-28'}
    if content_type:
        headers['Content-Type'] = content_type
    req = urllib.request.Request(base + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req) as r:
            out = r.read()
            return r.status, (json.loads(out) if out else None)
    except urllib.error.HTTPError as e:
        out = e.read()
        if e.code in ok:
            return e.code, None
        try:
            return e.code, json.loads(out)
        except ValueError:
            return e.code, out.decode('utf-8', 'replace')


def must(result, what):
    st, out = result
    distribute.require(200 <= st < 300, f'{what}: HTTP {st} {out}')
    return out


def _get(url):
    """GET without credentials, retrying GitHub's temporary 5xx / network errors."""
    import time
    for attempt in range(6):
        try:
            req = urllib.request.Request(url, headers={'User-Agent': 'tera-works-publisher/1.0', 'Accept': '*/*'})
            with urllib.request.urlopen(req, timeout=60) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code < 500 or attempt == 5:
                raise RuntimeError(f'GET {url}: HTTP {e.code}') from e
        except urllib.error.URLError:
            if attempt == 5:
                raise
        time.sleep(2 + attempt * 3)


def download(url, auth):
    """GET a URL; API redirects to storage are followed without the token."""
    if not auth:
        return _get(url)
    req = urllib.request.Request(url, headers={'Authorization': 'Bearer ' + token(), 'Accept': 'application/vnd.github+json'})
    try:
        with urllib.request.build_opener(_NoRedirect).open(req) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        if e.code in (301, 302, 303, 307, 308):
            return _get(e.headers['Location'])
        raise


def git(*args):
    return subprocess.check_output(['git', *args], text=True, cwd=ROOT).strip()


# ---------------------------------------------------------------- build

def fetch_package(tag, dest):
    # the build runs in the public AltStore repository (mirror_build.py pushed the tag there);
    # older releases were built here in the private repository, on the tag's own commit
    sha = git('rev-parse', f'{tag}^{{commit}}')
    repo, runs = BUILD, []
    for repo in (BUILD, PRIVATE):
        st, body = api('GET', f'/repos/{repo}/actions/workflows/{WORKFLOW}/runs?per_page=50', ok=(200, 404))
        if st != 200:
            continue
        runs = [r for r in body['workflow_runs'] if r['conclusion'] == 'success' and
                (r['head_branch'] == tag if repo == BUILD else r['head_sha'] == sha)]
        if runs:
            break
    distribute.require(bool(runs), f'no successful {WORKFLOW} run for {tag}: run mirror_build.py {tag} and wait for the build')
    run = runs[0]
    arts = must(api('GET', f"/repos/{repo}/actions/runs/{run['id']}/artifacts"), 'artifacts')['artifacts']
    art = next((a for a in arts if a['name'] == 'ios-package' and not a['expired']), None)
    distribute.require(art is not None, f"run {run['id']} has no ios-package artifact (expired?)")
    print(f"build: run {run['id']} ({run['html_url']})")
    with zipfile.ZipFile(io.BytesIO(download(art['archive_download_url'], auth=True))) as z:
        z.extractall(dest)
    for n in ('app.ipa', 'icon.png', 'manifest.json'):
        distribute.require((dest / n).is_file(), f'artifact lacks {n}')


def check_package(tag, dest):
    m = distribute.read(dest / 'manifest.json')
    info = distribute.inspect_ipa(dest / 'app.ipa')
    v = m['versions'][0]
    distribute.require(info['CFBundleIdentifier'] == m['bundleIdentifier'] and info['CFBundleVersion'] == v['buildVersion']
                       and info['CFBundleShortVersionString'] == v['version'], 'IPA metadata mismatch')
    distribute.require(hashlib.sha256((dest / 'app.ipa').read_bytes()).hexdigest() == m['_sha256'], 'IPA hash mismatch')
    distribute.require(tag[1:] == v['version'], f"tag {tag} and IPA version {v['version']} differ")
    # nothing from the private test material may be in the app
    with zipfile.ZipFile(dest / 'app.ipa') as z:
        bad = [n for n in z.namelist() if n.lower().endswith(('.mid', '.midi', '.sf2')) or '/soniccell/' in n.lower()
               or '/gm/' in n.lower() or '/user/' in n.lower()]
    distribute.require(not bad, 'test material in the IPA: ' + ', '.join(bad[:5]))
    return m


# ---------------------------------------------------------------- public repository

def releases():
    out, page = [], 1
    while True:
        batch = must(api('GET', f'/repos/{PUBLIC}/releases?per_page=100&page={page}'), 'releases')
        out += batch
        if len(batch) < 100:
            return [r for r in out if not r['draft'] and not r['prerelease']]
        page += 1


def asset_bytes(api_url):
    """A release asset through the API (octet-stream; the storage redirect is followed without the token)."""
    req = urllib.request.Request(api_url, headers={'Authorization': 'Bearer ' + token(), 'Accept': 'application/octet-stream',
                                                   'User-Agent': 'tera-works-publisher/1.0'})
    try:
        with urllib.request.build_opener(_NoRedirect).open(req, timeout=60) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        if e.code in (301, 302, 303, 307, 308):
            return _get(e.headers['Location'])
        raise


def manifests():
    result = []
    for rel in releases():
        assets = {a['name']: a for a in rel['assets']}
        if 'manifest.json' not in assets:
            continue
        distribute.require('app.ipa' in assets and 'icon.png' in assets, 'incomplete release: ' + rel['tag_name'])
        try:
            raw = download(assets['manifest.json']['browser_download_url'], auth=False)
        except Exception:  # the download link failed (GitHub 5xx): the same file through the API
            raw = asset_bytes(assets['manifest.json']['url'])
        m = json.loads(raw)
        v = m['versions'][0]
        distribute.require(v['size'] == assets['app.ipa']['size'], 'release IPA size mismatch: ' + rel['tag_name'])
        v['downloadURL'] = assets['app.ipa']['browser_download_url']
        v['date'] = rel['published_at']
        v['localizedDescription'] = rel.get('body') or ''
        m['iconURL'] = assets['icon.png']['browser_download_url']
        result.append(m)
    return result


def put_file(path, text, message):
    st, cur = api('GET', f'/repos/{PUBLIC}/contents/{path}', ok=(200, 404))
    body = {'message': message, 'content': __import__('base64').b64encode(text.encode('utf-8')).decode(),
            'author': IDENTITY, 'committer': IDENTITY}
    if st == 200 and isinstance(cur, dict):
        if __import__('base64').b64decode(cur['content']).decode('utf-8') == text:
            return False
        body['sha'] = cur['sha']
    must(api('PUT', f'/repos/{PUBLIC}/contents/{path}', body), 'commit ' + path)
    return True


README = """# HSynth for iOS (AltStore)

HSynth（ハイブリッドシンセサイザー）の iOS アプリを AltStore Classic で配布しています。
ソースコードは非公開です。このリポジトリには、署名していない IPA（Releases）と AltStore の Source だけを置いています。

## AltStore に追加する

AltStore の Sources 画面で次の URL を追加します。

```
https://tera-works.github.io/tera-synth-ios/source.json
```

インストールのときに、AltStore があなたの Apple ID で署名します。
無料の Apple ID では署名の期限が 7 日間なので、期限内に AltStore で Refresh してください。

## 音色と MIDI ファイル

アプリには内蔵プリセットと小さなデモ用サンプルだけが入っています。
自分のサンプル（SFZ / WAV / FLAC）や MIDI ファイルは、「ファイル」アプリや Finder から
HSynth のフォルダ（library / presets / midi）へコピーして使います。
"""


def ensure_repo():
    """README (the first commit of an empty repository) and GitHub Pages from main."""
    put_file('README.md', README, 'README')
    st, _ = api('GET', f'/repos/{PUBLIC}/pages', ok=(200, 404))
    if st == 404:
        must(api('POST', f'/repos/{PUBLIC}/pages', {'source': {'branch': 'main', 'path': '/'}}), 'enable Pages')
        print('Pages enabled (main /)')


def write_source():
    c = distribute.read(ROOT / 'distribution.json')
    src = distribute.assemble(c['sourceName'], manifests())
    src['identifier'] = 'io.github.tera-works.tera-synth-ios'
    src['sourceURL'] = 'https://tera-works.github.io/tera-synth-ios/source.json'
    if src['apps']:
        src['iconURL'] = src['apps'][0]['iconURL']
    changed = put_file('source.json', json.dumps(src, ensure_ascii=False, indent=2) + '\n', 'Update source.json')
    put_file('index.html', '<!doctype html><meta charset="utf-8"><title>HSynth AltStore Source</title>'
             '<p><a href="source.json">AltStore Source</a></p>\n', 'index')
    versions = [f"{v['version']} ({v['buildVersion']})" for a in src['apps'] for v in a['versions']]
    print('source.json:', 'updated' if changed else 'unchanged', '-', ', '.join(versions) or 'no versions')


def publish(tag, notes, dry):
    with tempfile.TemporaryDirectory() as tmp:
        dest = Path(tmp)
        fetch_package(tag, dest)
        m = check_package(tag, dest)
        v = m['versions'][0]
        for old in manifests():
            if old['bundleIdentifier'] != m['bundleIdentifier']:
                continue
            prev = old['versions'][0]
            distribute.require((distribute.number(v['version']), distribute.number(v['buildVersion'])) >
                               (distribute.number(prev['version']), distribute.number(prev['buildVersion'])),
                               f"{v['version']} ({v['buildVersion']}) is not newer than {prev['version']} ({prev['buildVersion']})")
            distribute.require(distribute.number(v['buildVersion']) > distribute.number(prev['buildVersion']),
                               'CFBundleVersion must increase across releases (ios/project.yml)')
        print(f"package: {m['name']} {v['version']} ({v['buildVersion']}), {v['size'] / 1e6:.1f} MB, iOS {v['minOSVersion']}+")
        if dry:
            print('dry run: nothing published')
            return
        ensure_repo()
        st, _ = api('GET', f'/repos/{PUBLIC}/releases/tags/{tag}', ok=(200, 404))
        distribute.require(st == 404, f'{tag} already exists in {PUBLIC} (published releases are never replaced)')
        rel = must(api('POST', f'/repos/{PUBLIC}/releases', {'tag_name': tag, 'target_commitish': 'main',
                                                                'name': f"{m['name']} {v['version']}",
                                                                'body': notes or f"{m['name']} {v['version']}", 'draft': True}),
                   'create release')
        for name, ctype in (('app.ipa', 'application/octet-stream'), ('icon.png', 'image/png'), ('manifest.json', 'application/json')):
            must(api('POST', f"/repos/{PUBLIC}/releases/{rel['id']}/assets?name={name}", raw=(dest / name).read_bytes(),
                     content_type=ctype, base='https://uploads.github.com'), 'upload ' + name)
        must(api('PATCH', f"/repos/{PUBLIC}/releases/{rel['id']}", {'draft': False}), 'publish release')
        print('released:', rel['html_url'].replace('/untagged-', '/'))
    write_source()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('tag', nargs='?')
    p.add_argument('--notes', default='')
    p.add_argument('--dry-run', action='store_true')
    p.add_argument('--source-only', action='store_true')
    a = p.parse_args()
    if a.source_only:
        ensure_repo()
        write_source()
        return
    distribute.require(bool(a.tag) and bool(__import__('re').fullmatch(r'v\d+\.\d+\.\d+', a.tag)), 'give the tag: vMAJOR.MINOR.PATCH')
    publish(a.tag, a.notes, a.dry_run)


if __name__ == '__main__':
    try:
        main()
    except ValueError as e:
        sys.exit(f'error: {e}')
