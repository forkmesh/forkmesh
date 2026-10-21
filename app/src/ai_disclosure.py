import json
import re
from urllib.parse import quote

PULLS_SETTINGS_PATH = ".forkmesh/pulls.json"
ISSUES_SETTINGS_PATH = ".forkmesh/issues.json"
AI_POLICY_PATH = "AI_POLICY.md"
MAX_AI_MODEL = 80
MAX_PROMPT_TEMPLATE = 500
MAX_SETTINGS_BYTES = 16 * 1024
DEFAULT_PROMPT_TEMPLATE = (
    "Fix {url}: read the issue, make the change with tests, and open a pull "
    "request on {host} following the repository's AI policy.")
CONTROL_RE = re.compile(r"[\x00-\x1f\x7f\u2028\u2029]")
SPACE_RE = re.compile(r"\s+")
LINE_SPLIT_RE = re.compile(r"\r\n|[\r\n\u2028\u2029]")
BODY_LINE_RE = re.compile(
    r"^\s*(?:[-*+>]\s+)?AI[ -]?models?(?:\(s\))?(?:\s+used)?\s*:\s*(.+?)\s*$",
    re.I)
COMMIT_SPLIT_RE = re.compile(r"(?=^From [0-9a-f]{7,64} )", re.M | re.I)
CO_AUTHOR_RE = re.compile(
    r"^\s*Co-authored-by:\s*([^<\n]+?)\s*(?:<[^>\n]*>)?\s*$", re.I | re.M)
AGENT_TRAILER_RE = re.compile(
    r"^\s*ForkMesh-Agent:\s*([A-Za-z0-9._-]{1,40})/([A-Za-z0-9._:-]{1,64})\s*$",
    re.I | re.M)
MODEL_NAME_RE = re.compile(
    r"\b(?:claude|opus|sonnet|haiku|gpt|chatgpt|openai|o[1-9]|gemini|copilot|"
    r"codex|cursor|llama|mistral|mixtral|deepseek|qwen|grok|devin|aider|"
    r"windsurf|jules|kimi|phi|glm)\b", re.I)
NONE_RE = re.compile(
    r"^(?:none|no|no ai|no ai used|not used|n/?a|nothing|human|human only|"
    r"no model|-)$", re.I)
TEMPLATE_FIELD_RE = re.compile(r"\{(url|number|title|repo|host)\}")


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def clean_ai_model(value):
    if not isinstance(value, str):
        return ""
    text = SPACE_RE.sub(" ", CONTROL_RE.sub(" ", value.replace("*", "")
                                            .replace("`", ""))).strip()
    return text[:MAX_AI_MODEL].strip()


def ai_model_is_none(value):
    return bool(NONE_RE.match(clean_ai_model(value)))


def ai_model_from_body(body):
    for line in LINE_SPLIT_RE.split(str(body or "")):
        match = BODY_LINE_RE.match(line.replace("*", ""))
        if match:
            value = clean_ai_model(match.group(1))
            if value:
                return value
    return ""


def ai_model_from_commits(mbox):
    messages = [part for part in COMMIT_SPLIT_RE.split(str(mbox or ""))
                if part.strip()]
    for message in reversed(messages):
        attested = AGENT_TRAILER_RE.search(message)
        if attested:
            return clean_ai_model(attested.group(2))
        for match in CO_AUTHOR_RE.finditer(message):
            name = clean_ai_model(match.group(1))
            if name and MODEL_NAME_RE.search(name):
                return name
    return ""


def pull_ai_model(pull):
    pull = pull if isinstance(pull, dict) else {}
    description = pull.get("description", pull.get("body", ""))
    return (clean_ai_model(pull.get("aiModel"))
            or ai_model_from_body(description)
            or ai_model_from_commits(pull.get("commits")))


def parse_settings(text):
    raw = str(text or "")
    if not raw.strip() or len(raw.encode("utf-8")) > MAX_SETTINGS_BYTES:
        return {}
    try:
        value = json.loads(raw)
    except ValueError:
        return {}
    return value if isinstance(value, dict) else {}


def requires_ai_disclosure(text):
    return parse_settings(text).get("requireAiDisclosure") is True


def prompt_template(text):
    value = parse_settings(text).get("aiPromptTemplate")
    if not isinstance(value, str):
        return ""
    value = SPACE_RE.sub(" ", CONTROL_RE.sub(" ", value)).strip()
    return value[:MAX_PROMPT_TEMPLATE]


def issue_ai_prompt(url, number, title, repo, host, template=""):
    fields = {"url": str(url or ""), "number": str(number or ""),
              "title": str(title or ""), "repo": str(repo or ""),
              "host": str(host or "")}
    text = TEMPLATE_FIELD_RE.sub(lambda match: fields[match.group(1)],
                                 str(template or "") or DEFAULT_PROMPT_TEMPLATE)
    return SPACE_RE.sub(" ", CONTROL_RE.sub(" ", text)).strip()


def disclosure_refusal(ai_model_name):
    return {
        "error": "ai_disclosure_required",
        "message": (
            "This repository asks every pull request to say which AI model "
            "was used. Send \"aiModel\" with the pull request (for example "
            "\"Claude Opus 5.5\", or \"None\"), add a line \"AI model: <name>\" "
            "to the description, or add a Co-Authored-By trailer naming the "
            "model to the head commit."),
        "field": ai_model_name,
    }


async def _settings_text(env, owner, repo, ref):
    reader = globals().get("_forkbot_repo_gateway_json")
    blob_text = globals().get("_forkbot_blob_text")
    if reader is None or blob_text is None:
        return None
    query = "blob?path=%s" % quote(PULLS_SETTINGS_PATH, safe="")
    if ref:
        query += "&ref=%s" % quote(str(ref), safe="")
    try:
        data = await reader(env, owner, repo, query)
    except Exception:
        return None
    if not data:
        return None
    return blob_text(data)


async def pull_ai_disclosure_check(env, owner, repo, pull):
    model = pull_ai_model(pull)
    if model:
        pull["aiModel"] = model
        return None
    pull.pop("aiModel", None)
    base = str(pull.get("base") or "").strip()
    text = await _settings_text(env, owner, repo, base)
    if text is None or not requires_ai_disclosure(text):
        return None
    return disclosure_refusal("aiModel")
