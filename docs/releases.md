# Signing and releases

Source: `Achxy/203-entrance`. Firmware assets: `Achxy/203-entrance-firmware`. The board needs no GitHub account or token. Its local hotspot remains usable without an uplink; downloading directly from GitHub requires an optional 2.4 GHz station connection with Internet access.

## Key custody

`./entrance setup` creates one RSA-3072 project key in `.local/keys/firmware.pem` if no key exists. Re-running setup preserves it. `./entrance keygen` prints the SHA-256 of the corresponding public PEM and refuses an incompatible key. Compare this fingerprint with the protected provisioning record before signing or restoring a key. Key files and device profiles use mode 0600 in ignored local storage. The private key is unencrypted on disk for the ESP-IDF build tool; protect the account and keep an encrypted offline backup.

The official verification key is published in `keys/release-public.pem`. Its PEM SHA-256 is `9f68e10ee55995cf51babeb6aa6567457275a0fa15c9f9971fcbac24d5bd54a6`. A fresh development checkout generates its own local signing key; restore the protected production key before building an official release. Firmware built with a different key will reject official release signatures.

Use the same production key for native ESP-IDF image signatures and RSA-PSS/SHA-256 manifest signatures (32-byte salt). Native image signatures use Espressif's standard format and tooling. Do not manually append a custom signature to an image. Public fingerprints are identifiers; they do not replace signature verification.

CI generates its own disposable key. CI artifacts are test firmware, never production updates. Production signing and publication happen locally, with no production signing secret in GitHub Actions.

## Publish a stable release

1. Complete the relevant hardware gate and update `docs/validation.md` with actual evidence.
2. Commit the final source and create the matching version tag, for example `git tag v0.1.0`. The worktree must be clean.
3. Run `./entrance build --version 0.1.0`. It records the commit, complete source fingerprint, SDK revision and signed binary digest in private build provenance.
4. Run `./entrance test` and browser checks. If source changed, commit/tag appropriately and rebuild.
5. Push the source commit and tag to `Achxy/203-entrance`.
6. Run `./entrance release build/203-entrance.bin --publish`.

The command rejects prerelease versions, dirty source, missing/mismatched tags, stale build provenance, incompatible images, excessive size and invalid native signatures. It writes and verifies `firmware.bin`, `manifest.json`, `manifest.sig`, `source.json` and `SHA256SUMS` under `dist/vVERSION/`, then publishes those files to the public firmware repository. It does not publish keys, profiles, crash dumps, flash backups, bootloaders or partition tables.

A release manifest is a bounded UTF-8 JSON object with exactly: `format`, `version`, `board`, `layout`, `schema`, `size`, `sha256`. The signature authenticates the exact bytes including the final newline. The image digest covers the complete signed file. A signature is checked before the device trusts any manifest field. Stable checks use GitHub's `latest` release, followed by versioned image URLs; races between release changes fail closed.

Checks run every six hours when online, with jitter; failed checks back off. The explicit browser/CLI check uses the same path. Checking never installs or reboots. Installation requires the administrator and succeeds only after health and updater reconnection confirm the expected image.

## Version policy

SemVer is limited to 31 ASCII characters to fit ESP-IDF's signed app descriptor. OTA requires a strictly newer version. Build metadata does not make an otherwise equal version newer. Prerelease versions support hardware qualification; only stable versions are published by the release command. Explicit administrator rollback is the supported path to the previous valid image. Arbitrary downgrade uploads are rejected.

For end-to-end qualification before stable promotion, publish the same tagged, stable-version image with `release ... --publish --prerelease`. GitHub marks it as a qualification candidate, so automatic stable checks will not offer it. An administrator can explicitly run `entrance check --version 0.1.0`, followed by `entrance install`, to test its versioned public assets without placing a token on the board. Promote that unchanged release only after its required hardware gates pass; attach the resulting validation evidence. This preserves the exact tested image digest.
