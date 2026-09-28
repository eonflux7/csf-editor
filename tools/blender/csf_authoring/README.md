# CSF authoring Blender add-on

The Blender side of a csf-editor authoring project: tag terrain and building
meshes, send them to the project (an open csf-editor rebuilds the map), and load
the built map with the mission's actors, routes, areas and dummies as a
view-only reference. See `docs/guides/blender-authoring.md` in csf-editor.

Install `csf_authoring.zip` from **Edit > Preferences > Add-ons > Install from
Disk**, then set the **csf-mod** executable in the add-on's preferences
(csf-editor's `build/Release/csf-mod`).

Without a project, **Import map** (sidebar, or File > Import > CSF map) opens a
shipped map (`.rws`, through `csf-mod world-source`) or a `.csfworld` with its
visual and collision geometry, and **Export .csfworld** (File > Export) writes
it back for `csf-mod world-build`; see "A map without a project" in the guide.

**Collision shade** paints or bakes (Cycles) the per-triangle shade byte of the
collision as the vertex colours `csf_shade_color`; see "Collision shade" in the
guide.
