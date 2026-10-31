import ipaddress
import json
import re
from urllib.parse import unquote
import instance_privacy


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


MODE_ENV = "FORKMESH_PROBE_BAN"
ALLOW_ENV = "FORKMESH_PROBE_BAN_ALLOW"
MODES = ("ban", "observe", "off")
HOUR_MS = 60 * 60 * 1000
DAY_MS = 24 * HOUR_MS
WINDOW_MS = DAY_MS
BAN_SCORE = 3
FIRST_BAN_MS = 7 * DAY_MS
REPEAT_BAN_MS = 30 * DAY_MS
REPEAT_WITHIN_MS = 90 * DAY_MS
VISITOR_RETAIN_MS = 90 * DAY_MS
BAN_HISTORY_MS = 7 * DAY_MS
LOCAL_TTL_MS = 2 * 60 * 1000
MAX_ACTIVE_BANS = 500
MAX_LOCAL = 5000
MAX_PATHS = 24
PATH_MAX = 300
IPV6_PREFIX = 64
RULE_PREFIX = "auto-probe-"
RULE_PRIORITY = 0
VISITOR_PREFIX = "probe-visitor:"
EXEMPT_PATHS = frozenset({"/health", "/readyz"})
EXEMPT_PREFIXES = ("/api/fleet/",)
WEIGHTS = {"secret": 3, "script": 3, "cms": 2, "backup": 2, "config": 1,
           "recon": 1}
LABELS = {
    "secret": "Secrets",
    "script": "Server scripts",
    "cms": "CMS and admin panels",
    "backup": "Backups and logs",
    "config": "Config files",
    "recon": "Recon paths",
}
CLOUDFLARE_RANGES = (
    "173.245.48.0/20", "103.21.244.0/22", "103.22.200.0/22", "103.31.4.0/22",
    "141.101.64.0/18", "108.162.192.0/18", "190.93.240.0/20",
    "188.114.96.0/20", "197.234.240.0/22", "198.41.128.0/17",
    "162.158.0.0/15", "104.16.0.0/13", "104.24.0.0/14", "172.64.0.0/13",
    "131.0.72.0/22", "2400:cb00::/32", "2606:4700::/32", "2803:f800::/32",
    "2405:b500::/32", "2405:8100::/32", "2a06:98c0::/29", "2c0f:f248::/32")
_CLOUDFLARE = tuple(ipaddress.ip_network(net) for net in CLOUDFLARE_RANGES)
_SKIP_FIRST = frozenset({
    ".well-known", "private-or-unpublished-repository", "_host", "docs"})
_API_SKIP = frozenset({"repo", "repos", "orgs", "users", "raw", "git"})
_REPO_FAMILIES = frozenset({
    "blob", "tree", "raw", "blame", "commit", "commits", "history", "edit",
    "compare", "releases", "archive", "pulls", "pull", "issues", "files",
    "src", "actions", "wiki", "settings", "insights", "branches", "tags"})
_SECRET_RE = re.compile(
    r"^(?:\.env.*|\.git|\.git-credentials|\.gitconfig|\.svn|\.hg|\.bzr|"
    r"\.aws|\.ssh|\.docker|\.dockercfg|\.kube|\.config|\.s3cfg|\.boto|"
    r"\.npmrc|\.pypirc|\.netrc|\.pgpass|\.my\.cnf|\.htpasswd|\.htaccess|"
    r"\.passwd.*|\.ftpconfig|\.msmtprc|\.vault-token|\.yarnrc|"
    r"\.(?:bash|zsh)rc|\.profile|\.ds_store|\.vscode|\.idea|\.terraform|\.tugboat|"
    r"\.secrets?|\.amplifyrc|web-inf|_netrc|_environment|\.?wgetrc|"
    r"\.?(?:bash|zsh|mysql|psql|node_repl|python|sqlite|rediscli)_history|"
    r"id_(?:rsa|dsa|ecdsa|ed25519)(?:\.pub)?|terraform\.tfstate.*|"
    r".*\.tfvars(?:\.json)?|googleservice-info\.plist|google-services\.json|"
    r"wp-config\.php.*|sftp-config\.json|credentials|"
    r".+\.(?:sql|sqlite3?|db|mdb|dump)(?:\.(?:gz|zip|bz2|xz|tar|tgz|7z))?)$")
