> **Archived plan — implemented.** Staging, packaging, and deployment as built are
> documented in [Guarded authoring and mods](../../guides/guarded-authoring-and-mods.md)
> and [Mission editor](../../guides/mission-editor.md); this document is kept for
> its overlay/deployment design, risks, and test plan.

# Phase 07: mod staging and ecosystem support

## Outcome

Turn validated edits into a reproducible mod project without copying or modifying
the source corpus. Stage only changed/new files under their game-relative paths,
validate the complete overlay, invoke the vetted `pak-man` CLI as the primary PAK
packaging path, and provide deployment/rollback metadata. Expand secondary-format
support where it serves concrete mod workflows.

## Dependencies

- Phase 02 complete dependency graph and resource-root model.
- Phase 06 guarded saves, validation, and change manifests.
- External packaging format evidence and game-version testing.

## In scope

- A mod-project manifest separate from game resources.
- Source installation/corpus roots referenced but never embedded.
- Staging tree containing changed and user-created files only.
- Deterministic validation of staged-overlay-plus-source resolution.
- Conflict detection between two staged mods.
- File hashes, intended relative paths, source provenance, tool version, and
  validation status.
- Reproducible build command producing a transparent staging directory.
- Primary PAK production through the independently validated `pakman-cli`.
- Deployment to an explicitly selected disposable/test installation with preview,
  confirmation, backup/rollback, and exact target reporting.
- Interoperability documentation for existing community tools.
- Secondary-format work selected by modding value:
  - particle-system preview/editing (`.sp`);
  - PRP/DST configuration;
  - textures and existing DDS staging workflow;
  - localization text (`.fli/.sub`) with encoding preservation;
  - UI definitions (`.fbs`, FontWapa, RAW) if demanded;
  - WAD/audio and SEC only after their schemas are established.
- Optional plugin/import-export boundaries so secondary tools do not bloat the
  mission core.

## Explicitly out of scope

- Shipping original copyrighted game resources in a mod package.
- Modifying the user's only game installation without backup and confirmation.
- Treating an external packer as trustworthy without inspecting output and
  testing recovery.
- Automatic download or redistribution of third-party tools without license and
  integrity review.
- Merging two conflicting binary/CSFFBS edits by guesswork.
- Claiming universal compatibility across game versions without a version matrix.
- Implementing every secondary resource editor merely because the format exists.

## Mod project model

Suggested manifest concepts:

```yaml
format_version: 1
name: Example Mod
game: commandos-strike-force
tool_version: ...
source_fingerprint: ...
files:
  - relative_path: Maps/ST08/Ambush.gsc
    staged_path: files/Maps/ST08/Ambush.gsc
    source_hash: ...
    output_hash: ...
    change_manifest: changes/Ambush.gsc.json
    validation: passed
dependencies: []
conflicts: []
```

Do not store absolute local source paths in distributable manifests by default.
Local workspace configuration can map logical roots to machine-specific paths in
user data outside the repository and mod package.

## Overlay validation

Resolve resources as the game would see them with staged files taking precedence
over source files. Then rerun:

- content/root-type sniffing;
- CSFFBS/RWS structural validation;
- mission dependency resolution;
- class/script/resource references;
- navigation/spatial checks;
- model/Physics/animation compatibility checks where relevant;
- changed-versus-source manifest verification.

Report newly missing or ambiguous dependencies introduced by the mod separately
from pre-existing source-corpus diagnostics.

## Packaging strategy

Support two output levels:

1. **PAK archive:** the primary distributable, built from changed/new files by
   invoking `pakman-cli` and accepted only after full verification succeeds.
2. **Staging directory:** the transparent and testable intermediate/fallback.

Existing community evidence indicates that an uncompressed CSF PAK variant is
accepted by the game. Use the user-configured CLI from the sibling `pak-man`
project rather than duplicating its writer. Record the executable hash when its
path is available, invoke it without a command shell, require its full post-create
verification, and never use a source archive as an output path.

## Deployment and rollback

Deployment is a destructive-adjacent operation and must be more conservative than
ordinary export:

- require an explicit test-installation root;
- enumerate exact target files before writing;
- reject roots that equal the resource corpus or repository;
- back up every replaced target to a timestamped manifest-backed location;
- write through temporary files and verify hashes;
- support dry run;
- support exact rollback from the deployment manifest;
- never delete unrecognized files during rollback;
- clearly distinguish loose-file deployment from PAK deployment.

The primary deployment command installs a freshly reverified PAK at an explicit
game-relative path. Loose-file deployment remains a separately named diagnostic
fallback; both use the same dry-run, backup manifest, hash verification, and
rollback safeguards.

The first release may stop at producing a staging directory and instructions. A
safe non-deployment workflow is preferable to an unreliable installer.

## Secondary-format priorities

### Particle systems (`.sp`)

High-value follow-up because the generic CSFFBS parser/serializer already covers
the container and fields are descriptive. Add a particle preview only after its
coordinate, timing, blending, and texture semantics are validated.

### Textures and lightmaps

