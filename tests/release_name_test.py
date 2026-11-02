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
