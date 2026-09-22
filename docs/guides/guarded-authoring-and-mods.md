# Guarded authoring and mod projects

`csf-mod` is the non-destructive authoring workbench. It writes a new CSFFBS
file, reopens and validates it, records SHA-256 hashes and exact entry changes,
then lets that output participate in a changed-files-only mod overlay. It never
uses a source resource as an output path.

The first authoring slice is intentionally narrow. `csf-mod edit` accepts only
fields returned by the reviewed schema registry: selected database values,
mission/player properties, transform components, supported references, and
script flags. Other numeric-looking records remain read-only. The C++
`csf::EditSession` is the serializer/history foundation used by typed views; it
supports exact undo/redo, finite-real validation, copy-on-write strings by
default, explicit global string replacement, preservation of trailing/unknown
bytes, and byte-identical no-op serialization.

## Inspect, edit, and validate

Use `csf-info` to locate an entry and understand its typed context. Then write a
copy, never the source:

```bash
./build-core/Release/csf-mod edit ../CSF_unpacks/Maps/ST08/Ambush.scn \
  /tmp/example-mod-authored/Maps/ST08/Ambush.scn --real 1234 90
```

The command emits `Ambush.scn.changes.json`. The save is published only after a
byte-for-byte reopen check, structural validation, and the applicable mission or
program typed validation. A string edit is copy-on-write, so another entry that
shares the original table value is unaffected. `--global-string` is the explicit
shared-value operation.

## Create and package a mod project

```bash
./build-core/Release/csf-mod init /tmp/example-mod ../CSF_unpacks "Example Mod"
./build-core/Release/csf-mod add /tmp/example-mod Maps/ST08/Ambush.scn \
  /tmp/example-mod-authored/Maps/ST08/Ambush.scn \
  /tmp/example-mod-authored/Maps/ST08/Ambush.scn.changes.json \
  --target actor:17:heading
./build-core/Release/csf-mod validate /tmp/example-mod
./build-core/Release/csf-mod package /tmp/example-mod /tmp/example-mod-stage \
  /tmp/example-mod.pak --pakman ../pak-man/build/linux/pakman-cli
```

`mod-project.json` is distributable and contains no absolute source root. The
machine-specific root stays in `local-config.json` and the private project state.
The primary distributable is the PAK produced by `pakman-cli`. `csf-mod` first
rebuilds a transparent stage containing only `files/<game-relative-path>`, copied
change manifests, and the project manifest. It validates hashes and CSFFBS
outputs, then passes only the contents of `files/` to `pakman-cli create` and
requires `pakman-cli verify` to succeed before publishing the archive. The
adjacent `.package.json` records the archive hash, PAK variant, platform, and the
packer executable hash when an explicit executable path was supplied.

PC stored PAKA is the default. Select another writer mode explicitly with
`--type stored|compressed` and `--platform pc|ps2|xbox|ps2-prototype`.
Existing archives are never replaced unless `--overwrite` is present. The loose
stage remains available for inspection, diagnostics, and recovery; it can also
be rebuilt alone when debugging:

```bash
./build-core/Release/csf-mod build /tmp/example-mod /tmp/example-mod-stage
```

The inspected sibling project identifies itself as `pak-man` 0.1.4 and is MIT
licensed. Its CLI creates stored PAKA and compressed PAKC for retail PC, PS2, and
Xbox, plus stored PS2-prototype archives; compressed prototype output is rejected.
Archive filenames use the game's Windows-1252 convention, so unrepresentable game
paths remain a packaging error rather than being silently renamed.

The sibling project builds the CLI natively at
`../pak-man/build/linux/pakman-cli` on Linux and
`../pak-man/build/Release/pakman-cli.exe` on Windows; pass the applicable path to
`--pakman`. A `pakman-cli` available on `PATH` is used when the option is omitted.
`pak-man` is not downloaded or redistributed by this project and remains a
separately built tool. The executable SHA-256 in `.package.json` identifies the
exact local build more reliably than a version string alone.

Check two projects before combining them:

```bash
./build-core/Release/csf-mod conflicts /tmp/example-mod /tmp/another-mod
```

Same-path replacements are conflicts. Matching `--target` identities additionally
report a semantic conflict; the tool never guesses how to merge binary edits.

## Whole mission archives

The game loads each mission from `maps/<Mission>.pak` and names only `Patch.pak`,
`PatchMP.pak` and `Global.pak` besides them, so a changed-files PAK under an
arbitrary path such as `Mods/example-mod.pak` is not picked up. To play edits,
rebuild the mission archive itself with `csf-mod export-mission` (or the GUI's
export) and deploy that to `maps/<Mission>.pak`; see
[mission-editor.md](mission-editor.md). The examples below still show the generic
deployment mechanics.

## Test deployment and rollback

PAK deployment to an explicitly selected test installation defaults to a dry run.
The final path is game-relative and is never inferred:

```bash
./build-core/Release/csf-mod deploy-pak /tmp/example-mod /tmp/example-mod.pak \
  /games/csf-test Mods/example-mod.pak --pakman ../pak-man/build/linux/pakman-cli
```

Review every `would-deploy` line, then explicitly apply:

```bash
./build-core/Release/csf-mod deploy-pak /tmp/example-mod /tmp/example-mod.pak \
  /games/csf-test Mods/example-mod.pak --pakman ../pak-man/build/linux/pakman-cli --apply
```

`deploy-pak` requires the adjacent package manifest, checks its recorded SHA-256,
and reruns full `pakman-cli verify` before either preview or application.
Loose-file deployment is retained only as a diagnostic fallback:

```bash
./build-core/Release/csf-mod deploy-loose \
  /tmp/example-mod /tmp/example-mod-stage /games/csf-test
```

The target must already exist and must not alias the source corpus, workspace,
stage, or package. Replaced files are hash-verified and backed up beneath
`.csf-mod-backups/<id>`. The command prints its exact rollback manifest:

```bash
./build-core/Release/csf-mod rollback \
  /games/csf-test/.csf-mod-backups/<id>/deployment.state
```

Rollback refuses to overwrite a deployed file that changed after deployment,
restores verified backups, removes only files recorded as newly deployed, and
does not touch unrelated files.

PAK creation and full verification are delegated directly to the inspected
`pak-man` CLI; no second archive writer is embedded here. Particle, localization,
UI, WAD/audio, and SEC editing remain outside this initial base because those
formats require their own proven schemas and preservation tests.
