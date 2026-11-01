# yyjson provenance

LemonVox vendors the unmodified upstream `yyjson.c`, `yyjson.h`, and `LICENSE`
from [yyjson 0.13.0](https://github.com/ibireme/yyjson/tree/6447536015f3d600f3d65323b10976103b337ca7),
exact commit `6447536015f3d600f3d65323b10976103b337ca7`.

The dependency is MIT licensed; its unchanged license is adjacent in
[`LICENSE`](LICENSE). It is compiled as C17. C++ consumers use C++17 and both
compilation units define `YYJSON_READER_DEPTH_LIMIT=513`. LemonVox uses the
default `YYJSON_READ_NOFLAG` reader configuration and does not enable relaxed
syntax or stop-when-done flags.

| File | SHA-256 | Bytes |
| --- | --- | ---: |
| `yyjson.c` | `d2d58ef0a3b2267862dc363832c4f3185fd375aea933d5d3419d1a940d6ece2e` | 424765 |
| `yyjson.h` | `ef803cda5c06b8962face6dfa39c3284b3bcf3e73f4d7317664690cc779679ca` | 330896 |
| `LICENSE` | `45e384d3d52c73cba3a64d6e6c25d47cd738cd8a55c30629e3201046eda62947` | 1084 |
