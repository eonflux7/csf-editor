#pragma once

#include "csf/mission_edit.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Mission operations as text: one MissionEditor operation or preset per line,
// `<op> key=value ...`, for scripts, agents and tests (`csf-mod mission-ops`).
// Values with spaces are double-quoted; positions are `x,y,z`, lists of them
// `x,y,z;x,y,z`, navigation points `x,y,z[,rotation-radians]`. '#' starts a
// comment line.
//
//   new-mission
//   import-class donor=<package root> class=<id>      import-anim donor=<root> anim=<id>
//   actor [id=] name= class= pos= [heading=] [pitch=] [cell=<group>/<point>] [scripts=a,b] [portrait=]
//   prop [id=] name= class= pos= [heading=]            (an actor without placement point)
//   nav-group [id=] name= type= points= [links=1-2,2-3 | loop | chain]
//   link from=<group>/<point> to=<group>/<point>       link-nearest group= target=
//   dummy [id=] name= pos= [rot=] [pitch=]             area [id=] name= height= points=
//   look actor= class=                                 player actor=
//   scale-class class= scale=                         (a scaled copy of a class, IDs from 500)
//   script file=<path>          cutscene-script file=<path>   (relative to the ops file)
//   guard-patrol [id=] name= class= [heading=] route= route-name= points= [pause=] [cover=]
//                [script=] script-name= [loop=0]
//   guard-idle [id=] name= class= pos= [heading=] [portrait=] [cover=] [script=] script-name=
//              loop=<anim>[:<min>-<max>],<anim>...
//   animal-patrol [id=] name= class= pos= [heading=] route= route-name= points= walk= [script=] script-name=
//   cover-group [id=] name= points=
//   walk-grid [id=] [name=] spacing= x=<min>..<max> z=<min>..<max> [stagger=0|1]
//             [exclude=x0,z0,x1,z1;...] [avoid=x,z,r;...]   (heights from the ground)
//   objective n= kind=zone|kill|use target=<zone or actor> label=<fli> done=<fli> [prompt=<fli>]
//             [secondary=1] [script=] [script-name=]      (collected; then:)
//   objectives [setup=] [setup-name=] [success=<fli>] [pause=]
//   kit actor= weapons=<class>[@<ammo>/<ammo>],... [select=<class>] [disguise=<class>]   (then:)
//   equipment [script=] [script-name=]
//   tips tips=<fli>,... [pos=<x>,<y>] [script=] [script-name=]
//   shot camera=x,y,z [end=x,y,z] target=x,y,z [seconds=] [aim=<rot>,<pitch>] [heading=] [speed=]   (then:)
//   intro [class=197] [send-init=0] [dummy=] [actor=] [group=] [script=] [script-name=]
//         [cutscene=<main>,<init>,<end>,<camera>] [cutscene-name=]
namespace csf {

struct MissionOpsOptions {
    std::filesystem::path base_directory;  // resolves script and donor paths
    // Ground height at (x, z) for walk grids; walk-grid fails without it.
    std::function<std::optional<float>(float, float)> ground;
};

struct MissionOpOutcome {
    std::size_t line{};
    std::string op;
    EditResult result;
};

// Runs every line until one is rejected (its outcome is the last one).
[[nodiscard]] std::vector<MissionOpOutcome> run_mission_ops(MissionEditor& editor, std::string_view text,
                                                            const MissionOpsOptions& options = {});

} // namespace csf
