#!/usr/bin/env python3
"""Offline APT release boundaries; no credentials, downloads or private keys."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / '.github/workflows/apt-release.yml'
BUILDER = ROOT / 'scripts/build-ubuntu-apt-repository.sh'


def blocks():
    return [m.group(1) for m in re.finditer(r'^        run: \|\n((?:          .*\n|\n)+)', WORKFLOW.read_text(), re.MULTILINE)]


def unindent(block):
    return '\n'.join(line[10:] if line.startswith('          ') else line for line in block.splitlines())


@unittest.skipUnless(shutil.which('bash') and shutil.which('jq'), 'bash and jq required')
class AptReleaseTests(unittest.TestCase):
    def gate(self, **changes):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'git').write_text('''#!/usr/bin/env bash
case "$1" in
rev-parse) printf '%s\\n' "${TEST_TAG_COMMIT:-$SOURCE_COMMIT}";;
merge-base) exit "${TEST_NOT_ON_MAIN:-0}";;
checkout) touch "$TEST_STATE/checked-out";;
status) printf '%s' "${TEST_DIRTY:-}";;
*) exit 99;;
esac
''')
            (path / 'gh').write_text('''#!/usr/bin/env bash
if [[ "${TEST_API_FAILURE:-0}" == 1 ]]; then exit 1; fi
printf '{"tag_name":"%s","draft":%s,"prerelease":%s}' "${TEST_API_TAG:-$RELEASE_TAG}" "${TEST_DRAFT:-false}" "${TEST_PRERELEASE:-false}"
''')
            for command in ('git', 'gh'):
                (path / command).chmod(0o755)
            env = dict(os.environ, PATH=f'{directory}:{os.environ["PATH"]}', GH_REPO='owner/repo',
                       RELEASE_TAG='ubuntu-v1.2.3', SOURCE_COMMIT='a' * 40, TEST_STATE=directory)
            env.update(changes)
            result = subprocess.run(['bash', '-c', unindent(blocks()[1])], env=env, capture_output=True)
            return result.returncode, (path / 'checked-out').exists()

    def test_only_exact_public_reviewed_release_reaches_checkout(self):
        self.assertEqual(self.gate(), (0, True))
        for changes in ({'RELEASE_TAG': 'v1.2.3'}, {'RELEASE_TAG': 'ubuntu-v01.2.3'},
                        {'RELEASE_TAG': 'ubuntu-v0.0.0'}, {'SOURCE_COMMIT': 'main'},
                        {'TEST_TAG_COMMIT': 'b' * 40}, {'TEST_NOT_ON_MAIN': '1'},
                        {'TEST_DRAFT': 'true'}, {'TEST_PRERELEASE': 'true'},
                        {'TEST_API_TAG': 'ubuntu-v9.9.9'}, {'TEST_API_FAILURE': '1'}):
            with self.subTest(changes=changes):
                code, checked_out = self.gate(**changes)
                self.assertNotEqual(code, 0)
                self.assertFalse(checked_out)
        self.assertNotEqual(self.gate(TEST_DIRTY='modified')[0], 0)

    def test_builder_refuses_invalid_arguments_and_existing_targets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            existing = root / 'existing'
            existing.mkdir()
            sentinel = existing / 'preserved'
            sentinel.write_text('preserved')
            link = root / 'link'
            link.symlink_to(existing, target_is_directory=True)
            for args in ([], [str(root / 'new'), 'bad-fingerprint', 'missing.deb'],
                         [str(existing), 'A' * 40, 'missing.deb'],
                         [str(link), 'A' * 40, 'missing.deb']):
                result = subprocess.run(['bash', str(BUILDER), *args], capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(sentinel.read_text(), 'preserved')
                self.assertFalse((root / 'new').exists())

    def test_scripts_parse_and_secret_output_is_separate(self):
        for block in blocks():
            result = subprocess.run(['bash', '-n'], input=unindent(block), text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        text = WORKFLOW.read_text()
        self.assertIn("if: github.event_name == 'workflow_dispatch' && github.ref == 'refs/heads/main'", text)
        self.assertIn('environment: release', text)
        self.assertIn('path: ${{ runner.temp }}/apt-public/', text)
        self.assertNotIn('contents: write', text)
        self.assertNotIn('gh-pages', text)
        self.assertIn('mkdir -m 700 "$GNUPGHOME"', text)
        self.assertIn('test "$fingerprints" = "$expected"', text)
        self.assertIn('bash scripts/build-ubuntu-apt-repository.sh', text)
        self.assertIn('apt-get download "lemonvox:$arch"', text)


if __name__ == '__main__':
    unittest.main()
