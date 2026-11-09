# Ubuntu package artifacts

Run `bash scripts/build-ubuntu.sh --output /absolute/new-directory --version X.Y.Z` to build the Ubuntu host, run CTest, and write a local `.deb`, checksum, and source metadata. It does not install the artifact or change the desktop.

Install a reviewed local artifact on the tested Ubuntu 24.04+ GNOME Wayland target:

```sh
sudo apt install ./lemonvox_X.Y.Z_amd64.deb
```

The package installs the GTK application, desktop entry, user D-Bus activation file, and `lemonvox@itomek.com` GNOME Shell extension. Enable the extension in GNOME Extensions before relying on global shortcut delivery. Lemonade remains a separate installed and running prerequisite.

The package does not support X11, Ubuntu versions before 24.04, or other Linux distributions. `sudo apt remove lemonvox` removes packaged files; user settings in the XDG configuration directory are intentionally preserved so an upgrade retains them.

The adjacent `snap/` recipe is a strict-confinement, `grade: devel` evaluation only. It cannot deliver the host GNOME Shell extension required by the tested workflow, and must not be submitted as a release candidate. See `snap/LIMITATIONS.md`.
