# libargon2 (PHC winner reference implementation)

Bundled for PScript Registry password hashing (Argon2id).

- **Upstream:** https://github.com/P-H-C/phc-winner-argon2
- **License:** [LICENSE](LICENSE) (dual-licensed CC0 / Apache 2.0 — see upstream `LICENSE` file)
- **Integration:** Static library `pscript_argon2` in the root `CMakeLists.txt`; `PSCRIPT_HAS_LIBARGON2` enables Argon2id in the runtime and registry auth code.

Do not modify the upstream sources except when required for build compatibility on supported platforms.
