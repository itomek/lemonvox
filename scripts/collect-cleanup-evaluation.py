#!/usr/bin/env python3
"""Synthetic-only sequential evaluation on an already resident Lemonade model.

Does not install/load/unload models or change server configuration. The explicit
endpoint must end in /v1. Only corpus text is sent; response artifacts contain
synthetic text. Run evaluate-cleanup.py on the output for the fidelity gate.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time
import urllib.error
import urllib.request


def fetch(url, data=None):
    req = urllib.request.Request(url, data=data, headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=180) as response:
        return json.load(response)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--endpoint', required=True)
    parser.add_argument('--model', required=True)
    parser.add_argument('--request-tool', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    endpoint = args.endpoint.rstrip('/')
    if not endpoint.endswith('/v1'):
        parser.error('Endpoint must end in /v1')
    try:
        health = fetch(endpoint + '/health')
        # Fail closed: never cause automatic switching/loading or eviction.
        resident = health.get('model_loaded')
        if resident != args.model:
            parser.exit(2, 'Selected model must already be the resident model.\n')
        inventory = fetch(endpoint + '/models')
        if not any(row.get('id') == args.model and row.get('downloaded') is True for row in inventory.get('data', [])):
            parser.exit(2, 'Selected model must already be installed.\n')
        corpus = json.loads((Path(__file__).resolve().parents[1] / 'fixtures/cleanup/corpus.json').read_text())
        with args.output.open('x') as output:
            # First request is recorded separately, never asserted to be cold.
            for index, fixture in enumerate([corpus[0]] + corpus):
                request = subprocess.check_output([str(args.request_tool.resolve()), args.model], input=fixture['transcript'].encode())
                started = time.monotonic()
                try:
                    response = fetch(endpoint + '/chat/completions', request)
                except urllib.error.HTTPError as error:
                    response = {'http_status': error.code}
                sample = {'fixture': fixture['id'], 'phase': 'first_request' if index == 0 else 'warm',
                          'elapsed_ms': round((time.monotonic() - started) * 1000, 3), 'response': response}
                output.write(json.dumps(sample, ensure_ascii=False) + '\n')
                output.flush()
                print(f"{fixture['id']}: recorded", flush=True)
    except (OSError, ValueError, subprocess.CalledProcessError):
        parser.exit(2, 'Evaluation failed; details omitted to protect endpoint and response contents.\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
