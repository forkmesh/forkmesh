"""Shared chrome for every administrator page.

The admin console, product analytics, acquisition, the hackathon admin and
the support inboxes used to be separate pages with separate looks and no way
to move between them. They now render inside this one shell: a grouped
sidebar that links every section, a top bar, and one set of cards, tables,
pills and charts drawn from GitHub's light and dark palettes.

The shell is script-free on purpose. The drawer below 768px is a hidden
checkbox and labels, the theme switch is a form that posts to
``/admin/theme`` and gets a cookie back, and every chart is inline SVG or
plain blocks. Pages that already run under ``default-src 'none'`` therefore
keep that policy after moving into the shell.

Nothing here reads the database except ``nav_badges``, which is two or three
indexed counts and never raises. Pages own their data and their auth; the
shell only draws.
"""

import html as _html
from urllib.parse import quote, parse_qs


def _bind_runtime(runtime):
    """Supply the entrypoint primitives the shell uses (Response, D1, env)."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


ADMIN_THEME_PATH = "/admin/theme"
ADMIN_THEME_COOKIE = "forkmesh_admin_theme"
ADMIN_THEME_TTL_S = 365 * 24 * 60 * 60
ADMIN_THEME_MAX_FORM_BYTES = 4096
THEMES = ("light", "dark", "system")
THEME_LABELS = {"light": "Light theme", "dark": "Dark theme",
                "system": "System theme"}
LOGO = "/assets/logo.svg"
PRODUCT_ANALYSIS_HREF = "/product-analysis"
ACQUISITION_HREF = "/admin/acquisition"
HACKATHON_HREF = "/admin/hackathon"
MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun",
          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")

# (group label or None, ((key, label, icon, target), ...)). A target is
# ("console", view) for a view of the secret-path console, or ("path", href)
# for a page with its own route. Console links are only drawn when the
# console path is known to the page, so a page never guesses it.
NAV = (
    (None, (
        ("overview", "Overview", "grid", ("console", "overview")),
    )),
    ("Growth", (
        ("analytics", "Product analytics", "chart",
         ("path", PRODUCT_ANALYSIS_HREF)),
        ("acquisition", "Acquisition", "megaphone",
         ("path", ACQUISITION_HREF)),
        ("hackathon", "Hackathon", "trophy", ("path", HACKATHON_HREF)),
        ("marketing-email", "Marketing email", "send",
         ("console", "marketing-email")),
        ("ecosystem", "Ecosystem", "globe", ("console", "ecosystem")),
        ("instances", "Instances", "external", ("console", "instances")),
    )),
    ("People", (
        ("users", "Users", "users", ("console", "users")),
    )),
    ("Support", (
        ("bugs", "Bug reports", "bug", ("console", "bugs")),
        ("feedback", "Docs & World feedback", "message",
         ("console", "feedback")),
        ("agent-notes", "Agent notes", "send", ("console", "agent-notes")),
    )),
    ("Reliability", (
        ("errors", "Errors", "alert", ("console", "errors")),
        ("not-found", "404s", "search", ("console", "not-found")),
        ("installs", "Install diagnostics", "download",
         ("console", "installs")),
        # Moved to the Pings settings page (#347); administrators only.
        ("alerts", "Alert delivery", "bell",
         ("path", "/dashboard/settings/notifications#admin-alerts")),
    )),
    ("Security", (
        ("ids", "Intrusion detection", "shield", ("console", "ids")),
    )),
    ("Platform", (
        ("controls", "Controls", "sliders", ("console", "controls")),
        ("update", "Update ForkMesh", "refresh", ("console", "update")),
        ("repo-addresses", "Repository addresses", "branch",
         ("console", "repo-addresses")),
        ("database", "Database", "database", ("console", "database")),
    )),
)
NAV_KEYS = tuple(key for _group, items in NAV for key, *_rest in items)


def esc(value):
    return _html.escape("" if value is None else str(value), quote=True)


def _int(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


# --- palette and components ---------------------------------------------------

_LIGHT = (
    "--bg:#ffffff;--canvas:#f6f8fa;--fg:#1f2328;--muted:#59636e;"
    "--faint:#818b98;--border:#d1d9e0;--border-soft:#e4e9ee;--hover:#eef1f4;"
    "--active:#e7ecf0;--accent:#0969da;--accent-fg:#ffffff;"
    "--accent-bg:#ddf4ff;--accent-strong:#0550ae;--second:#8c959f;"
    "--success:#1a7f37;--success-bg:#dafbe1;--danger:#cf222e;"
    "--danger-bg:#ffebe9;--attention:#9a6700;--attention-bg:#fff8c5;"
    "--done:#8250df;--done-bg:#fbefff;--tile:#0d1117;"
    "--scrim:rgba(31,35,40,.45);--code-bg:#f6f8fa")
_DARK = (
    "--bg:#0d1117;--canvas:#010409;--fg:#e6edf3;--muted:#8b949e;"
    "--faint:#6e7681;--border:#30363d;--border-soft:#21262d;--hover:#161b22;"
    "--active:#1f242c;--accent:#4493f8;--accent-fg:#ffffff;"
    "--accent-bg:#12233a;--accent-strong:#79c0ff;--second:#6e7681;"
    "--success:#3fb950;--success-bg:#12261b;--danger:#f85149;"
    "--danger-bg:#2d1618;--attention:#d29922;--attention-bg:#2b2111;"
    "--done:#a371f7;--done-bg:#231a36;--tile:#010409;"
    "--scrim:rgba(1,4,9,.6);--code-bg:#161b22")

STYLE = (
    ":root{" + _LIGHT + ";color-scheme:light}\n"
    "html.theme-dark{" + _DARK + ";color-scheme:dark}\n"
    "@media (prefers-color-scheme:dark){html:not(.theme-light){"
    + _DARK + ";color-scheme:dark}}\n"
    + """*{box-sizing:border-box}
html,body{margin:0}
body{background:var(--canvas);color:var(--fg);
  font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI","Noto Sans",
  Helvetica,Arial,sans-serif;-webkit-font-smoothing:antialiased}
a{color:var(--accent);text-decoration:none}
a:hover{text-decoration:underline}
:focus-visible{outline:2px solid var(--accent);outline-offset:2px}
svg.i{width:16px;height:16px;flex:none;display:inline-block;vertical-align:middle}
h1,h2,h3{margin:0;text-wrap:balance}
button,input,select,textarea{font:inherit;color:inherit}
code,.mono{font-family:ui-monospace,SFMono-Regular,"SF Mono",Menlo,Consolas,
  monospace;font-size:12.5px}
.num,table{font-variant-numeric:tabular-nums}
.muted{color:var(--muted)}
.nowrap{white-space:nowrap}
.sr{position:absolute;width:1px;height:1px;margin:-1px;padding:0;border:0;
  overflow:hidden;clip:rect(0 0 0 0);white-space:nowrap}

.ab{display:flex;min-height:100vh}
.ab-side{width:248px;flex:none;background:var(--bg);
  border-right:1px solid var(--border);position:sticky;top:0;height:100vh;
  display:flex;flex-direction:column;padding:14px 12px}
.ab-brand{display:flex;align-items:center;gap:10px;padding:4px 8px 16px}
.ab-brand img{width:32px;height:32px;border-radius:8px;background:var(--tile);
  padding:4px;object-fit:contain;flex:none}
.ab-brand b{display:block;font-weight:600;line-height:1.2;color:var(--fg)}
.ab-brand span{display:block;color:var(--muted);font-size:12px}
.ab-nav{flex:1;overflow-y:auto;display:flex;flex-direction:column;gap:1px;
  margin:0 -4px;padding:0 4px}
.ab-group{color:var(--muted);font-size:12px;font-weight:500;padding:14px 10px 4px}
.ab-item{display:flex;align-items:center;gap:10px;min-height:34px;padding:0 10px;
  border-radius:6px;color:var(--fg);font-weight:500;position:relative}
.ab-item svg{color:var(--muted)}
.ab-item:hover{background:var(--hover);text-decoration:none}
.ab-item.on{background:var(--accent-bg);color:var(--accent-strong)}
.ab-item.on svg{color:var(--accent)}
.ab-item.on::before{content:"";position:absolute;left:-12px;top:7px;bottom:7px;
  width:3px;border-radius:0 3px 3px 0;background:var(--accent)}
