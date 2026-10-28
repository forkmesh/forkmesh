from __future__ import annotations

import re

JS_DOT = r"[^\n\r  ]"

METRICS = (
    ("merge", "Merge"),
    ("complexity", "Complexity"),
    ("quality", "Quality"),
    ("security", "Security"),
    ("network", "Network"),
    ("practices", "Practices"),
)

CODE_EXTENSIONS = frozenset((
    "c", "cc", "cpp", "cxx", "h", "hpp", "cs", "java", "kt", "go", "rs", "py",
    "rb", "js", "jsx", "ts", "tsx", "mjs", "cjs", "php", "swift", "scala", "sh",
    "bash", "sql", "dart", "ex", "exs", "vue", "svelte"))
MANIFESTS = frozenset((
    "package.json", "pyproject.toml", "requirements.txt", "cargo.toml",
    "go.mod", "pom.xml", "build.gradle", "build.gradle.kts", "gemfile",
    "composer.json", "pubspec.yaml", "mix.exs"))
LOCKFILES = frozenset((
    "package-lock.json", "pnpm-lock.yaml", "yarn.lock", "cargo.lock", "go.sum",
    "gemfile.lock", "composer.lock", "pubspec.lock", "pipfile.lock",
    "poetry.lock"))
ODD_EXTENSIONS = frozenset((
    "exe", "dll", "dylib", "so", "a", "o", "db", "sqlite", "zip", "tar", "gz",
    "7z"))
CONTROL_FLOW = re.compile(
    r"\b(?:if|elif|else\s+if|for|foreach|while|switch|case|catch|except|match"
    r"|when)\b|&&|\|\|", re.I | re.A)
TODO_MARKER = re.compile(r"\b(?:TODO|FIXME|HACK|XXX)\b", re.A)
DEBUG_CALL = re.compile(
    r"\b(?:console\.(?:log|debug|trace)|debugger\s*;|qDebug\s*\(|"
    r"System\.out\.print|var_dump\s*\(|dd\s*\()", re.I | re.A)
BROAD_CATCH = re.compile(
    r"(?:except\s*(?::|Exception\b)|catch\s*\([^)]*(?:Exception|Throwable|"
    r"\.\.\.)[^)]*\))", re.I | re.A)
UNSAFE_API = re.compile(
    r"\b(?:eval|Function)\s*\(|\bexec\s*\(|\bsystem\s*\(|child_process\s*\.\s*"
    r"exec|shell\s*=\s*True|dangerouslySetInnerHTML|\.innerHTML\s*=", re.I | re.A)
TRANSPORT_BYPASS = re.compile(
    r"verify\s*=\s*(?:false|0)|rejectUnauthorized\s*:\s*false|"
    r"NODE_TLS_REJECT_UNAUTHORIZED\s*=\s*['\"]?0|CURLOPT_SSL_VERIFYPEER\s*,"
    r"\s*(?:0|false)|danger_accept_invalid_certs\s*\(\s*true", re.I | re.A)
PERMISSIVE_SECURITY = re.compile(
    r"Access-Control-Allow-Origin[^\n]*['\"]\*['\"]|\bcors\s*\(\s*\)|"
    r"\bchmod\s+(?:-R\s+)?777\b", re.I | re.A)
REQUEST_SITE = re.compile(
    r"\bfetch\s*\(|\baxios\s*(?:\.|\[)|\brequests\s*\.\s*(?:get|post|put|patch"
    r"|delete|request)\s*\(|\b(?:http|https)\s*\.\s*(?:get|request)\s*\(|"
    r"\bQNetwork(?:Request|AccessManager)\b|\bURLSession\b|\bWebSocket\s*\(|"
    r"\bcurl_easy_perform\b|\bHttpClient\b", re.I | re.A)
RESILIENCE = re.compile(
    r"\b(?:timeout|AbortController|CancellationToken|retry|retries|backoff|"
    r"cancell?ation)\b", re.I | re.A)
