#!/usr/bin/env python3
"""
Generates Qt/QML adapter headers from configuration headers
"""
import os
import re
import sys
from dataclasses import dataclass, field
from typing import Any

from jinja2 import Environment, FileSystemLoader, StrictUndefined

# ----------------------------
# Data model
# ----------------------------

@dataclass
class EnumEntry:
    name: str
    value: int


# an 'alternative' is one path of a variant
@dataclass
class Alternative:
    name: str        
    cpp_type: str    
    label: str       
    tag: str         
    index: int
    variant_path: str = "" 
    variant_ref: str = ""  
    leaves: list["Member"] = field(default_factory=list)


@dataclass
class Member:
    name: str            
    path: str            
    kind: str            
    cpp_type: str        
    qt_type: str         
    ref: str             
    is_rfl: bool         
    comment: str = ""
    default_value: Any = None
    # float3/quaternion initialisers, broken out into their components
    default_components: list[float] | None = None
    enum_default: int | None = None
    enum_qualified_type: str = ""
    enum_entries: list[EnumEntry] = field(default_factory=list)
    # a numeric member offers a fixed set of values through @options
    options: list[EnumEntry] = field(default_factory=list)
    min_max: tuple[str, str] | None = None
    optional: bool = False
    disabled: bool = False
    is_output: bool = False
    hidden: bool = False
    # a nested or variant member whose editor group starts folded
    folded: bool = False
    comparable: bool = False
    # stream output member of e.g. operator
    stream_label: str = ""
    # nested config members embed an adapter of this type
    adapter_type: str = ""
    # for a std::vector of structs: this is the element type,
    # so the editor can pick a delegate for it
    list_element_type: str = ""
    # the initialiser verbatim to use as a default value when its type isn't specialised
    default_expr: str = ""
    # variant members
    alternatives: list[Alternative] = field(default_factory=list)
    variant_default_index: int = 0
    # set on leaves that live inside a variant alternative
    alternative: Alternative | None = None

    @property
    def read(self) -> str:
        """C++ expression yielding the member's value."""
        return f"{self.ref}.value()" if self.is_rfl else self.ref

    @property
    def is_own(self) -> bool:
        """True for direct members, which get a Q_PROPERTY and accessors."""
        return self.alternative is None

    @property
    def variant_access(self) -> str:
        """Suffix that turns a variant member into an rfl::Variant expression."""
        return ".variant()" if TAGGED_UNION_RE.match(self.cpp_type) else ""

    @property
    def choices(self) -> list[EnumEntry]:
        """The values a dropdown offers: an enum's own, or a numeric @options."""
        return self.enum_entries or self.options

    @property
    def is_toggleable(self) -> bool:
        """True for the kinds carrying a switch alongside their value."""
        return self.kind in ("toggleable_bounds", "toggleable_radius",
                             "toggleable_int", "toggleable_float")


@dataclass
class Group:
    label: str
    paths: list[str]


@dataclass
class ParsedStruct:
    name: str
    qualified_name: str
    members: list[Member]
    needs_qvector3d: bool = False
    needs_qquaternion: bool = False


@dataclass
class StructEnum:
    entries: list[EnumEntry]
    owner: str  # qualified name of the struct the enum is declared in


@dataclass(frozen=True, slots=True)
class GeneratorArgs:
    templates_dir: str
    out_dir: str
    src_root: str
    input_headers: list[str]


KINDS = (
    "nested", "variant", "enum", "string", "bool", "int", "float", "float3",
    "quaternion", "position", "position_bounds", "length", "radius",
    "toggleable_bounds", "toggleable_radius", "toggleable_int",
    "toggleable_float", "stream",
    "opaque",
)


# ----------------------------
# regexes necessary for multiple funcs are compiled once up here
# ----------------------------

STRUCT_NAME_SUFFIXES = (
    "Configuration|Workspace|Entry|Session|SessionLayout|BindingTarget"
    "|putRoute|putMapping|putChangeDetection|ABB"
)

STRUCTS_RE = re.compile(
    rf"struct (\w+(?:{STRUCT_NAME_SUFFIXES})) {{([\s\S]*?^\}};)",
    re.MULTILINE,
)

ADAPTED_STRUCT_NAME_RE = re.compile(rf"^\w+(?:{STRUCT_NAME_SUFFIXES})$")

# template arguments may themselves be templates (rfl::Variant<A, B>), so the
# argument list only stops at a character that can't appear inside one
TYPE_PATTERN = r"([\w:]+(?:\:\:)?[\w:]+(?:<[^;{}=]*>)?)"
NAME_PATTERN = r"(\w+)"
INITIAL_VALUE_PATTERN = r"(?:\s*=\s*([^;]+)|\s*\{([^}]*)\})?"
COMMENT_PATTERN = r"(?:\s*//\s*(.*))?"

