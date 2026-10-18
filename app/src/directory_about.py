import base64
import json
import re
from collections import OrderedDict
from urllib.parse import quote, unquote, urlencode, urlparse, parse_qs

ABOUT_FILE = ".forkmesh/info.json"
LOGO_FILES = (("logo.svg", "image/svg+xml"), ("logo.png", "image/png"))
README_FILES = ("README.md", "readme.md", "Readme.md", "README.markdown",
                "README.txt", "README")
ACTIONS = frozenset({"dir-about", "dir-logo"})
MAX_ABOUT_BYTES = 64 * 1024
MAX_README_BYTES = 256 * 1024
MAX_LOGO_BYTES = 512 * 1024
MAX_PATH_BYTES = 1024
MAX_NAME = 80
MAX_DESCRIPTION = 240
MAX_WEBSITE = 240
MAX_LINK_URL = 400
MAX_LINK_LABEL = 60
MAX_LINKS = 8
MAX_TOPICS = 20
ABOUT_CACHE_ENTRIES = 256
LOGO_CACHE_ENTRIES = 24
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
FULL_COMMIT_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
REF_RE = re.compile(r"^[A-Za-z0-9._/-]{1,200}$")
TOPIC_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,49}$")
CONTROL_RE = re.compile(r"[\x00-\x1f\x7f\u2028\u2029]")
SPACE_RE = re.compile(r"\s+")
URL_RE = re.compile(r"https?://[^\s<>()\[\]\"']+", re.I)
LABEL_LINE_RE = re.compile(r"^\s*\*\*[^*\n]{1,40}:\s*\*\*")
WEBSITE_LINE_RE = re.compile(
    r"^\s*\*\*\s*(?:live site|website|homepage|home page)\s*:\s*\*\*(.*)$",
    re.I)
IMAGE_MD_RE = re.compile(r"!\[[^\]]*\]\([^)]*\)")
LINK_MD_RE = re.compile(r"\[([^\]]*)\]\([^)]*\)")
REF_LINK_MD_RE = re.compile(r"\[([^\]]*)\]\[[^\]]*\]")
TAG_RE = re.compile(r"<[^>]*>")
EMPHASIS_RE = re.compile(r"(\*\*|__|`)")
LIST_RE = re.compile(r"^\s*(?:[-*+]|\d+[.)])\s")
SETEXT_RE = re.compile(r"^\s*(?:=+|-+)\s*$")
SKIP_STARTS = ("#", "<", "![", "[!", "|", ">", "`", "~~~", "---", "***")
PROXY_HEADERS = ("authorization", "cookie", "accept", "accept-language",
                 "user-agent", "cf-connecting-ip", "x-forwarded-for",
                 "x-real-ip")

_about_cache = OrderedDict()
_logo_cache = OrderedDict()


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def clear_cache():
    _about_cache.clear()
    _logo_cache.clear()


def clean_path(value):
    raw = str(value if value is not None else "")
    if len(raw.encode("utf-8")) > MAX_PATH_BYTES:
        return None
    if "\\" in raw or CONTROL_RE.search(raw):
        return None
    parts = [part for part in raw.split("/") if part not in ("", ".")]
    if any(part in ("..", ".git") for part in parts):
        return None
    return "/".join(parts)


def clean_ref(value):
    ref = str(value or "").strip()
    if not ref:
        return ""
    if (not REF_RE.fullmatch(ref) or ref.startswith(("-", "/"))
            or ".." in ref or "@{" in ref or ref.endswith((".", "/"))):
        return None
    return ref


def join(path, name):
    return name if not path else path + "/" + name


def clean_text(value, limit):
    if not isinstance(value, str):
        return ""
    text = SPACE_RE.sub(" ", CONTROL_RE.sub(" ", value)).strip()
    return text[:limit].rstrip()


def clean_url(value, limit):
    if not isinstance(value, str):
        return ""
    url = value.strip()
    if (not url or len(url) > limit or CONTROL_RE.search(url)
            or SPACE_RE.search(url) or "\\" in url):
        return ""
    try:
        parsed = urlparse(url)
    except ValueError:
        return ""
    if parsed.scheme.lower() not in ("http", "https"):
        return ""
    host = parsed.netloc
    if not host or "@" in host or not parsed.hostname:
        return ""
    return url


def clean_topic(value):
    if not isinstance(value, str):
        return ""
    topic = SPACE_RE.sub("-", value.strip().lower())
    return topic if TOPIC_RE.fullmatch(topic) else ""


def _summary(text, limit):
    text = SPACE_RE.sub(" ", CONTROL_RE.sub(" ", text)).strip()
    if len(text) <= limit:
        return text
    cut = text[:limit - 1]
    space = cut.rfind(" ")
    if space > limit // 2:
        cut = cut[:space]
    return cut.rstrip(" ,;:.-") + "\u2026"