_SECRET_FILE_RE = re.compile(
    r"credential|secret|private[-_]?key|passw(?:or)?d|adminsdk|"
    r"service[-_]?account|aws-exports|claude.*\.json|opencode\.json|"
    r"vault\.json|filezilla\.xml|winscp\.rnd|"
    r"(?:^|[._-])mcp(?:[._-]|$)|"
    r"(?:^|[._-])(?:api[-_]?)?(?:keys?|keyfile|tokens?|creds?|sa|aws)"
    r"\.(?:json|txt|ya?ml)$|"
    r"^(?:aws-ses|aws-smtp|ses|smtp|gcp|vertex|neon|turso|planetscale|"
    r"supabase|writer|ai21|openai|anthropic|gemini|cohere|mistral|groq|"
    r"nextauth|workos|stytch|clerk|auth0|authorizenet|mercadopago|stripe|"
    r"paypal|braintree|razorpay|twilio|activecampaign|getresponse|omnisend|"
    r"mailgun|sendgrid|mailchimp|mailjet|mandrill|brevo|sendinblue|"
    r"sparkpost|postmark|resend|mailersend|datasources|betterstack|bigquery|"
    r"clickhouse|snowflake|keycloak|northflank|ovh(?:-backups?)?|plaid|stigg|"
    r"uptimerobot|argyle|sftp|postman(?:_collection)?|algolia|cloudinary|"
    r"heroku|digitalocean|linode|vultr|hetzner|cloudflare|azure|s3|r2|"
    r"backblaze|wasabi|pusher|slack|discord|telegram|github|gitlab|"
    r"bitbucket|mongo(?:db)?|redis|elastic(?:search)?|coinbase|binance)"
    r"\.(?:json|txt|ya?ml)$|"
    r"\.(?:pem|key|p12|pfx|jks|keystore)$")
_SECRET_NAME_RE = re.compile(
    r"^(?:[a-z0-9]+_)+(?:api_?)?(?:keys?|secret(?:_key)?|token|password|"
    r"passwd|access_key(?:_id)?|private_key)$|"
    r"^(?:api|access|aws|gcp|ssh|secret|private)[-_]keys?$|"
    r"^(?:[a-z0-9]+[-_])*(?:credentials?|creds)$|^service[-_]accounts?$")
_ENV_TOKEN_RE = re.compile(r"(?:^|[._-])env(?:ironment)?(?:$|[._~-]|\d)")
_SCRIPT_RE = re.compile(
    r"\.(?:php\d?|phtml|phar|phps|inc|asp|aspx|ashx|asmx|axd|jsp|jspx|"
    r"action|do|cgi|cfm|cfc|xgi|rsp)$|\.php\d?\.")
_SCRIPT_NAMES = frozenset({
    "phpinfo", "cgi-bin", "phpunit", "eval-stdin", "system32", "win.ini",
    "boot.ini"})
_SCRIPT_ROOTS = frozenset({
    "wp", "wordpress", "xmlrpc", "wlwmanifest.xml", "phpmyadmin", "myadmin",
    "mysqladmin", "pma", "adminer", "boaform", "hnap1", "_ignition",
    "_profiler"})
