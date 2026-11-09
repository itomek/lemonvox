# Ubuntu package verification

Status: reviewed product code, initial install, upgrade and removal checks pass.
Final clean-source reproducibility, reinstall and CI remain pending.

## Initial development artifact

- File: `/tmp/lemonvox-package-final-b/lemonvox_0.0.0_amd64.deb`
- SHA-256: `2f53b93fe4982a8aab063c14837a99ad9d6b0ccb35adaa260bce53e853812091`
- Dirty-tree development build; not release evidence.
- Native build and CTest passed 9/9; staged artifact inspection passed 6/6.
- `sudo apt-get install` installed the previously absent `lemonvox` package.
- `dpkg-query` reported `install ok installed 0.0.0`.
- Installed `/usr/bin/lemonvox --version` returned version 0.0.0.
- Desktop entry validation passed; extension schema cache is installed.
- Installed binary passed `dbus-run-session -- python3
  ubuntu/tests/service_test.py /usr/bin/lemonvox` with exact virtual audio,
  fake local HTTP and private settings.
- Installed D-Bus activation service passed the same test with `--activate`;
  its private activation environment explicitly selected the virtual monitor
  and disposable settings. No desktop input device/default was changed.
- Binary size: 830,000 bytes; archive size: 282,444 bytes; extension tree: 68 KiB
  on this machine. System dependency footprint is separate.

## Reproducibility gate

The initial A/B hashes differed because code changed between builds (including
an added headless-version test and rebuilt main.cpp). That comparison is not a
reproducibility verdict. Final verification must build the same source twice
with the same SOURCE_DATE_EPOCH and compare the resulting .deb bytes.

The build script normalizes staged timestamps, records source/version/dirty
state, uses uniform compression and atomically creates a new output directory.
It stages yyjson's license alongside project attribution. It never changes the
running desktop or installs its artifact automatically.

## Distribution boundary

The .deb is an Ubuntu local-install artifact; it does not create an apt
repository or claim Debian support. Snap metadata remains strict/devel and is
not a Store release candidate. #73 tracks host extension delivery, confined
D-Bus/audio behavior, publisher ownership and Store review. No public package
was published and no credentials were changed.

## Reviewed candidate upgrade and removal

- Reviewed candidate: `/tmp/lemonvox-reviewed-upgrade-20261109/lemonvox_0.2.0_amd64.deb`.
- SHA-256: `3f1931743ebdd618b65006325dc4ad4eb6e04c13bc11576be3b288da5692a841`.
- Fresh Release build and CTest passed 9/9; artifact inspection passed 6/6.
- Apt upgraded 0.0.0 to 0.2.0; installed version printed `0.2.0 (build 715813f-dirty)`.
- Upgraded installed binary passed private D-Bus activation/service regression.
- Apt removed only the idle LemonVox package. Binary, desktop entry, activation
  service and extension metadata were absent afterward.
- A disposable XDG settings file remained byte-identical before upgrade, after
  upgrade and after removal: SHA-256
  `72b42e97f6775a7c354ff9f325b23127195ee9e42b8531a4b6e2c93deb7fdca9`.
  This test did not write or replace the user's normal settings.

The reviewed candidate is a dirty-tree test artifact. Final installed-package
source correspondence will use a clean reviewed commit and newly built artifact.
