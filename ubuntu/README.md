# LemonVox for Ubuntu

Requires Ubuntu 24.04 GNOME Wayland (amd64 or arm64).

## Install

[Add the APT repository once](packaging/README.md#signed-apt-repository), then run:

```sh
sudo apt update
sudo apt install lemonvox
```

APT installs the required libraries automatically. Alternatively, install a
[downloaded `.deb`](https://github.com/itomek/lemonvox/releases) with `sudo apt install ./lemonvox_*.deb`.

Log out and back in, then enable the desktop integration:

```sh
gnome-extensions enable lemonvox@itomek.com
```

Open LemonVox and [connect Lemonade and select models](../docs/model-setup.md).
Press **Ctrl+Alt+Space** to start and stop; **Ctrl+Super+Escape** cancels.

[Cleanup settings](../docs/cleanup.md) · [Build, test and packaging](DEVELOPMENT.md) · [Report an issue](https://github.com/itomek/lemonvox/issues)