_ENV_NAME_RE = re.compile(r"^[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+$")
_PATH_SIGNATURES = (
    (re.compile(
        r"^(?:latest/(?:meta-data|user-data|dynamic|api/token)|"
        r"computemetadata/v1|metadata/(?:identity|instance|v1)|opc/v[12]/|"
        r"v1/sys/|v1/auth/token|etc/(?:passwd|shadow)|proc/self/|"
        r"app/etc/(?:local\.xml|env\.php)|"
        r"config/(?:environments|initializers)/[^/]+\.rb$|"
        r"(?:api/)?v\d+/secrets?(?:/|$))"), "secret"),
    (re.compile(r"(?:^|/)\.(?:git|svn|hg|bzr)/"), "secret"),
    (re.compile(r"^api/[^/]+\.php"), "script"),
    (re.compile(
        r"(?:^|/)api/session/properties$|^bin/(?:get|view)/main/|"
        r"^rest/api/(?:\d|latest)|^media/system/js/|^api/users-permissions|"
        r"^api/action/datastore_search|^api/(?:rds|db|data)/(?:execute|query)$|"
        r"^(?:api/)?(?:v1/)?(?:aws/)?bedrock(?:-?runtime)?(?:/|$)|"
        r"^(?:api/)?v1/models$|^(?:host-)?manager/(?:html|text|status)|"
        r"^(?:api/)?system/system/"), "cms"),
    (re.compile(
        r"^(?:(?:api/)?actuator(?:/|$)|debug/(?:pprof|vars|default)|"
        r"v1/(?:agent|catalog|status|health)/|v[23]/kv/|v1\.\d+/|"
        r"_(?:all|alias|aliases|analyze|cat|cluster|component_template|count|"
        r"data_stream|explain|index_template|license|mapping|ml|monitoring|"
        r"nodes|rank_eval|rollup|scripts|search|security|snapshot|sql|stats|"
        r"tasks|template|termvectors|transform|watcher|xpack)(?:/|$)|"
        r"(?:rabbitmq|grafana|kibana|jenkins|airflow|horizon)/(?:api|app)/|"
        r"(?:computer|job)/api/|(?:spark|hdfs|yarn|mapreduce)/"
        r"(?:master|worker|cluster)|api/0/|api/v4/ci/|rails/|"
        r"sites/default/|app/etc/|app/rest/server)"), "cms"),
    (re.compile(r"^var/log/"), "backup"),
)
_CMS_NAMES = frozenset({
    "wordpress", "wlwmanifest.xml", "xmlrpc", "joomla", "typo3", "magento",
    "drupal", "adminer", "myadmin", "mysqladmin", "boaform", "hnap1",
    "actuator", "server-status", "server-info", "solr", "geoserver",
    "jmx-console", "web-console", "telescope", "_ignition", "_profiler",
    "owa", "ecp", "+cscoe+", "cpanel", "webmail", "roundcube", "elfinder",
    "fckeditor", "ckfinder", "kcfinder", "umbraco",
    "laravel-filemanager", "logonpoint", "cfide", "vropspluginui",
    "helpdesk.woa", "functionrouter", "programexport", "cruise_config",
    "simzysh", "scripttext", "administrativemonitor"})
_CMS_ROOTS = frozenset({
    "administrator", "_debugbar", "__debug__", "_wdt", "silk", "fpm-status",
    "fpm-ping", "fpm-status.html", "awstats", "phpmailer", "jolokia",
    "webinterface", "fortisandbox", "xwiki", "resin-doc", "brightmail",
    "jenkins", "grafana", "kibana", "rabbitmq", "airflow", "jupyter",
    "pgadmin", "phppgadmin", "elmah", "nacos", "druid", "zabbix", "nagios",
    "cacti", "webmin", "portainer", "hudson"})
_CMS_PREFIXES = ("wp-", "phpmyadmin", "pma", "axis2", "adminer", "cpanel",
                 "___proxy_subdomain")
_BACKUP_RE = re.compile(
    r"(?:\.(?:sql|sqlite3?|db|mdb|dump|bak|backup|bkp|old|orig|save|swp|swo|"
    r"swn|tar|tgz|tbz2?|txz|gz|zip|rar|7z|bz2|xz|zst|war|"
    r"log(?:\.\d+)?)|~|^error_log|"
    r"^(?:db|database|dump|backup|users)\.(?:csv|json|xml))$")
_CONFIG_EXT_RE = re.compile(
    r"\.(?:ya?ml|ini|conf|cfg|cnf|toml|properties|config|dist|settings)$")
_CONFIG_STEM_RE = re.compile(
    r"(?:config|(?:^|[._-])(?:conf|settings|appsettings[^.]*|deploy|"
    r"deployment|parameters)[._-])")