def parse_about(text):
    if not isinstance(text, str) or len(text.encode("utf-8")) > MAX_ABOUT_BYTES:
        return None
    try:
        data = json.loads(text.lstrip("\ufeff"))
    except ValueError:
        return None
    if not isinstance(data, dict):
        return None
    description = data.get("about")
    if not isinstance(description, str) or not description.strip():
        description = data.get("description")
    website = data.get("website")
    if not isinstance(website, str) or not website.strip():
        website = data.get("homepage")
    links = []
    seen = set()
    raw_links = data.get("links")
    for item in raw_links if isinstance(raw_links, list) else []:
        if len(links) >= MAX_LINKS:
            break
        if not isinstance(item, dict):
            continue
        url = clean_url(item.get("url"), MAX_LINK_URL)
        if not url or url in seen:
            continue
        label = clean_text(item.get("label"), MAX_LINK_LABEL)
        if not label:
            label = clean_text(urlparse(url).hostname or url, MAX_LINK_LABEL)
        seen.add(url)
        links.append({"label": label, "url": url})
    topics = []
    raw_topics = data.get("topics")
    if not isinstance(raw_topics, list):
        raw_topics = data.get("tags")
    for item in raw_topics if isinstance(raw_topics, list) else []:
        if len(topics) >= MAX_TOPICS:
            break
        topic = clean_topic(item)
        if topic and topic not in topics:
            topics.append(topic)
    return {
        "name": clean_text(data.get("name"), MAX_NAME),
        "description": clean_text(description, MAX_DESCRIPTION),
        "website": clean_url(website, MAX_WEBSITE),
        "links": links,
        "topics": topics,
    }


def _paragraphs(text):
    paragraphs = []
    current = []
    fenced = False
    for line in text.replace("\r\n", "\n").split("\n")[:600]:
        stripped = line.strip()
        if stripped.startswith(("```", "~~~")):
            fenced = not fenced
            if current:
                paragraphs.append(current)
                current = []
            continue
        if fenced:
            continue
        if not stripped:
            if current:
                paragraphs.append(current)
                current = []
            continue
        current.append(line)
    if current:
        paragraphs.append(current)
    return paragraphs


def _plain(lines):
    text = " ".join(line.strip() for line in lines)
    text = IMAGE_MD_RE.sub(" ", text)
    text = LINK_MD_RE.sub(r"\1", text)
    text = REF_LINK_MD_RE.sub(r"\1", text)
    text = TAG_RE.sub(" ", text)
    text = EMPHASIS_RE.sub("", text)
    return SPACE_RE.sub(" ", text).strip()


def readme_summary(text):
    result = {"description": "", "website": ""}
    if not isinstance(text, str):
        return result
    for paragraph in _paragraphs(text[:MAX_README_BYTES]):
        for line in paragraph:
            match = WEBSITE_LINE_RE.match(line)
            if match and not result["website"]:
                found = URL_RE.search(match.group(1))
                if found:
                    result["website"] = clean_url(
                        found.group(0).rstrip(".,;"), MAX_WEBSITE)
        if result["description"]:
            continue
        first = paragraph[0].lstrip()
        if first.startswith(SKIP_STARTS) or LIST_RE.match(first):
            continue
        if any(SETEXT_RE.match(line) for line in paragraph[1:]):
            continue
        if all(LABEL_LINE_RE.match(line) for line in paragraph):
            continue
        plain = _plain(paragraph)
        if len(plain) < 20 or not re.search(r"[A-Za-z]{3}", plain):
            continue
        result["description"] = _summary(plain, MAX_DESCRIPTION)
    return result


def blob_bytes(blob):
    if not isinstance(blob, dict) or blob.get("truncated"):
        return None
    content = blob.get("content")
    if not isinstance(content, str):
        return None
    encoding = str(blob.get("encoding") or "utf8").lower()
    if encoding in ("utf8", "utf-8"):
        return content.encode("utf-8")
    if encoding == "base64":
        try:
            return base64.b64decode(content, validate=True)
        except (ValueError, TypeError):
            return None
    return None


def blob_text(blob):
    raw = blob_bytes(blob)
    if raw is None:
        return None
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return None


def logo_type(name, raw):
    if not isinstance(raw, (bytes, bytearray)) or not raw:
        return ""
    if len(raw) > MAX_LOGO_BYTES:
        return ""
    if name == "logo.png":
        return "image/png" if bytes(raw).startswith(PNG_MAGIC) else ""
    if name == "logo.svg":
        try:
            text = bytes(raw).decode("utf-8").lstrip("\ufeff")
        except UnicodeDecodeError:
            return ""
        head = text[:4096].lower()
        if "<svg" not in head or "<!entity" in text.lower():
            return ""
        before = head[:head.index("<svg")]
        if re.sub(r"<\?xml[^>]*\?>|<!--.*?-->|<!doctype[^>]*>|\s", "",
                  before, flags=re.S):
            return ""
        return "image/svg+xml"
    return ""


