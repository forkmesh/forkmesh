import html

DESKTOP_PATHS = ("/desktop", "/desktop/", "/download", "/download/",
                 "/downloads", "/downloads/")
DOCS_PATHS = ("/docs", "/docs/")
DOWNLOADS_URL = "https://forkmesh.com/desktop"
INSTALL_COMMAND = "curl -fsSL https://forkmesh.com/install.sh | bash"

STYLE = """
:root{color-scheme:light dark;--bg:#0b0b0c;--fg:#ededef;--muted:#a1a1aa;
--line:#27272a;--card:#141416;--accent:#2dd4bf}
@media (prefers-color-scheme:light){:root{--bg:#fafafa;--fg:#18181b;
--muted:#52525b;--line:#e4e4e7;--card:#fff;--accent:#0d9488}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
font:15px/1.65 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
header{display:flex;align-items:center;gap:16px;flex-wrap:wrap;
padding:14px 24px;border-bottom:1px solid var(--line)}
header a{color:var(--fg);text-decoration:none}
header .brand{display:inline-flex;align-items:center;gap:10px;font-weight:700}
header .brand img{width:28px;height:28px;border-radius:6px}
header nav{display:flex;gap:16px;margin-left:auto;font-size:14px}
header nav a{color:var(--muted)}
header nav a:hover,header nav a[aria-current]{color:var(--fg)}
main{max-width:46rem;margin:0 auto;padding:32px 24px 72px}
h1{font-size:2rem;line-height:1.2;margin:0 0 8px}
h2{font-size:1.25rem;margin:40px 0 8px;padding-top:8px;
border-top:1px solid var(--line)}
h3{font-size:1rem;margin:24px 0 4px}
p.lead{color:var(--muted);font-size:1.05rem;margin-top:0}
a{color:var(--accent)}
pre{background:var(--card);border:1px solid var(--line);border-radius:8px;
padding:12px 14px;overflow-x:auto;font-size:13px;line-height:1.55}
code{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
p code,li code{background:var(--card);border:1px solid var(--line);
border-radius:4px;padding:1px 5px;font-size:.88em}
ol,ul{padding-left:1.4rem}
li{margin:4px 0}
.toc{background:var(--card);border:1px solid var(--line);border-radius:8px;
padding:12px 18px;margin:24px 0}
.toc ol{margin:6px 0}
.cards{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(13rem,1fr));
margin:16px 0}
.card{display:block;background:var(--card);border:1px solid var(--line);
border-radius:10px;padding:14px 16px;color:var(--fg);text-decoration:none}
.card:hover{border-color:var(--accent)}
.card strong{display:block}
.card span{color:var(--muted);font-size:13px}
footer{color:var(--muted);font-size:13px;text-align:center;padding:24px}
footer a{color:inherit;font-weight:600}
"""


def _e(value):
    return html.escape(str(value or ""), quote=True)


def page(name, title, current, body):
    links = (("/dashboard", "Dashboard"), ("/docs", "Docs"),
             ("/desktop", "Desktop app"))
    nav = "".join(
        '<a href="%s"%s>%s</a>' % (
            href, ' aria-current="page"' if href == current else "", label)
        for href, label in links)
    return (
        '<!doctype html><html lang="en"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        "<title>%s - %s</title>"
        '<meta name="application-name" content="%s">'
        '<link rel="icon" href="/assets/logo.svg" type="image/svg+xml">'
        "<style>%s</style></head><body>"
        '<header><a class="brand" href="/dashboard">'
        '<img src="/assets/logo.svg" alt="">%s</a><nav>%s</nav></header>'
        "<main>%s</main>"
        '<footer>Powered by <a href="https://forkmesh.com/">ForkMesh</a>'
        "</footer></body></html>"
    ) % (_e(title), _e(name), _e(name), STYLE, _e(name), nav, body)