URL = re.compile(r"https?://[^\s'\"<>]+", re.I | re.A)
SECRET_ASSIGNMENT = "Secret/token assignment"
SECRET_PATTERNS = (
    ("GitHub token", re.compile(r"\bgh[pousr]_[A-Za-z0-9_]{36,}\b", re.A)),
    ("GitHub fine-grained PAT", re.compile(r"\bgithub_pat_[A-Za-z0-9_]{22,}\b", re.A)),
    ("AWS access key", re.compile(r"\bAKIA[0-9A-Z]{16,}\b", re.A)),
    ("AWS temporary access key", re.compile(r"\bASIA[0-9A-Z]{16,}\b", re.A)),
    ("Slack token", re.compile(r"\bxox[baprs]-[A-Za-z0-9-]{20,}\b", re.A)),
    ("OpenAI API key", re.compile(
        r"\bsk-proj-[A-Za-z0-9_-]{20,}\b|\bsk-[A-Za-z0-9]{48,}\b", re.A)),
    ("Anthropic API key", re.compile(r"\bsk-ant-[A-Za-z0-9_-]{20,}\b", re.A)),
    ("Stripe secret key", re.compile(r"\bsk_(live|test)_[A-Za-z0-9]{24,}\b", re.A)),
    ("Stripe restricted key", re.compile(r"\brk_(live|test)_[A-Za-z0-9]{24,}\b", re.A)),
    ("Google API key", re.compile(r"\bAIza[A-Za-z0-9_-]{35,}\b", re.A)),
    ("Google OAuth token", re.compile(r"\bya29\.[A-Za-z0-9_-]{20,}\b", re.A)),
    ("Google OAuth client secret", re.compile(r"\bGOCSPX-[A-Za-z0-9_-]{28,}\b", re.A)),
    ("SendGrid API key", re.compile(
        r"\bSG\.[A-Za-z0-9_-]{22,}\.[A-Za-z0-9_-]{43,}\b", re.A)),
    ("Twilio auth token", re.compile(r"\bSK[a-f0-9]{32,}\b", re.A)),
    ("npm access token", re.compile(r"\bnpm_[A-Za-z0-9]{36,}\b", re.A)),
    ("HashiCorp Vault token", re.compile(r"\bhv[sbr]\.[A-Za-z0-9]{24,}\b", re.A)),
    ("Cloudflare API token", re.compile(
        r"(?:CLOUDFLARE_API_TOKEN|CF_API_TOKEN)\s*=\s*['\"]?([A-Za-z0-9_-]{40,})"
        r"['\"]?")),
    ("PEM private key", re.compile(
        r"-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP |)PRIVATE KEY(?:-----| "
        r"BLOCK-----)")),
    (SECRET_ASSIGNMENT, re.compile(
        r"(?:password|passwd|api[_-]?(?:key|secret|token)|auth[_-]?token|"
        r"secret[_-]?key|access[_-]?token|private[_-]?key)\s*[:=]\s*['\"]"
        r"([^'\"\s]{20,})['\"]", re.I | re.A)),
)
SECRET_FRAGMENTS = (
    "ghp_", "gho_", "ghu_", "ghs_", "ghr_", "github_pat_", "akia", "asia",
    "xoxb-", "xoxa-", "xoxp-", "xoxr-", "xoxs-", "sk-proj-", "sk-", "sk_live_",
    "sk_test_", "rk_live_", "rk_test_", "aiza", "ya29.", "gocspx-", "sg.",
    "npm_", "hvs.", "hvb.", "hvr.", "cloudflare_api_token", "cf_api_token",
    "-----begin ", "password", "passwd", "api_key", "api-key", "apikey",
    "api_secret", "api-secret", "apisecret", "api_token", "api-token",
    "apitoken", "auth_token", "auth-token", "authtoken", "secret_key",
    "secret-key", "secretkey", "access_token", "access-token", "accesstoken",
    "private_key", "private-key", "privatekey")
PLACEHOLDER_MARKERS = (
    "correct-horse-battery-staple", "changeme", "change-me", "change_me",
    "placeholder", "example", "sample", "dummy", "redacted", "notreal",
    "not-a-real", "not_a_real", "fake", "your-", "your_", "test-password",
    "test_password", "testpassword", "${", "{{", "<")
INDIRECTION = re.compile(r"^(?:\$\w+|\$\{[^}]*\}|\$\([^)]*\)|%\w+%)\Z", re.A)
IGNORE_MARKER = "forkmesh-secret-scan:ignore-line"
MAX_LINES = 200000
DIFF_GIT = re.compile(r"^diff --git a/(%s*?) b/(%s*)\Z" % (JS_DOT, JS_DOT))
JS_SPACE = " \t\n\v\f\r        " \
    "         　﻿"


def js_length(text):
    return len(text.encode("utf-16-le", "surrogatepass")) // 2


