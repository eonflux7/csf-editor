#pragma once

#include <string>
#include <string_view>

// Readable names for the game's animation slots (csf::animation_slot_names:
// DISTRAIDO_IDLE_ARMA1, ALERTA_ANDAR_DEL, ...). The slots are Spanish engine
// names for "the animation the game plays in this situation"; the labels
// translate them word by word, so they are a reading aid, not documentation
// of what the game does in each slot.
namespace rwsman {

// "DISTRAIDO_IDLE_ARMA1" -> "Unaware · standing idle · weapon 1". Unknown
// words are kept as they are, in lower case.
[[nodiscard]] std::string animation_slot_label(std::string_view slot);

// The slot a guard standing at a post plays while nothing has alerted it,
// the one most overrides replace.
inline constexpr std::string_view default_idle_slot = "DISTRAIDO_IDLE";

// How a soldier holds his weapon, which decides the animations that fit him:
// across the shipped missions `SF*` clips are played only by rifle classes,
// `SM*` by submachine-gun classes, and `SP*` (weapon away: smoking, the
// radio, a map) by any (examples/country/README.md).
enum class WeaponStance : unsigned char { unknown, rifle, smg, pistol };
// A weapon by its Armas.bdd name: Mauser and Springfield are rifles, Mp40 and
// Thompson submachine guns, Luger a pistol.
[[nodiscard]] WeaponStance weapon_stance(std::string_view weapon_name);
// The stance an animation is made for by its name's prefix; unknown for
// SP* and every other clip, which fit any soldier.
[[nodiscard]] WeaponStance animation_stance(std::string_view animation_name);
[[nodiscard]] bool animation_fits(WeaponStance soldier, std::string_view animation_name);
// "rifle", "submachine gun", "pistol".
[[nodiscard]] const char* weapon_stance_name(WeaponStance stance) noexcept;

} // namespace rwsman