_CONFIG_NAMES = frozenset({
    "env", "config", "configuration", "package.json", "package-lock.json",
    "composer.json", "composer.lock", "yarn.lock", "installed.json",
    "auth.json", "db.json", "database.json", "firebase.json", "gemfile",
    "gemfile.lock", "dockerfile", "web.config", ".dockerenv", ".gitmodules",
    ".gitignore", ".gitattributes", ".circleci", ".github", ".gitlab",
    "agents.md", "claude.md", ".cursorrules", "go.mod", "go.sum",
    "cargo.toml", "cargo.lock", "pipfile", "pipfile.lock", "requirements.txt",
    "makefile", "gnumakefile", "rakefile", "procfile", "gruntfile.js",
    "gulpfile.js", "build.gradle", "build.gradle.kts", "pom.xml", "bun.lock",
    "bun.lockb", "npm-shrinkwrap.json", "manage.py", "setup.py", "wsgi.py",
    "asgi.py", "application.py", "settings.py", "node_modules", "venv",
    "virtualenv", "__pycache__", "htmlcov", "flask_session", "flask_sessions",
    "swagger", "swagger.json", "swagger-ui", "swagger-ui.html",
    "swagger-resources", "api-docs", "openapi.json", "graphiql",
    "swagger-config", "graphql", "gql", "redoc", "altair", "jenkinsfile",
    "build.sh", "ci.sh", "deploy.sh", "cmakelists.txt", "coverage.xml",
    "crossdomain.xml", "clientaccesspolicy.xml", "vercel.json", "host.json",
    "server.js", "postman.json"})
_CONFIG_DIRS = frozenset({
    "node_modules", "swagger-resources", "swagger-ui", "api-docs",
    "__pycache__", "venv"})
_RECON_ROOTS = frozenset({
    "keys", "tokens", "secret", "secrets", "private", "certs", "cert", "ssl",
    "tls", "backup", "backups", "bak", "old", "dump", "dumps", "db",
    "database", "sql", "mysql", "mongo", "mongodb", "redis", "redis-cli",
    "kafka", "hdfs", "spark", "mapreduce", "prometheus", "sentry", "ansible",
    "puppet", "chef", "salt", "terraform", "docker", "console", "debug",
    "shell", "cmd", "terminal", "invoke", "wsgi", "wsgi-scripts", "runtime",
    "staticfiles", "collected_static", "collectedstatic", "site_media",
    "aws_s3_bucket", "aws-codecommit", "bedrockruntime", "notebook",
    "notebooks", "lab", "cluster", "hosts", "smartportal", "backoffice",
    "panel", "internal", "exportfile", "fetch", "proxy", "render",
    "screenshot", "thumbnail", "serverinfo", "serverstatus", "systeminfo",
    "buildinfo", "build-info", "pinfo", "version", "tmp", "var", "etc",
    "log", "_next", "blog2", "site1", "elasticsmtp", "smtp", "ses",
    "ses-smtp", "mailgun", "mailjet", "mailchimp", "mailersend", "sendgrid",
    "sendinblue", "sparkpost", "postmark", "resend", "brevo", "whoami",
    "who-am-i", "metrics", "secure", "script", "soap", "startup", "vendor",
    "uploads", "theme", "themes", "horizon", "computer", "job", "jobs",
    "playground", "models", "foundation-models", "yarn", "ncfindweb",
    "actpt_5g.data", "siteinfo.json", "statistics.json", "workflow.json"})
_RECON_FILE_RE = re.compile(
    r"^(?:readme|changelog|changes|history|license|install|upgrade|"
    r"release[-_]?notes|todo|version|build[-_]?info|contributing|authors)\."
    r"(?:md|txt|json|html?|rst)$|^manifest\.in$|^reconx-")
_ALLOW_CACHE = {"key": None, "networks": ()}


def _now():
    return int(Date.now())


