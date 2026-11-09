#!/usr/bin/env python3
"""Validate the installable GNOME extension's public, machine-readable contract."""

import json
import pathlib
import unittest
import xml.etree.ElementTree as ET


ROOT = pathlib.Path(__file__).resolve().parents[1] / "extension"


class ExtensionContractTest(unittest.TestCase):
    def test_metadata_targets_the_tested_shell(self):
        metadata = json.loads((ROOT / "metadata.json").read_text(encoding="utf-8"))
        self.assertEqual(metadata["uuid"], "lemonvox@itomek.com")
        self.assertIn("46", metadata["shell-version"])
        self.assertEqual(metadata["settings-schema"], "org.gnome.shell.extensions.lemonvox")

    def test_schema_has_distinct_configurable_shortcuts(self):
        root = ET.parse(ROOT / "schemas/org.gnome.shell.extensions.lemonvox.gschema.xml").getroot()
        keys = {key.attrib["name"]: key for key in root.findall(".//key")}
        self.assertEqual(keys["toggle-shortcut"].attrib["type"], "as")
        self.assertEqual(keys["cancel-shortcut"].attrib["type"], "as")
        self.assertEqual(keys["toggle-shortcut"].findtext("default"), "['<Control><Alt>space']")
        self.assertEqual(keys["cancel-shortcut"].findtext("default"), "['<Control><Super>Escape']")
        self.assertEqual(keys["appearance"].attrib["type"], "s")
        self.assertEqual(keys["appearance"].findtext("default"), "'auto'")

    def test_backend_contract_contains_only_control_methods_and_safe_properties(self):
        root = ET.parse(ROOT / "dbus-interface.xml").getroot()
        interface = root.find("./interface[@name='com.itomek.LemonVox']")
        self.assertIsNotNone(interface)
        self.assertEqual(
            {method.attrib["name"] for method in interface.findall("method")},
            {"Toggle", "Stop", "Cancel", "ShowSettings", "ClearResult", "Quit"},
        )
        properties = {prop.attrib["name"]: prop for prop in interface.findall("property")}
        for name in ("State", "Message", "Result", "Palette"):
            self.assertEqual(properties[name].attrib["type"], "s")
            self.assertEqual(properties[name].attrib["access"], "read")
        self.assertEqual(properties["Error"].attrib["type"], "b")
        self.assertEqual(properties["Error"].attrib["access"], "read")
        # GDBus supplies org.freedesktop.DBus.Properties, including
        # PropertiesChanged, for an exported object. The app interface must not
        # shadow that standard interface in its own XML.
        self.assertIsNone(root.find("./interface[@name='org.freedesktop.DBus.Properties']"))


if __name__ == "__main__":
    unittest.main()
