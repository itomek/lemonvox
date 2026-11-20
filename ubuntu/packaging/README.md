# Ubuntu packaging and publication

For installation, use the [Ubuntu user guide](../README.md). This page is for
maintainers. The existing project APT repository is separate from official
Ubuntu archive inclusion and Snap Store publication.

## Local package build

```sh
bash scripts/build-ubuntu.sh --output /absolute/new-directory --version X.Y.Z
```

The script builds the native host, runs CTest, and writes a reproducible `.deb`,
checksums and source metadata. It does not install or publish anything. The
package contains the app, desktop entry, session D-Bus activation file and
GNOME extension. The inference server and models are separate prerequisites.
Published release assets and package versions are immutable.

## Signed APT repository

The project repository is `https://itomek.github.io/lemonvox/apt/`.
[User setup commands](../README.md#add-the-repository-once) verify the archive
key fingerprint `D873B423E88EA3EFF8D769E47EE4BCB0334EC5FE`.

With `apt-utils` and `gnupg` installed, pass a new output directory, the full
fingerprint of an available signing key and the reviewed packages:

```sh
bash scripts/build-ubuntu-apt-repository.sh /tmp/lemonvox-apt \
    "$LEMONVOX_APT_SIGNING_FINGERPRINT" /absolute/artifacts/lemonvox_*.deb
```

The output contains the `stable` suite, `main` component, indexes for the
supplied architectures, signed metadata and the public key. The script creates
a local directory ready for static HTTPS hosting; it does not publish it.

The protected `Prepare signed Ubuntu APT repository` workflow accepts an
existing public Ubuntu release tag and exact reviewed source commit. It verifies
both architectures and provenance, then produces a signed public-tree artifact
for review before GitHub Pages publication.

```sh
bash ubuntu/tests/apt_repository_test.sh
```

This checks signatures, downloads and install/upgrade resolution using isolated
APT state and disposable fixture packages.

## Official Debian and Ubuntu archives

The `debian/` directory prepares a debhelper source package. See
[`debian/README.source`](../../debian/README.source) for build commands and
submission prerequisites. It is an **UNRELEASED candidate**, not an official
archive submission or acceptance.

## Snap Store

The adjacent [Snap recipe](snap/snapcraft.yaml) remains a strict-confinement,
`grade: devel` evaluation. It is not a Store release candidate: host extension
delivery and service integration are unresolved. See
[limitations](snap/LIMITATIONS.md).