def _file_kinds(segment, first, last):
    if _SECRET_RE.match(segment):
        return {"secret"}
    dotted = "." in segment.strip(".")
    if dotted and (_SECRET_FILE_RE.search(segment)
                   or _ENV_TOKEN_RE.search(segment)):
        return {"secret"}
    if last and not dotted and _SECRET_NAME_RE.match(segment):
        return {"secret"}
    found = set()
    if (_SCRIPT_RE.search(segment) or segment in _SCRIPT_NAMES
            or segment.startswith(("phpinfo", "php_info"))
            or segment in ("..", ".") or "..;" in segment
            or "\ufffd" in segment or re.match(r"^[a-z]:$", segment)):
        found.add("script")
    if first and (segment in _SCRIPT_ROOTS or segment.startswith("wp-")):
        found.add("script")
    if (segment in _CMS_NAMES or segment.startswith(_CMS_PREFIXES)
            or (first and segment in _CMS_ROOTS)):
        found.add("cms")
    if last and _BACKUP_RE.search(segment):
        found.add("backup")
    if segment.startswith(".") and segment not in ("..", "."):
        found.add("config")
    if segment in _CONFIG_DIRS or segment.startswith("swagger"):
        found.add("config")
    if first and (segment in _RECON_ROOTS or _RECON_FILE_RE.search(segment)):
        found.add("recon")
    if last and (segment in _CONFIG_NAMES or _CONFIG_EXT_RE.search(segment)
                 or (dotted and _CONFIG_STEM_RE.search(segment))):
        found.add("config")
    return found


def classify(path):
    text = str(path or "")
    path = unquote(text)
    encoded_dot = "%2e" in (text + path).lower()
    if "%" in path:
        path = unquote(path)
    if not path.startswith("/"):
        return ""
    raw = [segment.strip() for segment in path.split("/") if segment.strip()]
    segments = [segment.lower() for segment in raw]
    if not segments or segments[0] in _SKIP_FIRST:
        return ""
    if segments[0] != "api" and len(segments) >= 3 and (
            segments[2] in _REPO_FAMILIES):
        return ""
    joined = "/".join(segments)
    found = {kind for pattern, kind in _PATH_SIGNATURES
             if pattern.search(joined)}
    if encoded_dot:
        found.add("script")
    if segments[0] == "api" and (len(segments) > 3 or (
            len(segments) > 1 and segments[1] in _API_SKIP)):
        segments = []
    if "secret" in found or (segments and len(raw) != 2
                             and _ENV_NAME_RE.match(raw[-1])):
        return "secret"
    for index, segment in enumerate(segments):
        found |= _file_kinds(segment, index == 0, index == len(segments) - 1)
        if "secret" in found:
            return "secret"
    if not found:
        return ""
    return max(found, key=lambda kind: (WEIGHTS[kind], kind))


def mode(env):
    if instance_privacy.active(env):
        return "off"
    value = str(getattr(env, MODE_ENV, "") or "").strip().lower()
    return value if value in MODES else "ban"


def _allowed_networks(env):
    raw = str(getattr(env, ALLOW_ENV, "") or "")
    if _ALLOW_CACHE["key"] == raw:
        return _ALLOW_CACHE["networks"]
    networks = []
    for item in raw.replace(";", ",").split(","):
        try:
            networks.append(ipaddress.ip_network(item.strip(), strict=False))
        except ValueError:
            continue
    _ALLOW_CACHE.update(key=raw, networks=tuple(networks))
    return _ALLOW_CACHE["networks"]


def parse_address(value):
    text = str(value or "").strip().strip("[]")
    try:
        address = ipaddress.ip_address(text.split("%")[0])
    except ValueError:
        return None
    return getattr(address, "ipv4_mapped", None) or address


def ban_target(env, value):
    address = parse_address(value)
    if address is None or not address.is_global:
        return ""
    for network in _CLOUDFLARE + _allowed_networks(env):
        if address.version == network.version and address in network:
            return ""
    return _target_key(address)


def _target_key(value):
    address = parse_address(value)
    if address is None:
        return ""
    if address.version == 6:
        return str(ipaddress.ip_network(
            "%s/%d" % (address, IPV6_PREFIX), strict=False))
    return str(address)


def _local():
    table = globals().get("_probe_ban_local")
    return table if isinstance(table, dict) else None


