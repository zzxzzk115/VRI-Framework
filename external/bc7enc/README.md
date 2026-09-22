Sources from https://github.com/richgel999/bc7enc_rdo at
`b9438627eef73a1157e84201b6fa6eb2ffd6d9f0` (unmodified).

* `bc7e.ispc`: Binomial SIMD BC7 encoder, Apache-2.0 (`LICENSE-APACHE-2.0`).
* `bc7enc.cpp`, `bc7enc.h`: portable BC7 fallback, MIT (`LICENSE`).
* `bc7decomp.cpp`, `bc7decomp.h`: reference decoding for tests/benchmarks only, MIT (`LICENSE`).

VRF compiles SSE2 and AVX2 variants with ISPC 1.28.2 and uses ISPC's runtime
dispatch. The portable encoder supports targets without that toolchain.
The two backends have separate cache recipes because their encoded bytes differ.
