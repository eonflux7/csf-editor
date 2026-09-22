#include "mission_loader.hpp"

#include "app_util.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace rwsman {
namespace {

constexpr int stage_count = 7;

struct LoadCancelled {};

void append_debug_log(const std::filesystem::path& directory, const std::string_view text) noexcept {
#ifndef NDEBUG
    try {
        if (directory.empty()) return;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        std::ofstream output(directory / "rws-man-debug.log", std::ios::app);
        output << text << '\n';
    } catch (...) {
    }
#else
    (void)directory;
    (void)text;
#endif
}

} // namespace

MissionLoader::~MissionLoader() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

bool MissionLoader::start(const std::filesystem::path& scene,
                          std::filesystem::path debug_log_directory) {
    if (running_.exchange(true)) return false;
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
    finished_ = false;
    fraction_ = 0.0F;
    {
        const std::lock_guard lock(mutex_);
        stage_ = "Starting";
        stage_index_ = 0;
        input_ = scene;
        outcome_ = std::monostate{};
    }
    worker_ = std::thread([this, scene, log = std::move(debug_log_directory)]() mutable {
        run(scene, std::move(log));
    });
    return true;
}

void MissionLoader::cancel() {
    cancel_ = true;
}

LoadProgress MissionLoader::progress() const {
    LoadProgress result;
    result.active = running_.load();
    result.cancel_requested = cancel_.load();
    result.fraction = fraction_.load();
    result.stage_count = stage_count;
    const std::lock_guard lock(mutex_);
    result.stage = stage_;
    result.stage_index = stage_index_;
    result.input = input_;
    return result;
}

MissionLoader::Outcome MissionLoader::poll() {
    if (!finished_.load()) return std::monostate{};
    if (worker_.joinable()) worker_.join();
    finished_ = false;
    const std::lock_guard lock(mutex_);
    return std::exchange(outcome_, std::monostate{});
}

void MissionLoader::set_stage(const int index, const char* text) {
    fraction_ = static_cast<float>(index - 1) / static_cast<float>(stage_count);
    const std::lock_guard lock(mutex_);
    stage_ = text;
    stage_index_ = index;
}