def _remember(target, now):
    table = _local()
    if table is None:
        return
    if len(table) >= MAX_LOCAL:
        for key, until in list(table.items()):
            if until <= now:
                del table[key]
        if len(table) >= MAX_LOCAL:
            table.clear()
    table[target] = now + LOCAL_TTL_MS


def _forget(target):
    table = _local()
    if table is not None:
        table.pop(target, None)


def locally_banned(env, ip, now=None):
    table = _local()
    if not table or mode(env) != "ban":
        return False
    key = _target_key(ip)
    until = table.get(key, 0) if key else 0
    if not until:
        return False
    if until <= int(now or _now()):
        table.pop(key, None)
        return False
    return True


def denied(env, request, path):
    if path in EXEMPT_PATHS or path.startswith(EXEMPT_PREFIXES):
        return None
    try:
        ip = request.headers.get("cf-connecting-ip")
    except Exception:
        return None
    if not locally_banned(env, ip):
        return None
    return Response("Forbidden\n", status=403, headers={
        "content-type": "text/plain; charset=utf-8",
        "cache-control": "no-store"})


def rule_id_for(visitor):
    return RULE_PREFIX + str(visitor)[:32]


def _load_paths(text):
    try:
        value = json.loads(text or "[]")
    except ValueError:
        return []
    if not isinstance(value, list):
        return []
    return [
        [str(item[0])[:PATH_MAX], str(item[1])]
        for item in value
        if isinstance(item, list) and len(item) == 2
        and str(item[1]) in WEIGHTS][:MAX_PATHS]


def score_of(paths):
    return sum(WEIGHTS.get(kind, 0) for _path, kind in paths)


def kinds_of(paths):
    return ",".join(sorted({kind for _path, kind in paths},
                           key=lambda kind: (-WEIGHTS[kind], kind)))


def browser_initiated(fetch_site):
    return str(fetch_site or "").strip().lower() not in ("", "none")


async def observe(env, path, client, now=None, fetch_site=""):
    try:
        kind = classify(path)
        if not kind or browser_initiated(fetch_site):
            return ""
        current = mode(env)
        if current == "off":
            return ""
        client = client or {}
        target = ban_target(env, client.get("ip"))
        if not target:
            return ""
        now = int(now or _now())
        await ensure_schema(env)
        visitor = await blind_index(env, VISITOR_PREFIX + target)
        row = dict(await d1_first(
            env,
            "SELECT window_start,paths,bans,last_ban_at,banned_until "
            "FROM probe_visitors WHERE visitor_bi=?", visitor) or {})
        if int(row.get("banned_until") or 0) > now:
            if current == "ban":
                _remember(target, now)
            return "banned"
        paths = _load_paths(row.get("paths"))
        window_start = int(row.get("window_start") or 0)
        if not row or now - window_start >= WINDOW_MS:
            paths, window_start = [], now
        key = str(path).lower()[:PATH_MAX]
        if any(seen == key for seen, _kind in paths):
            return "seen"
        if len(paths) < MAX_PATHS:
            paths.append([key, kind])
        score = score_of(paths)
        await d1_run(
            env,
            "INSERT INTO probe_visitors "
            "(visitor_bi,window_start,score,paths,last_seen) VALUES (?,?,?,?,?) "
            "ON CONFLICT(visitor_bi) DO UPDATE SET "
            "window_start=excluded.window_start,score=excluded.score,"
            "paths=excluded.paths,last_seen=excluded.last_seen",
            visitor, window_start, score, json.dumps(paths), now)
        if score < BAN_SCORE:
            return "scored"
        return await _ban(env, visitor, target, client, paths, row, now,
                          current == "ban")
    except Exception as error:
        print("probe ban observe failed: %r" % (error,))
        return ""


