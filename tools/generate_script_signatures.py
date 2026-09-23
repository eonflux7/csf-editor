#!/usr/bin/env python3
"""Generate src/csf_script_signatures.cpp from the shipped GSC/CSC programs.

Usage:
    tools/generate_script_signatures.py [--semantics [PATH]]
                                        <unpacked-resource-root> [output.cpp]

Every instruction in .ACCIONES/.CONDICIONES is a group whose first child is the
opcode string; every operand group starts with its tag string. For each head
and role (instruction or operand) the table records the observed argument
counts and, per argument position, the shapes seen there: an operand tag, or
int/real/string/group/array for plain values. Identical files are counted once.
The editor uses the table only to warn about script text that differs from
everything the shipped game data contains.

--semantics joins the engine semantics recovered statically in the
format-reversal workspace (`tools/script_semantics.tsv`: vm+0x18 driver mode,
ScriptOrder tag, wait wrapper, registry id/handler/operand kinds) onto the
corpus rows by opcode head name, as an additional column. The corpus columns
are never touched; without the flag the output is exactly the corpus-only table
(the byte-identical baseline of KB-scripting-40).
"""

import argparse
import collections
import hashlib
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "csffbs"))
import csffbs  # noqa: E402

DEFAULT_SEMANTICS = Path(__file__).resolve().parent / "script_semantics.tsv"
HEX = re.compile(r"^0x[0-9a-fA-F]+$")


def load_semantics(path: Path) -> dict:
    """Load the engine-semantics sidecar, keyed by opcode head name.

    Columns: opcode_id category name value_type handler dispatch_case
    operand_kinds vm_mode order_tag wait confirmed evidence. `-` means the
    behaviour was confirmed to be absent, `?` not analysed; both map to the
    default value here, and `confirmed` distinguishes them.
    """
    semantics = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        columns = line.split("\t")
        if len(columns) < 12:
            raise SystemExit(f"{path}: expected 12 tab-separated columns, got {len(columns)}")
        (opcode_id, category, name, _value_type, handler, _dispatch_case,
         operand_kinds, vm_mode, order_tag, wait, confirmed, _evidence) = columns[:12]
        if not HEX.match(opcode_id):
            raise SystemExit(f"{path}: opcode_id {opcode_id!r} is not a 0x hex id")
        if name in semantics:
            raise SystemExit(f"{path}: duplicate opcode head {name!r}")
        semantics[name] = {
            "opcode_id": int(opcode_id, 16),
            "category": category,
            "handler": int(handler, 16) if HEX.match(handler) else 0,
            "operand_kinds": operand_kinds if operand_kinds != "?" else "",
            "vm_mode": int(vm_mode, 16) if HEX.match(vm_mode) else -1,
            "order_tag": int(order_tag, 16) if HEX.match(order_tag) else -1,
            "wait": wait == "yes",
            "confirmed": confirmed == "yes",
        }
    return semantics


