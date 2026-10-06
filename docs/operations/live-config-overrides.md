# Governed live configuration: deployment and inspection

This implements the configuration subset of Trade #88 and AlgoLens #94. It does not close the broader IAM work or establish production readiness. Readers and the small Published configuration summary describe a completed source day, not currently active settings or an in-flight attempt. There is no configuration editor in this panel.

## Prerequisites and ordering

Generate new candidate evidence from exact reviewed source. Apply Trade `migrations/031_live_config_overrides.sql`, then AlgoLens `algolens-api/migrations/011_live_config_authority.sql`, then `deployment/database/qt_runtime_role_contract.sql`. The governed rehearsal migration manifest must contain those entries in order with explicit dependency edges. `apply-manifest` automatically verifies `qt-live-futures-roles.v2.json` for this extension; historical manifests retain their original behavior. Run positive and negative probes in an isolated rehearsal before proposing production grants. No production grants or pins are supplied by this change.

The API may read only the attempt's terminal classification/state/lifecycle/publication linkage; it cannot read recovery proof or update safety. Publishers use checked initialization/assertion/unsafe/finalization functions. API, publishers and workers cannot call owner-only recovery or financial-state proof functions. Existing route capability and investor isolation checks still apply.

Before rollout, inspect older running/failed attempts, missing selection/safety rows and NULL `classification_version`. Unknown history is not clean. Resolve it through separately reviewed maintenance; no migration backfills safety or invents pre-write facts. Fresh native attempts require classification version 2. Older candidates without submission authority require resubmission. Obtain a fresh equity baseline export from the exact reviewed validator/publisher build and revalidate it; do not reuse a baseline from an earlier build.

## Immutable release provisioning

The core `release-artifacts/v1` stays closed. Its exact native equity executable is `live_equity_mr`; earlier synthetic AlgoLens fixtures incorrectly used `live_equity_mean_reversion`. The shared verifier now rejects that erroneous name. Any evidence using it must be regenerated, never edited in place. No deployed evidence was inspected or changed here.

The image and release-files inventory contain `bin/Release/live_config_validate` plus `live-config-validator-bundle/`, including its manifest and complete ELF dependency closure. Once the immutable image digest is known, the native tool creates a detached `live-config-release/v1` sidecar binding the core manifest's canonical digest, source/build/image identity and validator executable/engine/bundle/manifest hashes. Publish both detached manifests through the existing immutable evidence channel and mount them read-only with the exact installed release; putting a digest-binding sidecar in its own image would create a digest cycle.

`LIVE_CONFIG_MANIFEST` points to an absolute immutable provisioning file with exactly these keys:

```json
{
  "version": 2,
  "expires_at": "<future UTC timestamp>",
  "validator": {
    "bundle_directory": "<absolute installed root>/live-config-validator-bundle",
    "bundle_sha256": "<actual bundle digest>",
    "executable_sha256": "<actual validator digest>",
    "build": "<exact core source short SHA>"
  },
  "release": {
    "sidecar_path": "<absolute immutable sidecar path>",
    "sidecar_sha256": "<SHA256 of sidecar file bytes>",
    "core_manifest_path": "<absolute immutable core manifest path>",
    "core_manifest_sha256": "<SHA256 of core manifest file bytes>",
    "installed_root": "<absolute installed root>",
    "qt_bundle_directory": "<absolute installed root>/qt-evaluator-bundle"
  },
  "scopes": [{
    "registry_id": "<registered strategy>",
    "portfolio_id": "<exact book>",
    "engine_strategy_id": "<exact native engine identity>",
    "config_snapshot": "<replace this placeholder with fresh native snapshot object>"
  }]
}
```

