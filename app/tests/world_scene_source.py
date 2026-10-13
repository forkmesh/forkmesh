import re
from pathlib import Path

WORLD_PUBLIC = Path(__file__).resolve().parents[2] / "world" / "public" / "world"
WORLD_SCENE_FACADE = WORLD_PUBLIC / "world-scene.js"
WORLD_SCENE_DIR = WORLD_PUBLIC / "scene"

READING_ORDER = (
    "canvas.js",
    "avatar-shared.js",
    "avatar-outfits.js",
    "layout.js",
    "object-utils.js",
    "settled-matrices.js",
    "shader-warmup.js",
    "gameplay-constants.js",
    "quality.js",
    "agent-bots.js",
    "movement.js",
    "avatars.js",
    "environment.js",
    "avatar-badges.js",
    "avatar-controls.js",
    "repositories.js",
    "town.js",
    "leaderboards.js",
    "office-marketing.js",
    "office-building.js",
    "boards-tasks.js",
    "batching.js",
    "materials.js",
    "geometry-dedupe.js",
    "avatar-instancing.js",
    "labels.js",
    "members-circle.js",
    "arrival.js",
    "member-engagement.js",
    "mirror-nodes.js",
    "office-infrastructure.js",
    "treasury.js",
    "sol-fountain.js",
    "landmarks.js",
    "boards.js",
    "office-aquarium.js",
    "weather.js",
)


def core_install_order():
    core = WORLD_SCENE_DIR / "core.js"
    if not core.is_file():
        return []
    text = core.read_text(encoding="utf-8")
    modules = dict(re.findall(r'^import \* as (\w+) from "\./([\w/-]+\.js)";', text, re.M))
    order = []
    for alias in re.findall(r"\b(\w+)\.(?:functions|build)\(", text):
        module = modules.get(alias)
        if module and module not in order:
            order.append(module)
    return order


def world_scene_files():
    present = {
        path.relative_to(WORLD_SCENE_DIR).as_posix(): path
        for path in WORLD_SCENE_DIR.rglob("*.js")
    }
    ordered = [present.pop(name) for name in READING_ORDER if name in present]
    core = present.pop("core.js", None)
    ordered += [present.pop(name) for name in core_install_order() if name in present]
    ordered += [present[name] for name in sorted(present)]
    if core is not None:
        ordered.append(core)
    return [WORLD_SCENE_FACADE, *ordered]


SCENE_CONTEXT_PREFIX = re.compile(r"(?<![\w$])(?:ctx|fns)\.(?=[A-Za-z_$])")


def without_scene_context(source):
    def strip(match):
        before = source[max(0, match.start() - 3) : match.start()]
        if before.endswith(".") and before != "...":
            return match.group(0)
        return ""

    return SCENE_CONTEXT_PREFIX.sub(strip, source)


def call_end(source, start):
    depth = 0
    quote = ""
    index = start
    while index < len(source):
        char = source[index]
        if quote:
            if char == "\\":
                index += 2
                continue
            if char == quote:
                quote = ""
        elif char in "\"'`":
            quote = char
        elif char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
            if depth == 0:
                return index
        index += 1
    return -1


RESOURCE_CALL = re.compile(r"resources\.(primitive|uniqueMaterial)\(\s*THREE,\s*")
TRAILING_TYPE = re.compile(r',\s*"(\w+Material)"\s*$')


def as_constructions(source):
    out = []
    at = 0
    for match in RESOURCE_CALL.finditer(source):
        if match.start() < at:
            continue
        end = call_end(source, source.index("(", match.start()))
        if end < 0:
            continue
        inner = source[match.end():end]
        if match.group(1) == "primitive":
            kind = re.match(r'"(\w+Geometry)"\s*,?\s*', inner)
            if not kind:
                continue
            text = f"new THREE.{kind.group(1)}({inner[kind.end():].rstrip()})"
        else:
            typed = TRAILING_TYPE.search(inner)
            if not typed:
                continue
            text = f"new THREE.{typed.group(1)}({inner[:typed.start()]})"
        out.append(source[at:match.start()])
        out.append(text)
        at = end + 1
    out.append(source[at:])
    return "".join(out)


def world_scene_source():
    return as_constructions(without_scene_context(
        "\n".join(
            path.read_text(encoding="utf-8") for path in world_scene_files()
        )
    ))


def world_scene_module(name):
    return (WORLD_SCENE_DIR / name).read_text(encoding="utf-8")
