"""Historical and renamed release metadata use the same upgrade build sequence."""
import hashlib, io, json, pathlib, plistlib, runpy, sys, tempfile, unittest.mock, zipfile
root = pathlib.Path(__file__).resolve().parents[1]
for names in [('LemonVox Prototype.app',), ('LemonVox.app',), ('LemonVox.app', 'LemonVox Prototype.app')]:
    data = io.BytesIO()
    with zipfile.ZipFile(data, 'w') as archive:
        for name in names:
            archive.writestr(name+'/Contents/Info.plist', plistlib.dumps(dict(CFBundleIdentifier='com.itomek.lemonvox.prototype', CFBundleVersion='4')))
    payload = data.getvalue()
    with tempfile.TemporaryDirectory() as directory:
        metadata = pathlib.Path(directory)/'release.json'
        metadata.write_text(json.dumps(dict(assets=[dict(name='LemonVox-0.1.0-arm64.zip', browser_download_url='https://github.com/itomek/lemonvox/releases/download/v0.1.0/LemonVox-0.1.0-arm64.zip', digest='sha256:'+hashlib.sha256(payload).hexdigest(), size=len(payload))])))
        with unittest.mock.patch.object(sys, 'argv', ['latest-release-build.py', str(metadata)]), unittest.mock.patch('subprocess.check_output', return_value=payload), unittest.mock.patch('sys.stdout', new_callable=io.StringIO) as output:
            try:
                runpy.run_path(str(root/'scripts/latest-release-build.py'), run_name='__main__')
            except AssertionError:
                if len(names)==1: raise
            else:
                assert len(names)==1, 'Duplicate bundle archive was accepted'
                assert output.getvalue().strip()=='4'
print('PASS: historical/new release names retain build sequence; duplicate recognized bundles rejected')

# GitHub's latest release can be Windows-only, and drafts/preview releases must
# not advance the production macOS update sequence. Pages need not be sorted.
data = io.BytesIO()
with zipfile.ZipFile(data, 'w') as archive:
    archive.writestr('LemonVox.app/Contents/Info.plist', plistlib.dumps(dict(CFBundleIdentifier='com.itomek.lemonvox.prototype', CFBundleVersion='7')))
payload = data.getvalue()
asset = dict(name='LemonVox-0.4.0-arm64.zip', browser_download_url='https://github.com/itomek/lemonvox/releases/download/v0.4.0/LemonVox-0.4.0-arm64.zip', digest='sha256:'+hashlib.sha256(payload).hexdigest(), size=len(payload))
mac = dict(draft=False, prerelease=False, published_at='2026-11-02T04:41:25Z', assets=[asset])
releases = [
    dict(draft=False, prerelease=False, published_at='2026-11-08T19:49:06Z', assets=[dict(name='LemonVox-0.5.1-windows-x64.zip')]),
    dict(mac, draft=True, published_at=None),
    dict(mac, prerelease=True, published_at='2026-11-10T00:00:00Z'),
    dict(draft=False, prerelease=False, published_at='2026-11-11T01:46:25Z', assets=[dict(name='lemonvox_0.2.1_arm64.deb')]),
    dict(mac, published_at='2026-11-01T00:00:00Z', assets=[dict(asset, browser_download_url='https://github.com/itomek/lemonvox/releases/download/v0.3.0/LemonVox-0.3.0-arm64.zip')]),
    mac,
]
for metadata_value in [releases, [releases[:3], releases[3:]]]:
    with tempfile.TemporaryDirectory() as directory:
        metadata = pathlib.Path(directory)/'releases.json'
        metadata.write_text(json.dumps(metadata_value))
        with unittest.mock.patch.object(sys, 'argv', ['latest-release-build.py', str(metadata)]), unittest.mock.patch('subprocess.check_output', return_value=payload) as download, unittest.mock.patch('sys.stdout', new_callable=io.StringIO) as output:
            runpy.run_path(str(root/'scripts/latest-release-build.py'), run_name='__main__')
            assert output.getvalue().strip() == '7'
            assert download.call_count == 1
            assert download.call_args.args[0][-1] == asset['browser_download_url']
print('PASS: mixed-platform paginated releases select the newest public stable macOS build')
