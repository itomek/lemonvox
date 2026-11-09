#!/usr/bin/env python3
"""Read-only Ubuntu capability inventory; never requests input/capture consent."""
import json
import os
import shutil
import subprocess
import xml.etree.ElementTree as ET


def run(args):
    try:
        result = subprocess.run(args, capture_output=True, text=True, timeout=10)
        return result.stdout.strip() if result.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired):
        return None


def main():
    packages = ['gnome-shell', 'xdg-desktop-portal', 'xdg-desktop-portal-gnome',
                'pipewire', 'libgtk-4-1', 'libadwaita-1-0']
    versions = {p: run(['dpkg-query', '-W', '-f=${Version}', p]) for p in packages}
    xml = run(['gdbus', 'introspect', '--session', '--xml', '--dest',
               'org.freedesktop.portal.Desktop', '--object-path',
               '/org/freedesktop/portal/desktop'])
    interfaces = None
    if xml:
        try:
            interfaces = sorted(i.attrib['name'] for i in ET.fromstring(xml).findall('interface'))
        except (ET.ParseError, KeyError):
            pass
    capabilities = {}
    for name in ['GlobalShortcuts', 'RemoteDesktop', 'Clipboard', 'InputCapture']:
        capabilities[name] = ('unknown' if interfaces is None else
                              'advertised' if 'org.freedesktop.portal.' + name in interfaces
                              else 'absent')
    print(json.dumps({
        'session_type': os.environ.get('XDG_SESSION_TYPE', 'unknown'),
        'desktop': os.environ.get('XDG_CURRENT_DESKTOP', 'unknown'),
        'packages': versions,
        'portal_interfaces': capabilities,
        'tools': {t: bool(shutil.which(t)) for t in ['pw-play', 'pw-record', 'pw-cli', 'snapcraft']},
        'note': 'Advertised interfaces do not prove consent, shortcut delivery or safe insertion. Unknown means inspection failed, not unsupported.'
    }, indent=2))


if __name__ == '__main__':
    main()
