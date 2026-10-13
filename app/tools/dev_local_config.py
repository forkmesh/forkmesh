#!/usr/bin/env python3
"""Write a local-only wrangler config for running the App Worker offline.

    python3 tools/dev_local_config.py
    uvx --from workers-py pywrangler dev --config .wrangler/dev-local.toml \
        --env '' --ip 127.0.0.1 --port 8787 --persist-to .wrangler/state

Two things in the production manifest get in the way of a plain local run:

* the ``[ai]`` Workers AI binding can only be served remotely, so wrangler
  opens an authenticated remote session even for local development and
  fails outright when the Cloudflare login has expired;
* the ``[[routes]]`` custom domain makes wrangler present
  ``app.forkmesh.com`` in ``request.url`` for every local request, which
  disables the localhost-only demo login and makes cookies carry a
  ``Domain=forkmesh.com`` that a browser on localhost rejects.

The generated file is the production config minus those two blocks, with
its relative paths made absolute so it can live in the gitignored
``.wrangler/`` directory. ``.dev.vars`` (needs a DATA_KEY) is copied next to
it because wrangler reads that file from the config's own directory.
"""

from __future__ import annotations

import re
import shutil
from pathlib import Path

APP = Path(__file__).resolve().parent.parent
SOURCE = APP / "wrangler.toml"
TARGET = APP / ".wrangler" / "dev-local.toml"
DEV_VARS = APP / ".dev.vars"


def derive(text: str) -> str:
    text, removed_ai = re.subn(
        r'\n\[ai\]\nbinding = "AI"\n',
        "\n# [ai] binding removed for local dev (Workers AI needs a remote "
        "session).\n",
        text,
    )
    text, removed_routes = re.subn(
        r'\n\[\[routes\]\]\npattern = "app\.forkmesh\.com"\ncustom_domain = true\n',
        "\n# [[routes]] removed for local dev so request.url stays on "
        "localhost.\n",
        text,
    )
    if removed_ai != 1 or removed_routes != 1:
        raise SystemExit(
            "wrangler.toml no longer matches the blocks this tool removes "
            "(ai=%d, routes=%d); update tools/dev_local_config.py"
            % (removed_ai, removed_routes))
    text = text.replace(
        'main = "src_build/entry.py"', 'main = "%s/src_build/entry.py"' % APP)
    text = text.replace('directory = "./dist"', 'directory = "%s/dist"' % APP)
    text = text.replace(
        'migrations_dir = "migrations"', 'migrations_dir = "%s/migrations"' % APP)
    text = text.replace(
        'command = "python3 tools/build_dashboard_assets.py',
        'cwd = "%s"\ncommand = "python3 tools/build_dashboard_assets.py' % APP,
        1,
    )
    return text


def main() -> None:
    TARGET.parent.mkdir(parents=True, exist_ok=True)
    TARGET.write_text(derive(SOURCE.read_text(encoding="utf-8")), encoding="utf-8")
    print("wrote", TARGET.relative_to(APP))
    if DEV_VARS.is_file():
        shutil.copyfile(DEV_VARS, TARGET.parent / ".dev.vars")
        print("copied .dev.vars beside it")
    else:
        print("note: no .dev.vars; the Worker needs DATA_KEY=<any local "
              "secret> in app/.dev.vars before it can encrypt rows")


if __name__ == "__main__":
    main()
