#!/usr/bin/env python3
"""Exercise the release workflow's dispatch preflight without network or publishing."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

WORKFLOW = Path(__file__).resolve().parents[1] / '.github/workflows/ubuntu-release.yml'


def run_blocks():
    text = WORKFLOW.read_text()
    return [match.group(1) for match in re.finditer(r'^        run: \|\n((?:          .*\n|\n)+)', text, re.MULTILINE)]


@unittest.skipUnless(shutil.which('bash') and shutil.which('jq'), 'bash and jq required')
class ReleasePreflightTests(unittest.TestCase):
    def preflight(self, **changes):
        script = '\n'.join(line[10:] if line.startswith('          ') else line for line in run_blocks()[0].splitlines())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'git').write_text('''#!/usr/bin/env bash
if [[ "$1" == rev-parse ]]; then printf '%s\\n' "$SOURCE_COMMIT"; exit 0; fi
if [[ "$1" == merge-base ]]; then exit "${TEST_NOT_ON_MAIN:-0}"; fi
exit 99
''')
            (root / 'gh').write_text('''#!/usr/bin/env bash
if [[ "$*" == *matching-refs* ]]; then
  if [[ "${TEST_EXISTING_TAG:-0}" == 1 ]]; then printf '[{"ref":"refs/tags/ubuntu-v1.2.3"}]'; else printf '[]'; fi
elif [[ "$*" == *releases* ]]; then
  if [[ "${TEST_EXISTING_RELEASE:-0}" == 1 ]]; then printf 'ubuntu-v1.2.3\\n'; fi
else exit 99; fi
''')
            for command in ('git', 'gh'):
                (root / command).chmod(0o755)
            env = dict(os.environ, PATH=f'{directory}:{os.environ["PATH"]}', GH_REPO='owner/repo',
                       EVENT_NAME='workflow_dispatch', DISPATCH_REF='refs/heads/main',
                       SOURCE_COMMIT='a' * 40, RELEASE_VERSION='1.2.3', CHECK_COMMIT='b' * 40,
                       GITHUB_OUTPUT=str(root / 'output'))
            env.update(changes)
            result = subprocess.run(['bash', '-c', script], env=env, capture_output=True, text=True)
            output = (root / 'output').read_text() if (root / 'output').exists() else ''
            return result.returncode, output

    def test_valid_dispatch_pins_exact_source_and_version(self):
        status, output = self.preflight()
        self.assertEqual(status, 0)
        self.assertEqual(output, 'source_commit=' + 'a' * 40 + '\nversion=1.2.3\n')

    def test_pull_request_uses_nonrelease_version(self):
        status, output = self.preflight(EVENT_NAME='pull_request', DISPATCH_REF='refs/pull/1/merge',
                                        SOURCE_COMMIT='', RELEASE_VERSION='')
        self.assertEqual(status, 0)
        self.assertEqual(output, 'source_commit=' + 'b' * 40 + '\nversion=0.0.0\n')

    def test_invalid_dispatch_boundaries_fail_without_outputs(self):
        for changes in ({'DISPATCH_REF': 'refs/heads/topic'}, {'SOURCE_COMMIT': 'main'},
                        {'SOURCE_COMMIT': 'a' * 39}, {'RELEASE_VERSION': '1.2.3; echo invalid'},
                        {'RELEASE_VERSION': '01.2.3'}, {'RELEASE_VERSION': '0.0.0'},
                        {'TEST_NOT_ON_MAIN': '1'}, {'TEST_EXISTING_TAG': '1'},
                        {'TEST_EXISTING_RELEASE': '1'}, {'EVENT_NAME': 'push'}):
            with self.subTest(changes=changes):
                status, output = self.preflight(**changes)
                self.assertNotEqual(status, 0)
                self.assertEqual(output, '')

    def test_every_embedded_script_has_valid_bash_syntax(self):
        for block in run_blocks():
            script = '\n'.join(line[10:] if line.startswith('          ') else line for line in block.splitlines())
            result = subprocess.run(['bash', '-n'], input=script, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_only_manual_main_dispatch_can_publish_after_both_architectures(self):
        text = WORKFLOW.read_text()
        self.assertNotIn('\n  push:', text)
        self.assertIn("if: github.event_name == 'workflow_dispatch' && github.ref == 'refs/heads/main'", text)
        self.assertIn('needs: [prepare, package]', text)
        self.assertIn('ref: ${{ needs.prepare.outputs.source_commit }}', text)
        self.assertNotIn('--clobber', text)
        self.assertNotIn('0.2.2', text)


if __name__ == '__main__':
    unittest.main()
