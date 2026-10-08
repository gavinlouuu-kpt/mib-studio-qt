# Linux processing-core detached signatures (A13)

Windows native processing cores are trusted through embedded Authenticode
plus a compiled approved signer SPKI SHA-256. Linux shared libraries have no
embedded-signature container, so the Linux trust adapter verifies an
**offline Ed25519 detached signature** behind the same injected
trust-verifier seam (`ProcessingCoreLoadRequirements::trustVerifier`). The
platform adapters differ; the C ABI, immutable registry, content-addressed
cache, and loader are shared platform contracts.

Tracking: [A13 #245](https://github.com/KPT1020/mib-studio-qt/issues/245).

## Envelope format

The signature is detached from the artifact and travels inside the immutable
registry metadata, not inside the `.so`:

```json
"signing": {
  "scheme": "ed25519",
  "required": true,
  "public_key_spki_base64": "<base64 of the 44-byte DER SubjectPublicKeyInfo>",
  "public_key_spki_sha256": "<64-hex SHA-256 of those DER bytes>",
  "signature_base64": "<base64 of the raw 64-byte Ed25519 signature>"
}
```

- The signature is computed over the **exact artifact bytes** (RFC 8032
  Ed25519, no pre-hash), so any post-sign mutation fails verification.
- The fields appear in the native sidecar descriptor (appended by the signing
  step after the artifact bytes are final), are validated and normalized by
  `scripts/release/publish-processing-core.py` (which re-derives `public_key_spki_sha256`
  from the actual key bytes and rejects a mismatch), and are copied into the
  immutable `versions/<version>.json` manifest and `index.json`.
- The envelope is verifiable offline with stock tooling:

```bash
openssl pkeyutl -verify -pubin -inkey signer-spki.der -keyform DER \
  -rawin -in mib_processing_core-<v>-linux_x86_64.so -sigfile artifact.sig
```

## Trust root and verification order

Manifest fields are transport, never a trust root. The desktop trusts a core
only when **all** of the following hold, in order
(`verifyProcessingCoreEd25519` in
`src/backend/processing/ProcessingCoreEd25519.cpp`, invoked by
`loadProcessingCorePlugin` after the SHA-256 gate):

1. A non-empty approved signer SPKI SHA-256 is **compiled into the
   application** (`MIB_PROCESSING_CORE_ED25519_SPKI_SHA256`, mirroring the
   Windows `MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256` pin). An empty pin fails
   closed — there is no unsigned production path.
2. The transported public key decodes to a canonical 44-byte DER
   SubjectPublicKeyInfo whose SHA-256 equals the compiled pin, and OpenSSL
   parses it as an Ed25519 key (any other key type is rejected even when its
   hash matches).
3. The transported signature decodes to exactly 64 bytes and verifies over
   the artifact bytes read from the content-addressed cache path being
   loaded.

Debug builds accept `MIB_STUDIO_PROCESSING_CORE_ED25519_SPKI_SHA256` as an
environment override (release builds ignore it), matching the Windows debug
override.

## Key identity, rotation, and revocation

- **Identity.** The production signer is a single Ed25519 keypair. Its
  public identity is the SHA-256 of its DER SPKI, stored as the GitHub
  repository variable that release CI passes to
  `MIB_PROCESSING_CORE_ED25519_SPKI_SHA256`, and compiled into every
  production Linux desktop build. The private key lives only in the
  Production-environment signing secret (parallel to the Authenticode
  certificate secret) and is never present in PR CI.
- **Rotation.** Keys rotate by publishing a new pin, not by mutating
  history: generate the successor keypair, sign new releases with it, update
  the repository variable, and ship a desktop release compiled with the new
  pin. Because every desktop release embeds exactly one pin, rotation is
  coordinated through the app-compatibility bounds already carried by each
  native entry (`app_min_version`): releases signed by the new key declare
  the first app version that pins it. Old app builds keep trusting old-key
  releases; immutable history is never re-signed.
- **Revocation.** A compromised key is revoked by (1) shipping a desktop
  release whose compiled pin no longer matches that key — every artifact it
  signed then fails closed on updated apps — and (2) moving the channel
  pointer off affected versions with `--promote-version` rollback so
  unpinned clients stop being offered them. Immutable version documents are
  never rewritten; revocation is a pin change plus a pointer change, and the
  incident is recorded in the release governance log.

## CI lane

`python-wheel.yml` job `build-native-plugin-linux` builds the `.so` and its
sidecar with hidden symbol visibility, runs the
`processing.core_(abi_c|loader|cache|fixture_matrix|ed25519)` CTests
(`processing.core_ed25519` generates an ephemeral keypair, runs the
accept/reject matrix — wrong pin, substituted key, non-Ed25519 key, corrupted
or missing signature, tampered artifact — and loads the signed artifact
through the content-addressed cache and `dlopen`), audits that the module
exports exactly `mib_processing_get_api` and imports no Qt/HDF5/Python/host
libraries, rehearses detached signing with an ephemeral key through the real
descriptor and publisher path, and uploads the unsigned artifact.

## Remaining live gates (deliberately open)

- ~~Provision the production Ed25519 keypair and repository SPKI variable~~
  Done 2026-07-14: the keypair was generated offline by the operator, the
  private key lives in the Production secret `LINUX_ED25519_SIGNING_KEY_PEM`,
  and `MIB_PROCESSING_CORE_ED25519_SPKI_SHA256` holds the pin
  (`94172d4c0651b1e1511b4e243e0acb8e005cd3aacedbac77a3a4609b76668946`).
- ~~Add the Production-gated signing job and the Linux asset pair to the
  release allowlists~~ Done 2026-07-14: `sign-native-plugin-linux` verifies
  the key against the repository pin before signing, injects the envelope
  into the sidecar, and the release job requires the signed `.so`/`.json`
  pair in the flat asset set.
- Publish a real signed `.so` through a `mib-processing-v*` tag and record
  the evidence on #245.
- Validate the dynamic OpenCV/spdlog link baseline (or static linking)
  across supported distros; the CI import audit currently reports these
  dependencies without pinning distro baselines.