.ab-count{margin-left:auto;font-size:12px;font-weight:600;min-width:22px;
  height:20px;padding:0 7px;border-radius:999px;display:inline-flex;
  align-items:center;justify-content:center;background:var(--hover);
  color:var(--muted)}
.ab-count.bad{background:var(--danger-bg);color:var(--danger)}
.ab-count.warn{background:var(--attention-bg);color:var(--attention)}
.ab-count.ok{background:var(--success-bg);color:var(--success)}
.ab-foot{border-top:1px solid var(--border);padding-top:12px;margin-top:8px;
  display:flex;flex-direction:column;gap:10px}
.seg{display:flex;border:1px solid var(--border);border-radius:6px;
  background:var(--bg);overflow:hidden;margin:0}
.seg button,.seg a{flex:1;min-height:30px;border:0;background:transparent;
  color:var(--muted);cursor:pointer;display:flex;align-items:center;
  justify-content:center;gap:6px;font-size:13px;padding:0 10px;margin:0;
  white-space:nowrap}
.seg button+button,.seg a+a{border-left:1px solid var(--border)}
.seg button:hover,.seg a:hover{background:var(--hover);color:var(--fg);
  text-decoration:none}
.seg .on,.seg [aria-pressed="true"],.seg [aria-current="true"]{
  background:var(--active);color:var(--fg);font-weight:500}
.ab-main{flex:1;min-width:0;display:flex;flex-direction:column}
.ab-top{min-height:60px;display:flex;align-items:center;gap:12px;padding:0 28px;
  background:var(--bg);border-bottom:1px solid var(--border);position:sticky;
  top:0;z-index:20}
.ab-top .grow{flex:1}
.ab-menu,.ab-close,.ab-scrim{display:none}
.ab-search{display:flex;align-items:center;gap:8px;height:36px;
  width:min(380px,100%);padding:0 10px;border:1px solid var(--border);
  border-radius:8px;background:var(--canvas);color:var(--muted);margin:0}
.ab-search input{border:0;background:transparent;outline:0;flex:1;min-width:0;
  color:var(--fg);height:34px}
.ab-search input::placeholder{color:var(--muted)}
.chip{display:inline-flex;align-items:center;gap:6px;min-height:24px;
  padding:0 9px;border-radius:999px;font-size:12px;font-weight:500;
  border:1px solid var(--border);color:var(--muted);background:var(--bg);
  white-space:nowrap}
.avatar{width:32px;height:32px;border-radius:50%;background:var(--accent-bg);
  color:var(--accent-strong);display:inline-flex;align-items:center;
  justify-content:center;font-weight:600;font-size:13px;flex:none}
.ab-content{padding:24px 28px 64px;max-width:1360px;width:100%}

.head{display:flex;align-items:flex-end;justify-content:space-between;gap:16px;
  flex-wrap:wrap;margin:0 0 20px}
.crumb{color:var(--muted);font-size:13px;margin:0 0 2px;display:flex;gap:6px;
  align-items:center;flex-wrap:wrap}
.head h1{font-size:24px;font-weight:600;line-height:1.25}
.head p{margin:2px 0 0;color:var(--muted);max-width:72ch}
.actions{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:0}
.btn{min-height:34px;padding:0 12px;border-radius:6px;
  border:1px solid var(--border);background:var(--bg);color:var(--fg);
  font-weight:500;font-size:13px;display:inline-flex;align-items:center;
  justify-content:center;gap:7px;cursor:pointer;white-space:nowrap;margin:0}
.btn:hover{background:var(--hover);text-decoration:none}
.btn-primary{background:var(--accent);border-color:var(--accent);
  color:var(--accent-fg)}
.btn-primary:hover{background:var(--accent-strong);border-color:var(--accent-strong)}
.btn-danger{color:var(--danger)}
.btn-danger:hover{background:var(--danger-bg);border-color:var(--danger)}
.btn-sm{min-height:28px;padding:0 9px;font-size:12px}
.btn-block{width:100%}
.btn[disabled]{opacity:.55;cursor:not-allowed}
.input,.select,.textarea,
input[type=text],input[type=password],input[type=email],input[type=search],
input[type=date],input[type=datetime-local],input[type=number],select,textarea{
  min-height:34px;padding:0 10px;border:1px solid var(--border);
  border-radius:6px;background:var(--bg);color:var(--fg);min-width:0}
textarea,.textarea{padding:10px;resize:vertical;width:100%}
input::placeholder,textarea::placeholder{color:var(--faint)}
.ab-search input{border:0;background:transparent;padding:0}
.field{display:flex;flex-direction:column;gap:6px;min-width:0}
.field label,.field .label{font-size:13px;font-weight:500}
.hint{font-size:12px;color:var(--muted)}
.form-grid{display:grid;gap:14px;grid-template-columns:repeat(2,minmax(0,1fr))}
.form-grid .wide{grid-column:1/-1}
.form-row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:0}
.form-row .grow{flex:1 1 180px}
form.inline{display:inline-flex;margin:0}

.grid{display:grid;gap:16px;margin:0 0 16px}
.cols-4{grid-template-columns:repeat(4,minmax(0,1fr))}
.cols-3{grid-template-columns:repeat(3,minmax(0,1fr))}
.cols-2{grid-template-columns:repeat(2,minmax(0,1fr));align-items:start}
.split{grid-template-columns:minmax(0,1.75fr) minmax(0,1fr);align-items:start}
.stack{display:flex;flex-direction:column;gap:16px;min-width:0}

.card{background:var(--bg);border:1px solid var(--border);border-radius:12px;
  min-width:0;margin:0}
.grid>.card,.stack>.card{margin:0}
.card+.card{margin-top:16px}
.grid>.card+.card,.stack>.card+.card{margin-top:0}
.card-h{display:flex;align-items:center;justify-content:space-between;gap:12px;
  padding:16px 18px 0;flex-wrap:wrap}
.card-h h2{font-size:15px;font-weight:600}
.card-h p{margin:2px 0 0;color:var(--muted);font-size:13px}
.card-b{padding:14px 18px 18px}
.card-t{overflow-x:auto;margin-top:12px;border-top:1px solid var(--border)}
.card-t:first-child{margin-top:0;border-top:0;border-radius:12px 12px 0 0}
.more{color:var(--muted);display:inline-flex;align-items:center;gap:4px;
  font-size:13px;white-space:nowrap}
.more:hover{color:var(--accent);text-decoration:none}

.kpi{padding:16px 18px;display:block;color:inherit}
a.kpi:hover{text-decoration:none;border-color:var(--accent)}
.kpi .l{display:flex;align-items:center;justify-content:space-between;gap:8px;
  font-weight:500}
.kpi .l svg{color:var(--accent)}
.kpi.bad .l svg{color:var(--danger)}
.kpi .n{font-size:30px;font-weight:600;line-height:1.1;margin:10px 0 6px;
  font-variant-numeric:tabular-nums;display:flex;align-items:center;gap:10px;
  flex-wrap:wrap;overflow-wrap:anywhere}
.kpi .w{font-size:12px;color:var(--muted)}
.kpi .c{font-size:13px;color:var(--muted);margin-top:8px}
.kpi .e{font-size:13px;color:var(--danger);margin-top:8px}

.pill{display:inline-flex;align-items:center;gap:4px;min-height:22px;
  padding:0 8px;border-radius:999px;font-size:12px;font-weight:600;
  white-space:nowrap;line-height:1;background:var(--hover);color:var(--muted)}
.pill.up,.pill.ok{background:var(--success-bg);color:var(--success)}
.pill.down,.pill.bad{background:var(--danger-bg);color:var(--danger)}
.pill.warn{background:var(--attention-bg);color:var(--attention)}
.pill.info{background:var(--accent-bg);color:var(--accent-strong)}
.pill.done{background:var(--done-bg);color:var(--done)}
.pill.line{background:transparent;border:1px solid var(--border);
  color:var(--muted);font-weight:500}
.pill svg{width:12px;height:12px}

table{border-collapse:collapse;width:100%}
th,td{text-align:left;padding:0 14px;vertical-align:middle}
thead th{height:40px;font-size:12px;font-weight:600;color:var(--muted);
  white-space:nowrap;border-bottom:1px solid var(--border);
  background:var(--canvas)}
