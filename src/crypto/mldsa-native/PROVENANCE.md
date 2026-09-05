# mldsa-native

Unmodified portable C subset of
[mldsa-native v1.0.0-beta2](https://github.com/pq-code-package/mldsa-native/releases/tag/v1.0.0-beta2),
commit `9b0ee84f4cf399043eca59eca4e5f8531ca1d61b`.

Files retain their upstream paths after removing the leading `mldsa/`.
`LICENSE` is copied from the repository root. Native architecture backends,
assembly, test tools, and test RNGs are excluded. The integration configuration
is outside this vendor directory in `../mldsa44_config.h`.

Only ML-DSA-44 is compiled, using the serial portable FIPS 202 implementation.
The existing libsodium dependency supplies OS-backed randomness and zeroization;
the existing OrganicLifeCoin secure allocator owns the wrapper's secret keys.
No downloads occur when building.

This is experimental, unused by wallet/consensus code. Upstream's formal
verification has a specific scope and does not constitute an audit of this
integration. See the pinned
[SOUNDNESS.md](https://github.com/pq-code-package/mldsa-native/blob/v1.0.0-beta2/SOUNDNESS.md).