MEMBER_RE = re.compile(
    fr"{TYPE_PATTERN}\s+{NAME_PATTERN}{INITIAL_VALUE_PATTERN}\s*;{COMMENT_PATTERN}",
    re.VERBOSE,
)

MINMAX_RE = re.compile(r"@minmax\(([^)]+)\)")
OPTIONS_RE = re.compile(r"@options\(([^)]+)\)")
SUFFIX_RE = re.compile(r"@suffix\(([^)]+)\)")
OPTIONAL_RE = re.compile(r"@optional")
DISABLED_RE = re.compile(r"@disabled")
HIDDEN_RE = re.compile(r"@hidden")
FOLDED_RE = re.compile(r"@folded")
VERBATIM_RE = re.compile(
    r"^\s*((?:static|constexpr|inline|virtual|extern|using)\b.*)$",
    re.MULTILINE,
)

STANDALONE_COMMENT_RE = re.compile(r"^[ \t]*//[^\n]*$", re.MULTILINE)

# for matching `using X = Y;'
USING_ALIAS_RE = re.compile(r"[ \t]*using\s+(\w+)\s*=\s*([^;]+);[ \t]*\n?")

# for matching 'using Tag = rfl::Literal<"tag_str", ...>;'
TAG_LITERAL_RE = re.compile(r'using\s+Tag\s*=\s*rfl::Literal<\s*"([^"]*)"')

NAMESPACE_RE = re.compile(r"namespace\s+([A-Za-z_][\w:]*)\s*{")

ENUM_DECL_RE = re.compile(
    r"^\s*enum\s+class\s+(\w+)(?:\s*:\s*[\w:]+)?\s*\{([^}]*)\}\s*;\s*$",
    re.MULTILINE,
)

INNER_STRUCT_RE = re.compile(r"^[ \t]+struct\s+(\w+)\s*\{", re.MULTILINE)

INTLIKE_RE = re.compile(r"(u?)int(8|16|32|64)_t")
OPTIONAL_T_RE = re.compile(r"std::optional<\s*([^>]+)\s*>")
VARIANT_RE = re.compile(r"^(?:std|rfl)::[Vv]ariant<(.*)>$")
# a tagged union's first template argument is its discriminator, followed
# by the alternatives
TAGGED_UNION_RE = re.compile(r"^rfl::TaggedUnion<(.*)>$")

RFL_WRAPPER_RE = re.compile(r"^(?:rfl::\w+|(?:pc::)?Output)<\s*(.+)\s*>$")

OUTPUT_TYPE_RE = re.compile(r"^(?:pc::)?Output<")

TITLE_SPLIT_RE = re.compile(r"([-\s({\[<]+)")


# ----------------------------
# Naming helpers
# ----------------------------

def format_struct_name(name: str) -> str:
    name = name.replace("Configuration", "")
    name = name.replace("Operator", "")
    result: list[str] = []
    for i, char in enumerate(name):
        if i > 0 and char.isupper() and name[i - 1].islower():
            result.append(" ")
        result.append(char)
    return "".join(result)


def snake_case(name: str) -> str:
    name = name.replace("Configuration", "")
    out: list[str] = []
    for i, char in enumerate(name):
        if i > 0 and char.isupper() and (name[i - 1].islower() or name[i - 1].isdigit()):
            out.append("_")
        out.append(char.lower())
    return "".join(out)


def title_case(text: str) -> str:
    """Matches jinja's `title` filter, which the path labels used to go through."""
    # a snake_cased member name is words, not one word carrying underscores
    text = text.replace("_", " ")
    return "".join(
        part[:1].upper() + part[1:].lower()
        for part in TITLE_SPLIT_RE.split(text)
        if part
    )


def _bare_type_name(type_name: str) -> str:
    t = type_name.strip().split("<", 1)[0].strip()
    return t.rsplit("::", 1)[-1] if "::" in t else t


# ----------------------------
# Type classification
# ----------------------------

# the point cloud streams an operator can write into its own config as output
STREAM_TYPE_LABELS = {
    "PointCloudPtr": "Point Cloud",
    "VoxelisedCloudPtr": "Voxels",
    "AabbListPtr": "AABB List",
}


def stream_type_label(type_name: str) -> str:
    return STREAM_TYPE_LABELS.get(_bare_type_name(type_name), "")


def is_float3_type(type_name: str) -> bool:
    return type_name.strip() in ("pc::float3", "float3")


def is_quaternion_type(type_name: str) -> bool:
    return type_name.strip() in ("pc::quaternion", "quaternion")


def is_position_type(type_name: str) -> bool:
    return type_name.strip() in ("pc::position", "position")


def is_position_bounds_type(type_name: str) -> bool:
    return type_name.strip() in ("pc::position_bounds", "position_bounds")


def is_length_type(type_name: str) -> bool:
    return type_name.strip() in ("pc::length", "length")


