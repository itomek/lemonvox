#!/usr/bin/env python3
"""Opt-in real PipeWire/Lemonade test, with isolated virtual input and cleanup."""
import argparse
import pathlib
import select
import subprocess
import tempfile
import time
import uuid

p = argparse.ArgumentParser()
p.add_argument('--driver', required=True)
p.add_argument('--endpoint', default='http://127.0.0.1:13305/v1')
p.add_argument('--asr', required=True)
p.add_argument('--cleanup', required=True)
a = p.parse_args()
sink = 'lemonvox_test_' + uuid.uuid4().hex
module = None
process = None
try:
    with tempfile.TemporaryDirectory(prefix='lemonvox-audio-') as directory:
        wave = pathlib.Path(directory) / 'synthetic.wav'
        subprocess.run(['espeak-ng', '-s', '145', '-w', str(wave),
                        'Send Alex the fourteen invoices on Friday.'], check=True)
        module = subprocess.check_output(['pactl', 'load-module', 'module-null-sink',
                    f'sink_name={sink}', 'rate=48000', 'channels=1'], text=True).strip()
        sources = subprocess.check_output(['pactl', 'list', 'short', 'sources'], text=True)
        if sink + '.monitor' not in sources:
            raise RuntimeError('Exact virtual monitor is unavailable; refusing default input')
        process = subprocess.Popen([a.driver, sink + '.monitor', a.endpoint, a.asr, a.cleanup],
                                   stdout=subprocess.PIPE, text=True)
        deadline = time.monotonic() + 30
        ready = False
        while time.monotonic() < deadline and process.poll() is None:
            readable, _, _ = select.select([process.stdout], [], [], 0.2)
            if readable:
                line = process.stdout.readline().strip()
                if line == 'RECORDING':
                    ready = True
                    break
        if not ready:
            raise RuntimeError('Production capture did not become ready')
        # Give source negotiation a brief settling period; no physical audio is used.
        time.sleep(0.4)
        subprocess.run(['paplay', '--device=' + sink, str(wave)], check=True, timeout=12)
        output, _ = process.communicate(timeout=180)
        print(output.strip())
        if process.returncode:
            raise RuntimeError('Synthetic speech workflow failed')
finally:
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    if module:
        subprocess.run(['pactl', 'unload-module', module], check=True)