tbody td,tbody th{height:48px;border-bottom:1px solid var(--border-soft)}
tbody th{font-weight:600}
tbody tr:last-child td,tbody tr:last-child th{border-bottom:0}
tbody tr:hover td,tbody tr:hover th{background:var(--hover)}
td.r,th.r{text-align:right}
tr.total td{font-weight:600;background:var(--canvas)}
th a{color:inherit}
th a.on,th a:hover{color:var(--fg);text-decoration:none}
.ellipsis{display:block;max-width:320px;overflow:hidden;text-overflow:ellipsis;
  white-space:nowrap}
.wrap-text{white-space:pre-wrap;overflow-wrap:anywhere}
input[type=checkbox]{width:15px;height:15px;accent-color:var(--accent);
  margin:0;vertical-align:middle}
.who{display:flex;align-items:center;gap:10px;min-width:0}
.who .avatar{width:28px;height:28px;font-size:12px}
.who b{display:block;font-weight:600;line-height:1.25;color:var(--fg)}
.who span{display:block;color:var(--muted);font-size:12px;line-height:1.3}
a.who:hover{text-decoration:none}
a.who:hover b{color:var(--accent)}

.tabs{display:flex;gap:2px;border-bottom:1px solid var(--border);
  margin:0 0 18px;overflow-x:auto}
.card>.tabs{padding:0 18px;margin:0}
.tabs a{padding:10px 12px;color:var(--muted);font-weight:500;
  border-bottom:2px solid transparent;margin-bottom:-1px;white-space:nowrap;
  display:inline-flex;gap:6px;align-items:center}
.tabs a:hover{color:var(--fg);text-decoration:none}
.tabs a[aria-current="page"]{color:var(--fg);border-color:var(--accent);
  font-weight:600}
.tabs .count{font-size:11px;background:var(--hover);border-radius:999px;
  padding:0 6px;color:var(--muted);font-weight:600}
.toolbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;
  padding:14px 18px 0;margin:0}
.toolbar .grow{flex:1}
.pager{display:flex;justify-content:space-between;align-items:center;gap:12px;
  padding:12px 18px;border-top:1px solid var(--border);color:var(--muted);
  font-size:13px;flex-wrap:wrap}
.banner{display:flex;gap:10px;align-items:flex-start;padding:10px 14px;
  border-radius:8px;border:1px solid var(--border);background:var(--bg);
  margin:0 0 16px;font-size:13px;white-space:pre-wrap;overflow-wrap:anywhere}
.banner.ok{border-color:var(--success);background:var(--success-bg)}
.banner.info{border-color:var(--accent);background:var(--accent-bg)}
.banner.warn{border-color:var(--attention);background:var(--attention-bg)}
.banner.bad{border-color:var(--danger);background:var(--danger-bg)}
.banner svg{margin-top:2px}
.empty{color:var(--muted);padding:18px;margin:0}
.card-b .empty{padding:6px 0}
.note{color:var(--muted);font-size:13px;margin:12px 0 0}

.chart svg{display:block;width:100%;height:auto;overflow:visible}
.g-line{stroke:var(--border-soft)}
.g-axis{fill:var(--muted);font-size:11px}
.s-accent{stroke:var(--accent)}
.s-second{stroke:var(--second)}
.f-accent{fill:var(--accent)}
.f-area{fill:var(--accent);fill-opacity:.1}
.f-second{fill:var(--second)}
.f-soft{fill:var(--active)}
.f-bg{fill:var(--bg)}
.f-ok{fill:var(--success)}
.t-ok{stroke:var(--success)}
.t-off{stroke:var(--border)}
.legend{display:flex;gap:18px;flex-wrap:wrap;color:var(--muted);font-size:12px;
  margin:10px 0 0;align-items:center}
.legend i{display:inline-block;width:16px;height:0;
  border-top:2px solid var(--accent);vertical-align:middle;margin-right:6px}
.legend i.dash{border-top:2px dashed var(--second)}
.legend i.box{height:10px;border:0;background:var(--accent);border-radius:2px}
.legend i.soft{height:10px;border:0;background:var(--active);border-radius:2px}
.legend svg.hatch{width:16px;height:10px;vertical-align:middle;margin-right:6px;
  border:1px solid var(--second);border-radius:2px}
.bigstat{display:flex;flex-direction:column;justify-content:center;gap:6px}
.bigstat .n{font-size:36px;font-weight:600;line-height:1;
  font-variant-numeric:tabular-nums}
.bigstat .w{color:var(--muted);font-size:13px}
.hero-chart{display:grid;grid-template-columns:200px minmax(0,1fr);gap:12px;
  align-items:center}
.segments{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));
  border:1px solid var(--border);border-radius:10px;margin-top:16px;
  overflow:hidden}
.segment{padding:12px 14px 0;border-right:1px solid var(--border-soft);
  display:flex;flex-direction:column;gap:2px;min-width:0}
.segment:last-child{border-right:0}
.segment .t{display:flex;align-items:center;gap:6px;font-size:12px;
  color:var(--muted)}
.segment .sw{width:10px;height:10px;border-radius:3px;flex:none}
.segment b{font-size:20px;font-weight:600;font-variant-numeric:tabular-nums}
.segment .bar{height:5px;margin:10px -14px 0}
.tone-accent{background:var(--accent)}
.tone-ok{background:var(--success)}
.tone-warn{background:var(--attention)}
.tone-second{background:var(--second)}
.tone-bad{background:var(--danger)}
.tone-off{opacity:.3}
.hours{display:grid;grid-template-columns:repeat(24,minmax(0,1fr));gap:3px;
  align-items:end;height:150px;padding-top:24px}
.hours span{display:block;border-radius:3px 3px 1px 1px;background:var(--active);
  min-height:3px;position:relative}
.hours span.peak{background:var(--danger)}
.hours span.peak em{position:absolute;bottom:calc(100% + 4px);left:50%;
  transform:translateX(-50%);font-style:normal;font-size:12px;font-weight:600;
  color:var(--fg)}
.hours-x{display:flex;justify-content:space-between;color:var(--muted);
  font-size:11px;margin-top:6px}
.gauge{display:flex;flex-direction:column;align-items:center;text-align:center}
.gauge svg{width:220px;max-width:100%;height:auto}
.gauge .n{font-size:34px;font-weight:600;line-height:1;margin-top:-58px;
  font-variant-numeric:tabular-nums}
.gauge .w{font-size:12px;color:var(--muted);margin:14px 0 10px}
.minis{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:1px;
  background:var(--border-soft);border-top:1px solid var(--border-soft);
  margin-top:14px;border-radius:0 0 12px 12px;overflow:hidden}
.minis.three{grid-template-columns:repeat(3,minmax(0,1fr))}
.mini{background:var(--bg);padding:12px 18px}
.mini b{display:block;font-size:20px;font-weight:600;
  font-variant-numeric:tabular-nums}
.mini span{font-size:12px;color:var(--muted)}
.hbar{height:8px;border-radius:999px;background:var(--hover);overflow:hidden;
  min-width:80px}
.hbar i{display:block;height:100%;background:var(--accent);border-radius:999px}
.hbar.bad i{background:var(--danger)}
.meter{display:grid;grid-template-columns:minmax(90px,140px) minmax(0,1fr) 56px;
  gap:10px;align-items:center;margin:8px 0;font-size:13px}
.meter span:last-child{text-align:right;font-variant-numeric:tabular-nums}
.spark{display:grid;grid-template-columns:repeat(24,1fr);gap:1px;align-items:end;
  width:120px;height:26px}
.spark i{display:block;background:var(--accent);min-height:2px;border-radius:1px}
.spark i.z{background:var(--border)}
.stack-bar{display:flex;height:12px;border-radius:3px;overflow:hidden;
  min-width:60px;border:1px solid var(--border);background:var(--bg)}
.stack-bar .a{background:var(--accent)}
.stack-bar svg{flex:1;height:100%;display:block}

.sev{display:inline-flex;align-items:center;gap:6px;font-size:12px;
  font-weight:500;white-space:nowrap}
.sev::before{content:"";width:8px;height:8px;border-radius:2px;
  background:var(--second)}
