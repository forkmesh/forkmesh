"""Composition of the dashboard behaviour script from JS fragments.

The dashboard's client script used to be one ~5450-line ``public/dashboard.js``.
It is now split into ordered fragment files under ``public/dashboard/js/`` that
``tools/build_dashboard_assets.py`` concatenates into a single static
``public/dashboard.js`` before deploy. This mirrors how the HTML chrome is split
into partials and composed by ``dashboard_shell.py``.

Every fragment is a *contiguous slice* of the original single closure
``(() => { ... })();`` — the fragments are NOT independently valid modules; they
share one lexical scope and one ``state`` object. So the concatenation must be
byte-exact and in exactly ``FRAGMENTS`` order to reproduce the shipped script.

This module is intentionally js-free — no ``js``/``workers`` imports — so both
the Worker (on Cloudflare) and the test suite can import it directly, the same
way they import ``urls.py`` and ``dashboard_shell.py``. The composition is a pure
string concatenation, so it is trivially unit-testable against the on-disk
fragments.
"""

import json
import re

# Ordered fragments of the single dashboard IIFE. Concatenated verbatim (with no
# separator) they reproduce the original dashboard.js byte-for-byte. Keep this in
# lexical/source order; renaming or reordering changes the shipped script.
FRAGMENTS = (
    "01-state.js",
    "02-helpers.js",
    "03-issues-profile-io.js",
    "04-account.js",
    "05-repo-list-explorer.js",
    "06-repo-actions.js",
    "06-repo-content.js",
    "07-repo-compose-branch.js",
    "07-repo-pull-batch.js",
    "07-repo-security.js",
    "08-repo-detail-network.js",
    "09-achievements.js",
    "09-ads.js",
    "09-agent-profiles.js",
    "09-agents.js",
    "09-apps.js",
    "09-bounties.js",
    "09-calendar.js",
    "09-challenges.js",
    "09-daily-interface.js",
    "09-docs.js",
    "09-feedback.js",
    "09-hackathon.js",
    "09-links.js",
    "09-microjobs.js",
    "09-notes.js",
    "09-plugin-menu.js",
    "09-recent-pushes.js",
    "09-referrals.js",
    "09-repo-activity.js",
    "09-runners.js",
    "09-tasks.js",
    "09-update-notice.js",
    "09-workflows.js",
    "10-boot.js",
)


def fragment_path(name):
    """public/-relative path to a bundle fragment by its file name."""
    return "dashboard/js/%s" % name


def assemble_bundle(fragments):
    """Concatenate fragment sources, in order, into the single dashboard script.

    ``fragments`` is an iterable of raw JS strings in ``FRAGMENTS`` order. The
    join has no separator so the result is byte-identical to the pre-split file.
    """
    return "".join(fragments)


def compose_from_reader(read):
    """Assemble ``/dashboard.js`` using ``read(public_relative_path) -> str``.

    Shared by the asset build and the test suite. Reads each fragment in
    ``FRAGMENTS`` order and returns the fully composed script.
    """
    return assemble_bundle([read(fragment_path(name)) for name in FRAGMENTS])


# -- per-page chunks ----------------------------------------------------------
# Page modules that only their own page runs are served as separate scripts
# (``/dashboard-page-<name>.js``) and loaded when that page's init is called,
# so every other page downloads and compiles ~230 KB less. A chunk is still
# the same fragment text: it runs inside a factory that receives the core
# bundle's names it uses (``chunk_imports``), and the core keeps an async
# stub for each name other fragments call (``chunk_exports``). The build
# refuses a chunk whose code the core reaches in any other way.
LAZY_CHUNKS = {
    "achievements": "09-achievements.js",
    "agents": "09-agents.js",
    # Settings > Agents only (04-account.js initSettingsAgents).
    "agent-profiles": "09-agent-profiles.js",
    "apps": "09-apps.js",
    "bounties": "09-bounties.js",
    "calendar": "09-calendar.js",
    "challenges": "09-challenges.js",
    "docs": "09-docs.js",
    "hackathon": "09-hackathon.js",
    "links": "09-links.js",
    "notes": "09-notes.js",
    "referrals": "09-referrals.js",
    "repo-activity": "09-repo-activity.js",
    "tasks": "09-tasks.js",
    "workflows": "09-workflows.js",
}
CHUNK_GLOBAL = "__forkmeshDashboardChunks"
_DECL_RE = re.compile(
    r"^  (async\s+function\*?|function\*?|const|let|var|class)\s+"
    r"([A-Za-z_$][\w$]*)", re.M)
_IDENT_RE = re.compile(r"(?<![\w$.])([A-Za-z_$][\w$]*)(?![\w$])")


class ChunkError(ValueError):
    pass


CHUNK_RETRY_MS = 1500


def chunk_asset(name):
    return "dashboard-page-%s.js" % name


def _declared(source):
    return {name: kind.split()[-1] for kind, name in _DECL_RE.findall(source)}