void MissionLoader::run(std::filesystem::path path, std::filesystem::path debug_log_directory) {
    const auto started = std::chrono::steady_clock::now();
    append_debug_log(debug_log_directory, "rws-man Debug mission load\n  input (UTF-8): " + path_utf8(path));
    const auto check_cancelled = [this] {
        if (cancel_.load()) throw LoadCancelled{};
    };
    MissionLoadResult result;
    result.input = path;
    try {
        set_stage(1, "Resolving mission resources");
            auto candidate_graph = std::make_unique<csf::MissionGraph>(
                csf::MissionGraph::load(csf::MissionOptions{path}));
            set_stage(2, "Parsing scene");
            auto candidate_document =
                std::make_unique<csf::Document>(csf::Document::load(candidate_graph->scene_path()));
            auto candidate_scene = std::make_unique<csf::MissionScene>(
                csf::MissionScene::project(*candidate_document));
            auto candidate_symbols = std::make_unique<csf::MissionSymbolIndex>();
            csf::ScriptAnimationIndex candidate_script_animations;
            std::vector<std::pair<std::filesystem::path, csf::ProgramDocument>> candidate_programs;
            std::vector<csf::Document> candidate_program_documents;
            candidate_symbols->add_scene(*candidate_scene);
            set_stage(3, "Loading scripts and databases");
            for (const auto& node : candidate_graph->nodes()) {
                check_cancelled();
                if (node.resolved_path.empty() ||
                    node.resolved_path == candidate_graph->scene_path() ||
                    (node.kind != csf::ResourceKind::mission_script &&
                     node.kind != csf::ResourceKind::cutscene_script &&
                     node.kind != csf::ResourceKind::database))
                    continue;
                const auto reference_document = csf::Document::load(node.resolved_path);
                if (reference_document.state() != csf::ParseState::non_csffbs) {
                    candidate_symbols->add_document(reference_document);
                    if (node.kind == csf::ResourceKind::mission_script ||
                        node.kind == csf::ResourceKind::cutscene_script) {
                        candidate_programs.emplace_back(
                            node.resolved_path, csf::ProgramDocument::project(reference_document));
                        candidate_program_documents.push_back(reference_document);
                    }
                    if (node.kind == csf::ResourceKind::mission_script)
                        candidate_script_animations.add_document(reference_document);
                }
            }
            auto candidate_objects = std::make_unique<csf::ObjectDatabase>();
            auto candidate_weapons = std::make_unique<csf::WeaponDatabase>();
            auto candidate_animations = std::make_unique<csf::AnimationCatalog>();
            std::vector<std::pair<std::filesystem::path, csf::CutsceneTimeline>>
                candidate_cutscenes;
            set_stage(4, "Projecting databases and cutscenes");
            for (const auto& node : candidate_graph->nodes()) {
                check_cancelled();
                auto name = path_utf8(node.resolved_path.filename());
                std::ranges::transform(name, name.begin(), [](const unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });
                if (name == "objetos.bdd" && node.state == csf::LoadState::available) {
                    *candidate_objects =
                        csf::ObjectDatabase::project(csf::Document::load(node.resolved_path));
                }
                if (name == "armas.bdd" && node.state == csf::LoadState::available)
                    *candidate_weapons =
                        csf::WeaponDatabase::project(csf::Document::load(node.resolved_path));
                if (name == "anims.bdd" && node.state == csf::LoadState::available)
                    *candidate_animations = csf::AnimationCatalog::project(
                        csf::Document::load(node.resolved_path), &candidate_graph->index());
                if (node.kind == csf::ResourceKind::cutscene_script &&
                    node.state == csf::LoadState::available) {
                    const auto cutscene_document = csf::Document::load(node.resolved_path);
                    if (cutscene_document.state() != csf::ParseState::non_csffbs)
                        candidate_cutscenes.emplace_back(
                            node.resolved_path, csf::CutsceneTimeline::project(cutscene_document));
                }
            }
            auto candidate_associations = csf::associate_actors(
                *candidate_scene, *candidate_objects, candidate_graph->index());
            csf::ProgramReferenceIndex candidate_program_references;
            for (const auto& [program_path, program] : candidate_programs) {
                (void)program_path;
                candidate_program_references.add_program(program, candidate_scene.get(),
                                                         candidate_animations.get());
            }
            set_stage(5, "Loading actor models");
            std::vector<rwsman::GeometryPreview::MissionActorModel> candidate_actor_models;
            std::unordered_map<std::string, std::shared_ptr<const rws::Document>>
                actor_prototype_cache;
            constexpr std::size_t actor_budget = 512, prototype_budget = 64;
            for (std::size_t i = 0;
                 i < candidate_associations.size() && candidate_actor_models.size() < actor_budget;
                 ++i) {
                check_cancelled();
                fraction_ = (4.0F + static_cast<float>(i) / static_cast<float>(std::max<std::size_t>(candidate_associations.size(), 1))) / stage_count;
                const auto& association = candidate_associations[i];
                if (association.visual_models.size() != 1 ||
                    !association.visual_models.front().resolved_path)
                    continue;
                const auto key = csf::ResourceIndex::normalize_path(
                    *association.visual_models.front().resolved_path);
                auto cached = actor_prototype_cache.find(key);
                if (cached == actor_prototype_cache.end()) {
                    if (actor_prototype_cache.size() >= prototype_budget) continue;
                    try {
                        auto loaded = std::make_shared<rws::Document>(
                            rws::Document::load(*association.visual_models.front().resolved_path));
                        if (loaded->chunks().empty() || loaded->chunks().front().type != 0x10U)
                            continue;
                        cached = actor_prototype_cache.emplace(key, std::move(loaded)).first;
                    } catch (const std::exception&) {
                        continue;
                    }
                }
                const auto& actor = candidate_scene->actors()[i];
                const auto spawn = candidate_scene->actor_spawn_position(actor);
                if (!spawn) continue;
                rwsman::GeometryPreview::MissionActorModel actor_model{
                    actor.source.entry_index, cached->second, rws_point(*spawn),
                    csf::mission_actor_angle_radians(actor.heading.value_or(0)),
                    csf::mission_actor_angle_radians(actor.pitch.value_or(0))};
                if (association.definitions.size() == 1) {
                    for (const auto weapon_id : association.definitions.front()->weapon_ids) {
                        const auto* weapon = candidate_weapons->find_id(weapon_id);
                        if (!weapon || !weapon->third_person_model) continue;
                        const auto resolution = candidate_graph->index().resolve(
                            *weapon->third_person_model);
                        if (resolution.candidate_indices.size() != 1 ||
                            resolution.status == csf::ResolutionStatus::ambiguous)
                            continue;
                        const auto& weapon_path = candidate_graph->index()
                                                      .resources()[resolution.candidate_indices.front()]
                                                      .path;
                        const auto weapon_key = csf::ResourceIndex::normalize_path(weapon_path);
                        auto weapon_cached = actor_prototype_cache.find(weapon_key);
                        if (weapon_cached == actor_prototype_cache.end()) {
                            if (actor_prototype_cache.size() >= prototype_budget) break;
                            try {
                                auto loaded =
                                    std::make_shared<rws::Document>(rws::Document::load(weapon_path));
                                if (loaded->chunks().empty() ||
                                    loaded->chunks().front().type != 0x10U)
                                    continue;
                                weapon_cached =
                                    actor_prototype_cache.emplace(weapon_key, std::move(loaded)).first;
                            } catch (const std::exception&) {
                                continue;
                            }
                        }
                        auto hand = weapon->hand.value_or("");
                        std::ranges::transform(hand, hand.begin(), [](const unsigned char value) {
                            return static_cast<char>(std::toupper(value));
                        });
                        actor_model.attachments.push_back(
                            {weapon_cached->second,
                             weapon->name.value_or("Weapon " + std::to_string(weapon_id)),
                             hand.find("IZQUIERDA") != std::string::npos ||
                                 hand.find("LEFT") != std::string::npos});
                        // Object definitions list inventory in preference order. Display the
                        // first resolved third-person model as the default equipped item.
                        break;
                    }
                }
                candidate_actor_models.push_back(std::move(actor_model));
            }
            const auto resolved = [&](const csf::DependencyKind kind) -> std::filesystem::path {
                for (const auto& edge : candidate_graph->edges()) {
                    if (edge.kind != kind || !edge.target) continue;
                    const auto found =
                        std::ranges::find_if(candidate_graph->nodes(), [&](const auto& node) {
                            return node.id == *edge.target &&
                                   node.state == csf::LoadState::available;
                        });
                    if (found != candidate_graph->nodes().end()) return found->resolved_path;
                }
                return {};
            };
            const auto visual_path = resolved(csf::DependencyKind::visual_map);
            if (visual_path.empty()) throw std::runtime_error("Mission has no resolved visual map");
            set_stage(6, "Loading visual and collision maps");
            auto candidate_visual =
                std::make_unique<rws::Document>(rws::Document::load(visual_path));
            std::unique_ptr<rws::Document> candidate_collision;
            const auto collision_path = resolved(csf::DependencyKind::collision_map);
            if (!collision_path.empty())
                candidate_collision =
                    std::make_unique<rws::Document>(rws::Document::load(collision_path));
            check_cancelled();
            set_stage(7, "Building overlays");
            auto overlays = make_mission_overlays(*candidate_scene);
            append_cutscene_camera_overlays(*candidate_scene, candidate_cutscenes, overlays);
            append_actor_collision_overlays(*candidate_scene, candidate_associations, overlays);
        check_cancelled();
        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        auto& mission = result.mission;
        result.visual = std::move(candidate_visual);
        result.collision = std::move(candidate_collision);
        result.collision_path = collision_path;
        mission.graph = std::move(candidate_graph);
        mission.document = std::move(candidate_document);
        mission.scene = std::move(candidate_scene);
        mission.symbols = std::move(candidate_symbols);
        mission.objects = std::move(candidate_objects);
        mission.animations = std::move(candidate_animations);
        mission.script_animations = std::move(candidate_script_animations);
        mission.cutscenes = std::move(candidate_cutscenes);
        mission.programs = std::move(candidate_programs);
        mission.program_documents = std::move(candidate_program_documents);
        mission.program_references = std::move(candidate_program_references);
        mission.actor_associations = std::move(candidate_associations);
        result.actor_models = std::move(candidate_actor_models);
        result.overlays = std::move(overlays);
        fraction_ = 1.0F;
        {
            const std::lock_guard lock(mutex_);
            outcome_ = std::move(result);
        }
    } catch (const LoadCancelled&) {
        const std::lock_guard lock(mutex_);
        outcome_ = MissionLoadFailure{path, stage_, "cancelled", true};
    } catch (const std::exception& error) {
        std::string stage;
        {
            const std::lock_guard lock(mutex_);
            stage = stage_;
        }
        append_debug_log(debug_log_directory, "mission load failed\n  stage: " + stage +
                                                  "\n  input (UTF-8): " + path_utf8(path) +
                                                  "\n  error: " + error.what());
        const std::lock_guard lock(mutex_);
        outcome_ = MissionLoadFailure{path, stage, error.what(), false};
    }
    running_ = false;
    finished_ = true;
}

} // namespace rwsman