.sev.crash::before{background:var(--danger)}
.sev.high::before{background:var(--attention)}
.sev.normal::before{background:var(--accent)}
.src{display:inline-flex;min-height:20px;align-items:center;padding:0 7px;
  border-radius:999px;border:1px solid var(--border);font-size:11px;
  font-weight:600;white-space:nowrap}
.src.worker{color:var(--accent);border-color:var(--accent)}
.src.javascript{color:var(--done);border-color:var(--done)}
.src.desktop{color:var(--success);border-color:var(--success)}
.code{display:inline-flex;min-height:20px;align-items:center;padding:0 6px;
  border-radius:4px;font-weight:600;font-size:12px;background:var(--hover)}
.code.s5{background:var(--danger-bg);color:var(--danger)}
.code.s4{background:var(--attention-bg);color:var(--attention)}

.inbox{display:grid;grid-template-columns:minmax(0,420px) minmax(0,1fr)}
.inbox-list{border-right:1px solid var(--border);min-width:0}
.report{display:grid;grid-template-columns:4px minmax(0,1fr);gap:12px;
  padding:12px 16px 12px 0;border-bottom:1px solid var(--border-soft);
  color:var(--fg)}
.report:hover{background:var(--hover);text-decoration:none}
.report.on{background:var(--accent-bg)}
.report .stripe{border-radius:0 3px 3px 0;background:var(--second)}
.report .stripe.crash{background:var(--danger)}
.report .stripe.high{background:var(--attention)}
.report .stripe.normal{background:var(--accent)}
.report .msg{font-weight:500;display:-webkit-box;-webkit-line-clamp:2;
  -webkit-box-orient:vertical;overflow:hidden;overflow-wrap:anywhere}
.report .meta{display:flex;gap:8px;align-items:center;flex-wrap:wrap;
  color:var(--muted);font-size:12px;margin-top:4px}
.detail{padding:20px 24px;min-width:0}
.quote{margin:14px 0 18px;padding:14px 16px;border-radius:8px;
  background:var(--canvas);border:1px solid var(--border-soft);
  white-space:pre-wrap;overflow-wrap:anywhere;line-height:1.6}
dl.kv{display:grid;grid-template-columns:max-content minmax(0,1fr);margin:0;
  border:1px solid var(--border);border-radius:10px;overflow:hidden}
dl.kv dt,dl.kv dd{margin:0;padding:9px 14px;
  border-bottom:1px solid var(--border-soft)}
dl.kv dt{color:var(--muted);font-size:13px;background:var(--canvas)}
dl.kv dd{overflow-wrap:anywhere;min-width:0}
dl.kv dt:last-of-type,dl.kv dd:last-of-type{border-bottom:0}
dl.kv dd.mono{white-space:pre-wrap}

.switch{position:relative;display:inline-flex;align-items:center;gap:8px;
  cursor:pointer;font-size:13px;white-space:nowrap}
.switch input{position:absolute;opacity:0;width:1px;height:1px}
.switch .track{width:34px;height:20px;border-radius:999px;
  background:var(--border);position:relative;flex:none;transition:background .15s}
.switch .track::after{content:"";position:absolute;top:2px;left:2px;width:16px;
  height:16px;border-radius:50%;background:var(--bg);transition:transform .15s}
.switch input:checked+.track{background:var(--accent)}
.switch input:checked+.track::after{transform:translateX(14px)}
.switch input:focus-visible+.track{outline:2px solid var(--accent);
  outline-offset:2px}
.setting{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:16px;
  align-items:center;padding:16px 18px;border-top:1px solid var(--border-soft)}
.card-h+.setting{border-top:0}
.setting h3{font-size:14px;font-weight:600}
.setting p{margin:2px 0 0;color:var(--muted);font-size:13px;max-width:62ch}
.codebox{font-family:ui-monospace,Menlo,monospace;background:var(--code-bg);
  border:1px solid var(--border);border-radius:6px;padding:2px 8px;font-size:13px}
.split-db{display:grid;grid-template-columns:260px minmax(0,1fr);gap:16px;
  align-items:start}
.tlist{max-height:70vh;overflow-y:auto;padding:6px}
.tlist a{display:flex;align-items:center;gap:8px;min-height:32px;padding:0 10px;
  border-radius:6px;color:var(--fg);font-size:13px}
.tlist a:hover{background:var(--hover);text-decoration:none}
.tlist a.on{background:var(--accent-bg);color:var(--accent-strong);
  font-weight:600}
.tlist a .c{margin-left:auto;color:var(--muted);font-size:12px;
  font-variant-numeric:tabular-nums}
.tlist a.lock{color:var(--muted)}
.campaigns{display:flex;gap:8px;flex-wrap:wrap;margin:0 0 16px}
.camp{display:inline-flex;align-items:center;gap:8px;min-height:34px;
  padding:0 12px;border-radius:8px;border:1px solid var(--border);
  background:var(--bg);color:var(--fg);font-weight:500}
.camp:hover{background:var(--hover);text-decoration:none}
.camp.on{border-color:var(--accent);outline:1px solid var(--accent);
  outline-offset:-2px}
details.more-text summary{cursor:pointer;color:var(--accent);font-size:12px}
details.more-text[open] summary{margin-bottom:6px}

@media (max-width:1180px){
  .cols-4{grid-template-columns:repeat(2,minmax(0,1fr))}
  .split{grid-template-columns:minmax(0,1fr)}
  .hero-chart{grid-template-columns:minmax(0,1fr)}
}
@media (max-width:900px){
  .cols-2,.cols-3{grid-template-columns:minmax(0,1fr)}
  .inbox{grid-template-columns:minmax(0,1fr)}
  .inbox-list{border-right:0;border-bottom:1px solid var(--border)}
  .split-db{grid-template-columns:minmax(0,1fr)}
  .tlist{max-height:240px}
}
@media (max-width:767px){
  .ab{display:block}
  .ab-side{position:fixed;left:0;top:0;bottom:0;z-index:50;width:280px;
    max-width:86vw;height:auto;transform:translateX(-100%);
    transition:transform .2s ease}
  .ab-close{display:inline-flex;position:absolute;top:14px;right:12px;width:32px;
    height:32px;align-items:center;justify-content:center;
    border:1px solid var(--border);border-radius:6px;background:var(--bg);
    color:var(--muted);cursor:pointer}
  .ab-brand{padding-right:44px}
  .ab-menu{display:inline-flex;width:36px;height:36px;border-radius:8px;
    border:1px solid var(--border);background:var(--bg);color:var(--fg);
    align-items:center;justify-content:center;cursor:pointer;flex:none}
  .ab-scrim{display:block;position:fixed;inset:0;z-index:40;
    background:var(--scrim);opacity:0;visibility:hidden;
    transition:opacity .2s ease,visibility .2s}
  #ab-nav:checked~.ab-side{transform:none}
  #ab-nav:checked~.ab-scrim{opacity:1;visibility:visible}
  #ab-nav:focus-visible~.ab-main .ab-menu{outline:2px solid var(--accent);
    outline-offset:2px}
  .ab-top{padding:0 16px}
  .ab-top .chip{display:none}
  .ab-content{padding:18px 16px 48px}
  .cols-4{grid-template-columns:minmax(0,1fr)}
  .segments{grid-template-columns:repeat(2,minmax(0,1fr))}
  .segment:nth-child(2){border-right:0}
  .setting,.form-grid{grid-template-columns:minmax(0,1fr)}
  .head h1{font-size:21px}
}
@media (prefers-reduced-motion:reduce){*{transition:none!important;
  animation:none!important}}
