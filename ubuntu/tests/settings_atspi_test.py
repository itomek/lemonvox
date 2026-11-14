#!/usr/bin/env python3
"""Run only inside a disposable GNOME/AT-SPI session; no real audio/server."""
import configparser
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import gi
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi

binary = sys.argv[1]
requests = []
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self):
        requests.append(self.path)
        data = json.dumps({'data': [
            {'id': 'synthetic-asr', 'downloaded': True, 'recipe': 'whispercpp', 'labels': ['transcription']},
            {'id': 'synthetic-cleanup', 'downloaded': True, 'recipe': 'llamacpp', 'labels': ['chat']}]}).encode()
        self.send_response(200); self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)
    def do_POST(self):
        requests.append('UNEXPECTED POST ' + self.path)
        self.send_error(500)
server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
endpoint = f'http://127.0.0.1:{server.server_port}/v1'
path = Path(os.environ['XDG_CONFIG_HOME']) / 'lemonvox/settings.ini'
path.parent.mkdir(parents=True, exist_ok=True)
path.write_text('[Settings]\nendpoint=' + endpoint + '\nasr_model=synthetic-asr\ncleanup_model=synthetic-cleanup\ncleanup_prompt=Preserve 14 invoices.\ncleanup_backend=lemonade\ncleanup_thinking=disabled\n')

def walk(n):
    if n is None: return
    yield n
    for i in range(n.get_child_count()):
        try: yield from walk(n.get_child_at_index(i))
        except Exception: pass

def find(role, name, root=None):
    for n in walk(root or Atspi.get_desktop(0)):
        if n.get_role_name() == role and n.get_name() == name: return n
    raise AssertionError(f'Missing {role}: {name}')

def wait_for(fn, timeout=8):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        try: return fn()
        except Exception: time.sleep(.1)
    return fn()

def action(n):
    iface = n.get_action_iface()
    assert iface and iface.get_n_actions(), (n.get_role_name(), n.get_name())
    assert iface.do_action(0)

def tab(name):
    action(find('page tab', name)); time.sleep(.3)

def text(name): return find('text', name)
def value(n): return Atspi.Text.get_text(n, 0, -1)
def edit(name, contents):
    n = text(name)
    assert Atspi.EditableText.set_text_contents(n, contents)

def settings():
    c = configparser.ConfigParser(interpolation=None); c.read(path); return c['Settings']

def key(keyval):
    js = "import('gi://Clutter').then(({default: C}) => { const d = global.__lvUiKeyboard ??= C.get_default_backend().get_default_seat().create_virtual_device(C.InputDeviceType.KEYBOARD_DEVICE); d.notify_keyval(0," + str(keyval) + ",C.KeyState.PRESSED); d.notify_keyval(0," + str(keyval) + ",C.KeyState.RELEASED); return 'injected'; })"
    subprocess.run(['gdbus','call','--session','--dest','org.gnome.Shell','--object-path','/org/gnome/Shell','--method','org.gnome.Shell.Eval',js],check=True,stdout=subprocess.DEVNULL)
    time.sleep(.15)

def choose(name, option):
    subprocess.run(['gdbus','call','--session','--dest','org.gnome.Shell','--object-path','/org/gnome/Shell','--method','org.gnome.Shell.Eval', "import('resource:///org/gnome/shell/ui/main.js').then(M => { M.overview.hide(); global.get_window_actors().find(a => a.meta_window.get_title() === 'LemonVox Settings')?.meta_window.activate(global.get_current_time()); return 'focused'; })"], check=True, stdout=subprocess.DEVNULL)
    time.sleep(.3)
    combo = find('combo box', name)
    for _ in range(60):
        focused = [n for n in walk(combo) if n.get_state_set().contains(Atspi.StateType.FOCUSED)]
        if focused: break
        key(65289)
    else: raise AssertionError('Keyboard Tab could not reach ' + name)
    print('PASS: native keyboard Tab reached ' + name, flush=True)
    key(65293) # Return opens native combo popup.
    key(65360) # Home selects first item.
    index = {'Compatible server · server default':0, 'Lemonade · thinking off':1, 'Lemonade · server default':2, 'Auto':0, 'Light':1, 'Dark':2}[option]
    for _ in range(index): key(65364) # Down
    key(65293)
    time.sleep(.3)

def launch():
    p = subprocess.Popen([binary, '--settings'], env={**os.environ, 'LD_PRELOAD': os.environ.get('LEMONVOX_TEST_CONNECT_SHIM', ''), 'LEMONVOX_TEST_REDIRECT_PORT': str(server.server_port)}, stdout=subprocess.DEVNULL, stderr=open('/tmp/pr103-atspi-app.log', 'a'))
    wait_for(lambda: find('page tab', 'Models'))
    return p