def pick_logo(path, blobs):
    for name, _media in LOGO_FILES:
        raw = blob_bytes((blobs or {}).get(join(path, name)))
        media = logo_type(name, raw)
        if media:
            return name, media, bytes(raw)
    return "", "", b""


def candidate_paths(path):
    return ([join(path, ABOUT_FILE)]
            + [join(path, name) for name, _media in LOGO_FILES]
            + [join(path, name) for name in README_FILES])


def project_name(projects, short, owners, repo, path):
    projects = projects if isinstance(projects, dict) else {}
    short = short if isinstance(short, dict) else {}
    target = str(projects.get("repo") or "")
    if not target or target.lower() != str(repo or "").lower() or not path:
        return ""
    short_owner = str(short.get("owner") or "").strip().lower()
    names = {str(owner or "").strip().lower() for owner in owners or ()}
    if short_owner and short_owner not in names:
        return ""
    return clean_text((projects.get("folders") or {}).get(path), MAX_NAME)


def logo_url(owner, repo, path, ref):
    query = urlencode([("path", path), ("ref", ref)])
    return "/api/repo/%s/%s/dir-logo?%s" % (
        quote(owner, safe=""), quote(repo, safe=""), query)


def build(path, blobs, project="", owner="", repo="", commit="", ref=""):
    blobs = blobs if isinstance(blobs, dict) else {}
    about_blob = blobs.get(join(path, ABOUT_FILE))
    has_file = isinstance(about_blob, dict)
    parsed = parse_about(blob_text(about_blob)) if has_file else None
    if not has_file and not project:
        return None
    about = parsed or {"name": "", "description": "", "website": "",
                       "links": [], "topics": []}
    readme_path = ""
    readme = {"description": "", "website": ""}
    for name in README_FILES:
        text = blob_text(blobs.get(join(path, name)))
        if text is not None:
            readme_path = join(path, name)
            readme = readme_summary(text)
            break
    description_source = "about" if about["description"] else ""
    if not about["description"] and readme["description"]:
        about["description"] = readme["description"]
        description_source = "readme"
    website_source = "about" if about["website"] else ""
    if not about["website"] and readme["website"]:
        about["website"] = readme["website"]
        website_source = "readme"
    logo_name, logo_media, _raw = pick_logo(path, blobs)
    pin = commit if FULL_COMMIT_RE.fullmatch(commit or "") else ref
    logo = None
    if logo_name:
        logo = {
            "path": join(path, logo_name),
            "type": logo_media,
            "url": logo_url(owner, repo, path, pin),
        }
    folder = path.rsplit("/", 1)[-1] if path else repo
    return {
        "path": path,
        "source": "file" if has_file else "project",
        "invalid": bool(has_file and parsed is None),
        "name": about["name"] or project or folder,
        "description": about["description"],
        "descriptionSource": description_source,
        "website": about["website"],
        "websiteSource": website_source,
        "links": about["links"],
        "topics": about["topics"],
        "logo": logo,
        "readme": readme_path,
    }


def _remember(cache, key, value, limit):
    cache[key] = value
    cache.move_to_end(key)
    while len(cache) > limit:
        cache.popitem(last=False)


def _recall(cache, key):
    if key in cache:
        cache.move_to_end(key)
        return cache[key]
    return None


def cache_control(private, ref):
    if private:
        return "private, no-store, max-age=0, must-revalidate"
    if FULL_COMMIT_RE.fullmatch(ref or ""):
        return "public, max-age=300"
    return "no-cache"


def public_owner(request, owner):
    try:
        segments = [part for part in urlparse(str(request.url)).path.split("/")
                    if part]
    except Exception:
        return owner
    if len(segments) >= 3 and segments[0] == "api" and segments[1] == "repo":
        return unquote(segments[2]) or owner
    return owner


def _sub_url(request, operation, pairs):
    parsed = urlparse(str(request.url))
    base = parsed.path.rsplit("/", 1)[0]
    return "%s://%s%s/%s?%s" % (
        parsed.scheme or "https", parsed.netloc, base, operation,
        urlencode(pairs))


def _sub_request(request, url):
    headers = {}
    for name in PROXY_HEADERS:
        try:
            value = request.headers.get(name)
        except Exception:
            value = None
        if value:
            headers[name] = str(value)
    return JsRequest.new(url, to_js({"method": "GET", "headers": headers}))