async def _ban(env, visitor, target, client, paths, row, now, enforce):
    repeat = int(row.get("bans") or 0) > 0 and (
        now - int(row.get("last_ban_at") or 0) < REPEAT_WITHIN_MS)
    until = now + (REPEAT_BAN_MS if repeat else FIRST_BAN_MS)
    if enforce:
        active = await d1_first(
            env,
            "SELECT COUNT(*) AS n FROM probe_bans "
            "WHERE enforced=1 AND expires_at>? AND rule_id<>?",
            now, rule_id_for(visitor))
        if int(dict(active or {}).get("n") or 0) >= MAX_ACTIVE_BANS:
            enforce = False
    rule_id = rule_id_for(visitor)
    try:
        sealed = await encrypt_row(env, {
            "target": target,
            "ip": str(client.get("ip") or ""),
            "country": str(client.get("country") or ""),
            "ua": str(client.get("ua") or "")})
    except Exception:
        sealed = ""
    kinds = kinds_of(paths)
    await d1_run(
        env,
        "INSERT INTO probe_bans "
        "(rule_id,visitor_bi,client,kinds,paths,score,enforced,created_at,"
        "expires_at) VALUES (?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(rule_id) DO UPDATE SET client=excluded.client,"
        "kinds=excluded.kinds,paths=excluded.paths,score=excluded.score,"
        "enforced=excluded.enforced,created_at=excluded.created_at,"
        "expires_at=excluded.expires_at,lifted_at=0,lifted_by=''",
        rule_id, visitor, sealed, kinds, json.dumps(paths), score_of(paths),
        1 if enforce else 0, now, until)
    if enforce:
        await d1_run(
            env,
            "INSERT INTO request_block_rules "
            "(rule_id,match_type,pattern,enabled,priority,note,created_at,"
            "updated_at) VALUES (?,?,?,1,?,?,?,?) ON CONFLICT DO NOTHING",
            rule_id, "cidr" if "/" in target else "ip", target, RULE_PRIORITY,
            ("auto probe ban: %s, score %d" % (kinds, score_of(paths)))[:200],
            now, now)
        _remember(target, now)
        await d1_run(
            env,
            "UPDATE probe_visitors SET bans=bans+1,last_ban_at=?,"
            "banned_until=?,paths='[]',score=0,window_start=? "
            "WHERE visitor_bi=?",
            now, until, now, visitor)
        return "banned"
    await d1_run(
        env,
        "UPDATE probe_visitors SET paths='[]',score=0,window_start=? "
        "WHERE visitor_bi=?",
        now, visitor)
    return "observed"


async def sweep(env, now=None):
    await ensure_schema(env)
    now = int(now or _now())
    enforcing = mode(env) == "ban"
    await d1_run(
        env,
        "DELETE FROM request_block_rules WHERE substr(rule_id,1,?)=? "
        "AND (? OR rule_id NOT IN (SELECT rule_id FROM probe_bans "
        "WHERE enforced=1 AND lifted_at=0 AND expires_at>?))",
        len(RULE_PREFIX), RULE_PREFIX, 0 if enforcing else 1, now)
    await d1_run(
        env, "DELETE FROM probe_bans WHERE expires_at<?", now - BAN_HISTORY_MS)
    await d1_run(
        env,
        "DELETE FROM probe_visitors WHERE last_seen<? AND banned_until<?",
        now - VISITOR_RETAIN_MS, now)
    table = _local()
    if table:
        for key, until in list(table.items()):
            if until <= now or not enforcing:
                del table[key]
    return True


async def lift(env, rule_id, account, now=None):
    rule_id = str(rule_id or "")
    if not rule_id.startswith(RULE_PREFIX) or len(rule_id) > 64:
        return "ban_invalid"
    now = int(now or _now())
    row = await d1_first(
        env,
        "SELECT visitor_bi,client,kinds,score,expires_at,lifted_at "
        "FROM probe_bans WHERE rule_id=?", rule_id)
    if not row:
        return "ban_missing"
    row = dict(row)
    await d1_run(
        env,
        "UPDATE probe_bans SET lifted_at=?,lifted_by=?,expires_at=? "
        "WHERE rule_id=?",
        now, str(account or "")[:80], now, rule_id)
    await d1_run(env, "DELETE FROM request_block_rules WHERE rule_id=?",
                 rule_id)
    await d1_run(
        env,
        "UPDATE probe_visitors SET bans=0,last_ban_at=0,banned_until=0,"
        "paths='[]',score=0,window_start=? WHERE visitor_bi=?",
        now, str(row.get("visitor_bi") or ""))
    client = await _open(env, row.get("client"))
    if client.get("target"):
        _forget(str(client["target"]))
    await _audit_sensitive_action(
        env, account, "admin.console_lift_probe_ban", "probe_bans", rule_id,
        "success", {"kinds": str(row.get("kinds") or ""),
                    "score": int(row.get("score") or 0)})
    return "ban_lifted"


