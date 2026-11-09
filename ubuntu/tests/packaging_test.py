#!/usr/bin/env python3
"""Contract checks for the Ubuntu package inputs.

These tests deliberately inspect the package sources rather than installing a
package on the developer's desktop.  `scripts/build-ubuntu.sh` performs the
staged-build integration check when the native host is available.
"""

from __future__ import annotations

import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class UbuntuPackagingContractTest(unittest.TestCase):
    def test_build_script_validates_an_absolute_new_output_directory(self) -> None:
        script = ROOT / "scripts" / "build-ubuntu.sh"
        self.assertTrue(script.is_file(), "Ubuntu build script is missing")
        result = subprocess.run(
            ["bash", str(script), "--output", "relative", "--version", "0.0.0"],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("absolute", result.stderr.lower())

    def test_build_script_preserves_an_existing_output_directory(self) -> None:
        script = ROOT / "scripts" / "build-ubuntu.sh"
        with tempfile.TemporaryDirectory(prefix="lemonvox-existing-output-") as directory:
            marker = pathlib.Path(directory) / "keep"
            marker.write_text("preserve")
            result = subprocess.run(
                ["bash", str(script), "--output", directory, "--version", "0.0.0"],
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn("preserved", result.stderr.lower())
            self.assertEqual(marker.read_text(), "preserve")

    def test_deb_inputs_keep_desktop_dbus_and_extension_together(self) -> None:
        packaging = ROOT / "ubuntu" / "packaging"
        desktop = (packaging / "com.itomek.LemonVox.desktop").read_text()
        service = (packaging / "com.itomek.LemonVox.service").read_text()
        control = (packaging / "control.in").read_text()
        self.assertIn("DBusActivatable=true", desktop)
        self.assertIn("Exec=lemonvox --settings", desktop)
        self.assertIn("Name=com.itomek.LemonVox", service)
        self.assertIn("Exec=/usr/bin/lemonvox --service", service)
        self.assertIn("Package: lemonvox", control)
        self.assertIn("gstreamer1.0-pulseaudio", control)
        self.assertTrue((ROOT / "ubuntu" / "extension").is_dir())

    def test_strict_snap_recipe_is_explicitly_nonrelease_until_extension_delivery_exists(self) -> None:
        snap = (ROOT / "ubuntu" / "packaging" / "snap" / "snapcraft.yaml").read_text()
        limitations = (ROOT / "ubuntu" / "packaging" / "snap" / "LIMITATIONS.md").read_text()
        self.assertIn("confinement: strict", snap)
        self.assertIn("grade: devel", snap)
        self.assertIn("GNOME Shell extension", limitations)
        self.assertIn("not a release candidate", limitations)

    def test_extension_metadata_declares_the_installed_uuid(self) -> None:
        metadata = ROOT / "ubuntu" / "extension" / "metadata.json"
        parsed = json.loads(metadata.read_text())
        self.assertEqual(parsed["uuid"], "lemonvox@itomek.com")
        self.assertTrue(
            (ROOT / "ubuntu" / "extension" / "schemas" /
             "org.gnome.shell.extensions.lemonvox.gschema.xml").is_file()
        )

    def test_built_deb_stages_without_touching_the_running_system(self) -> None:
        artifact = os.environ.get("LEMONVOX_DEB")
        if artifact is None:
            self.skipTest("LEMONVOX_DEB is set only by a package build")
        package = pathlib.Path(artifact)
        self.assertTrue(package.is_file(), package)
        fields = {
            field: subprocess.run(
                ["dpkg-deb", "-f", str(package), field],
                text=True,
                capture_output=True,
                check=True,
            ).stdout.strip()
            for field in ("Package", "Version", "Architecture")
        }
        self.assertEqual(fields["Package"], "lemonvox")
        self.assertRegex(fields["Version"], r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
        self.assertEqual(fields["Architecture"], subprocess.check_output(["dpkg", "--print-architecture"], text=True).strip())
        with tempfile.TemporaryDirectory(prefix="lemonvox-package-stage-") as directory:
            stage = pathlib.Path(directory) / "root"
            subprocess.run(["dpkg-deb", "-x", str(package), str(stage)], check=True)
            expected = [
                "usr/bin/lemonvox",
                "usr/share/applications/com.itomek.LemonVox.desktop",
                "usr/share/dbus-1/services/com.itomek.LemonVox.service",
                "usr/share/gnome-shell/extensions/lemonvox@itomek.com/metadata.json",
                "usr/share/gnome-shell/extensions/lemonvox@itomek.com/schemas/gschemas.compiled",
                "usr/share/doc/lemonvox/build-info.txt",
                "usr/share/doc/lemonvox/yyjson-LICENSE",
            ]
            for relative in expected:
                self.assertTrue((stage / relative).is_file(), relative)
            # A second extraction represents a staged package replacement. It
            # proves payload paths can be overlaid without touching host state.
            subprocess.run(["dpkg-deb", "-x", str(package), str(stage)], check=True)
            self.assertTrue((stage / "usr/bin/lemonvox").is_file())
            shutil.rmtree(stage)
            self.assertFalse(stage.exists())


if __name__ == "__main__":
    unittest.main()