def desktop_body(name, origin):
    return (
        "<h1>Get the ForkMesh desktop app</h1>"
        '<p class="lead">%(name)s runs on ForkMesh, so the ForkMesh desktop '
        "app works with it. Download it from forkmesh.com, then add "
        "%(name)s as an instance.</p>"
        "<h2>1. Download and install</h2>"
        "<p>On Linux or macOS, run this in a terminal:</p>"
        "<pre><code>%(install)s</code></pre>"
        "<p>Or pick an installer for your system: a Windows setup program, a "
        "macOS disk image (Apple silicon or Intel) or a Linux AppImage. Every "
        "release is signed and listed on "
        '<a href="%(downloads)s" rel="noopener">forkmesh.com/desktop</a>.</p>'
        '<div class="cards">'
        '<a class="card" href="%(downloads)s" rel="noopener">'
        "<strong>Windows</strong><span>Setup program</span></a>"
        '<a class="card" href="%(downloads)s" rel="noopener">'
        "<strong>macOS</strong><span>Disk image or the install command</span></a>"
        '<a class="card" href="%(downloads)s" rel="noopener">'
        "<strong>Linux</strong><span>AppImage or the install command</span></a>"
        "</div>"
        "<h2>2. Connect it to %(name)s</h2>"
        "<ol>"
        "<li>Open the desktop app.</li>"
        "<li>Click the instance picker at the top left and choose "
        "<strong>Add relay&hellip;</strong></li>"
        "<li>Enter <code>%(origin)s</code> and give it a name, for example "
        "<em>%(name)s</em>.</li>"
        "<li>Sign in with your %(name)s account.</li>"
        "</ol>"
        "<p>The desktop keeps every instance apart: its repositories, chat and "
        "sign-in stay with the instance they belong to, and you can switch "
        "between instances from the same picker.</p>"
        "<h2>You do not need it for Git</h2>"
        "<p>Plain <code>git</code> over HTTPS works with %(name)s without the "
        'desktop app. See <a href="/docs">how to use %(name)s with Git</a>.'
        "</p>"
    ) % {"name": _e(name), "origin": _e(origin), "install": _e(INSTALL_COMMAND),
         "downloads": _e(DOWNLOADS_URL)}


