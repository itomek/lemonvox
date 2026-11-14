#!/usr/bin/env python3
"""Apply a scale only to the launcher's disposable Mutter virtual monitor."""
import sys
import time
from gi.repository import Gio, GLib
scale = float(sys.argv[1])
assert scale in (1.0, 2.0)
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
def call(method, value=None):
    return bus.call_sync('org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig',
        'org.gnome.Mutter.DisplayConfig', method, value, None, Gio.DBusCallFlags.NONE, 5000, None).unpack()
state = call('GetCurrentState')
serial, monitors, logical, properties = state
assert len(monitors) == 1 and monitors[0][0][0] == 'Meta-0', 'Only disposable Meta virtual monitor is permitted'
monitor = monitors[0]
assert monitor[0][1:3] == ('MetaVendor', 'MetaVirtualMonitor') or list(monitor[0][1:3]) == ['MetaVendor', 'MetaVirtualMonitor']
mode = next(m for m in monitor[1] if m[6].get('is-current'))
assert scale in mode[5]
params = GLib.Variant('(uua(iiduba(ssa{sv}))a{sv})',
    (serial, 1, [(0, 0, scale, 0, True, [(monitor[0][0], mode[0], {})])], {}))
call('ApplyMonitorsConfig', params) # 1 = temporary; never persist display configuration.
for _ in range(50):
    current = call('GetCurrentState')
    if current[2] and current[2][0][2] == scale:
        print('PASS: disposable compositor actual logical monitor scale', scale, current)
        break
    time.sleep(.1)
else:
    raise AssertionError('Mutter did not apply requested monitor scale')
