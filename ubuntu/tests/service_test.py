#!/usr/bin/env python3
"""Opt-in desktop service test. Uses only a named virtual input and fake HTTP."""
import configparser
import http.server
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from gi.repository import Gio, GLib

BINARY = pathlib.Path(sys.argv[1]).resolve()
ACTIVATE = len(sys.argv) > 2 and sys.argv[2] == '--activate'
BUS = 'com.itomek.LemonVox'
PATH = '/com/itomek/LemonVox'
INTERFACE = BUS
mode = 'normal'
seen = {'asr': 0, 'cleanup': 0, 'wav': False}

class Server(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def reply(self, status, body):
        encoded = json.dumps(body).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(encoded)))
        self.end_headers()
        try:
            self.wfile.write(encoded)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        self.reply(200, {'data': [
            {'id': 'asr', 'downloaded': True, 'recipe': 'whispercpp', 'labels': ['transcription']},
            {'id': 'cleanup', 'downloaded': True, 'recipe': 'llamacpp', 'labels': ['chat']}]})

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
        if self.path.endswith('/audio/transcriptions'):
            seen['asr'] += 1
            seen['wav'] = b'RIFF' in body and b'WAVE' in body and len(body) > 1000
            if mode == 'delay':
                time.sleep(0.6)
            self.reply(200, {'text': '' if mode == 'empty' else 'send Alex the 14 invoices on Friday'})
        else:
            seen['cleanup'] += 1
            self.reply(200, {'choices': [{'finish_reason': 'stop', 'message': {'content': 'Send Alex the 14 invoices on Friday.'}}]})

def wait_for(check, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if check():
                return
        except GLib.Error:
            pass
        time.sleep(0.02)
    raise AssertionError('Timed out waiting for service state')

bus = Gio.bus_get_sync(Gio.BusType.SESSION)
# The private test bus has no desktop portal. Supply a Settings-only fixture so
# GTK startup cannot accidentally activate the real desktop's portal/keyring.
portal_info = Gio.DBusNodeInfo.new_for_xml("""<node><interface name="org.freedesktop.portal.Settings">
<method name="ReadAll"><arg type="as" direction="in"/><arg type="a{sa{sv}}" direction="out"/></method>
<method name="Read"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="out"/></method>
<method name="ReadOne"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="out"/></method>
<property name="version" type="u" access="read"/></interface></node>""")
def portal_call(_connection, _sender, _path, _interface, method, _parameters, invocation):
    if method == 'ReadAll':
        invocation.return_value(GLib.Variant('(a{sa{sv}})', ({'org.freedesktop.appearance': {'color-scheme': GLib.Variant('u', 0)}},)))
    elif _parameters.unpack() in [('org.freedesktop.appearance', 'color-scheme'), ('org.freedesktop.appearance', 'contrast')]:
        value = GLib.Variant('u', 0)
        # The deprecated portal Read method historically double-wraps values;
        # ReadOne fixed that wire contract. Match the actual desktop behavior.
        if method == 'Read':
            value = GLib.Variant('v', value)
        invocation.return_value(GLib.Variant('(v)', (value,)))
    else:
        invocation.return_dbus_error('org.freedesktop.portal.Error.NotFound', 'No test setting')
portal_registration = bus.register_object('/org/freedesktop/portal/desktop', portal_info.interfaces[0],
    portal_call, lambda *_args: GLib.Variant('u', 2), None)
portal_owner = bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
    'RequestName', GLib.Variant('(su)', ('org.freedesktop.portal.Desktop', 4)), None,
    Gio.DBusCallFlags.NONE, 4000, None)
if portal_owner.unpack()[0] != 1:
    raise RuntimeError('Use dbus-run-session: refusing to replace an existing desktop portal')
portal_loop = GLib.MainLoop()
threading.Thread(target=portal_loop.run, daemon=True).start()
def call(method):
    return bus.call_sync(BUS, PATH, INTERFACE, method, None, None,
                         Gio.DBusCallFlags.NO_AUTO_START, 4000, None)

def prop(name):
    value = bus.call_sync(BUS, PATH, 'org.freedesktop.DBus.Properties', 'Get',
                         GLib.Variant('(ss)', (INTERFACE, name)), None,
                         Gio.DBusCallFlags.NO_AUTO_START, 4000, None)
    return value.unpack()[0]