async def _open(env, sealed):
    if not sealed:
        return {}
    try:
        value = await decrypt_row(env, sealed)
    except Exception:
        return {}
    return value if isinstance(value, dict) else {}


def _status(row, now):
    if int(row.get("lifted_at") or 0):
        return "Lifted", ""
    if int(row.get("expires_at") or 0) <= now:
        return "Expired", ""
    if not int(row.get("enforced") or 0):
        return "Observed", "warn"
    return "Banned", "bad"


async def render(env, form_for, client_cell, now=None):
    shell = _admin_shell
    now = int(now or _now())
    rows = await d1_all(
        env,
        "SELECT rule_id,client,kinds,paths,score,enforced,created_at,"
        "expires_at,lifted_at,lifted_by FROM probe_bans "
        "ORDER BY created_at DESC LIMIT 100")
    counts = dict(await d1_first(
        env,
        "SELECT COALESCE(SUM(CASE WHEN enforced=1 AND lifted_at=0 "
        "AND expires_at>? THEN 1 ELSE 0 END),0) AS active,"
        "COALESCE(SUM(CASE WHEN created_at>=? THEN 1 ELSE 0 END),0) AS today "
        "FROM probe_bans", now, now - DAY_MS) or {})
    body = []
    for row in rows or []:
        row = dict(row)
        client = await _open(env, row.get("client"))
        paths = _load_paths(row.get("paths"))
        status, tone = _status(row, now)
        kinds = [kind for kind in str(row.get("kinds") or "").split(",")
                 if kind in LABELS]
        shown = "<br>".join(_html_escape(path) for path, _kind in paths[:4])
        if len(paths) > 4:
            shown += '<br><span class="muted">and %d more</span>' % (
                len(paths) - 4)
        if client.get("target"):
            client = dict(client, ip=client["target"])
        active = status in ("Banned", "Observed")
        body.append(
            "<tr>%s<td>%s</td><td class=\"mono\">%s</td>"
            '<td class="r">%d</td><td>%s</td><td>%s</td><td>%s</td>'
            '<td class="r">%s</td></tr>' % (
                client_cell(client),
                " ".join(shell.pill(LABELS[kind]) for kind in kinds),
                shown,
                int(row.get("score") or 0),
                shell.time_html(row.get("created_at"), now),
                _html_escape(shell.utc_label(int(row.get("expires_at") or 0))),
                shell.pill(status, tone)
                + ((' <span class="muted">by %s</span>' % _html_escape(
                    row.get("lifted_by"))) if row.get("lifted_by") else ""),
                form_for(row) if active else ""))
    table = shell.table(
        "<th>Visitor</th><th>Probed for</th><th>Paths</th>"
        '<th class="r">Score</th><th>Banned</th><th>Until</th>'
        '<th>Status</th><th class="r">Actions</th>',
        body, "No visitor has been banned for probing.")
    current = mode(env)
    state = {
        "ban": "On: a ban is enforced on every core within a minute.",
        "observe": "Observe only: bans are recorded but nothing is blocked.",
        "off": "Off: probe paths are not scored.",
    }[current]
    return shell.card(
        title="Auto bans",
        description=(
            "%s Each 404 on a path that only a scanner asks for (secrets such "
            "as .env, server scripts such as .php, CMS panels, backups, "
            "config files, recon paths) scores its visitor; %d points within "
            "a day bans the address for a week, a month when it comes back "
            "within 90 days. %s active, %s in the last 24 hours." % (
                state, BAN_SCORE, shell.format_int(counts.get("active") or 0),
                shell.format_int(counts.get("today") or 0))),
        table=table, cls="not-found-bans")
