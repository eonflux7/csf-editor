# Examples

Two missions for *Commandos: Strike Force*, each built from nothing by its
recipe script, from the unpacked game (`../CSF_unpacks`) and the untouched
original archives. Neither ships game data: the scripts read it from your
installation.

| Example | Built with | Shows |
|---|---|---|
| [hello-world](hello-world/README.md) | `examples/hello-world/build.sh` | a Blender terrain, props and actors, patrols and cover, a radio objective and a travelling intro; plays in the game (v13) |
| [country](country/README.md) | `examples/country/build.sh` | a village assembled from other missions' map pieces, imported models, pulled scripts, 35 soldiers, a vehicle arrival and a body-found alarm |

Both write an authoring project that the editor opens
(`csf-editor <project folder>`), and both keep every recipe as an editable
component. `hello-world/parity.sh` checks that hello world builds
byte-identically through the editor's operations.
