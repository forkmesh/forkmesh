#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
DOCS_ROOT = REPOSITORY_ROOT / "www" / "docs"
DEPENDENCIES = REPOSITORY_ROOT / "marketplace" / "packages" / "docs" / "data" / "dependencies.json"
BUNDLE = REPOSITORY_ROOT / "native" / "ios" / "ForkMesh" / "Resources" / "DocsLibrary.json"
SCHEMA = "forkmesh-docs-library-v1"
WEB_ORIGIN = "https://forkmesh.com"
SOURCE_TREE = "https://app.forkmesh.com/forkmesh/forkmesh/tree/main/"
DEPENDENCIES_AREA = {
    "key": "dependencies",
    "label": "Dependencies",
    "blurb": "The open-source projects ForkMesh builds on, their GitHub repositories and top authors.",
}

APP_TOOLS = REPOSITORY_ROOT / "app" / "tools"
if str(APP_TOOLS) not in sys.path:
    sys.path.insert(0, str(APP_TOOLS))


def bundle(docs_root: Path = DOCS_ROOT, dependencies: Path = DEPENDENCIES) -> dict:
    import docs_site

    catalog = docs_site.load_catalog(docs_root)
    pages = []
    for entry in catalog.entries:
        if entry.kind not in {"doc", "asset"} or not entry.source.startswith("www/docs/"):
            continue
        path = entry.source.removeprefix("www/docs/")
        text = (docs_root / path).read_text(encoding="utf-8")
        if entry.kind == "asset":
            text = f"# {entry.title}\n\n{entry.description}\n\n```json\n{text.strip()}\n```\n"
        pages.append({
            "path": path,
            "title": entry.title,
            "description": entry.description,
            "area": entry.area,
            "url": WEB_ORIGIN + entry.url,
            "asset": entry.kind == "asset",
            "markdown": text,
        })
    snapshot = json.loads(dependencies.read_text(encoding="utf-8")) if dependencies.is_file() else {}
    areas = [
        {"key": area.key, "label": area.label, "blurb": area.blurb}
        for area in docs_site.AREAS
        if area.key != "product"
    ]
    return {
        "schema": SCHEMA,
        "generator": "marketplace/tools/docs_mobile_bundle.py",
        "webOrigin": WEB_ORIGIN,
        "sourceTree": SOURCE_TREE,
        "areas": areas + [DEPENDENCIES_AREA],
        "pages": pages,
        "dependenciesGenerated": snapshot.get("generated", ""),
        "dependencies": snapshot.get("dependencies", []),
    }


def render(payload: dict) -> str:
    return json.dumps(payload, indent=1, ensure_ascii=False) + "\n"


def write(path: Path = BUNDLE) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(render(bundle()), encoding="utf-8")
    temporary.replace(path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Write the Docs library bundle the iPhone and Android apps ship: every page of "
                    "www/docs with the web library's titles, areas and order, and the dependency snapshot.")
    parser.add_argument("--output", type=Path, default=BUNDLE, help="bundle to write")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 if the bundle is out of date; write nothing")
    args = parser.parse_args(argv)
    if args.check:
        current = args.output.read_text(encoding="utf-8") if args.output.is_file() else ""
        if current != render(bundle()):
            print(f"{args.output} is out of date; run python3 marketplace/tools/docs_mobile_bundle.py")
            return 1
        return 0
    write(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
