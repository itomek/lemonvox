#!/usr/bin/env python3
"""Read the current public release's verified build number without executing it."""
import hashlib,io,json,plistlib,re,subprocess,sys,zipfile
metadata=json.load(open(sys.argv[1]))
assets=[a for a in metadata['assets'] if re.fullmatch(r'LemonVox-[0-9]+\.[0-9]+\.[0-9]+-arm64\.zip',a['name'])]
assert len(assets)==1, 'Cannot identify the previous release archive'
a=assets[0];url=a['browser_download_url'];digest=a.get('digest','')
assert url.startswith('https://github.com/itomek/lemonvox/releases/download/'), 'Unexpected release host'
assert re.fullmatch('sha256:[0-9a-f]{64}',digest), 'Missing trusted asset digest'
assert 0<a['size']<=209715200, 'Unexpected archive size'
data=subprocess.check_output(['curl','--fail','--location','--silent','--show-error','--max-time','120','--max-filesize','209715200',url])
assert 'sha256:'+hashlib.sha256(data).hexdigest()==digest, 'Previous archive digest mismatch'
with zipfile.ZipFile(io.BytesIO(data)) as z:
    entry=z.getinfo('LemonVox Prototype.app/Contents/Info.plist');assert entry.file_size<65536
    info=plistlib.loads(z.read(entry))
assert info['CFBundleIdentifier']=='com.itomek.lemonvox.prototype'
print(int(info['CFBundleVersion']))
