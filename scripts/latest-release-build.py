#!/usr/bin/env python3
"""Read the newest public stable macOS release's verified build without executing it."""
import hashlib,io,json,plistlib,re,subprocess,sys,zipfile
metadata=json.load(open(sys.argv[1]))
def macos_assets(release):
    return [a for a in release['assets'] if re.fullmatch(r'LemonVox-[0-9]+\.[0-9]+\.[0-9]+-arm64\.zip', a['name'])]

if isinstance(metadata, list):
    # gh api --paginate --slurp produces one array per page. Also accept a flat
    # release list, and retain the historical single-release input contract.
    releases = [release for page in metadata for release in (page if isinstance(page, list) else [page])]
    candidates = [release for release in releases
                  if not release.get('draft', False) and not release.get('prerelease', False)
                  and release.get('published_at') and macos_assets(release)]
    assert candidates, 'Cannot identify a public stable macOS release'
    metadata = max(candidates, key=lambda release: release['published_at'])
assets = macos_assets(metadata)
assert len(assets)==1, 'Cannot identify the previous release archive'
a=assets[0];url=a['browser_download_url'];digest=a.get('digest','')
assert url.startswith('https://github.com/itomek/lemonvox/releases/download/'), 'Unexpected release host'
assert re.fullmatch('sha256:[0-9a-f]{64}',digest), 'Missing trusted asset digest'
assert 0<a['size']<=209715200, 'Unexpected archive size'
data=subprocess.check_output(['curl','--fail','--location','--silent','--show-error','--max-time','120','--max-filesize','209715200',url])
assert 'sha256:'+hashlib.sha256(data).hexdigest()==digest, 'Previous archive digest mismatch'
with zipfile.ZipFile(io.BytesIO(data)) as z:
    entries=[z.getinfo(name) for name in ('LemonVox.app/Contents/Info.plist','LemonVox Prototype.app/Contents/Info.plist') if name in z.namelist()]
    assert len(entries)==1, 'Expected exactly one recognized app bundle'
    entry=entries[0];assert entry.file_size<65536
    info=plistlib.loads(z.read(entry))
assert info['CFBundleIdentifier']=='com.itomek.lemonvox.prototype'
print(int(info['CFBundleVersion']))
