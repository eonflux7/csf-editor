#include "rwsman/problems.hpp"

#include "csf/authoring_project.hpp"
#include "csf/mission_edit.hpp"
#include "csf/mission_flow.hpp"
#include "csf/mission_scene.hpp"
#include "csf/object_database.hpp"

#include <algorithm>
#include <cstdio>

namespace rwsman {
namespace {

std::string subject_key(const ProblemSubject& subject) {
    return std::to_string(static_cast<int>(subject.kind)) + ":" + std::to_string(subject.id) + ":" + subject.text;
}

} // namespace

std::vector<Problem> collect_problems(const ProblemInputs& inputs) {
    std::vector<Problem> problems;
    const auto add = [&](Problem problem) {
        problem.id = problem.source + "|" + subject_key(problem.subject) + "|" + problem.message;
        problems.push_back(std::move(problem));
    };

    if (inputs.flow) {
        for (const auto& finding : inputs.flow->findings()) {
            Problem problem;
            problem.severity = finding.severity == csf::FlowFinding::Severity::error     ? Problem::Severity::error
                               : finding.severity == csf::FlowFinding::Severity::warning ? Problem::Severity::warning
                                                                                          : Problem::Severity::note;
            problem.source = "Flow";
            problem.message = finding.message;
            problem.kind = EntityKind::script;
            if (finding.script) {
                const bool mission = inputs.flow->script("mission", *finding.script) != nullptr;
                problem.subject = {ProblemSubject::Kind::script, *finding.script, mission ? "mission" : "cutscene"};
            }
            add(std::move(problem));
        }
    }

    for (const auto& finding : inputs.heights) {
        Problem problem;
        problem.source = "Heights";
        problem.kind = finding.subject == csf::HeightFinding::Subject::actor ? EntityKind::prop : EntityKind::vegetation;
        char text[160];
        if (finding.resolved)
            std::snprintf(text, sizeof(text), "%s %s is %+.1f cm off its height rule",
                          finding.subject == csf::HeightFinding::Subject::actor ? "Actor" : "Placement",
                          finding.id.c_str(), *finding.resolved - finding.position.y);
        else
            std::snprintf(text, sizeof(text), "%s %s: %s",
                          finding.subject == csf::HeightFinding::Subject::actor ? "Actor" : "Placement",
                          finding.id.c_str(), finding.problem.c_str());
        problem.message = text;
        problem.severity = finding.resolved ? Problem::Severity::warning : Problem::Severity::error;
        problem.subject = finding.subject == csf::HeightFinding::Subject::actor
                              ? ProblemSubject{ProblemSubject::Kind::actor, finding.actor_id, {}}
                              : ProblemSubject{ProblemSubject::Kind::placement, 0, finding.id};
        if (finding.resolved) {
            problem.fix_command = "mission.project_resnap";
            problem.fix_label = "Resnap all";
        }
        add(std::move(problem));
    }

    for (const auto& check : inputs.project_checks) {
        Problem problem;
        problem.severity = Problem::Severity::error;
        problem.source = "Project";
        problem.message = check;
        problem.kind = EntityKind::building;
        add(std::move(problem));
    }

    if (inputs.scene) {
        for (const auto& area : inputs.scene->areas()) {
            for (const auto& message : csf::area_polygon_problems(area.points)) {
                Problem problem;
                problem.severity = Problem::Severity::error;
                problem.source = "Zones";
                problem.message = "Zone " + area.name.value_or(std::to_string(area.id.value_or(0))) + ": " + message;
                problem.kind = EntityKind::zone;
                problem.subject = {ProblemSubject::Kind::area, area.id.value_or(0), {}};
                add(std::move(problem));
            }
        }
        if (inputs.objects)
            for (const auto& actor : inputs.scene->actors()) {
                if (!actor.class_id || !inputs.objects->find_class(*actor.class_id).empty()) continue;
                Problem problem;
                problem.severity = Problem::Severity::error;
                problem.source = "Classes";
                problem.message = "Actor " + actor.name.value_or("?") + " uses class " + std::to_string(*actor.class_id) +
                                  ", which is not in Objetos.bdd";
                problem.kind = EntityKind::unresolved;
                problem.subject = {ProblemSubject::Kind::actor, actor.id.value_or(0), {}};
                add(std::move(problem));
            }
    }

    std::ranges::stable_sort(problems, [](const Problem& a, const Problem& b) {
        return a.severity != b.severity ? a.severity < b.severity : a.source < b.source;
    });
    return problems;
}

std::size_t count_problems(const std::span<const Problem> problems, const Problem::Severity severity) {
    return static_cast<std::size_t>(
        std::ranges::count_if(problems, [&](const Problem& problem) { return problem.severity == severity; }));
}

} // namespace rwsman
