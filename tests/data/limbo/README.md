# x509-limbo test cases

`limbo_subset.json` is a part of `limbo.json` from x509-limbo
(https://github.com/C2SP/x509-limbo, commit b12423782a88abead01141fa6582254e3eb49e74,
downloaded 2026-11-20), kept by `tools/crypto/trim_limbo.py`: every case but bettertls's,
leaving out those that need certificate revocation lists or ML-DSA keys, and every 20th of
bettertls's name-constraint cases. The cases are otherwise unchanged, and only fields the tests
read were kept.

x509-limbo is licensed under the Apache License, Version 2.0, the same license as WinterWolf;
its text is in the `LICENSE` file at the root of this repository. It is used only by the tests
(`tests/certificates_limbo.ww`) and is not part of ww.exe.

`limbo_quick.json` is every 15th case of the subset and a few chosen ones (51 in all), which the
test reads in its quick mode (WW_TEST_QUICK=1, under GC stress).