def is_radius_type(type_name: str) -> bool:
    return type_name.strip() in ("pc::radius", "radius")


TOGGLEABLE_RE = re.compile(r"^(?:pc::)?Toggleable<\s*(.+?)\s*>$")


def toggleable_inner_type(type_name: str) -> str | None:
    """The type a `Toggleable<T>` puts a switch on, or None if it is not one."""
    match = TOGGLEABLE_RE.match(type_name.strip())
    return match.group(1).strip() if match else None


def is_simple_comparable_type(type_name: str) -> bool:
    t = type_name.strip()

    if t in ("bool", "int", "unsigned", "float", "double", "std::string", "QString"):
        return True

    if is_float3_type(t) or is_quaternion_type(t):
        return True

    if is_position_type(t) or is_position_bounds_type(t) or is_length_type(t):
        return True

    if is_radius_type(t):
        return True

    inner = toggleable_inner_type(t)
    if inner:
        return is_simple_comparable_type(inner)

    if INTLIKE_RE.fullmatch(t):
        return True

    m = OPTIONAL_T_RE.fullmatch(t)
    if m:
        return is_simple_comparable_type(m.group(1).strip())

    return False


def _rfl_inner_type(type_name: str) -> str | None:
    m = RFL_WRAPPER_RE.match(type_name.strip())
    return m.group(1).strip() if m else None


def _effective_type(type_name: str) -> str:
    """The underlying type, unwrapping a single rfl wrapper if present."""
    inner = _rfl_inner_type(type_name)
    return inner if inner is not None else type_name.strip()


def _resolve_aliases(type_name: str, aliases: dict[str, str]) -> str:
    """Expands `using` aliases declared in the enclosing struct."""
    resolved = type_name.strip()
    for _ in range(8):
        replaced = aliases.get(resolved)
        if replaced is None or replaced == resolved:
            return resolved
        resolved = replaced
    return resolved


def _split_template_args(args: str) -> list[str]:
    parts: list[str] = []
    depth = 0
    current: list[str] = []
    for char in args:
        if char == "<":
            depth += 1
        elif char == ">":
            depth -= 1
        if char == "," and depth == 0:
            parts.append("".join(current).strip())
            current = []
            continue
        current.append(char)
    tail = "".join(current).strip()
    if tail:
        parts.append(tail)
    return parts


def _variant_alternative_names(type_name: str) -> list[str] | None:
    t = type_name.strip()
    m = VARIANT_RE.match(t)
    if m:
        return _split_template_args(m.group(1))
    m = TAGGED_UNION_RE.match(t)
    if m:
        return _split_template_args(m.group(1))[1:]
    return None


def _is_nested_config_type(type_name: str) -> bool:
    t = _effective_type(type_name)
    if not t.endswith("Configuration"):
        return False
    if is_float3_type(t) or is_quaternion_type(t) or is_simple_comparable_type(t):
        return False
    return True


VECTOR_RE = re.compile(r"^std::vector\s*<(.+)>$")


def list_element_type(type_name: str) -> str:
    """The element type of a std::vector of structs, or "" for anything else."""
    match = VECTOR_RE.match(_effective_type(type_name).strip())
    if not match:
        return ""
    element = match.group(1).strip()
    # a vector of numbers or strings is not a list of editable rows
    if element in ("bool", "int", "unsigned", "float", "double", "std::string",
                   "std::byte", "QString") or INTLIKE_RE.fullmatch(element):
        return ""
    return element


def classify(cpp_type: str, is_enum: bool, is_variant: bool) -> tuple[str, str]:
    """Returns (kind, qt_type) for a member's rfl-unwrapped type."""
    if is_variant:
        return "variant", "int"
    if is_enum:
        return "enum", "int"
    if stream_type_label(cpp_type):
        return "stream", ""
    if _is_nested_config_type(cpp_type):
        return "nested", ""
    if is_float3_type(cpp_type):
        return "float3", "QVector3D"
    if is_quaternion_type(cpp_type):
        return "quaternion", "QQuaternion"
    if is_position_type(cpp_type):
        return "position", "QVector3D"
    if is_position_bounds_type(cpp_type):
        return "position_bounds", "QVariantMap"
    if is_length_type(cpp_type):
        return "length", "double"
    if is_radius_type(cpp_type):
        return "radius", "double"
    inner = toggleable_inner_type(cpp_type)
    if inner:
        if is_position_bounds_type(inner):
            return "toggleable_bounds", "QVariantMap"
        if is_radius_type(inner):
            return "toggleable_radius", "QVariantMap"
        if inner in ("int", "unsigned") or INTLIKE_RE.fullmatch(inner):
            return "toggleable_int", "QVariantMap"
        if inner in ("float", "double"):
            return "toggleable_float", "QVariantMap"
        return "opaque", ""
    if cpp_type in ("std::string", "QString"):
        return "string", "QString"
    if cpp_type == "bool":
        return "bool", "bool"
    if cpp_type in ("int", "unsigned") or INTLIKE_RE.fullmatch(cpp_type):
        return "int", cpp_type
    if cpp_type in ("float", "double"):
        return "float", cpp_type
    return "opaque", ""