p = launch()
try:
    tab('Models')
    assert value(text('API endpoint')) == endpoint
    assert value(text('Cleanup prompt')) == 'Preserve 14 invoices.'
    print('PASS: named editable endpoint and Cleanup prompt exposed through AT-SPI', flush=True)
    edit('API endpoint', 'invalid endpoint'); time.sleep(.6)
    error = wait_for(lambda: next(n for n in walk(Atspi.get_desktop(0)) if n.get_role_name() == 'label' and n.get_name().startswith('API endpoint: Use an HTTP/HTTPS')))
    print('PASS: inline endpoint error exposed through AT-SPI:', error.get_name(), flush=True)
    print('Endpoint AT-SPI relations:', [(r.get_relation_type().value_nick, r.get_n_targets()) for r in text('API endpoint').get_relation_set()], flush=True)
    relations = [r for r in text('API endpoint').get_relation_set() if r.get_relation_type() == Atspi.RelationType.DESCRIBED_BY]
    assert relations and relations[0].get_n_targets() == 1
    assert relations[0].get_target(0).get_name() == error.get_name()
    assert text('API endpoint').get_state_set().contains(Atspi.StateType.INVALID_ENTRY)
    assert settings()['endpoint'] == endpoint
    edit('Cleanup prompt', 'Keep 42 invoices.\nPreserve Alex.'); time.sleep(.6)
    assert settings()['cleanup_prompt'] == 'Keep 42 invoices.\\nPreserve Alex.'
    edit('API endpoint', endpoint); time.sleep(.6)
    assert not any(r.get_relation_type() == Atspi.RelationType.DESCRIBED_BY for r in text('API endpoint').get_relation_set())
    assert not text('API endpoint').get_state_set().contains(Atspi.StateType.INVALID_ENTRY)
    tab('Advanced')
    choose('API and thinking behavior', 'Lemonade · server default')
    assert settings()['cleanup_thinking'] == 'server_default'
    print('PASS: Advanced server-default selection autosaves via AT-SPI', flush=True)
    tab('General'); choose('Theme', 'Dark')
    assert settings()['appearance'] == 'dark'
    tab('Diagnostics')
    action(find('check box', 'Debug traces')); time.sleep(.3)
    assert settings().getboolean('debug')
    print('PASS: named Diagnostics Debug traces autosaves through AT-SPI', flush=True)
    print('PASS: General theme autosaves via AT-SPI', flush=True)
    action(find('push button', 'Close')); time.sleep(.3)
    subprocess.run([binary, '--settings'], check=True, timeout=5)
    time.sleep(.4); tab('Models')
    assert value(text('Cleanup prompt')) == 'Keep 42 invoices.\nPreserve Alex.'
    assert value(text('API endpoint')) == endpoint
    print('PASS: actual app close/reopen preserves endpoint and multiline prompt', flush=True)
    p.terminate(); p.wait(timeout=5); time.sleep(.4)
    p = launch(); tab('Models')
    assert value(text('Cleanup prompt')) == 'Keep 42 invoices.\nPreserve Alex.'
    assert value(text('API endpoint')) == endpoint
    tab('Advanced'); find('label', 'Lemonade · server default')
    assert settings()['asr_model'] == 'synthetic-asr' and settings()['cleanup_model'] == 'synthetic-cleanup'
    tab('General'); find('label', 'Dark'); assert settings()['appearance'] == 'dark'
    assert settings().getboolean('debug')
    print('PASS: full process relaunch preserves endpoint/models/custom prompt/theme/Advanced', flush=True)
    assert all(r in ('/v1/models', '/v1/models?show_all=true', '/v1/health') for r in requests), requests
    p.terminate(); p.wait(timeout=5); time.sleep(.3)
    # Exact missing-file defaults: redirect only this subprocess's factory
    # 127.0.0.1:13305 connect to our synthetic inventory fixture.
    assert os.environ.get('LEMONVOX_TEST_CONNECT_SHIM'), 'Fresh case requires isolated connect shim'
    path.unlink()
    p = launch(); tab('Advanced'); find('label', 'Lemonade · thinking off')
    assert not path.exists(), 'Initialization must not create preferences'
    choose('API and thinking behavior', 'Lemonade · server default')
    assert settings()['cleanup_backend'] == 'lemonade' and settings()['cleanup_thinking'] == 'server_default'
    p.terminate(); p.wait(timeout=5); time.sleep(.3)
    p = launch(); tab('Advanced'); find('label', 'Lemonade · server default')
    print('PASS: exact fresh missing-file default thinking off and explicit override survive process relaunch', flush=True)
    p.terminate(); p.wait(timeout=5); time.sleep(.3)
    path.write_text('[Settings]\nendpoint=' + endpoint + '\nasr_model=synthetic-asr\ncleanup_model=synthetic-cleanup\ncleanup_prompt=Legacy preserve 14 invoices.\n')
    before = path.read_text()
    p = launch(); tab('Advanced'); find('label', 'Compatible server · server default')
    tab('Models'); assert value(text('Cleanup prompt')) == 'Legacy preserve 14 invoices.'
    assert path.read_text() == before
    p.terminate(); p.wait(timeout=5); time.sleep(.3)
    p = launch(); tab('Advanced'); find('label', 'Compatible server · server default')
    tab('Models'); assert value(text('Cleanup prompt')) == 'Legacy preserve 14 invoices.'
    assert path.read_text() == before
    print('PASS: legacy marker-free file remains generic with exact model/prompt values across process relaunch', flush=True)
    assert all(r in ('/v1/models', '/v1/models?show_all=true', '/v1/health') for r in requests), requests
    print('PASS: only synthetic inventory/health GETs; no inference or model downloads:', requests, flush=True)
finally:
    p.terminate(); p.wait(timeout=5); server.shutdown()