Reuse the existing DDS inspection and Blender staging workflow. Integrate texture
dependencies and manifests with mod projects rather than creating a second encoder
path.

### Localization (`.fli`, `.sub`)

Preserve BOM, UTF-16 line endings, identifiers, and empty entries. Do not confuse
CSF `.fli` text with Autodesk FLIC animation.

### UI (`.fbs`, CSFFBS `.txt`, `.raw`, `.fnt`)

Generic inspection/editing can arrive cheaply through CSFFBS. A WYSIWYG UI editor
requires separate raster/font/control rendering and should be demand-driven.

### WAD/audio

Before authoring, recover `0x802` bank/chunk structure, codec/sample metadata, and
logical mappings from `Sonidos.bdd`. Until then, retain WAD as an opaque dependency
and interoperate with established community converters where lawful and safe.

### SEC

Do not edit until its spatial role and complete record structure are independently
established. Comparing SEC against SCN sector maps, RWS BSP bounds, and executable
readers is a reverse-engineering task, not a packaging prerequisite.

## Work breakdown

### P07-01: mod workspace and manifest

- Define versioned project and local configuration schemas.
- Separate distributable logical paths from local absolute roots.
- Import Phase 06 change manifests into one project view.

### P07-02: overlay builder

- Produce a clean staging directory containing changed/new files only.
- Verify source and output hashes.
- Make output deterministic and safe to rebuild.
- Never copy unchanged corpus content merely to make a “complete” tree.

### P07-03: overlay validation and conflicts

- Resolve staged-over-source resources.
- Distinguish introduced diagnostics from source baseline.
- Compare two mod manifests for path and semantic-record conflicts.

### P07-04: PAK research/integration

- Document archive variants and timestamps/compression behavior.
- Integrate the validated `pakman-cli` command contract directly.
- List/extract/repack controlled samples and compare file paths/content.
- Keep staging-directory output usable for inspection and recovery.

### P07-05: deployment and rollback

- Add dry-run target enumeration.
- Add explicit test-root validation, backup manifest, verified writes, and rollback.
- Defer GUI one-click deployment until CLI behavior is thoroughly tested.

### P07-06: selected secondary formats

- Choose work from demonstrated mod workflows, not raw file counts.
- Reuse CSFFBS and resource-graph infrastructure.
- Keep audio/UI/particle-specific renderers outside core mission semantics.

### P07-07: ecosystem documentation

- Document compatible external tools and file handoffs.
- Record tool versions, licenses, and limitations.
- Provide end-to-end tutorials for inspect → edit → validate → stage → package →
  test → rollback.

## Test plan

### Mod project tests

1. New project contains no absolute distributable source path.
2. Staging includes changed/new files only.
3. Rebuild is deterministic.
4. Source hash mismatch is diagnosed before applying an edit based on stale data.
5. Overlay resolution prefers staged file and falls back to source.
6. Introduced versus baseline diagnostics are separated.
7. Path conflict between two mods is reported.
8. Compatible independent changes coexist.
9. Project/manifest version migration is explicit and tested.

### Packaging tests

- empty and single-file archive where valid;
- paths with spaces, case variants, and Western European bytes;
- deterministic list/extract/repack result;
- compressed versus uncompressed variant behavior if supported;
- timestamp handling;
- archive output never aliases an input/source archive;
- corrupted archive is rejected without partial deployment.

### Deployment tests

- dry run performs no writes;
- invalid/broad/source/repository target roots are rejected;
- exact target list and backup list match;
- interrupted write retains recoverable backup state;
- rollback restores hashes and does not delete unrelated files;
- repeated deploy/rollback is idempotent within documented constraints.

Use synthetic directories and disposable test installations. Never use the only
installed game or the read-only reference corpus as a destructive test target.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Mod package accidentally includes original resources | Changed/new-only staging plus manifest/license checks |
| Source update makes binary edits stale | Record source hashes and refuse silent application on mismatch |
| PAK writer produces subtly invalid archives | Retain transparent staging; require independent round-trip tests and full post-create verification |
| Deployment damages an installation | Explicit disposable root, dry run, backup, verified writes, rollback |
| Scope drifts into unrelated asset editors | Select secondary formats by concrete workflow and plugin boundaries |
| Two mods alter the same structured file | Report path and semantic conflicts; never guess-merge binary output |

## Definition of done

- A mod project references source data without embedding or altering it.
- Deterministic staging contains only changed/new files and complete validation
  manifests.
- Overlay validation reports dependencies and diagnostics as they would appear with
  staged precedence.
- Conflicts between mod projects are detectable at least by path and, for supported
  CSFFBS edits, semantic target.
- PAK creation through `pakman-cli` is the primary packaging outcome; staging
  remains a complete inspection and recovery artifact.
- The integrated PAK path requires full post-create verification and never
  overwrites source archives.
- Any deployment path supports dry run, explicit targets, backups, verification,
  and exact rollback without deleting unrelated files.
- Secondary-format support is documented, tested, and justified by a real modding
  workflow.
- An end-to-end tutorial demonstrates non-destructive inspect, edit, validate,
  stage, package/test, and rollback.
