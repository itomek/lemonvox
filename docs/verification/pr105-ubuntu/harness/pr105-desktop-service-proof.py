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
        assert prop('HasLastTranscription') is False
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
        assert prop('HasLastTranscription') is True
        assert call('CopyLastTranscription').unpack() == ('Send Alex the 14 invoices on Friday.',)
        assert call('CopyLastTranscription').unpack() == ('Send Alex the 14 invoices on Friday.',)
        assert prop('HasLastTranscription') is True

        # Native PR105 proof: production backend, private GNOME, real clipboard.
        def se(code):
            return subprocess.check_output(['gdbus','call','--session','--dest','org.gnome.Shell','--object-path','/org/gnome/Shell','--method','org.gnome.Shell.Eval',code],text=True)
        def sw(code, marker):
            wait_for(lambda: marker in se(code))
        prefix="import('resource:///org/gnome/shell/ui/main.js').then(Main => { const e = Main.extensionManager.lookup('lemonvox@itomek.com').stateObj; "
        def extension(code):
            return prefix+code+' })'
        subprocess.run(['gdbus','call','--session','--dest','org.gnome.Shell','--object-path','/org/gnome/Shell','--method','org.gnome.Shell.Extensions.EnableExtension','lemonvox@itomek.com'],check=True)
        sw(extension("return e?._client.snapshot.hasLastTranscription && e._indicator._copyLast.sensitive ? 'last-ready' : 'pending';"),'last-ready')
        def clear_clipboard():
            se("import('gi://St').then(({default: St}) => { St.Clipboard.get_default().set_text(St.ClipboardType.CLIPBOARD,'sentinel'); return 'cleared'; })")
        def check_copy():
            clear_clipboard()
            se(extension("e._indicator._copyLast.emit('activate', null); return 'copy-clicked';"))
            sw("import('gi://St').then(({default: St}) => new Promise(resolve => St.Clipboard.get_default().get_text(St.ClipboardType.CLIPBOARD, (_c,text) => resolve(text === 'Send Alex the 14 invoices on Friday.' ? 'clipboard-match' : 'pending'))))",'clipboard-match')
        check_copy(); check_copy()
        print('PASS: production GNOME Copy Last Transcription menu repeatedly writes real Wayland clipboard',flush=True)
        se("import('resource:///org/gnome/shell/ui/main.js').then(Main => { Main.overview.hide(); return 'overview-hidden'; })")
        fixture_files=[directory+'/'+x for x in ['events','focus','text','activation']]
        fixture=subprocess.Popen(['gjs','-m','ubuntu/tests/extension_native_shell_fixture.mjs',*fixture_files],env=env)
        sw("JSON.stringify(global.get_window_actors().map(a => a.meta_window?.get_title()))",'LemonVox Native Fixture')
        se("import('resource:///org/gnome/shell/ui/main.js').then(Main => { Main.activateWindow(global.get_window_actors().map(a => a.meta_window).find(w => w?.get_title() === 'LemonVox Native Fixture'), global.get_current_time()); return 'activated'; })")
        wait_for(lambda: pathlib.Path(fixture_files[0]).exists() and pathlib.Path(fixture_files[0]).read_text().strip()=='ready')
        check_copy()
        se("import('gi://Clutter').then(({default: Clutter}) => { const d=global.__proofKeyboard ??= Clutter.get_default_backend().get_default_seat().create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE); let t=1; d.notify_keyval(t++,Clutter.KEY_Control_L,Clutter.KeyState.PRESSED); d.notify_keyval(t++,Clutter.KEY_v,Clutter.KeyState.PRESSED); d.notify_keyval(t++,Clutter.KEY_v,Clutter.KeyState.RELEASED); d.notify_keyval(t++,Clutter.KEY_Control_L,Clutter.KeyState.RELEASED); return 'paste-sent'; })")
        wait_for(lambda: pathlib.Path(fixture_files[2]).read_text()=='Send Alex the 14 invoices on Friday.')
        assert not pathlib.Path(fixture_files[3]).exists()
        print('PASS: copied durable transcription pasted into real GTK Wayland Entry without Enter',flush=True)
        fixture.terminate(); fixture.wait(timeout=5)
        call('ShowSettings')
        sw("JSON.stringify(global.get_window_actors().map(a => a.meta_window?.get_title()))",'LemonVox Settings')
        se("import('resource:///org/gnome/shell/ui/main.js').then(Main => { Main.overview.hide(); Main.activateWindow(global.get_window_actors().map(a => a.meta_window).find(w => w?.get_title() === 'LemonVox Settings'), global.get_current_time()); return 'settings-focused'; })")
        sw("global.display.focus_window?.get_title() === 'LemonVox Settings' ? 'settings-focused' : 'pending'",'settings-focused')
        time.sleep(1)
        evidence=pathlib.Path(os.environ['LEMONVOX_EVIDENCE_DIR'])
        def screenshot(name):
            target=str(evidence/name)
            result=subprocess.check_output(['gdbus','call','--session','--dest','org.gnome.Shell.Screenshot','--object-path','/org/gnome/Shell/Screenshot','--method','org.gnome.Shell.Screenshot.Screenshot','false','false',target],text=True)
            assert 'true' in result and pathlib.Path(target).stat().st_size>0
        screenshot('pr105-production-settings-copy-last.png')
        clear_clipboard()
        def key(keyname):
            se("Promise.all([import('gi://Clutter'),import('gi://GLib')]).then(([{default:Clutter},{default:GLib}]) => { const d=global.__proofKeyboard; let t=GLib.get_monotonic_time(); d.notify_keyval(t++,Clutter.KEY_"+keyname+",Clutter.KeyState.PRESSED); d.notify_keyval(t++,Clutter.KEY_"+keyname+",Clutter.KeyState.RELEASED); return 'key-sent'; })")
        for i in range(5):
            key('Tab'); time.sleep(.15)
        key('space')
        wait_for(lambda: prop('Message')=='Copied. Paste into your field.')
        sw("import('gi://St').then(({default: St}) => new Promise(resolve => St.Clipboard.get_default().get_text(St.ClipboardType.CLIPBOARD, (_c,text) => resolve(text === 'Send Alex the 14 invoices on Friday.' ? 'clipboard-match' : 'pending'))))",'clipboard-match')
        time.sleep(.5)
        screenshot('pr105-production-settings-copy-last.png')
        print('PASS: actual GTK Settings Copy Last Transcription button activated with Tab/Space; status and independent Wayland clipboard match',flush=True)
        se(extension("e._indicator._indicator.menu.open(); return 'opened';"))
        time.sleep(.4)
        screenshot('pr105-production-menu-copy-last.png')
        se(extension("e._indicator._indicator.menu.close(); return 'closed';"))
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
        assert prop('HasLastTranscription') is True
        check_copy()
        assert call('CopyLastTranscription').unpack() == ('Send Alex the 14 invoices on Friday.',)
        screenshot('pr105-production-retained-after-empty.png')
        print('PASS: failed empty transcription retains previous result and GNOME menu copies it to real clipboard',flush=True)
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