# ----------------------------
# Value parsing
# ----------------------------

NUMERIC_SUFFIX_RE = re.compile(r"^([-+]?[0-9][0-9.eE+-]*?)([uUlLfF]+)$")


def _strip_numeric_suffix(token: str) -> str:
    """`60.0f`, `1024u` and `1ull` name the same numbers without their suffix."""
    match = NUMERIC_SUFFIX_RE.match(token.strip())
    return match.group(1) if match else token.strip()


def _try_parse_int(token: str) -> int | None:
    t = _strip_numeric_suffix(token)
    if not t:
        return None
    try:
        return int(t, 0)
    except ValueError:
        return None


def _parse_default_value(raw: str) -> Any:
    if raw is None:
        return None
    s = raw.strip()
    if not s:
        return ""

    if s in ("true", "false"):
        return s == "true"

    parsed_int = _try_parse_int(s)
    if parsed_int is not None:
        return parsed_int

    try:
        parsed_float = float(_strip_numeric_suffix(s))
    except ValueError:
        parsed_float = None

    if parsed_float is not None and not isinstance(parsed_float, bool):
        return parsed_float

    if (len(s) >= 2) and (
        (s[0] == '"' and s[-1] == '"') or (s[0] == "'" and s[-1] == "'")
    ):
        return s[1:-1]

    return s


def _parse_number_literal(token: str) -> float | None:
    try:
        return float(_strip_numeric_suffix(token))
    except ValueError:
        return None


AGGREGATE_INIT_RE = re.compile(r"^[\w:]*\s*[({](.*)[)}]$", re.DOTALL)


def _parse_vector_components(raw: str, count: int) -> list[float] | None:
    """
    Breaks an aggregate initialiser into its components.
    """
    if not raw:
        return None

    inner = raw.strip()
    match = AGGREGATE_INIT_RE.match(inner)
    if match:
        inner = match.group(1)

    tokens = [token.strip() for token in inner.split(",") if token.strip()]
    if not tokens or len(tokens) > count:
        return None

    components: list[float] = []
    for token in tokens:
        parsed = _parse_number_literal(token)
        if parsed is None:
            return None
        components.append(parsed)

    return components + [0.0] * (count - len(components))


def _parse_enum_entries(enum_body: str) -> list[EnumEntry]:
    items: list[EnumEntry] = []
    next_value = 0

    for raw_item in enum_body.split(","):
        entry = raw_item.strip()
        if not entry:
            continue

        entry = re.sub(r"//.*$", "", entry).strip()
        entry = re.sub(r"/\*.*?\*/", "", entry).strip()
        if not entry:
            continue

        if "=" in entry:
            name_part, value_part = entry.split("=", 1)
            explicit_value = _try_parse_int(value_part)
            if explicit_value is None:
                continue
            items.append(EnumEntry(name=name_part.strip(), value=explicit_value))
            next_value = explicit_value + 1
        else:
            items.append(EnumEntry(name=entry, value=next_value))
            next_value += 1

    return items


def _enum_default_to_int(default_value: Any, entries: list[EnumEntry]) -> int | None:
    if isinstance(default_value, int) and not isinstance(default_value, bool):
        return default_value

    text = str(default_value or "").strip()
    if text:
        # `DepthMode::Narrow`, `Config::DepthMode::Narrow` and a bare `Narrow`
        # all identify the enumerator by its trailing name
        enumerator = text.rsplit("::", 1)[-1].strip()
        for entry in entries:
            if entry.name == enumerator:
                return entry.value

    return entries[0].value if entries else None


def _parse_options(comment: str) -> list[EnumEntry]:
    """`@options(15, 20, 30) @suffix(Hz)` puts a numeric member in a dropdown."""
    match = OPTIONS_RE.search(comment)
    if not match:
        return []

    suffix = SUFFIX_RE.search(comment)
    unit = f" {suffix.group(1).strip()}" if suffix else ""

    entries: list[EnumEntry] = []
    for token in match.group(1).split(","):
        value = _try_parse_int(token)
        if value is not None:
            entries.append(EnumEntry(name=f"{value}{unit}", value=value))
    return entries


def _parse_min_max(comment: str) -> tuple[str, str] | None:
    match = MINMAX_RE.search(comment)
    if not match:
        return None
    parts = [p.strip() for p in match.group(1).split(",")]
    minimum = parts[0] if parts and parts[0] else "0"
    maximum = parts[1] if len(parts) >= 2 and parts[1] else minimum
    return minimum, maximum


# ----------------------------
# Struct body extraction
# ----------------------------