def parse_files(patch):
    lines = str(patch or "").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    files = []
    current = None
    for line in lines:
        if line.startswith("diff --git "):
            match = DIFF_GIT.match(line)
            old = match.group(1) if match else ""
            new = (match.group(2) if match else "") or old or "file"
            current = {"oldPath": old, "newPath": new, "status": "modified",
                       "binary": False, "adds": 0, "dels": 0, "added": []}
            files.append(current)
            continue
        if current is None:
            continue
        if line.startswith("new file mode"):
            current["status"] = "added"
        elif line.startswith("deleted file mode"):
            current["status"] = "deleted"
        elif line.startswith("rename from "):
            current["status"] = "renamed"
            current["oldPath"] = line[len("rename from "):]
        elif line.startswith("rename to "):
            current["newPath"] = line[len("rename to "):]
        elif line.startswith("Binary files ") or line.startswith("GIT binary patch"):
            current["binary"] = True
        elif line.startswith(("similarity index", "index ", "old mode",
                              "new mode", "--- ", "+++ ", "@@")):
            continue
        elif line.startswith("+"):
            current["added"].append(line[1:])
            current["adds"] += 1
        elif line.startswith("-"):
            current["dels"] += 1
    return files


def _name(path):
    text = str(path or "")
    return text[text.rfind("/") + 1:]


def _suffix(path):
    name = _name(path)
    dot = name.rfind(".")
    return "" if dot < 0 else name[dot + 1:].lower()


def is_code(path):
    return _suffix(path) in CODE_EXTENSIONS


def is_test(path):
    lower = str(path or "").lower()
    if lower.startswith(("test/", "tests/")) or any(
            part in lower for part in ("/test/", "/tests/", "/__tests__/",
                                       "/spec/")):
        return True
    base = _name(lower)
    return base.startswith("test_") or any(
        part in base for part in ("_test.", ".test.", ".spec.", "_spec."))


def is_generated(path):
    lower = "/" + str(path or "").lower()
    return any(part in lower for part in (
        "/node_modules/", "/vendor/", "/dist/", "/generated/", "/build/",
        ".generated.")) or lower.endswith((".min.js", ".min.css"))


def _placeholder(value):
    lower = value.lower()
    if any(marker in lower for marker in PLACEHOLDER_MARKERS):
        return True
    if INDIRECTION.match(value):
        return True
    return bool(lower) and all(ch == lower[0] for ch in lower)


def secret_matches(text):
    if IGNORE_MARKER in text:
        return 0
    lower = text.lower()
    if "SK" not in text and not any(part in lower for part in SECRET_FRAGMENTS):
        return 0
    found = 0
    for name, pattern in SECRET_PATTERNS:
        for match in pattern.finditer(text):
            if name == SECRET_ASSIGNMENT and _placeholder(match.group(1) or ""):
                continue
            found += 1
    return found


def urls(text):
    total = insecure = 0
    for match in URL.finditer(text):
        found = match.group(0).lower()
        total += 1
        if not found.startswith("http://"):
            continue
        if found.startswith(("http://localhost", "http://127.0.0.1",
                             "http://0.0.0.0", "http://[::1]")) \
                or "example." in found or ".invalid" in found:
            continue
        insecure += 1
    return total, insecure


def _conflict_marker(text):
    trimmed = text.strip(JS_SPACE)
    return trimmed.startswith(("<<<<<<<", ">>>>>>>")) or trimmed == "======="


def _indent(text):
    return js_length(text) - js_length(text.lstrip(JS_SPACE))


def bounded(score):
    return min(max(int(score), 0), 100)


def tone(score):
    return "green" if score >= 85 else "orange" if score >= 60 else "red"


