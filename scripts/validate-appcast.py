#!/usr/bin/env python3
"""Check feed/archive correspondence before publishing; signature verification is separate."""
import argparse,base64,hashlib,plistlib,xml.etree.ElementTree as ET,zipfile
from pathlib import Path

def validate(archive,feed,previous_build):
    namespace='{http://www.andymatuschak.org/xml-namespaces/sparkle}'
    with zipfile.ZipFile(archive) as z:
        info=plistlib.loads(z.read('LemonVox.app/Contents/Info.plist'))
    build=int(info['CFBundleVersion']);version=info['CFBundleShortVersionString']
    assert build>previous_build, 'Release build number must increase'
    assert info['CFBundleIdentifier']=='com.itomek.lemonvox.prototype', 'Wrong application identity'
    items=ET.parse(feed).getroot().findall('./channel/item')
    assert len(items)==1, 'Expected exactly one current full update'
    enclosure=items[0].find('enclosure');assert enclosure is not None
    value=lambda key:items[0].findtext(namespace+key) or enclosure.get(namespace+key)
    assert value('version')==str(build) and value('shortVersionString')==version, 'Version mismatch'
    assert items[0].findtext(namespace+'minimumSystemVersion')=='26.0', 'Wrong OS floor'
    expected='https://github.com/itomek/lemonvox/releases/download/v'+version+'/'+archive.name
    assert enclosure.get('url')==expected, 'Unexpected release URL'
    assert int(enclosure.get('length','-1'))==archive.stat().st_size, 'Archive length mismatch'
    signature=enclosure.get(namespace+'edSignature','')
    assert len(base64.b64decode(signature,validate=True))==64, 'Invalid signature encoding'
    return signature

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--archive',type=Path,required=True);p.add_argument('--feed',type=Path,required=True);p.add_argument('--previous-build',type=int,required=True)
    a=p.parse_args();validate(a.archive,a.feed,a.previous_build)
    print('PASS: feed matches archive version/build, identity, URL, length and platform')