def _extract_nested_enums(struct_body: str, owner: str) -> tuple[dict[str, StructEnum], str]:
    enum_map: dict[str, StructEnum] = {}

    def _strip_and_collect(match: re.Match) -> str:
        enum_map[match.group(1)] = StructEnum(
            entries=_parse_enum_entries(match.group(2)), owner=owner
        )
        return ""

    return enum_map, ENUM_DECL_RE.sub(_strip_and_collect, struct_body)


def _extract_using_aliases(struct_body: str) -> tuple[dict[str, str], str]:
    aliases: dict[str, str] = {}

    def _strip_and_collect(match: re.Match) -> str:
        aliases[match.group(1)] = " ".join(match.group(2).split())
        return ""

    return aliases, USING_ALIAS_RE.sub(_strip_and_collect, struct_body)


def _extract_inner_structs(struct_body: str) -> tuple[list[tuple[str, str]], str]:
    inner: list[tuple[str, str]] = []
    out: list[str] = []
    cursor = 0

    while True:
        match = INNER_STRUCT_RE.search(struct_body, cursor)
        if not match:
            break

        # walk from the opening brace to its match
        depth = 0
        end = None
        for i in range(match.end() - 1, len(struct_body)):
            char = struct_body[i]
            if char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    end = i
                    break
        if end is None:
            break

        inner.append((match.group(1), struct_body[match.end():end]))
        out.append(struct_body[cursor:match.start()])

        # swallow the trailing '};' and its newline
        after = end + 1
        while after < len(struct_body) and struct_body[after] in ";\r\n\t ":
            if struct_body[after] == "\n":
                after += 1
                break
            after += 1
        cursor = after

    out.append(struct_body[cursor:])
    return inner, "".join(out)


# ----------------------------
# Member parsing
# ----------------------------

@dataclass
class ParseContext:
    body: str = ""
    enums: dict[str, StructEnum] = field(default_factory=dict)
    aliases: dict[str, str] = field(default_factory=dict)
    # the struct's own Tag literal, if it declares one
    tag: str = ""
    # inner struct name -> (qualified name, its own context)
    inner: dict[str, tuple[str, "ParseContext"]] = field(default_factory=dict)


def build_context(
    struct_body: str, qualified_name: str, inherited: ParseContext | None = None
) -> ParseContext:
    """
    Strips the declarations that aren't members out of a struct body and
    records what they mean, inheriting whatever the enclosing struct declared.
    """
    context = ParseContext(
        enums=dict(inherited.enums) if inherited else {},
        aliases=dict(inherited.aliases) if inherited else {},
    )

    own_aliases, body = _extract_using_aliases(struct_body)
    context.aliases.update(own_aliases)

    own_enums, body = _extract_nested_enums(body, qualified_name)
    context.enums.update(own_enums)

    inner_tags = {
        inner_name: match.group(1)
        for inner_name, raw_body in _extract_inner_structs(struct_body)[0]
        if (match := TAG_LITERAL_RE.search(raw_body))
    }

    inner_declarations, body = _extract_inner_structs(body)
    for inner_name, inner_body in inner_declarations:
        if not ADAPTED_STRUCT_NAME_RE.match(inner_name):
            continue
        inner_qualified = f"{qualified_name}::{inner_name}"
        inner_context = build_context(inner_body, inner_qualified, context)
        inner_context.tag = inner_tags.get(inner_name, "")
        context.inner[inner_name] = (inner_qualified, inner_context)

    context.body = STANDALONE_COMMENT_RE.sub("", VERBATIM_RE.sub("", body))
    return context