sink = 'lemonvox_service_' + uuid.uuid4().hex
module = None
process = None
server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Server)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory(prefix='lemonvox-service-') as directory:
        config = pathlib.Path(directory) / 'config' / 'lemonvox'
        config.mkdir(parents=True)
        prefs = configparser.ConfigParser()
        prefs['Settings'] = {'endpoint': f'http://127.0.0.1:{server.server_port}/v1',
                             'asr_model': 'asr', 'cleanup_model': 'cleanup'}
        with (config / 'settings.ini').open('w') as f:
            prefs.write(f)
        module = subprocess.check_output(['pactl', 'load-module', 'module-null-sink',
                    'sink_name=' + sink, 'channels=1'], text=True).strip()
        env = {**os.environ, 'XDG_CONFIG_HOME': str(config.parent),
               'XDG_STATE_HOME': directory + '/state', 'GSETTINGS_BACKEND': 'memory',
               'GDK_DEBUG': 'no-portals', 'GTK_A11Y': 'none', 'GIO_USE_VFS': 'local', 'LEMONVOX_AUDIO_DEVICE': sink + '.monitor'}
        if ACTIVATE:
            activation_env = {key: env[key] for key in ['XDG_CONFIG_HOME', 'XDG_STATE_HOME',
                'GSETTINGS_BACKEND', 'GDK_DEBUG', 'GTK_A11Y', 'GIO_USE_VFS', 'LEMONVOX_AUDIO_DEVICE']}
            bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                'UpdateActivationEnvironment', GLib.Variant('(a{ss})', (activation_env,)), None,
                Gio.DBusCallFlags.NONE, 4000, None)
            bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                'StartServiceByName', GLib.Variant('(su)', (BUS, 0)), None,
                Gio.DBusCallFlags.NONE, 10000, None)
        else:
            process = subprocess.Popen([str(BINARY), '--service'], env=env)
        wait_for(lambda: prop('State') == 'idle')
        palette = prop('Palette').split()
        assert len(palette) == 4 and all(len(color) == 9 and color.startswith('#') for color in palette)
        light_palette = prop('Palette')
        bus.emit_signal(None, '/org/freedesktop/portal/desktop', 'org.freedesktop.portal.Settings',
            'SettingChanged', GLib.Variant('(ssv)', ('org.freedesktop.appearance', 'color-scheme', GLib.Variant('u', 1))))
        wait_for(lambda: prop('Palette') != light_palette)
        bus.emit_signal(None, '/org/freedesktop/portal/desktop', 'org.freedesktop.portal.Settings',
            'SettingChanged', GLib.Variant('(ssv)', ('org.freedesktop.appearance', 'color-scheme', GLib.Variant('u', 0))))
        wait_for(lambda: prop('Palette') == light_palette)
        call('Toggle')
        wait_for(lambda: prop('State') == 'recording')
        time.sleep(0.35)
        call('Stop')
        wait_for(lambda: bool(prop('Result')))
        assert prop('Result') == 'Send Alex the 14 invoices on Friday.'
        assert seen['wav'] and seen['asr'] == 1 and seen['cleanup'] == 1
        call('ClearResult')
        assert prop('Result') == ''
        call('Toggle')
        wait_for(lambda: prop('State') == 'recording')
        call('Cancel')
        assert prop('State') == 'idle' and prop('Result') == ''
        assert seen['asr'] == 1
        mode = 'delay'
        call('Toggle')
        wait_for(lambda: prop('State') == 'recording')
        time.sleep(0.3)
        call('Stop')
        wait_for(lambda: seen['asr'] == 2)
        call('Cancel')
        time.sleep(0.8)
        assert prop('State') == 'idle' and prop('Result') == '' and seen['cleanup'] == 1
        mode = 'empty'
        call('Toggle')
        wait_for(lambda: prop('State') == 'recording')
        time.sleep(0.3)
        call('Stop')
        wait_for(lambda: prop('State') == 'idle')
        assert prop('Result') == '' and 'No speech' in prop('Message') and prop('Error') is True
        # Errors must leave the service startable.
        mode = 'normal'
        call('Toggle')
        wait_for(lambda: prop('State') == 'recording')
        call('Cancel')
        assert prop('Error') is False
        call('Quit')
        if process:
            assert process.wait(timeout=5) == 0
        else:
            wait_for(lambda: not bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
                'org.freedesktop.DBus', 'NameHasOwner', GLib.Variant('(s)', (BUS,)), None,
                Gio.DBusCallFlags.NONE, 4000, None).unpack()[0])
        assert not list(pathlib.Path(directory).rglob('*.wav'))
        logs = list(pathlib.Path(directory).rglob('events.log*'))
        assert all('Alex' not in path.read_text() for path in logs)
        print('PASS: native service start/Stop/result, recording/processing cancel, empty response recovery, private transient state')
finally:
    if ACTIVATE:
        try:
            call('Quit')
        except GLib.Error:
            pass
    if process and process.poll() is None:
        process.terminate()
        process.wait(timeout=5)
    if module:
        subprocess.run(['pactl', 'unload-module', module], check=True)
    server.shutdown()
    portal_loop.quit()
    bus.unregister_object(portal_registration)
