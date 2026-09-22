#include "rwsman/diagnostics.hpp"

#include <algorithm>
#include <cctype>
#include <numeric>

namespace rwsman {
namespace {

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

std::string file_name(const std::filesystem::path& path) {
    const auto value = path.filename().generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

DiagnosticSeverity severity_of(const csf::Diagnostic::Severity severity) {
    return severity == csf::Diagnostic::Severity::error ? DiagnosticSeverity::error
                                                        : DiagnosticSeverity::warning;
}

DiagnosticSeverity severity_of(const csf::MissionDiagnostic::Severity severity) {
    switch (severity) {
    case csf::MissionDiagnostic::Severity::note:
        return DiagnosticSeverity::note;
    case csf::MissionDiagnostic::Severity::warning:
        return DiagnosticSeverity::warning;
    case csf::MissionDiagnostic::Severity::error:
        return DiagnosticSeverity::error;
    }
    return DiagnosticSeverity::warning;
}

// Typed diagnostics point at a CSFFBS entry. Map it to the closest thing the
// GUI can select: a mission record in the scene, or the script that contains it.
SelectionRef target_for(const DiagnosticInputs& inputs, const csf::CsfSourceId& source) {
    if (inputs.scene && source.file == inputs.scene->source_path())
        return SelectionRef::mission_entry(source.entry_index);
    if (inputs.programs)
        for (std::size_t document = 0; document < inputs.programs->size(); ++document) {
            const auto& [path, program] = (*inputs.programs)[document];
            if (path != source.file) continue;
            std::optional<std::size_t> owner;
            for (std::size_t script = 0; script < program.scripts().size(); ++script)
                if (program.scripts()[script].source.entry_index <= source.entry_index)
                    owner = script;
            if (owner) return SelectionRef::program_script(document, *owner);
        }
    return {};
}

void add_typed(std::vector<DiagnosticRow>& rows, const DiagnosticInputs& inputs,
               const std::vector<csf::TypedDiagnostic>& diagnostics, const std::string& source) {
    for (const auto& diagnostic : diagnostics) {
        DiagnosticRow row;
        row.severity = severity_of(diagnostic.severity);
        row.source = source;
        row.file = file_name(diagnostic.source.file);
        row.entry = diagnostic.source.entry_index;
        row.offset = diagnostic.source.range.offset;
        row.code = diagnostic.code;
        row.message = diagnostic.message;
        row.target = target_for(inputs, diagnostic.source);
        rows.push_back(std::move(row));
    }
}

} // namespace

const char* diagnostic_severity_name(const DiagnosticSeverity severity) noexcept {
    switch (severity) {
    case DiagnosticSeverity::note:
        return "note";
    case DiagnosticSeverity::warning:
        return "warning";
    case DiagnosticSeverity::error:
        return "error";
    }
    return "warning";
}

std::vector<DiagnosticRow> collect_diagnostics(const DiagnosticInputs& inputs) {
    std::vector<DiagnosticRow> rows;
    if (inputs.document)
        for (const auto& diagnostic : inputs.document->diagnostics()) {
            DiagnosticRow row;
            row.severity = diagnostic.severity == rws::Diagnostic::Severity::error
                               ? DiagnosticSeverity::error
                               : DiagnosticSeverity::warning;
            row.source = "RWS";
            row.file = file_name(inputs.document->source_path());
            row.offset = diagnostic.offset;
            row.message = diagnostic.message;
            row.target = SelectionRef::chunk(diagnostic.offset);
            rows.push_back(std::move(row));
        }
    if (inputs.scene_document)
        for (const auto& diagnostic : inputs.scene_document->diagnostics()) {
            DiagnosticRow row;
            row.severity = severity_of(diagnostic.severity);
            row.source = "CSFFBS";
            row.file = file_name(inputs.scene_document->source_path());
            row.entry = diagnostic.entry_index;
            row.offset = diagnostic.offset;
            row.message = diagnostic.message;
            if (diagnostic.entry_index && inputs.scene)
                row.target = SelectionRef::mission_entry(*diagnostic.entry_index);
            rows.push_back(std::move(row));
        }
    if (inputs.scene) add_typed(rows, inputs, inputs.scene->diagnostics(), "Mission");
    if (inputs.objects) add_typed(rows, inputs, inputs.objects->diagnostics(), "Objects");
    if (inputs.animations) add_typed(rows, inputs, inputs.animations->diagnostics(), "Animations");
    if (inputs.programs)
        for (const auto& [path, program] : *inputs.programs) {
            (void)path;
            add_typed(rows, inputs, program.diagnostics(), "Scripts");
        }
    if (inputs.graph)
        for (const auto& diagnostic : inputs.graph->diagnostics()) {
            DiagnosticRow row;
            row.severity = severity_of(diagnostic.severity);
            row.source = "Resources";
            if (diagnostic.source) {
                row.file = file_name(diagnostic.source->file);
                row.offset = diagnostic.source->offset;
            }
            row.code = diagnostic.code;
            row.message = diagnostic.message;
            rows.push_back(std::move(row));
        }
    if (inputs.associations)
        for (const auto& association : *inputs.associations)
            for (const auto& message : association.diagnostics) {
                DiagnosticRow row;
                row.severity = DiagnosticSeverity::warning;
                row.source = "Actors";
                row.file = inputs.scene ? file_name(inputs.scene->source_path()) : std::string{};
                row.entry = association.actor.entry_index;
                row.offset = association.actor.range.offset;
                row.message = message;
                row.target = SelectionRef::mission_entry(association.actor.entry_index);
                rows.push_back(std::move(row));
            }
    if (inputs.extra) rows.insert(rows.end(), inputs.extra->begin(), inputs.extra->end());
    return rows;
}

std::vector<std::size_t> select_diagnostics(const std::vector<DiagnosticRow>& rows,
                                            const DiagnosticFilter& filter,
                                            const DiagnosticColumn column, const bool ascending) {
    const auto needle = lower(filter.text);
    std::vector<std::size_t> selected;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if (row.severity == DiagnosticSeverity::note && !filter.notes) continue;
        if (row.severity == DiagnosticSeverity::warning && !filter.warnings) continue;
        if (row.severity == DiagnosticSeverity::error && !filter.errors) continue;
        if (!filter.source.empty() && row.source != filter.source) continue;
        if (!needle.empty() &&
            lower(row.source + ' ' + row.file + ' ' + row.code + ' ' + row.message).find(needle) ==
                std::string::npos)
            continue;
        selected.push_back(i);
    }
    const auto key_less = [&](const std::size_t a, const std::size_t b) {
        const auto& x = rows[a];
        const auto& y = rows[b];
        switch (column) {
        case DiagnosticColumn::severity:
            return x.severity > y.severity; // Errors first when ascending.
        case DiagnosticColumn::source:
            return x.source < y.source;
        case DiagnosticColumn::file:
            return x.file < y.file;
        case DiagnosticColumn::entry:
            return x.entry.value_or(~0U) < y.entry.value_or(~0U);
        case DiagnosticColumn::offset:
            return x.offset.value_or(~0ULL) < y.offset.value_or(~0ULL);
        case DiagnosticColumn::message:
            return x.message < y.message;
        }
        return false;
    };
    std::ranges::stable_sort(selected, [&](const std::size_t a, const std::size_t b) {
        return ascending ? key_less(a, b) : key_less(b, a);
    });
    return selected;
}

} // namespace rwsman