async def _response_json(response):
    try:
        data = await response.json()
    except Exception:
        return None
    if hasattr(data, "to_py"):
        data = data.to_py()
    return data if isinstance(data, dict) else None


async def read_blobs(env, request, owner, repo, paths, ref, private):
    pairs = [("path", path) for path in paths]
    if ref:
        pairs.append(("ref", ref))
    sub = _sub_request(request, _sub_url(request, "blobs", pairs))
    if private:
        hosted = globals().get("hosted_private_read")
        response = (await hosted(env, sub, owner, repo, "blobs")
                    if callable(hosted) else None)
        if response is None:
            return 404, None
    else:
        response = await _https_mirror_proxy(env, sub, owner, repo, "blobs")
    status = int(getattr(response, "status", 0) or 0)
    if status != 200:
        return status, None
    data = await _response_json(response)
    if not data or data.get("ok") is False:
        return 502, None
    return 200, data


def _not_found():
    return json_response({"ok": False, "error": "not_found"}, status=404,
                         cache_control="no-store")


def _bad(code):
    return json_response({"ok": False, "error": code}, status=400,
                         cache_control="no-store")


def _unavailable(status):
    if status == 404:
        return _not_found()
    return json_response({"ok": False, "error": "mirror_unavailable"},
                         status=503, cache_control="no-store")


def _query(request):
    try:
        return parse_qs(urlparse(str(request.url)).query,
                        keep_blank_values=True)
    except Exception:
        return {}


def image_response(raw, media_type, cache):
    name = "logo.svg" if media_type == "image/svg+xml" else "logo.png"
    return JsResponse.new(Uint8Array.new(_to_js(bytes(raw))), to_js({
        "status": 200,
        "headers": {
            "content-type": media_type,
            "content-length": str(len(raw)),
            "content-disposition": 'inline; filename="%s"' % name,
            "content-security-policy":
                "default-src 'none'; style-src 'unsafe-inline'; sandbox",
            "x-content-type-options": "nosniff",
            "cross-origin-resource-policy": "same-origin",
            "cache-control": cache,
        },
    }))


async def route(env, request, owner, repo, action, projects=None,
                short=None):
    if action not in ACTIONS:
        return _not_found()
    if method_name(request) not in ("GET", "HEAD"):
        return json_response({"ok": False, "error": "method_not_allowed"},
                             status=405, cache_control="no-store")
    query = _query(request)
    path = clean_path(query.get("path", [""])[0])
    if path is None:
        return _bad("invalid_path")
    ref = clean_ref(query.get("ref", [""])[0])
    if ref is None:
        return _bad("invalid_ref")
    private = bool(await _repo_is_private(env, owner, repo))
    display_owner = public_owner(request, owner)
    if action == "dir-logo":
        return await _logo(env, request, owner, repo, display_owner, path,
                           ref, private)
    name = ""
    if callable(projects) and callable(short):
        try:
            name = project_name(await projects(), await short(),
                                (owner, display_owner), repo, path)
        except Exception:
            name = ""
    key = (owner.lower(), repo.lower(), path, ref, name)
    cache = cache_control(private, ref)
    if not private and FULL_COMMIT_RE.fullmatch(ref):
        cached = _recall(_about_cache, key)
        if cached is not None:
            return json_response(cached, cache_control=cache)
    status, data = await read_blobs(env, request, owner, repo,
                                    candidate_paths(path), ref, private)
    if status != 200:
        return _unavailable(status)
    commit = str(data.get("commit") or "").lower()
    if not FULL_COMMIT_RE.fullmatch(commit):
        commit = ""
    about = build(path, data.get("blobs"), name, display_owner, repo,
                  commit, ref)
    body = {"ok": True, "path": path, "commit": commit, "about": about}
    if not private and commit:
        _remember(_about_cache, (owner.lower(), repo.lower(), path, commit,
                                 name), body, ABOUT_CACHE_ENTRIES)
    return json_response(body, cache_control=cache)


async def _logo(env, request, owner, repo, display_owner, path, ref,
                private):
    cache = cache_control(private, ref)
    key = (owner.lower(), repo.lower(), path, ref)
    if not private and FULL_COMMIT_RE.fullmatch(ref):
        cached = _recall(_logo_cache, key)
        if cached is not None:
            return image_response(cached[1], cached[0], cache)
    paths = [join(path, name) for name, _media in LOGO_FILES]
    status, data = await read_blobs(env, request, owner, repo, paths, ref,
                                    private)
    if status != 200:
        return _unavailable(status)
    _name, media, raw = pick_logo(path, data.get("blobs"))
    if not media:
        return _not_found()
    if not private and FULL_COMMIT_RE.fullmatch(ref):
        _remember(_logo_cache, key, (media, raw), LOGO_CACHE_ENTRIES)
    return image_response(raw, media, cache)