def plan_chunks(sources):
    """{chunk: (imports, let_imports, exports)} for FRAGMENTS sources
    (``{fragment name: text}``), or ChunkError when a chunk cannot be split."""
    chunk_fragments = set(LAZY_CHUNKS.values())
    core_decl = {}
    for name in FRAGMENTS:
        if name not in chunk_fragments:
            for ident, kind in _declared(sources[name]).items():
                core_decl.setdefault(ident, kind)
    # Identifiers are scanned in the raw text, strings and comments included:
    # an extra import or stub is harmless, a missing one is not.
    core_idents = set(_IDENT_RE.findall("".join(
        sources[name] for name in FRAGMENTS if name not in chunk_fragments)))
    plans = {}
    for chunk, fragment in LAZY_CHUNKS.items():
        text = sources[fragment]
        own = _declared(text)
        used = set(_IDENT_RE.findall(text))
        imports = sorted(n for n in used if n in core_decl and n not in own)
        let_imports = [n for n in imports if core_decl[n] in ("let", "var")]
        for name in let_imports:
            pattern = r"(?<![\w$.])%s(?![\w$])" % re.escape(name)
            for line in text.split("\n"):
                for match in re.finditer(pattern, line):
                    before = line[:match.start()]
                    if before.lstrip().startswith("//") or any(
                            before.count(q) % 2 for q in "'\"`"):
                        raise ChunkError("%s: %s appears outside code"
                                         % (chunk, name))
                    if re.match(r"\s*(?:[-+*/%&|^]|\*\*|<<|>>>?|\?\?|&&|"
                                r"\|\|)?=(?!=)", line[match.end():]):
                        raise ChunkError("%s assigns core variable %s"
                                         % (chunk, name))
        exports = sorted(n for n in own if n in core_idents)
        for name in exports:
            if own[name] != "function":
                raise ChunkError("%s: core uses non-function %s" % (chunk, name))
        for other, other_fragment in LAZY_CHUNKS.items():
            if other != chunk and set(_declared(sources[other_fragment])) \
                    & (used - set(own)):
                raise ChunkError("%s uses names of chunk %s" % (chunk, other))
        plans[chunk] = (imports, let_imports, exports)
    return plans


def compose_chunk(chunk, text, plan):
    imports, let_imports, exports = plan
    plain = [n for n in imports if n not in let_imports]
    for name in let_imports:
        text = re.sub(r"(?<![\w$.])%s(?![\w$])" % re.escape(name),
                      "core.%s" % name, text)
    return (
        "// %s, loaded by dashboard.js when its page runs "
        "(app/src/dashboard_bundle.py LAZY_CHUNKS).\n"
        "(() => {\n"
        "  const chunks = window.%s = window.%s || {};\n"
        "  chunks[%s] = (core) => {\n"
        "  const { %s } = core;\n"
        "%s"
        "  return { %s };\n"
        "  };\n"
        "})();\n" % (
            LAZY_CHUNKS[chunk], CHUNK_GLOBAL, CHUNK_GLOBAL, json.dumps(chunk),
            ", ".join(plain), text, ", ".join(exports)))


def _core_loader(plans, versions):
    names = sorted({n for plan in plans.values() for n in plan[0]})
    lets = sorted({n for plan in plans.values() for n in plan[1]})
    scope = ["    %s," % n for n in names if n not in lets]
    scope += ["    get %s() { return %s; }," % (n, n) for n in lets]
    urls = ",\n".join('    %s: "/%s?v=%s"' % (json.dumps(chunk),
                                               chunk_asset(chunk),
                                               versions[chunk])
                      for chunk in sorted(plans))
    return (
        "  // Page modules served as their own scripts (dashboard_bundle.py\n"
        "  // LAZY_CHUNKS). A chunk's factory receives the names it uses from\n"
        "  // this bundle; the stubs where the modules were load it on first use.\n"
        "  const DASHBOARD_CHUNK_URLS = {\n%s,\n  };\n"
        "  const dashboardChunkScope = {\n%s\n  };\n"
        "  const dashboardChunkLoads = {};\n"
        "  const DASHBOARD_CHUNK_RETRY_MS = %d;\n"
        "  function injectDashboardChunk(name, src) {\n"
        "    return new Promise((resolve, reject) => {\n"
        "      const script = document.createElement(\"script\");\n"
        "      script.src = src;\n"
        "      script.onload = () => {\n"
        "        const factory = window.%s?.[name];\n"
        "        if (typeof factory === \"function\") resolve(factory(dashboardChunkScope));\n"
        "        else reject(new Error(\"dashboard_chunk_missing\"));\n"
        "      };\n"
        "      script.onerror = () => {\n"
        "        script.remove();\n"
        "        reject(new Error(\"dashboard_chunk_failed\"));\n"
        "      };\n"
        "      document.head.appendChild(script);\n"
        "    });\n"
        "  }\n"
        "  function showDashboardChunkFailure() {\n"
        "    if (document.querySelector(\"[data-dashboard-chunk-failure]\")) return;\n"
        "    const notice = document.createElement(\"div\");\n"
        "    notice.setAttribute(\"data-dashboard-chunk-failure\", \"\");\n"
        "    notice.setAttribute(\"role\", \"alert\");\n"
        "    notice.className = \"m-4 rounded-md border border-border bg-secondary p-3 text-sm text-foreground\";\n"
        "    notice.textContent = \"Part of this page could not be loaded. \";\n"
        "    const reload = document.createElement(\"button\");\n"
        "    reload.type = \"button\";\n"
        "    reload.className = \"underline\";\n"
        "    reload.textContent = \"Reload the page\";\n"
        "    reload.addEventListener(\"click\", () => location.reload());\n"
        "    notice.appendChild(reload);\n"
        "    (document.querySelector(\"main\") || document.body).prepend(notice);\n"
        "  }\n"
        "  function loadDashboardChunk(name) {\n"
        "    if (!dashboardChunkLoads[name]) {\n"
        "      const src = DASHBOARD_CHUNK_URLS[name];\n"
        "      dashboardChunkLoads[name] = injectDashboardChunk(name, src)\n"
        "        .catch((error) => {\n"
        "          if (error?.message !== \"dashboard_chunk_failed\") throw error;\n"
        "          return new Promise((resolve) => setTimeout(resolve, DASHBOARD_CHUNK_RETRY_MS))\n"
        "            .then(() => injectDashboardChunk(name, src + \"&retry=\" + Date.now()));\n"
        "        })\n"
        "        .catch((error) => {\n"
        "          delete dashboardChunkLoads[name];\n"
        "          if (error?.message === \"dashboard_chunk_failed\") showDashboardChunkFailure();\n"
        "          throw error;\n"
        "        });\n"
        "    }\n"
        "    return dashboardChunkLoads[name];\n"
        "  }\n\n" % (urls, "\n".join(scope), CHUNK_RETRY_MS, CHUNK_GLOBAL))


