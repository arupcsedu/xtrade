# Third-Party Dependencies

| Dependency | Version | Integrity | License | Scope |
| --- | --- | --- | --- | --- |
| FlatBuffers | 25.12.19 | SHA-256 `f81c3162b1046fe8b84b9a0dbdd383e24fdbcf88583b9cb6028f90d04d90696a` for the C++ source archive; Python artifacts are hash-locked | Apache-2.0 | Canonical C++/Python schema runtime and pinned code generator |
| GoogleTest | 1.17.0 | SHA-256 `65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c` | BSD-3-Clause | C++ tests only |
| Google Benchmark | 1.9.5 | SHA-256 `9631341c82bac4a288bef951f8b26b41f69021794184ece969f8473977eaa340` | Apache-2.0 | C++ benchmark harness only |
| Python development tools | Exact versions and artifact hashes in [`../../python/requirements-dev.lock`](../../python/requirements-dev.lock) | pip `--require-hashes` | Development-only; SBOM records package metadata | Format, lint, type, test, coverage, build, audit, secret scan |
| Time-series service runtime | FlatBuffers 25.12.19 with artifact hash in [`../../python/requirements-service.lock`](../../python/requirements-service.lock) | pip `--require-hashes` | Apache-2.0 | Service-only canonical contract runtime; no TimesFM, GPU runtime, or checkpoint is bundled |
| Go toolchain | 1.26.4 | Linux amd64 SHA-256 `1153d3d50e0ac764b447adfe05c2bcf08e889d42a02e0fe0259bd47f6733ad7f` | BSD-3-Clause | Control-plane build/test |

## Policy

Dependencies must be necessary, pinned, integrity checked where fetched outside
the language lock, represented in the generated SBOM, and reviewed for license
and vulnerability impact. The current Go package has no third-party module
dependency. C++ release archives are fetched only during configuration and are
not vendored into the repository. FlatBuffers-generated bindings are checked in
and contain no licensed exchange, news-provider, or market-data content.

GitHub Actions are build-infrastructure dependencies. Both workflows pin every
action to an immutable commit SHA and retain the corresponding release version
as an inline comment. Dependabot or an equivalent reviewed process should update
those pins; floating branches and tags are prohibited.

The project itself currently has no declared distribution license, so generated
SBOMs use `NOASSERTION` for Aegis-MX and for Python tool metadata that does not
provide a normalized SPDX identifier through installed metadata. This must be
resolved before external distribution.