def semantics_literal(entry: dict) -> str:
    """Render one ScriptSemantics aggregate initializer (field order as in the header)."""
    return (f'{{{entry["vm_mode"]}, {entry["order_tag"]}, '
            f'{"true" if entry["wait"] else "false"}, '
            f'0x{entry["opcode_id"]:x}, 0x{entry["handler"]:08x}, '
            f'{"true" if entry["confirmed"] else "false"}, '
            f'"{entry["operand_kinds"]}"sv}}')


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate the script signature table from shipped GSC/CSC programs.")
    parser.add_argument("root", type=Path, help="unpacked resource root")
    parser.add_argument("output", nargs="?", type=Path,
                        default=Path(__file__).resolve().parent.parent / "src" / "csf_script_signatures.cpp")
    parser.add_argument("--semantics", nargs="?", const=DEFAULT_SEMANTICS, default=None,
                        metavar="PATH",
                        help="join engine semantics from PATH (default: tools/script_semantics.tsv)")
    args = parser.parse_args()
    root = args.root
    output = args.output
    semantics_by_head = load_semantics(args.semantics) if args.semantics else None

    signatures = collections.defaultdict(
        lambda: {"arity": set(), "positions": collections.defaultdict(set), "count": 0})
    seen = set()
    files = 0
    for path in sorted(root.rglob("*")):
        if path.suffix.lower() not in (".gsc", ".csc") or not path.is_file():
            continue
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest in seen:
            continue
        seen.add(digest)
        files += 1
        document = csffbs.Document.load(str(path))
        entries = document.entries

        def label(node):
            if node.identifier_index is None:
                return ""
            return document.identifiers[node.identifier_index].data.rstrip(b"\0").decode("latin1")

        def string_value(node):
            entry = entries[node.entry_index]
            if entry.raw_type != 5:
                return None
            return document.strings[entry.raw_value_or_size].data.rstrip(b"\0").decode("latin1")

        def head(node):
            entry = entries[node.entry_index]
            if entry.raw_type != 1 or not node.children:
                return None
            return string_value(node.children[0])

        def shape(node):
            tag = head(node)
            if tag is not None:
                return tag
            return {1: "group", 2: "array", 3: "int", 4: "real", 5: "string"}[
                entries[node.entry_index].raw_type]

        def record(role, node):
            tag = head(node)
            if tag is None:
                return
            signature = signatures[(tag, role)]
            signature["count"] += 1
            arguments = node.children[1:]
            signature["arity"].add(len(arguments))
            for index, argument in enumerate(arguments):
                signature["positions"][index].add(shape(argument))
                record("operand", argument)

        def walk(node):
            if label(node) in (".ACCIONES", ".CONDICIONES"):
                for instruction in node.children:
                    record("instruction", instruction)
                return
            for child in node.children:
                walk(child)

        for top in document.roots:
            walk(top)

    def emitted(tag, role, signature):
        maximum = max(signature["arity"])
        positions = ";".join("|".join(sorted(signature["positions"][i])) for i in range(maximum))
        prefix = (f'    {{"{tag}"sv, {"true" if role == "instruction" else "false"}, '
                  f'{min(signature["arity"])}, {maximum}, {signature["count"]}, "{positions}"sv')
        if semantics_by_head is None:
            return prefix + "},"
        entry = semantics_by_head.get(tag)
        literal = semantics_literal(entry) if entry else "{}"
        return prefix + ", " + literal + "},"

    rows = [((tag, role), emitted(tag, role, signature))
            for (tag, role), signature in signatures.items()]

    joined = engine_only = unused = 0
    if semantics_by_head is not None:
        instruction_heads = {tag for (tag, role) in signatures if role == "instruction"}
        joined = sum(1 for tag, _role in signatures if tag in semantics_by_head)
        # An engine opcode the corpus never uses as an instruction still needs an
        # instruction row (`uses = 0`) so the editor can tell "registered, unused"
        # from "unknown name". Only confirmed opcodes get one: an unconfirmed row
        # would claim an id/handler is authoritative when it is not.
        for name in sorted(semantics_by_head):
            entry = semantics_by_head[name]
            if entry["confirmed"] and name not in instruction_heads:
                rows.append(((name, "instruction"),
                             f'    {{"{name}"sv, true, 0, 0, 0, ""sv, {semantics_literal(entry)}}},'))
                engine_only += 1
        unused = sum(1 for name, entry in semantics_by_head.items()
                     if entry["category"] != "0" and name not in instruction_heads)
    rows.sort(key=lambda row: row[0])

    header = ("// Generated by tools/generate_script_signatures.py from "
              f"{files} distinct shipped GSC/CSC files.\n")
    if semantics_by_head is not None:
        header += (f"// Engine semantics joined from {Path(args.semantics).name} "
                   f"({joined} corpus rows, {engine_only} engine-only rows).\n")
    header += "// Do not edit by hand; rerun the generator instead.\n"

    output.write_text(
        header +
        '#include "csf/script_signatures.hpp"\n\n'
        "namespace csf {\nnamespace {\n\nusing namespace std::string_view_literals;\n\n"
        "// clang-format off\nconstexpr ScriptSignature table[]{\n" + "\n".join(text for _, text in rows) +
        "\n};\n// clang-format on\n\n} // namespace\n\n"
        "std::span<const ScriptSignature> script_signatures() noexcept { return table; }\n\n"
        "} // namespace csf\n",
        encoding="utf-8", newline="\n")

    summary = f"{output}: {len(rows)} signatures from {files} files"
    if semantics_by_head is not None:
        summary += (f"; {joined} corpus rows joined, {engine_only} engine-only rows added, "
                    f"{unused} registry opcodes unused by the corpus")
    print(summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
