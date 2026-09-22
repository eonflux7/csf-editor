# Geometry and scene export

Selecting a Geometry in the GUI exposes local-space OBJ export (in the Inspector's Decoded section). The same operation
is available for every Geometry through `rws-info --export-obj`.

**Export whole scene (glTF)** writes three sibling files:

```text
map.gltf
map.bin
map.manifest.json
```

The glTF is Y-up and keeps each Atomic, resolved CSF placement, and World Sector as
a separately named node. Transforms are baked into positions. Original normals,
material assignments, base UVs, and existing lightmap UVs are retained where
available; zero-area and coplanar duplicate World faces are removed. glTF extras
and the manifest record texture names and original RWS source offsets.

CSF's centimetre-scale coordinates are converted to glTF metres (`0.01x`). The
conversion is recorded in the glTF metadata and manifest so it can be reversed.
DDS textures are referenced in the manifest but are not copied or converted.

**Export selected Clump (glTF)** exports the top-level Clump containing the current
tree selection. Its default filename includes the Clump's source offset. The CLI
equivalent accepts that offset explicitly with `--export-clump-gltf`.

**Export collision only (glTF/OBJ)** uses only validated sectors in the active
collision document (or a directly opened `_col.rws`). It never discovers a sibling
in the CLI. glTF applies the documented `0.01x` conversion and records source
offset/material/surface metadata in extras and its manifest. OBJ preserves source
RWS axes and units, uses stable World/sector groups and sanitized material names,
and writes sibling MTL and manifest files. Invalid triangles are omitted and
reported; input RWS bytes are never modified.

Every export refuses to replace an existing file unless you choose the
confirm-before-overwrite policy in Preferences. See the flag table in
[Command-line usage](../getting-started/cli.md) for the CLI forms.