def _parse_members(
    struct_name: str,
    members_body: str,
    context: ParseContext,
    path_prefix: str = "",
    ref_prefix: str = "m_config.",
    alternative: Alternative | None = None,
) -> tuple[list[Member], bool, bool]:
    needs_qvector3d = False
    needs_qquaternion = False
    members: list[Member] = []

    for raw_type, raw_name, init_eq, init_brace, raw_comment in MEMBER_RE.findall(members_body):
        raw_name = raw_name.strip()
        raw_type = " ".join(raw_type.split())

        init_raw = ((init_eq or "").strip() or (init_brace or "").strip() or "")
        default_value = _parse_default_value(
            init_raw.replace("{", "").replace("}", "").strip()
        )

        comment = raw_comment.strip() if raw_comment else ""

        is_rfl = _rfl_inner_type(raw_type) is not None
        is_output = bool(OUTPUT_TYPE_RE.match(raw_type.strip()))
        cpp_type = _resolve_aliases(_effective_type(raw_type), context.aliases)

        # enums declared by this struct or one enclosing it
        enum_name = cpp_type.replace(f"{struct_name}::", "").strip()
        struct_enum = context.enums.get(enum_name)
        enum_entries = struct_enum.entries if struct_enum else []
        enum_qualified_type = f"{struct_enum.owner}::{enum_name}" if struct_enum else ""
        if "BackendType" in enum_name and not struct_enum:
            enum_entries = [EnumEntry("CPU", 0), EnumEntry("CUDA", 1)]
            enum_qualified_type = "BackendType"

        alternative_names = _variant_alternative_names(cpp_type) or []
        kind, qt_type = classify(cpp_type, bool(enum_entries), bool(alternative_names))

        # an output holding something the ui has no control for
        if is_output and kind == "opaque":
            continue

        default_components: list[float] | None = None
        if kind == "float3":
            needs_qvector3d = True
            default_components = _parse_vector_components(init_raw, 3)
        if kind == "quaternion":
            needs_qquaternion = True
            default_components = _parse_vector_components(init_raw, 4)
        if kind in ("position", "position_bounds", "toggleable_bounds"):
            needs_qvector3d = True
        if kind in ("length", "radius"):
            components = _parse_vector_components(init_raw, 1)
            if components:
                default_value = components[0]

        ref = f"{ref_prefix}{raw_name}"
        member = Member(
            name=raw_name,
            path=f"{path_prefix}{raw_name}",
            kind=kind,
            cpp_type=cpp_type,
            qt_type=qt_type,
            ref=ref,
            is_rfl=is_rfl,
            comment=comment,
            default_value=default_value,
            default_expr=" ".join(init_raw.split()),
            default_components=default_components,
            enum_default=_enum_default_to_int(default_value, enum_entries)
            if enum_entries else None,
            enum_qualified_type=enum_qualified_type,
            enum_entries=enum_entries,
            options=_parse_options(comment),
            min_max=_parse_min_max(comment),
            optional=bool(OPTIONAL_RE.search(comment)),
            disabled=bool(DISABLED_RE.search(comment)) or is_output,
            is_output=is_output,
            hidden=bool(HIDDEN_RE.search(comment)),
            folded=bool(FOLDED_RE.search(comment)),
            comparable=is_simple_comparable_type(cpp_type),
            stream_label=stream_type_label(cpp_type) if kind == "stream" else "",
            adapter_type=f"{cpp_type}Adapter" if kind == "nested" else "",
            list_element_type=list_element_type(cpp_type),
            alternative=alternative,
        )
        members.append(member)

        if kind != "variant":
            continue

        # expand the variant: every alternative's fields get their own paths,
        # read through a pointer to that alternative
        variant_ref = f"{member.read}{member.variant_access}"
        for index, alternative_name in enumerate(alternative_names):
            simple = _bare_type_name(alternative_name)
            qualified, inner_context = context.inner.get(
                simple, (alternative_name, ParseContext())
            )
            alt = Alternative(
                name=simple,
                cpp_type=qualified,
                label=format_struct_name(simple),
                tag=inner_context.tag or snake_case(simple),
                index=index,
                variant_path=member.path,
                variant_ref=variant_ref,
            )
            alt.leaves, alt_v3, alt_quat = _parse_members(
                simple,
                inner_context.body,
                inner_context,
                path_prefix=f"{member.path}/{alt.tag}/",
                ref_prefix="alt->",
                alternative=alt,
            )
            needs_qvector3d = needs_qvector3d or alt_v3
            needs_qquaternion = needs_qquaternion or alt_quat
            member.alternatives.append(alt)
            members.extend(alt.leaves)

        for alt in member.alternatives:
            if alt.name == str(default_value).strip():
                member.variant_default_index = alt.index
                break

    return members, needs_qvector3d, needs_qquaternion


def parse_struct(
    struct_name: str,
    struct_body: str,
    enclosing_qualified_name: str = "",
    inherited: ParseContext | None = None,
) -> ParsedStruct:
    """
    Parses a struct into its members...
    """
    qualified_name = (
        f"{enclosing_qualified_name}::{struct_name}"
        if enclosing_qualified_name
        else struct_name
    )

    context = build_context(struct_body, qualified_name, inherited)

    members, needs_qvector3d, needs_qquaternion = _parse_members(
        struct_name, context.body, context
    )

    return ParsedStruct(
        name=struct_name,
        qualified_name=qualified_name,
        members=members,
        needs_qvector3d=needs_qvector3d,
        needs_qquaternion=needs_qquaternion,
    )


def parse_header(input_text: str) -> list[ParsedStruct]:
    return [
        parse_struct(struct_name, struct_body)
        for struct_name, struct_body in STRUCTS_RE.findall(input_text)
    ]


# ----------------------------
# Path layout
# ----------------------------

