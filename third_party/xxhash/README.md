xxHash v0.8.3 (https://github.com/Cyan4973/xxHash, BSD 2-Clause, see LICENSE).

Only `xxhash.h` is vendored. The runtime uses XXH64 with seed 0 to compute Dolphin-compatible
custom texture names (runtime/src/gx/custom_textures.cpp).
