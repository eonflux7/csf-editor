#include "rwsman/animation_slots.hpp"

#include <array>
#include <cctype>
#include <utility>
#include <vector>

namespace rwsman {
namespace {

using Word = std::pair<std::string_view, std::string_view>;

// The state an actor is in, when the slot starts with it.
constexpr std::array states{
    Word{"AGACHADO", "Crouched"}, Word{"ALERTA", "Alert"},        Word{"COMBATE", "Combat"},
    Word{"DISTRAIDO", "Unaware"}, Word{"DUDOSO", "Suspicious"},   Word{"DUAL", "Two pistols"},
};

constexpr std::array words{
    Word{"ACCEDER", "use"},         Word{"ACCION", "action"},        Word{"ACUBIERTO", "to cover"},
    Word{"ACUCHILLADO", "knifed"},  Word{"ACUCHILLAR", "knife"},     Word{"AGACHARSE", "crouch down"},
    Word{"AHORCADO", "garrotted"},  Word{"AHORCAR", "garrotte"},     Word{"AMARTILLAR", "cock the weapon"},
    Word{"AMENAZADO", "held at gunpoint"}, Word{"AMENAZAR", "threaten"}, Word{"AMENAZA", "threat"},
    Word{"ANDAR", "walk"},          Word{"APUNTAR", "aim"},          Word{"ARMA", "weapon"},
    Word{"ASOMARSE", "peek"},       Word{"ATR", "backwards"},        Word{"ATRAS", "backwards"},
    Word{"ATURDIDO", "stunned"},    Word{"AVANZANDO", "advancing"},  Word{"AYUDA", "for help"},
    Word{"BAJAR", "get off"},       Word{"BOMBA", "bomb"},           Word{"CONTEXTUAL", "context"},
    Word{"CORRER", "run"},          Word{"CORRIENDO", "running"},    Word{"CORTO", "short"},
    Word{"CUBIERTO", "to cover"},   Word{"CURAR", "heal"},           Word{"CURARSE", "heal self"},
    Word{"DEL", "forwards"},        Word{"DER", "right"},            Word{"DERRIBO", "knocked down"},
    Word{"DESTRUCCION", "destroyed"}, Word{"DINAMICO", "moving"},    Word{"DISFRAZARSE", "put on a disguise"},
    Word{"DISPARANDO", "shooting"}, Word{"DISPARAR", "shoot"},       Word{"DISPARO", "shot"},
    Word{"ENTRAR", "go down"},      Word{"ESPERAR", "wait"},         Word{"ESTATICO", "in place"},
    Word{"ESTRANGULADO", "strangled"}, Word{"ESTRANGULAR", "strangle"}, Word{"FUEGO", "by fire"},
    Word{"GAS", "by gas"},          Word{"GIRO", "turn"},            Word{"GUARDAR", "put away"},
    Word{"IDLE", "standing idle"},  Word{"IMPACTO", "hit"},          Word{"INTERROGANDO", "interrogating"},
    Word{"IZQ", "left"},            Word{"LANZAR", "throw"},         Word{"LEFT", "left"},
    Word{"LEVANTARSE", "get up"},   Word{"LLAMADA", "a call"},       Word{"MANIPULA", "handle"},
    Word{"MATAR", "to kill"},       Word{"MELEE", "melee"},          Word{"METER", "holster"},
    Word{"MORIBUNDO", "wounded on the ground"}, Word{"MORIR", "die"}, Word{"MUERTE", "death"},
    Word{"NADAR", "swim"},          Word{"OBJETO", "object"},        Word{"OCIO", "fidgeting"},
    Word{"PEDIR", "call"},          Word{"PREPARAR", "prepare"},     Word{"PUESTO", "seat"},
    Word{"REACTIVIDAD", "reaction"}, Word{"RECARGA", "reload"},      Word{"RECARGAR", "reload"},
    Word{"REPOSO", "at rest"},      Word{"RESPONDER", "answer"},     Word{"RETROCEDIENDO", "backing off"},
    Word{"RIGHT", "right"},         Word{"SACAR", "draw"},           Word{"SALIR", "leave"},
    Word{"SALTANDO", "jumping"},    Word{"SALUDO", "salute"},        Word{"SENTARSE", "sit down"},
    Word{"SIN", "without"},         Word{"STRAFE", "sidestep"},      Word{"SUBIR", "get on"},
    Word{"TERMINAR", "finish"},     Word{"TUMBARSE", "lie down"},    Word{"VEHICULO", "vehicle"},
    Word{"VIGILANDO", "watching"},
};

std::string_view find(const auto& table, const std::string_view key) {
    for (const auto& [spanish, english] : table)
        if (spanish == key) return english;
    return {};
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

std::string animation_slot_label(const std::string_view slot) {
    std::vector<std::string_view> tokens;
    for (std::size_t begin = 0; begin <= slot.size();) {
        const auto end = std::min(slot.find('_', begin), slot.size());
        if (end > begin) tokens.push_back(slot.substr(begin, end - begin));
        begin = end + 1;
    }
    if (tokens.empty()) return std::string(slot);
    std::string state, weapon;
    if (const auto english = find(states, tokens.front()); !english.empty() && tokens.size() > 1) {
        state = english;
        tokens.erase(tokens.begin());
    }
    if (tokens.back() == "ARMA1" || tokens.back() == "ARMA2") {
        weapon = std::string("weapon ") + tokens.back().back();
        tokens.pop_back();
    }
    std::string action;
    for (const auto token : tokens) {
        std::string word;
        if (token == "90" || token == "180") word = std::string(token) + "\xC2\xB0";  // degrees
        else if (const auto english = find(words, token); !english.empty()) word = english;
        else if (const auto english = find(states, token); !english.empty()) word = lower(english);
        else word = lower(token);
        action += (action.empty() ? "" : " ") + word;
    }
    std::string label = state;
    for (const auto& part : {action, weapon})
        if (!part.empty()) label += (label.empty() ? "" : " \xC2\xB7 ") + part;  // middle dot
    if (!label.empty()) label.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(label.front())));
    return label;
}

namespace {

bool starts_with_ignoring_case(const std::string_view text, const std::string_view prefix) {
    if (text.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(text[i])) != std::toupper(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

bool contains_ignoring_case(const std::string_view text, const std::string_view word) {
    for (std::size_t i = 0; i + word.size() <= text.size(); ++i)
        if (starts_with_ignoring_case(text.substr(i), word)) return true;
    return false;
}

} // namespace

WeaponStance weapon_stance(const std::string_view weapon_name) {
    for (const auto rifle : {"Mauser", "Springfield", "Kar98", "Rifle", "Fusil"})
        if (contains_ignoring_case(weapon_name, rifle)) return WeaponStance::rifle;
    for (const auto smg : {"Mp40", "Mp 40", "Thompson", "Sten", "Subfusil"})
        if (contains_ignoring_case(weapon_name, smg)) return WeaponStance::smg;
    for (const auto pistol : {"Luger", "Pistol", "Walther", "Colt"})
        if (contains_ignoring_case(weapon_name, pistol)) return WeaponStance::pistol;
    return WeaponStance::unknown;
}

WeaponStance animation_stance(const std::string_view animation_name) {
    if (starts_with_ignoring_case(animation_name, "SF")) return WeaponStance::rifle;
    if (starts_with_ignoring_case(animation_name, "SM")) return WeaponStance::smg;
    return WeaponStance::unknown;
}

bool animation_fits(const WeaponStance soldier, const std::string_view animation_name) {
    const auto needed = animation_stance(animation_name);
    return soldier == WeaponStance::unknown || needed == WeaponStance::unknown || needed == soldier;
}

const char* weapon_stance_name(const WeaponStance stance) noexcept {
    switch (stance) {
    case WeaponStance::rifle: return "rifle";
    case WeaponStance::smg: return "submachine gun";
    case WeaponStance::pistol: return "pistol";
    case WeaponStance::unknown: break;
    }
    return "unknown weapon";
}

} // namespace rwsman