def report(files):
    files = files if isinstance(files, list) else []
    total_adds = total_dels = max_file_churn = 0
    source_files = test_files = test_adds = test_dels = 0
    binary_files = generated_files = sensitive_files = odd_artifacts = 0
    lockfile_changed = manifest_changed = False
    s = dict.fromkeys((
        "controlFlow", "nested", "longLines", "todo", "debug", "broadCatch",
        "unsafeApi", "transportBypass", "permissive", "request", "urls",
        "insecureUrls", "conflictMarkers", "trailingWhitespace", "secrets"), 0)
    resilience = False
    seen = 0
    for file in files:
        path = file.get("newPath") or file.get("oldPath") or "file"
        adds = int(file.get("adds") or 0)
        dels = int(file.get("dels") or 0)
        total_adds += adds
        total_dels += dels
        max_file_churn = max(max_file_churn, adds + dels)
        test = is_test(path)
        if is_code(path) and not test:
            source_files += 1
        if test:
            test_files += 1
            test_adds += adds
            test_dels += dels
        if file.get("binary"):
            binary_files += 1
        if is_generated(path):
            generated_files += 1
        base = _name(path).lower()
        suffix = _suffix(path)
        added_file = file.get("status") == "added"
        if added_file and (base in (".env", "id_rsa", "id_ed25519")
                           or suffix in ("pem", "p12", "pfx", "key")):
            sensitive_files += 1
        if added_file and suffix in ODD_EXTENSIONS:
            odd_artifacts += 1
        lockfile_changed = lockfile_changed or base in LOCKFILES
        manifest_changed = manifest_changed or base in MANIFESTS
        code = file.get("status") != "deleted" and is_code(path)
        for added in file.get("added") or ():
            if seen > MAX_LINES:
                break
            seen += 1
            if code:
                if CONTROL_FLOW.search(added):
                    s["controlFlow"] += 1
                    if _indent(added) >= 12:
                        s["nested"] += 1
                if js_length(added) > 140:
                    s["longLines"] += 1
                if added.endswith((" ", "\t")):
                    s["trailingWhitespace"] += 1
            s["todo"] += bool(TODO_MARKER.search(added))
            s["debug"] += bool(DEBUG_CALL.search(added))
            s["broadCatch"] += bool(BROAD_CATCH.search(added))
            s["unsafeApi"] += bool(UNSAFE_API.search(added))
            s["transportBypass"] += bool(TRANSPORT_BYPASS.search(added))
            s["permissive"] += bool(PERMISSIVE_SECURITY.search(added))
            s["request"] += bool(REQUEST_SITE.search(added))
            resilience = resilience or bool(RESILIENCE.search(added))
            s["conflictMarkers"] += _conflict_marker(added)
            found, insecure = urls(added)
            s["urls"] += found
            s["insecureUrls"] += insecure
            s["secrets"] += secret_matches(added)
    churn = total_adds + total_dels
    file_count = len(files)

    complexity = 100
    for limit, cost in ((80, 4), (250, 8), (600, 14), (1500, 20)):
        if churn > limit:
            complexity -= cost
    for limit, cost in ((250, 8), (750, 10)):
        if max_file_churn > limit:
            complexity -= cost
    for limit, cost in ((20, 6), (50, 10), (120, 15)):
        if s["controlFlow"] > limit:
            complexity -= cost
    for limit, cost in ((8, 8), (30, 12)):
        if s["nested"] > limit:
            complexity -= cost
    complexity = bounded(complexity)

    quality = 100
    if source_files and not test_files:
        quality -= 18
        if total_adds > 250:
            quality -= 10
    if test_files and not test_adds and test_dels:
        quality -= 20
    quality -= min(25, s["todo"] * 5)
    quality -= min(15, s["debug"] * 5)
    quality -= min(15, s["broadCatch"] * 5)
    quality -= min(15, s["longLines"] * 2)
    quality = bounded(quality)

    security = 100
    if s["secrets"]:
        security -= 70 + min(20, (s["secrets"] - 1) * 10)
    security -= min(70, s["transportBypass"] * 35)
    security -= min(50, s["unsafeApi"] * 20)
    security -= min(30, s["permissive"] * 15)
    security -= min(70, sensitive_files * 55)
    security = bounded(security)

    network = 100
    if s["request"]:
        network -= min(20, s["request"] * 4)
        if not resilience:
            network -= 18
    if s["urls"]:
        network -= min(10, s["urls"] * 2)
    if s["insecureUrls"]:
        network -= min(80, s["insecureUrls"] * 40)
    network = bounded(network)

    practices = 100
    if s["conflictMarkers"]:
        practices = 10
    practices -= min(30, generated_files * 12)
    practices -= min(20, binary_files * 8)
    practices -= min(50, odd_artifacts * 25)
    practices -= min(15, s["trailingWhitespace"] * 2)
    practices -= min(15, (s["todo"] + s["debug"]) * 3)
    if lockfile_changed and not manifest_changed:
        practices -= 8
    if churn > 1000:
        practices -= 10
    practices = bounded(practices)

    weighted = (complexity * 25 + quality * 20 + security * 25 + network * 10
                + practices * 20 + 50) // 100
    merge = min(weighted, 84)
    if file_count > 25:
        merge -= 5
    if file_count > 75:
        merge -= 10
    if security < 60 or network < 40 or practices < 30:
        merge = min(merge, 55)
    merge = bounded(merge)

    scores = {"merge": merge, "complexity": complexity, "quality": quality,
              "security": security, "network": network,
              "practices": practices}
    return [{"key": key, "label": label, "score": scores[key],
             "tone": tone(scores[key])} for key, label in METRICS]