def flatten_paths(
    members: list[Member], struct_members_map: dict[str, list[Member]]
) -> list[str]:
    def _expand(member: Member, prefix: str, visiting: set[str]) -> list[str]:
        # a variant alternative's leaf already knows where it sits inside its
        # own struct ("compression/meshopt/level"), so only the prefix is added
        if member.alternative is not None:
            return [f"{prefix}{member.path}"]
        path = f"{prefix}{member.name}"
        nested_struct = _bare_type_name(member.cpp_type)
        if member.kind != "nested" or nested_struct in visiting:
            return [path]
        if nested_struct not in struct_members_map:
            return [path]

        visiting.add(nested_struct)
        out: list[str] = []
        for nested_member in struct_members_map[nested_struct]:
            out.extend(_expand(nested_member, f"{path}/", visiting))
        visiting.remove(nested_struct)
        return out

    paths: list[str] = []
    for member in members:
        if member.alternative is not None:
            # alternative leaves already carry their full path
            paths.append(member.path)
        else:
            paths.extend(_expand(member, "", set()))
    return paths


def build_groups(
    struct_label: str,
    members: list[Member],
    paths: list[str],
    struct_members_map: dict[str, list[Member]] | None = None,
) -> tuple[list[Group], dict[str, str], list[str]]:
    # a list of structs is a group of its own, so it is left out of the
    # struct's scalars and appears where it was declared instead
    list_paths = {m.path for m in members if m.list_element_type}
    top_level = [p for p in paths if "/" not in p and p not in list_paths]
    groups: list[Group] = [Group(label=f"{struct_label} Properties", paths=top_level)]
    parent_names = {path: f"{struct_label} Properties" for path in top_level}
    folded_paths: list[str] = []

    for member in members:
        if member.list_element_type:
            label = title_case(member.name)
            groups.append(Group(label=label, paths=[member.path]))
            parent_names[member.path] = label
            if member.folded:
                folded_paths.append(member.path)
        elif member.kind == "nested":
            prefix = f"{member.name}/"
            nested_paths = [p for p in paths if p.startswith(prefix)]
            # a variant inside the nested configuration still wants a group per
            # alternative, or the editor cannot tell which fields belong to the
            # alternative currently held
            nested_struct = _bare_type_name(member.cpp_type)
            alternative_groups: list[Group] = []
            for nested_member in (struct_members_map or {}).get(nested_struct, []):
                if nested_member.kind != "variant":
                    continue
                for alt in nested_member.alternatives:
                    alt_prefix = f"{prefix}{nested_member.path}/{alt.tag}/"
                    alt_paths = [p for p in paths if p.startswith(alt_prefix)]
                    if not alt_paths:
                        continue
                    alternative_groups.append(
                        Group(label=alt.label, paths=alt_paths)
                    )
                    parent_names.update({p: alt.label for p in alt_paths})

            claimed = {p for group in alternative_groups for p in group.paths}
            own_paths = [p for p in nested_paths if p not in claimed]
            groups.append(Group(label=title_case(member.name), paths=own_paths))
            # a configuration nested inside another one names itself after the
            # segment it sits under, however deep that is
            parent_names.update(
                {p: title_case(p.split("/")[-2]) for p in own_paths}
            )
            groups.extend(alternative_groups)
            if member.folded:
                folded_paths.extend(nested_paths)
        elif member.kind == "variant":
            for alt in member.alternatives:
                prefix = f"{member.path}/{alt.tag}/"
                alt_paths = [p for p in paths if p.startswith(prefix)]
                groups.append(Group(label=alt.label, paths=alt_paths))
                parent_names.update({p: alt.label for p in alt_paths})
                if member.folded:
                    folded_paths.extend(alt_paths)

    return groups, parent_names, folded_paths


# ----------------------------
# Path helpers
# ----------------------------

def _rel_header_path(file_path: str, src_root: str) -> str:
    rel = os.path.relpath(
        os.path.abspath(file_path), os.path.abspath(src_root)
    ).replace("\\", "/")
    return rel[len("src/"):] if rel.startswith("src/") else rel


def _adapter_stem(rel_header: str) -> str:
    """
    'camera/camera_config.h' -> 'camera_config'
    'plugins/devices/orbbec/orbbec_device_config.h' -> 'orbbec_device'
    """
    base_no_ext = re.sub(r"\.h$", "", os.path.basename(rel_header))
    if base_no_ext.endswith("_device_config"):
        return base_no_ext[: -len("_config")]
    return base_no_ext


def _generated_adapter_include_for_header_rel(rel_header: str) -> str:
    return os.path.join(
        os.path.dirname(rel_header), f"{_adapter_stem(rel_header)}_adapter.gen.h"
    ).replace("\\", "/")


# ----------------------------
# Main header generation
# ----------------------------