Placeholders are explanatory, not deployable pins. File-byte digests in provisioning differ from the internal canonical `manifest_sha256` fields. Loading and every recheck verify the installed core bytes, closed QT bundle, no pending worker inventory, exact source/build/compiler/image relations, closed validator bundle, matching engine bytes, and detached pins. Mutable `latest`, symlinks and a validator-only hash are insufficient. Provisioning v1 is refused by default; `allow_legacy_provisioning=True` exists solely as explicit isolated test/development constructor injection. It is never enabled by app_factory, environment variables or production/rehearsal wiring. Existing v1 provisioning must be regenerated as v2 for governed use.

## Completed evidence and recovery

Historical publications 1/2/3 remain readable. Their source/version is explicitly “not recorded.” New futures 4/equity 5 publications carry projection 2, exact native source/version/hashes and immutable configuration-attempt linkage. Approved snapshots retain strict allocation rules; file-only evidence preserves raw positive allocations such as 2+2 without normalizing or recreating hashes in Python/TypeScript. Composite equity covers primary strategy/portfolio invocations only: full-run stages and account execution costs remain Not collected.

Registered HOUSE composite, futures and legacy MR (including empty MR) support the existing registry-based HTTP surface. True investor documents retain empty registry, revision 0, uncontrolled, file-only identity and are validated by native/domain readers only. They have no current dashboard route; no registry membership or approval is invented. All-empty composite HOUSE remains unsupported by the legacy empty-owner publication contract. Empty investor composite native publication remains supported.

Unsafe writes block every retry, including unchanged configuration. Under the same engine/book locks, the trusted schema owner must restore actual financial state from reliable evidence/backups, then call `trading.recover_live_config_attempt(attempt_id, reason)`. The function compares the recorded pre-write proof and audits actor/reason/time; a reason or supplied hash cannot certify recovery. Recovery fences the original process. Clean failed/aborted attempts can retry immediately; unresolved running or unclassified history cannot. Never rewrite a completed successful day to make recovery pass.

Local synthetic Debug tests and captured fixtures are acceptance evidence only. They are not an image attestation, production release pin, deployed grant or production-readiness decision.

## Native packaging and replay commands

Using a reviewed clean Release build tree (the existing core release target requires the pinned toolchain and resolved image digest), stage both closed bundles, generate the existing core manifest, then create and verify the detached sidecar:

```sh
cmake --build "$BUILD_DIR" --target qt_evaluator_bundle live_config_validator_bundle
cmake --build "$BUILD_DIR" --target release_artifacts
python3 -B apps/tools/live_config_release.py --create \
  --manifest "$BUILD_DIR/live-config-release.json" \
  --core-manifest "$BUILD_DIR/release-artifacts.json" \
  --installed-root "$BUILD_DIR" --qt-bundle "$BUILD_DIR/qt-evaluator-bundle" \
  --validator-bundle "$BUILD_DIR/live-config-validator-bundle"
python3 -B apps/tools/live_config_release.py \
  --manifest "$BUILD_DIR/live-config-release.json" \
  --core-manifest "$BUILD_DIR/release-artifacts.json" \
  --installed-root "$BUILD_DIR" --qt-bundle "$BUILD_DIR/qt-evaluator-bundle" \
  --validator-bundle "$BUILD_DIR/live-config-validator-bundle"
```

The `live_config_release` CMake target composes those dependencies. Sidecar creation refuses an existing output; use a new owned release directory. CI ships exact tested bundle bytes, hashes them in `release-files.sha256`, builds the runtime image without recompiling, then produces detached evidence after digest resolution. These commands do not supply or update production pins.

Export a fresh baseline with `bin/Release/live_config_validate --export-base --config-root <reviewed-config-root> --portfolio <exact-portfolio-name>`. The shipped base/conservative/equity_mr templates are schema 2 and export unchanged. The tool does not silently strip removed schema-1 keys.

`benchmark_replay` dispatches recorded snapshot versions explicitly. Version 1 retains historical flat risk/flags; version 2 uses the native typed schema-2 parser and exact snapshot roundtrip, preserving risk modules, sleeves, optimizer choice and covariance history. Unknown versions and malformed records refuse. Replay does not enable database active overrides for file-only backtests.