""")

# Lucide line icons on a 24-unit grid, stroked in currentColor.
ICONS = {
    "grid": ('<rect width="7" height="7" x="3" y="3" rx="1"/>'
             '<rect width="7" height="7" x="14" y="3" rx="1"/>'
             '<rect width="7" height="7" x="14" y="14" rx="1"/>'
             '<rect width="7" height="7" x="3" y="14" rx="1"/>'),
    "chart": ('<path d="M3 3v18h18"/><path d="M18 17V9"/>'
              '<path d="M13 17V5"/><path d="M8 17v-3"/>'),
    "megaphone": ('<path d="m3 11 18-5v12L3 14v-3z"/>'
                  '<path d="M11.6 16.8a3 3 0 1 1-5.8-1.6"/>'),
    "trophy": ('<path d="M6 9H4.5a2.5 2.5 0 0 1 0-5H6"/>'
               '<path d="M18 9h1.5a2.5 2.5 0 0 0 0-5H18"/><path d="M4 22h16"/>'
               '<path d="M10 14.66V17c0 .55-.47.98-.97 1.21C7.85 18.75 7 20.24 7 22"/>'
               '<path d="M14 14.66V17c0 .55.47.98.97 1.21C16.15 18.75 17 20.24 17 22"/>'
               '<path d="M18 2H6v7a6 6 0 0 0 12 0V2Z"/>'),
    "users": ('<path d="M16 21v-2a4 4 0 0 0-4-4H6a4 4 0 0 0-4 4v2"/>'
              '<circle cx="9" cy="7" r="4"/><path d="M22 21v-2a4 4 0 0 0-3-3.87"/>'
              '<path d="M16 3.13a4 4 0 0 1 0 7.75"/>'),
    "bug": ('<path d="m8 2 1.88 1.88"/><path d="M14.12 3.88 16 2"/>'
            '<path d="M9 7.13v-1a3.003 3.003 0 1 1 6 0v1"/>'
            '<path d="M12 20c-3.3 0-6-2.7-6-6v-3a4 4 0 0 1 4-4h4a4 4 0 0 1 4 4v3c0 3.3-2.7 6-6 6"/>'
            '<path d="M12 20v-9"/><path d="M6.53 9C4.6 8.8 3 7.1 3 5"/>'
            '<path d="M6 13H2"/><path d="M3 21c0-2.1 1.7-3.9 3.8-4"/>'
            '<path d="M20.97 5c0 2.1-1.6 3.8-3.5 4"/><path d="M22 13h-4"/>'
            '<path d="M17.2 17c2.1.1 3.8 1.9 3.8 4"/>'),
    "message": '<path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/>',
    "alert": ('<path d="m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3"/>'
              '<path d="M12 9v4"/><path d="M12 17h.01"/>'),
    "download": ('<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/>'
                 '<path d="m7 10 5 5 5-5"/><path d="M12 15V3"/>'),
    "bell": ('<path d="M6 8a6 6 0 0 1 12 0c0 7 3 9 3 9H3s3-2 3-9"/>'
             '<path d="M10.3 21a1.94 1.94 0 0 0 3.4 0"/>'),
    "sliders": ('<line x1="21" x2="14" y1="4" y2="4"/><line x1="10" x2="3" y1="4" y2="4"/>'
                '<line x1="21" x2="12" y1="12" y2="12"/><line x1="8" x2="3" y1="12" y2="12"/>'
                '<line x1="21" x2="16" y1="20" y2="20"/><line x1="12" x2="3" y1="20" y2="20"/>'
                '<line x1="14" x2="14" y1="2" y2="6"/><line x1="8" x2="8" y1="10" y2="14"/>'
                '<line x1="16" x2="16" y1="18" y2="22"/>'),
    "database": ('<ellipse cx="12" cy="5" rx="9" ry="3"/>'
                 '<path d="M3 5V19A9 3 0 0 0 21 19V5"/>'
                 '<path d="M3 12A9 3 0 0 0 21 12"/>'),
    "search": '<circle cx="11" cy="11" r="8"/><path d="m21 21-4.3-4.3"/>',
    "light": ('<circle cx="12" cy="12" r="4"/><path d="M12 2v2"/><path d="M12 20v2"/>'
              '<path d="m4.93 4.93 1.41 1.41"/><path d="m17.66 17.66 1.41 1.41"/>'
              '<path d="M2 12h2"/><path d="M20 12h2"/><path d="m6.34 17.66-1.41 1.41"/>'
              '<path d="m19.07 4.93-1.41 1.41"/>'),
    "dark": '<path d="M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z"/>',
    "system": ('<rect width="20" height="14" x="2" y="3" rx="2"/>'
               '<line x1="8" x2="16" y1="21" y2="21"/><line x1="12" x2="12" y1="17" y2="21"/>'),
    "menu": ('<line x1="4" x2="20" y1="6" y2="6"/><line x1="4" x2="20" y1="12" y2="12"/>'
             '<line x1="4" x2="20" y1="18" y2="18"/>'),
    "close": '<path d="M18 6 6 18"/><path d="m6 6 12 12"/>',
    "calendar": ('<rect width="18" height="18" x="3" y="4" rx="2"/><path d="M16 2v4"/>'
                 '<path d="M8 2v4"/><path d="M3 10h18"/>'),
    "refresh": ('<path d="M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8"/>'
                '<path d="M21 3v5h-5"/><path d="M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16"/>'
                '<path d="M8 16H3v5"/>'),
    "branch": ('<line x1="6" x2="6" y1="3" y2="15"/><circle cx="18" cy="6" r="3"/>'
               '<circle cx="6" cy="18" r="3"/><path d="M18 9a9 9 0 0 1-9 9"/>'),
    "up": '<path d="m18 15-6-6-6 6"/>',
    "down": '<path d="m6 9 6 6 6-6"/>',
    "chev": '<path d="m9 18 6-6-6-6"/>',
    "check": '<path d="M20 6 9 17l-5-5"/>',
    "plus": '<path d="M5 12h14"/><path d="M12 5v14"/>',
    "trash": ('<path d="M3 6h18"/><path d="M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6"/>'
              '<path d="M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2"/>'),
    "send": '<path d="m22 2-7 20-4-9-9-4Z"/><path d="M22 2 11 13"/>',
    "lock": ('<rect width="18" height="11" x="3" y="11" rx="2"/>'
             '<path d="M7 11V7a5 5 0 0 1 10 0v4"/>'),
    "key": ('<circle cx="7.5" cy="15.5" r="5.5"/><path d="m21 2-9.6 9.6"/>'
            '<path d="m15.5 7.5 3 3L22 7l-3-3"/>'),
    "flag": ('<path d="M4 15s1-1 4-1 5 2 8 2 4-1 4-1V3s-1 1-4 1-5-2-8-2-4 1-4 1z"/>'
             '<line x1="4" x2="4" y1="22" y2="15"/>'),
    "wallet": ('<path d="M19 7V4a1 1 0 0 0-1-1H5a2 2 0 0 0 0 4h15a1 1 0 0 1 1 1v4h-3a2 2 0 0 0 0 4h3a1 1 0 0 0 1-1v-2a1 1 0 0 0-1-1"/>'
               '<path d="M3 5v14a2 2 0 0 0 2 2h15a1 1 0 0 0 1-1v-4"/>'),
    "logout": ('<path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4"/>'
               '<path d="m16 17 5-5-5-5"/><line x1="21" x2="9" y1="12" y2="12"/>'),
    "thumbsup": ('<path d="M7 10v12"/>'
                 '<path d="M15 5.88 14 10h5.83a2 2 0 0 1 1.92 2.56l-2.33 8A2 2 0 0 1 17.5 22H4a2 2 0 0 1-2-2v-8a2 2 0 0 1 2-2h2.76a2 2 0 0 0 1.79-1.11L12 2a3.13 3.13 0 0 1 3 3.88Z"/>'),
    "thumbsdown": ('<path d="M17 14V2"/>'
                   '<path d="M9 18.12 10 14H4.17a2 2 0 0 1-1.92-2.56l2.33-8A2 2 0 0 1 6.5 2H20a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2h-2.76a2 2 0 0 0-1.79 1.11L12 22a3.13 3.13 0 0 1-3-3.88Z"/>'),
    "transfer": ('<path d="m16 3 4 4-4 4"/><path d="M20 7H4"/>'
                 '<path d="m8 21-4-4 4-4"/><path d="M4 17h16"/>'),
    "external": ('<path d="M15 3h6v6"/><path d="M10 14 21 3"/>'
                 '<path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"/>'),
    "shield": ('<path d="M20 13c0 5-3.5 7.5-7.66 8.95a1 1 0 0 1-.67-.01'
               'C7.5 20.5 4 18 4 13V6a1 1 0 0 1 1-1c2 0 4.5-1.2 6.24-2.72'
               'a1.17 1.17 0 0 1 1.52 0C14.51 3.81 17 5 19 5a1 1 0 0 1 1 1z"/>'),
    "globe": ('<circle cx="12" cy="12" r="10"/>'
              '<path d="M12 2a14.5 14.5 0 0 0 0 20 14.5 14.5 0 0 0 0-20"/>'
              '<path d="M2 12h20"/>'),
}


def icon(name):
    return ('<svg class="i" viewBox="0 0 24 24" fill="none" stroke="currentColor" '
            'stroke-width="2" stroke-linecap="round" stroke-linejoin="round" '
            'aria-hidden="true">%s</svg>' % ICONS.get(name, ""))


# The hatched fill for "the rest" of a split bar, drawn as an SVG pattern so
# the split never depends on hue alone and no gradient is involved.
HATCH_DEFS = (
    '<svg width="0" height="0" style="position:absolute" aria-hidden="true">'
    '<defs><pattern id="ab-hatch" width="5" height="5" '
    'patternUnits="userSpaceOnUse" patternTransform="rotate(45)">'
    '<rect width="5" height="5" class="f-bg"/>'
    '<line x1="0" y1="0" x2="0" y2="5" class="s-second" stroke-width="1.6"/>'
    '</pattern></defs></svg>')


# --- theme ----------------------------------------------------------------------

def _cookie_value(request, name):
    try:
        header = request.headers.get("cookie") or ""
    except Exception:
        header = ""
    for part in str(header).split(";"):
        key, sep, value = part.strip().partition("=")
        if sep and key == name:
            return value.strip()
    return ""


def theme_from_request(request):
    value = _cookie_value(request, ADMIN_THEME_COOKIE)
    return value if value in ("light", "dark") else "system"


def theme_cookie(theme):
    """Pin light or dark for every admin page for a year; system clears it."""
    if theme in ("light", "dark"):
        return ("%s=%s; Path=/; Max-Age=%d; HttpOnly; Secure; SameSite=Lax"
                % (ADMIN_THEME_COOKIE, theme, ADMIN_THEME_TTL_S))
    return ("%s=; Path=/; Max-Age=0; HttpOnly; Secure; SameSite=Lax"
            % ADMIN_THEME_COOKIE)


def safe_next(value):
    """A same-site path to send the viewer back to, or ``/``.

    Only an absolute path on this origin survives: no scheme, no host, no
    protocol-relative ``//``, no backslash that a browser would read as a
    slash, and no control characters that could split a header.
    """
    text = str(value or "")[:2000]
    if (not text.startswith("/") or text.startswith("//")
            or "\\" in text or any(ord(ch) < 32 or ord(ch) == 127
                                   for ch in text)):
        return "/"
    return text


async def theme_handler(env, request):
    """``POST /admin/theme``: remember the admin theme, then go back."""
    method = str(getattr(request, "method", "") or "").upper()
    if method != "POST":
        return Response("Method not allowed.", status=405, headers={
            "allow": "POST",
            "content-type": "text/plain; charset=utf-8",
            "cache-control": "no-store",
        })
    try:
        raw = await request.text()
    except Exception:
        raw = ""
    form = parse_qs(str(raw or "")[:ADMIN_THEME_MAX_FORM_BYTES],
                    keep_blank_values=True)
    choice = (form.get("theme") or [""])[0]
    target = safe_next((form.get("next") or [""])[0])
    if choice not in THEMES:
        return Response("", status=303, headers={
            "location": target, "cache-control": "no-store"})
    return Response("", status=303, headers={
        "location": target,
        "set-cookie": theme_cookie(choice),
        "cache-control": "no-store",
        "x-robots-tag": "noindex, nofollow",
    })


def theme_control(theme, next_path):
    buttons = []
    for key in THEMES:
        selected = key == theme
        buttons.append(
            '<button type="submit" name="theme" value="%s" title="%s" '
            'aria-label="%s" aria-pressed="%s">%s</button>' % (
                key, esc(THEME_LABELS[key]), esc(THEME_LABELS[key]),
                "true" if selected else "false", icon(key)))
    return (
        '<form class="seg" method="post" action="%s" aria-label="Theme">'
        '<input type="hidden" name="next" value="%s">%s</form>' % (
            ADMIN_THEME_PATH, esc(safe_next(next_path)), "".join(buttons)))


# --- formatting -------------------------------------------------------------------

def format_int(value):
    return "{:,}".format(_int(value))


def _ms(value):
    ms = _int(value)
    # error_log and a few older tables stored seconds.
    if 0 < ms < 10 ** 11:
        ms *= 1000
    return ms


def utc_label(ms, with_time=True):
    ms = _ms(ms)
    if ms <= 0:
        return "unknown"
    import datetime
    moment = datetime.datetime.fromtimestamp(ms / 1000, datetime.timezone.utc)
    label = "%s %d, %d" % (MONTHS[moment.month - 1], moment.day, moment.year)
    if with_time:
        label += " %02d:%02d UTC" % (moment.hour, moment.minute)
    return label


def relative_label(ms, now_ms):
    ms = _ms(ms)
    if ms <= 0:
        return "never"
    delta = int(now_ms) - ms
    future = delta < 0
    seconds = abs(delta) // 1000
    if seconds < 60:
        return "just now"
    for size, unit in ((86400 * 365, "year"), (86400 * 30, "month"),
                       (86400, "day"), (3600, "hour"), (60, "minute")):
        if seconds >= size:
            value = seconds // size
            text = "%d %s%s" % (value, unit, "" if value == 1 else "s")
            return "in " + text if future else text + " ago"
    return "just now"


def time_html(ms, now_ms):
    """Relative time with the exact UTC moment one hover away."""
    ms = _ms(ms)
    if ms <= 0:
        return '<span class="muted">unknown</span>'
    import datetime
    iso = datetime.datetime.fromtimestamp(
        ms / 1000, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    return '<time class="nowrap" datetime="%s" title="%s">%s</time>' % (
        iso, esc(utc_label(ms)), esc(relative_label(ms, now_ms)))


def initials(name):
    parts = [part for part in str(name or "").replace("_", "-").split("-")
             if part]
    if not parts:
        return "?"
    if len(parts) == 1:
        return parts[0][:2].upper()
    return (parts[0][:1] + parts[1][:1]).upper()


# --- components -------------------------------------------------------------------

def pill(text, tone="", icon_name=""):
    return '<span class="pill%s">%s%s</span>' % (
        (" " + tone) if tone else "", icon(icon_name) if icon_name else "",
        esc(text))


def who(name, sub="", href=""):
    body = ('<span class="avatar" aria-hidden="true">%s</span>'
            '<span><b>%s</b>%s</span>') % (
        esc(initials(name)), esc(name),
        ('<span>%s</span>' % esc(sub)) if sub else "")
    if href:
        return '<a class="who" href="%s">%s</a>' % (href, body)
    return '<div class="who">%s</div>' % body


def page_head(title, description="", crumb="", actions=""):
    return (
        '<div class="head"><div>%s<h1>%s</h1>%s</div>%s</div>' % (
            ('<div class="crumb">%s</div>' % crumb) if crumb else "",
            esc(title),
            ('<p>%s</p>' % esc(description)) if description else "",
            ('<div class="actions">%s</div>' % actions) if actions else ""))


def banner(text, tone="ok"):
    if not text:
        return ""
    icon_name = {"ok": "check", "bad": "alert", "warn": "alert"}.get(tone, "")
    return '<div class="banner %s" role="status">%s<span>%s</span></div>' % (
        tone, icon(icon_name) if icon_name else "", esc(text))


def banner_tone(text):
    lowered = str(text or "").lower()
    if "failed" in lowered or "blocked" in lowered or "invalid" in lowered:
        return "bad"
    return "ok"


def card(body="", title="", description="", actions="", table="",
         footer="", cls="", id_attr=""):
    """A bordered card: optional heading, body padding, flush table, footer."""
    head = ""
    if title or actions:
        head = '<div class="card-h"><div>%s%s</div>%s</div>' % (
            ('<h2>%s</h2>' % esc(title)) if title else "",
            ('<p>%s</p>' % esc(description)) if description else "",
            actions)
    return '<section class="card%s"%s>%s%s%s%s</section>' % (
        (" " + cls) if cls else "",
        (' id="%s" tabindex="-1"' % esc(id_attr)) if id_attr else "",
        head,
        ('<div class="card-b">%s</div>' % body) if body else "",
        ('<div class="card-t">%s</div>' % table) if table else "",
        footer)


def kpi(label, value, window="", change="", icon_name="", tone="", href="",
        extra=""):
    inner = (
        '<div class="l">%s%s</div><div class="n">%s%s</div>%s%s' % (
            esc(label), icon(icon_name) if icon_name else "", esc(value),
            (" " + change) if change else "",
            ('<div class="w">%s</div>' % esc(window)) if window else "",
            extra))
    cls = "card kpi" + ((" " + tone) if tone else "")
    if href:
        return '<a class="%s" href="%s">%s</a>' % (cls, href, inner)
    return '<div class="%s">%s</div>' % (cls, inner)


def delta_pill(current, previous, good_when_up=True):
    """``+N`` against the previous window, coloured by whether that is good."""
    current = _int(current)
    previous = _int(previous)
    diff = current - previous
    if diff == 0:
        return pill("no change", "")
    rising = diff > 0
    tone = "up" if rising == bool(good_when_up) else "down"
    return pill(("+" if rising else "−") + format_int(abs(diff)), tone,
                "up" if rising else "down")


def tabs(items, active):
    """Link tabs: ``items`` is ``(key, label, href, count)``."""
    links = []
    for key, label, href, count in items:
        current = key == active
        links.append('<a href="%s"%s>%s%s</a>' % (
            href, ' aria-current="page"' if current else "", esc(label),
            (' <span class="count">%s</span>' % esc(count))
            if count not in (None, "") else ""))
    return '<nav class="tabs" aria-label="Sections">%s</nav>' % "".join(links)


def empty(text):
    return '<p class="empty">%s</p>' % esc(text)


def table(head, rows, empty_text="Nothing to show yet."):
    """``head`` is pre-rendered ``<th>`` cells, ``rows`` pre-rendered ``<tr>``."""
    if not rows:
        return empty(empty_text)
    return '<table><thead><tr>%s</tr></thead><tbody>%s</tbody></table>' % (
        head, "".join(rows))


def switch(name, label, checked=False, value="1", id_attr=""):
    return (
        '<label class="switch"><input type="checkbox" name="%s" value="%s"%s%s>'
        '<span class="track"></span>%s</label>' % (
            esc(name), esc(value),
            (' id="%s"' % esc(id_attr)) if id_attr else "",
            " checked" if checked else "", esc(label) if label else ""))


def hbar(percent, tone=""):
    percent = max(0.0, min(100.0, float(percent or 0)))
    return '<div class="hbar%s"><i style="width:%.1f%%"></i></div>' % (
        (" " + tone) if tone else "", percent)


def meter(label, percent, value_text, tone=""):
    return ('<div class="meter"><span class="mono">%s</span>%s<span>%s</span>'
            '</div>' % (esc(label), hbar(percent, tone), esc(value_text)))


def stacked_bar(share_percent, width_px=160):
    share = max(0.0, min(100.0, float(share_percent or 0)))
    return (
        '<div class="stack-bar" style="width:%dpx"><span class="a" '
        'style="width:%.1f%%"></span><svg aria-hidden="true" '
        'preserveAspectRatio="none"><rect width="100%%" height="100%%" '
        'fill="url(#ab-hatch)"/></svg></div>' % (max(40, int(width_px)), share))


def sparkline(values, label="24-hour frequency"):
    values = [max(0, _int(v)) for v in values]
    peak = max(values) if values else 0
    bars = []
    for value in values:
        if value:
            bars.append('<i style="height:%dpx"></i>'
                        % max(3, round(26 * value / peak)))
        else:
            bars.append('<i class="z" style="height:2px"></i>')
    columns = ("" if len(values) in (0, 24) else
               ' style="grid-template-columns:repeat(%d,1fr)"' % len(values))
    return '<span class="spark"%s role="img" aria-label="%s">%s</span>' % (
        columns, esc(label), "".join(bars))


def hour_bars(values, labels=None, label_fn=None):
    """Tall bars with the peak called out, oldest first."""
    values = [max(0, _int(v)) for v in values]
    peak = max(values) if values else 0
    # Only the most recent peak is called out, so a tie reads as one answer.
    peak_index = (len(values) - 1 - values[::-1].index(peak)) if peak else -1
    out = []
    for index, value in enumerate(values):
        text = label_fn(index, value) if label_fn else "%d" % value
        height = max(3, round(100 * value / peak)) if peak else 3
        is_peak = index == peak_index
        out.append(
            '<span style="height:%d%%"%s tabindex="0" role="img" '
            'aria-label="%s" title="%s">%s</span>' % (
                height, ' class="peak"' if is_peak else "", esc(text),
                esc(text), ('<em>%d</em>' % value) if is_peak else ""))
    axis = ""
    if labels:
        axis = '<div class="hours-x">%s</div>' % "".join(
            '<span>%s</span>' % esc(item) for item in labels)
    return '<div class="hours">%s</div>%s' % ("".join(out), axis)


def _nice_ceiling(value):
    value = max(1, value)
    if value <= 4:
        return 4
    magnitude = 10 ** (len(str(int(value))) - 1)
    for step in (1, 2, 2.5, 5, 10):
        if step * magnitude >= value:
            return step * magnitude
    return 10 * magnitude


def line_chart(labels, series, width=720, height=220, bars=False,
               title="Chart"):
    """An SVG line (or column) chart on one shared scale.

    ``series`` is ``[(values, "accent"|"second"), ...]``. The first series
    gets the area fill and an emphasised end point; a second is dashed. In
    column mode the first series is drawn soft and the second, a subset of
    it, in the accent colour on top.
    """
    count = len(labels)
    if not count:
        return ""
    left, right, top, bottom = 38, 10, 12, 26
    inner_w = width - left - right
    inner_h = height - top - bottom
    peak = max([1] + [max(0, _int(v)) for values, _t in series for v in values])
    ceiling = _nice_ceiling(peak)

    def x_at(index):
        if bars:
            return left + (index + 0.5) * inner_w / count
        return left + index * inner_w / max(1, count - 1)

    def y_at(value):
        return top + inner_h - (max(0, value) / ceiling) * inner_h

    parts = ['<svg viewBox="0 0 %d %d" role="img" aria-label="%s">' % (
        width, height, esc(title))]
    for tick in range(5):
        value = ceiling * tick / 4
        y = y_at(value)
        tick_text = ("%g" % value) if value % 1 else "%d" % value
        parts.append(
            '<line class="g-line" x1="%d" x2="%d" y1="%.1f" y2="%.1f"/>'
            '<text class="g-axis" x="%d" y="%.1f" text-anchor="end">%s</text>'
            % (left, width - right, y, y, left - 8, y + 4, tick_text))
    step = max(1, round(count / 6))
    for index, text in enumerate(labels):
        last = index == count - 1
        if index % step and not last:
            continue
        if not last and count - 1 - index < step * 0.6:
            continue
        anchor = "middle"
        if not bars and index == 0:
            anchor = "start"
        elif not bars and last:
            anchor = "end"
        parts.append('<text class="g-axis" x="%.1f" y="%d" text-anchor="%s">'
                     '%s</text>' % (x_at(index), height - 6, anchor, esc(text)))
    if bars:
        bar_w = max(3.0, inner_w / count * 0.62)
        for series_index, (values, _tone) in enumerate(series):
            css = "f-soft" if series_index == 0 and len(series) > 1 else "f-accent"
            for index, value in enumerate(values[:count]):
                bar_h = max(1.0, (max(0, _int(value)) / ceiling) * inner_h)
                parts.append(
                    '<rect class="%s" x="%.1f" y="%.1f" width="%.1f" '
                    'height="%.1f" rx="2"><title>%s: %d</title></rect>' % (
                        css, x_at(index) - bar_w / 2, top + inner_h - bar_h,
                        bar_w, bar_h, esc(labels[index]), _int(value)))
    else:
        for series_index, (values, tone) in enumerate(series):
            points = " ".join("%.1f,%.1f" % (x_at(i), y_at(_int(v)))
                              for i, v in enumerate(values[:count]))
            if series_index == 0:
                parts.append('<polygon class="f-area" points="%d,%d %s %d,%d"/>'
                             % (left, top + inner_h, points, width - right,
                                top + inner_h))
            dashed = tone == "second"
            parts.append(
                '<polyline class="%s" points="%s" fill="none" '
                'stroke-width="%s" stroke-linejoin="round"%s/>' % (
                    "s-second" if dashed else "s-accent", points,
                    "1.6" if dashed else "2",
                    ' stroke-dasharray="4 4"' if dashed else ""))
            if series_index == 0 and values:
                last_value = _int(values[min(count, len(values)) - 1])
                parts.append(
                    '<circle class="f-bg s-accent" cx="%.1f" cy="%.1f" r="4" '
                    'stroke-width="2"/>' % (x_at(count - 1), y_at(last_value)))
    parts.append("</svg>")
    return '<div class="chart">%s</div>' % "".join(parts)


def gauge(fraction, label=""):
    """A semicircle of ticks, filled up to ``fraction`` of the target."""
    fraction = max(0.0, min(1.0, float(fraction or 0)))
    import math
    cx, cy, r1, r2, ticks = 110, 104, 74, 94, 44
    parts = ['<svg viewBox="0 0 220 118" role="img" aria-label="%s">'
             % esc(label or "%d%%" % round(fraction * 100))]
    for tick in range(ticks):
        angle = math.pi * (1 - tick / (ticks - 1))
        cos, sin = math.cos(angle), math.sin(angle)
        parts.append(
            '<line class="%s" x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" '
            'stroke-width="3.2" stroke-linecap="round"/>' % (
                "t-ok" if tick / (ticks - 1) <= fraction else "t-off",
                cx + r1 * cos, cy - r1 * sin, cx + r2 * cos, cy - r2 * sin))
    parts.append("</svg>")
    return "".join(parts)


# --- navigation and page ----------------------------------------------------------

def console_base(env):
    """``/<ADMIN_PATH>`` or ``""`` when the console is not configured."""
    path = str(getattr(env, "ADMIN_PATH", "") or "").strip("/")
    return ("/" + quote(path, safe="/-_.~")) if path else ""


def nav_href(console, target):
    kind, value = target
    if kind == "path":
        return value
    if not console:
        return ""
    return console + "?view=" + quote(value)


async def nav_badges(env):
    """Attention counts for the sidebar. Best-effort: missing data is no badge."""
    badges = {}
    try:
        now = int(Date.now())
    except Exception:
        import time
        now = int(time.time() * 1000)

    async def scalar(sql, *args):
        try:
            row = await d1_first(env, sql, *args)
        except Exception:
            return None
        if not row:
            return None
        return _int(dict(row).get("n"))

    new_reports = await scalar(
        "SELECT COUNT(*) AS n FROM bug_reports WHERE status='new'")
    if new_reports:
        badges["bugs"] = (format_int(new_reports), "bad")
    pending_notes = await scalar(
        "SELECT COUNT(*) AS n FROM agent_note_queue WHERE status='pending'")
    if pending_notes:
        badges["agent-notes"] = (format_int(pending_notes), "warn")
    errors = await scalar(
        "SELECT COUNT(*) AS n FROM error_log WHERE ts>=?", now - 86_400_000)
    if errors:
        badges["errors"] = (format_int(errors), "bad")
    not_found = await scalar(
        "SELECT COUNT(*) AS n FROM not_found_paths "
        "WHERE alerted_at>0 AND alerted_at>=?", now - 86_400_000)
    if not_found:
        badges["not-found"] = (format_int(not_found), "warn")
    try:
        row = await d1_first(
            env,
            "SELECT COALESCE(SUM(CASE WHEN severity='lockdown' THEN 1 ELSE 0 "
            "END),0) AS lockdowns,COUNT(*) AS n FROM security_incidents "
            "WHERE created_ms>=? AND severity IN ('alert','lockdown','test')",
            now - 86_400_000)
    except Exception:
        row = None
    row = dict(row) if row else {}
    lockdowns = _int(row.get("lockdowns"))
    alerts = _int(row.get("n")) - lockdowns
    if lockdowns:
        badges["ids"] = (format_int(lockdowns), "bad")
    elif alerts:
        badges["ids"] = (format_int(alerts), "warn")
    try:
        row = await d1_first(
            env, "SELECT status FROM hackathons ORDER BY created_at DESC LIMIT 1")
    except Exception:
        row = None
    status = str(dict(row).get("status") or "") if row else ""
    if status:
        badges["hackathon"] = (status, "ok" if status == "live" else "warn")
    return badges


def sidebar(active, console="", badges=None, theme="system", next_path="/",
            foot_extra="", allowed=None):
    """The grouped section list. ``allowed`` limits which keys are drawn."""
    badges = badges or {}
    groups = []
    for group_label, items in NAV:
        links = []
        for key, label, icon_name, target in items:
            if allowed is not None and key not in allowed:
                continue
            href = nav_href(console, target)
            if not href:
                continue
            current = key == active
            badge = badges.get(key)
            links.append(
                '<a class="ab-item%s" href="%s"%s>%s<span>%s</span>%s</a>' % (
                    " on" if current else "", esc(href),
                    ' aria-current="page"' if current else "",
                    icon(icon_name), esc(label),
                    ('<span class="ab-count %s">%s</span>' % (
                        esc(badge[1]), esc(badge[0]))) if badge else ""))
        if not links:
            continue
        if group_label:
            groups.append('<div class="ab-group">%s</div>' % esc(group_label))
        groups.extend(links)
    return (
        '<aside class="ab-side" aria-label="Admin navigation">'
        '<label for="ab-nav" class="ab-close" title="Close navigation">%s'
        '<span class="sr">Close navigation</span></label>'
        '<div class="ab-brand"><img src="%s" alt="" width="32" height="32">'
        '<div><b>ForkMesh</b><span>Admin</span></div></div>'
        '<nav class="ab-nav" aria-label="Sections">%s</nav>'
        '<div class="ab-foot">%s%s</div></aside>' % (
            icon("close"), esc(LOGO), "".join(groups), foot_extra,
            theme_control(theme, next_path)))


def top_bar(console="", account="", search_value=""):
    search = ""
    if console:
        search = (
            '<form class="ab-search" method="get" action="%s" role="search">'
            '%s<input type="hidden" name="view" value="users">'
            '<label class="sr" for="ab-q">Search users</label>'
            '<input id="ab-q" type="search" name="q" value="%s" '
            'placeholder="Search users by name or email" maxlength="80">'
            '</form>' % (esc(console), icon("search"), esc(search_value)))
    avatar = ""
    if account:
        avatar = ('<span class="avatar" title="Signed in as %s">%s</span>'
                  % (esc(account), esc(initials(account))))
    return (
        '<header class="ab-top"><label for="ab-nav" class="ab-menu" '
        'title="Open navigation">%s<span class="sr">Open navigation</span>'
        '</label>%s<div class="grow"></div><span class="chip">%sAll times in UTC'
        '</span>%s</header>' % (icon("menu"), search, icon("calendar"), avatar))


def document(title, body, theme="system", head_extra=""):
    html_class = (' class="theme-%s"' % theme) if theme in ("light", "dark") else ""
    return (
        '<!doctype html><html lang="en"%s><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width, initial-scale=1">'
        '<meta name="robots" content="noindex, nofollow">'
        '<meta name="color-scheme" content="light dark">'
        '<title>%s · ForkMesh admin</title><style>%s</style>%s</head>'
        '<body>%s</body></html>' % (html_class, esc(title), STYLE, head_extra,
                                    body))


def render_page(title, content, active="", console="", theme="system",
                badges=None, account="", next_path="/", search_value="",
                foot_extra="", allowed=None, head_extra="", body_extra=""):
    """One complete admin page: sidebar, top bar and ``content``."""
    # The drawer checkbox must precede the sidebar, scrim and main column as a
    # sibling so the ``#ab-nav:checked ~`` rules can reach all three.
    body = (
        HATCH_DEFS
        + '<div class="ab"><input type="checkbox" id="ab-nav" class="sr" '
          'aria-label="Show navigation">'
        + sidebar(active, console, badges, theme, next_path, foot_extra,
                  allowed)
        + '<label for="ab-nav" class="ab-scrim" aria-hidden="true"></label>'
        + '<div class="ab-main">'
        + top_bar(console if allowed is None or "users" in allowed else "",
                  account, search_value)
        + '<main class="ab-content" id="main">' + content + '</main></div></div>'
        + body_extra)
    return document(title, body, theme, head_extra)
