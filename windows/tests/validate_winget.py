"""Validate generated YAML against pinned official schemas on runners without WinGet.

Development dependencies: PyYAML 6.0.2, jsonschema 4.23.0. This does not replace
native `winget validate` or an installation lifecycle test before submission.
"""
import json
import pathlib
import sys
import urllib.request

import jsonschema
import yaml

SCHEMA_COMMIT = "afc9a471496c4cd80ac363df269362f84ea5a457"
SCHEMA_BASE = (
    f"https://raw.githubusercontent.com/microsoft/winget-cli/{SCHEMA_COMMIT}/"
    "schemas/JSON/manifests/v1.12.0/"
)


def validate(directory: pathlib.Path) -> None:
    manifests = list(directory.glob("*.yaml"))
    if len(manifests) != 3:
        raise ValueError("Expected three WinGet YAML manifests")
    identities = set()
    types = set()
    for path in manifests:
        document = yaml.safe_load(path.read_text(encoding="utf-8-sig"))
        kind = document["ManifestType"]
        if kind not in {"version", "installer", "defaultLocale"} or kind in types:
            raise ValueError("Expected version, installer and defaultLocale manifests")
        types.add(kind)
        url = f"{SCHEMA_BASE}manifest.{kind}.1.12.0.json"
        with urllib.request.urlopen(url, timeout=30) as response:
            schema = json.load(response)
        jsonschema.Draft7Validator(schema).validate(document)
        identities.add((document["PackageIdentifier"], document["PackageVersion"]))
    if len(identities) != 1:
        raise ValueError("Package identity/version differs between manifests")
    print("WinGet JSON schemas and cross-file identity validated (native installation remains untested)")


if __name__ == "__main__":
    validate(pathlib.Path(sys.argv[1]))