def _core_stub(chunk, exports):
    return "".join(
        "  // %s lives in %s (a per-page chunk).\n"
        "  function %s(...args) {\n"
        "    return loadDashboardChunk(%s).then((chunk) => chunk.%s(...args));\n"
        "  }\n\n" % (name, LAZY_CHUNKS[chunk], name, json.dumps(chunk), name)
        for name in exports)


# -- deferred dialog markup ---------------------------------------------------
# Dialogs no page shows on load stay out of the page documents: their partial
# is embedded in dashboard.js (cached across pages) and added to the document
# the first time one of them is opened (02-helpers.js ensureDeferredMarkup).
# ``marker`` is an element of the partial that tells it is already there;
# ``after`` is where it goes (after that element), else the end of <body>.
DEFERRED_MARKUP = {
    # Profile, about, pings, provider sign-in and feedback dialogs.
    "dialogs": {"partial": "modals", "marker": "#notificationModal",
                "after": None},
    # The header's "Start an agent" popover, positioned under its toggle.
    "agent": {"partial": "agent-modal", "marker": "#agentModal",
              "after": "#agentModalToggle"},
}


def deferred_markup_path(name):
    return "dashboard/partials/%s.html" % DEFERRED_MARKUP[name]["partial"]


def _deferred_markup(read):
    entries = "".join(
        "      %s: { marker: %s, after: %s, html: %s },\n" % (
            json.dumps(name), json.dumps(spec["marker"]),
            json.dumps(spec["after"]),
            json.dumps(read(deferred_markup_path(name))))
        for name, spec in DEFERRED_MARKUP.items())
    return (
        "  // Dialog markup the page documents leave out (dashboard_bundle.py\n"
        "  // DEFERRED_MARKUP); ensureDeferredMarkup adds it on first use.\n"
        "  function deferredMarkup(name) {\n"
        "    return {\n%s    }[name] || null;\n"
        "  }\n\n" % entries)


def compose_split_from_reader(read, version=None):
    """(core dashboard.js, {asset: chunk script}) from the fragments.

    ``version(text) -> str`` names a chunk's content hash for its URL."""
    if version is None:
        import hashlib

        def version(text):
            return hashlib.sha256(text.encode("utf-8")).hexdigest()[:12]
    sources = {name: read(fragment_path(name)) for name in FRAGMENTS}
    plans = plan_chunks(sources)
    chunks = {chunk: compose_chunk(chunk, sources[LAZY_CHUNKS[chunk]], plan)
              for chunk, plan in plans.items()}
    versions = {chunk: version(text) for chunk, text in chunks.items()}
    by_fragment = {fragment: chunk for chunk, fragment in LAZY_CHUNKS.items()}
    parts = []
    for name in FRAGMENTS:
        if name == "10-boot.js":
            parts.append(_core_loader(plans, versions))
            parts.append(_deferred_markup(read))
        if name in by_fragment:
            chunk = by_fragment[name]
            parts.append(_core_stub(chunk, plans[chunk][2]))
        else:
            parts.append(sources[name])
    return (assemble_bundle(parts),
            {chunk_asset(chunk): text for chunk, text in chunks.items()})