def process_cpp_header(
    input_text: str,
    file_path: str,
    env: Environment,
    src_root: str,
    struct_to_adapter_include: dict[str, str],
    struct_members_map: dict[str, list[Member]],
) -> str:
    parsed_structs = parse_header(input_text)

    ns_match = NAMESPACE_RE.search(input_text)
    namespace_name = ns_match.group(1) if ns_match else ""
    is_device_namespace = namespace_name == "pc::devices"

    needs_qvector3d = False
    needs_qquaternion = False
    rendered_structs: list[dict[str, Any]] = []

    for parsed in parsed_structs:
        members = parsed.members
        needs_qvector3d = needs_qvector3d or parsed.needs_qvector3d
        needs_qquaternion = needs_qquaternion or parsed.needs_qquaternion

        adapter_class_base = parsed.name
        if is_device_namespace and parsed.name.endswith("DeviceConfiguration"):
            adapter_class_base = parsed.name[: -len("Configuration")]

        nested_adapter_includes: list[str] = []
        for m in members:
            if m.kind != "nested":
                continue
            include = struct_to_adapter_include.get(_bare_type_name(m.cpp_type))
            if include and include not in nested_adapter_includes:
                nested_adapter_includes.append(include)

        label = format_struct_name(parsed.name)
        paths = flatten_paths(members, struct_members_map)
        groups, parent_names, folded_paths = build_groups(
            label, members, paths, struct_members_map
        )

        rendered_structs.append(
            {
                "struct_name": parsed.qualified_name,
                "adapter_name": f"{adapter_class_base}Adapter",
                "label": label,
                "members": members,
                "variants": [m for m in members if m.kind == "variant"],
                "any_variants": any(m.kind in ("variant") for m in members),
                "any_enums": any(
                    m.kind in ("enum", "variant", "nested") or m.options
                    for m in members
                ),
                "any_lists": any(
                    m.list_element_type or m.kind == "nested" for m in members
                ),
                "nested_adapter_includes": nested_adapter_includes,
                "paths": paths,
                "groups": groups,
                "parent_names": parent_names,
                "folded_paths": folded_paths,
            }
        )

    template_name = (
        "device_adapter_impl.h.j2" if is_device_namespace
        else "config_adapter_impl.h.j2"
    )

    return env.get_template(template_name).render(
        header_include_path=_rel_header_path(file_path, src_root),
        namespace_name=namespace_name,
        needs_qvector3d=needs_qvector3d,
        needs_qquaternion=needs_qquaternion,
        structs=rendered_structs,
    )


# ----------------------------
# entrypoint
# ----------------------------

def _parse_args(argv: list[str]) -> GeneratorArgs:
    templates_dir: str | None = None
    out_dir: str | None = None
    src_root: str | None = None
    input_headers: list[str] = []

    it = iter(argv)
    for tok in it:
        if tok in ("--templates-dir", "-t"):
            templates_dir = next(it, None)
        elif tok in ("--out-dir", "-o"):
            out_dir = next(it, None)
        elif tok == "--src-root":
            src_root = next(it, None)
        else:
            input_headers.append(tok)

    if not templates_dir or not out_dir or not src_root:
        print(
            "Usage: python generate-qt-adapters.py "
            "--templates-dir <dir> --out-dir <dir> --src-root <dir> <file1> <file2> ...",
            file=sys.stderr,
        )
        raise SystemExit(2)

    if not input_headers:
        print("error: no input headers provided", file=sys.stderr)
        raise SystemExit(2)

    return GeneratorArgs(
        templates_dir=os.path.abspath(templates_dir),
        out_dir=os.path.abspath(out_dir),
        src_root=os.path.abspath(src_root),
        input_headers=input_headers,
    )


def main() -> int:
    args = _parse_args(sys.argv[1:])

    env = Environment(
        loader=FileSystemLoader(args.templates_dir),
        undefined=StrictUndefined,
        autoescape=False,
        trim_blocks=True,
        lstrip_blocks=True,
    )

    # ---------- Pass 1: scan headers, cache text, build maps ----------
    header_cache: dict[str, str] = {}
    struct_to_adapter_include: dict[str, str] = {}
    struct_members_map: dict[str, list[Member]] = {}

    for file_name in sorted(args.input_headers):
        with open(file_name, "r", encoding="utf-8") as input_file:
            file_content = input_file.read()
        header_cache[file_name] = file_content

        adapter_include = _generated_adapter_include_for_header_rel(
            _rel_header_path(file_name, args.src_root)
        )
        for parsed in parse_header(file_content):
            struct_to_adapter_include.setdefault(parsed.name, adapter_include)
            struct_members_map.setdefault(parsed.name, parsed.members)

    # ---------- Pass 2: render + write ----------
    for file_name in sorted(args.input_headers):
        generated_content = process_cpp_header(
            header_cache[file_name],
            file_name,
            env,
            args.src_root,
            struct_to_adapter_include,
            struct_members_map,
        )

        rel = _rel_header_path(file_name, args.src_root)
        generated_file_name = os.path.join(
            args.out_dir, os.path.dirname(rel), f"{_adapter_stem(rel)}_adapter.gen.h"
        )
        os.makedirs(os.path.dirname(generated_file_name), exist_ok=True)

        with open(generated_file_name, "w", encoding="utf-8") as output_file:
            output_file.write(generated_content)

        print("--", generated_file_name)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