def docs_body(name, origin):
    return (
        "<h1>Using %(name)s with Git</h1>"
        '<p class="lead">%(name)s is a Git host. You keep your code in '
        "repositories here and work on it with the ordinary "
        "<code>git</code> command line, from any computer.</p>"
        '<nav class="toc" aria-label="On this page"><strong>On this page</strong><ol>'
        '<li><a href="#account">Create an account</a></li>'
        '<li><a href="#repository">Create a repository</a></li>'
        '<li><a href="#clone">Clone a repository</a></li>'
        '<li><a href="#push">Push your work</a></li>'
        '<li><a href="#branches">Branches</a></li>'
        '<li><a href="#forks">Forks</a></li>'
        '<li><a href="#pulls">Pull requests</a></li>'
        '<li><a href="#other-instances">Forks on other instances</a></li>'
        '<li><a href="#issues">Issues</a></li>'
        '<li><a href="#desktop">The desktop app</a></li>'
        "</ol></nav>"
        '<h2 id="account">Create an account</h2>'
        '<p><a href="/signup">Sign up</a> with a username, an email address '
        'and a password, or <a href="/login">sign in</a>. Your username is the '
        "owner name in your repositories' addresses, for example "
        "<code>%(origin)s/<em>you</em>/<em>project</em></code>.</p>"
        '<h2 id="repository">Create a repository</h2>'
        "<ol>"
        '<li>Open <a href="/dashboard/repos/new">New repository</a> (Dashboard, '
        "Repositories, New).</li>"
        "<li>Choose the owner (you or one of your organizations) and a name. "
        "Names use letters, numbers, <code>.</code>, <code>_</code> and "
        "<code>-</code>.</li>"
        "<li>Choose <strong>Public</strong> (anyone can browse and clone it) or "
        "<strong>Private</strong> (only you and your organization can).</li>"
        "<li>Optionally start it with a README, a <code>.gitignore</code> and a "
        "license, then create it.</li>"
        "</ol>"
        "<p>To bring in an existing project, give the address of a public "
        "repository on GitHub, GitLab or Codeberg as the source and "
        "%(name)s copies it with its history.</p>"
        '<h2 id="clone">Clone a repository</h2>'
        "<p>Every repository page shows its clone address. Clone it with:</p>"
        "<pre><code>git clone %(origin)s/<em>owner</em>/<em>repository</em>.git</code></pre>"
        "<p>Public repositories clone without signing in. A private repository "
        "asks for your username and push password (below).</p>"
        '<h2 id="push">Push your work</h2>'
        "<p>When you create a repository, %(name)s shows the commands to use "
        "and a <strong>username</strong> (your account name) and a "
        "<strong>password</strong> for Git. Copy the password then: it is shown "
        "once and lasts 7 days.</p>"
        "<p>For a new, empty repository:</p>"
        "<pre><code>git init\n"
        "git add .\n"
        'git commit -m "First commit"\n'
        "git branch -M main\n"
        "git remote add origin %(origin)s/<em>owner</em>/<em>repository</em>.git\n"
        "git push -u origin main</code></pre>"
        "<p>For a repository you cloned, commit and run "
        "<code>git push</code>. When Git asks, give your username and the "
        "password. A credential helper "
        "(<code>git config --global credential.helper cache</code>) saves "
        "typing it on every push.</p>"
        "<h3>What the server accepts</h3>"
        "<ul>"
        "<li>Fast-forward pushes only: no force pushes, and branches cannot "
        "be deleted by a push.</li>"
        "<li>New tags can be pushed; an existing tag never moves.</li>"
        "<li>Only branches (<code>refs/heads/*</code>) and tags "
        "(<code>refs/tags/*</code>).</li>"
        "</ul>"
        "<p>To rewrite history, push it to a new branch instead.</p>"
        '<h2 id="branches">Branches</h2>'
        "<pre><code>git switch -c my-change\n"
        "# edit, then\n"
        "git commit -am \"Describe the change\"\n"
        "git push -u origin my-change</code></pre>"
        "<p>The repository page lists every branch, and the compare view shows "
        "what a branch changes against another.</p>"
        '<h2 id="forks">Forks</h2>'
        "<p>A fork is your own copy of someone else's repository that keeps "
        "its history. Open the repository, click <strong>Fork</strong>, "
        "and push your changes to your fork. The Fork menu also lists who "
        "else has forked it.</p>"
        '<h2 id="pulls">Pull requests</h2>'
        "<ol>"
        "<li>Push your changes to a branch, in the repository or in your fork.</li>"
        "<li>Open the repository's <strong>Pull requests</strong> tab and "
        "choose <strong>New pull request</strong>.</li>"
        "<li>Choose the base branch (where the change should go) and your "
        "branch, write a title and a description, and send it.</li>"
        "</ol>"
        "<p>The owner reviews it, comments on lines, and merges it. A pull "
        "request is a signed patch, so the owner can review it even while "
        "your fork is offline.</p>"
        '<h2 id="other-instances">Forks on other instances</h2>'
        "<p>The same project can live on more than one ForkMesh instance, for "
        "example here and on forkmesh.com. Link them, and you can send pull "
        "requests from one to the other:</p>"
        "<ol>"
        "<li>On your repository, open the <strong>Fork</strong> menu and, under "
        "<strong>Forks on other instances</strong>, enter the other copy's "
        "address, such as <code>https://forkmesh.com/<em>owner</em>/"
        "<em>repository</em></code>, and save it.</li>"
        "<li>Choose <strong>New pull request</strong>. In "
        "<strong>Send to</strong>, pick the saved fork, then choose the base "
        "branch and your branch.</li>"
        "<li>%(name)s compares the branches here, signs the patch in your "
        "browser and delivers it to the other instance, which shows it in that "
        "repository's pull requests.</li>"
        "</ol>"
        "<p>Both copies need to share history: the base branch you pick must "
        "exist here too.</p>"
        '<h2 id="issues">Issues</h2>'
        "<p>Each repository has an <strong>Issues</strong> tab to report bugs "
        "and plan work. Mention people with <code>@name</code> and refer to an "
        "issue or pull request with <code>#number</code>.</p>"
        '<h2 id="desktop">The desktop app</h2>'
        "<p>Everything above works with plain <code>git</code>. If you prefer a "
        "desktop app, the ForkMesh desktop app works with %(name)s: "
        '<a href="/desktop">download it and connect it</a>.</p>'
    ) % {"name": _e(name), "origin": _e(origin)}


def desktop_page(name, origin):
    return page(name, "Desktop app", "/desktop", desktop_body(name, origin))


def docs_page(name, origin):
    return page(name, "Docs", "/docs", docs_body(name, origin))


def page_for(path, name, origin):
    if path in DESKTOP_PATHS:
        return desktop_page(name, origin)
    if path in DOCS_PATHS:
        return docs_page(name, origin)
    return ""
