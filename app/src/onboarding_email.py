import html as _html
import re

STEPS = ("first_repo", "day_3", "day_7")
SITE = "https://forkmesh.com"
APP = "https://app.forkmesh.com"
REPOS_URL = APP + "/dashboard/repos"
NEW_REPO_URL = APP + "/dashboard/repos/new"
SETTINGS_URL = APP + "/dashboard/settings/notifications"
DOCS_URL = SITE + "/docs/onboarding"
PROMISE_URL = SITE + "/docs/product-promise"
DESKTOP_URL = SITE + "/desktop"
MOBILE_URL = SITE + "/mobile"
FEATURES_URL = SITE + "/features"
MARKET_URL = SITE + "/market"
INSTALL = "curl -fsSL https://forkmesh.com/install.sh | bash"
FEEDBACK = "founders@forkmesh.com"
REPO_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,62}/[A-Za-z0-9._-]{1,100}$")

_TEXT = "class=\"fm-text\" style=\"margin:0 0 8px;color:#d4d4d8;font-size:14px\""
_LAST = "class=\"fm-text\" style=\"margin:0 0 18px;color:#d4d4d8;font-size:14px\""
_LINK = "class=\"fm-link\" style=\"color:#4ade80;text-decoration:none\""
_CODE = ("style=\"font-family:ui-monospace,SFMono-Regular,Menlo,monospace;"
         "font-size:13px\"")


def _e(value):
    return _html.escape(str(value or ""), quote=True)


def _name(name):
    name = " ".join(str(name or "").split())[:64]
    return name or "there"


def repo_name(repo):
    repo = str(repo or "").strip()
    return repo if REPO_RE.match(repo) else ""


def _a(url, label):
    return "<a href=\"%s\" %s>%s</a>" % (_e(url), _LINK, _e(label))


def _section(title, rows):
    out = ["<div class=\"fm-border\" style=\"border-top:1px solid #313134;"
           "padding-top:18px;margin-top:4px\">",
           "<p class=\"fm-strong\" style=\"margin:0 0 10px;color:#f5f5f5;"
           "font-size:15px;font-weight:700\">%s</p>" % _e(title)]
    for index, row in enumerate(rows):
        out.append("<p %s>%s</p>" % (
            _LAST if index == len(rows) - 1 else _TEXT, row))
    out.append("</div>")
    return "".join(out)


def _feedback_html():
    return ("<p class=\"fm-muted\" style=\"margin:0;color:#a3a3a3;"
            "font-size:14px\">What was confusing, missing, or most useful? "
            "Send feedback to <a href=\"mailto:%s\" %s>%s</a>."
            "</p>" % (FEEDBACK, _LINK, FEEDBACK))


def _first_repo(name, repo):
    clone = SITE + "/" + repo if repo else ""
    label = repo or "Your repository"
    text = [
        "Hi %s," % name,
        "%s is on ForkMesh. Anyone you share it with can clone it with plain "
        "Git:" % label + ("\n" + "git clone " + clone if clone else ""),
        "Right now it lives on one host. The next step is a second copy on a "
        "mirror, so a clone still works when that host is offline.",
        "Next steps:\n"
        "- Add a mirror: install the desktop app (" + DESKTOP_URL + ") or "
        "turn any Linux machine into a mirror node with\n  " + INSTALL + "\n"
        "- Open an issue or invite a collaborator from the repository page: "
        + REPOS_URL + "\n"
        "- Read the getting-started guide: " + DOCS_URL,
        "What was confusing, missing, or most useful? Send feedback "
        "to " + FEEDBACK + ".",
    ]
    clone_html = ("<p %s><code %s>git clone %s</code></p>" % (
        _LAST, _CODE, _e(clone))) if clone else ""
    body = clone_html + _section("Next steps", [
        "Add a mirror: install the %s or run <code %s>%s</code> on any Linux "
        "machine." % (_a(DESKTOP_URL, "desktop app"), _CODE, _e(INSTALL)),
        "Open an issue or invite a collaborator from %s." % _a(
            REPOS_URL, "your repositories"),
        "Read the %s." % _a(DOCS_URL, "getting-started guide"),
    ]) + _feedback_html()
    return {
        "subject": "Your first repository is on ForkMesh",
        "preheader": "Next: give %s a second copy on a mirror." % (
            repo or "it"),
        "heading": "%s is on ForkMesh" % _e(label),
        "intro": "Hi <strong class=\"fm-strong\" style=\"color:#f5f5f5\">%s"
                 "</strong>, your first repository is published. Here is how "
                 "to make it outlive the host it is on." % _e(name),
        "text": text,
        "body_html": body,
        "action_label": "Open your repositories",
        "action_url": REPOS_URL,
    }


def _day_3(name, has_repo):
    first = ("Your repository is on ForkMesh. A mirror is what lets it "
             "survive its host going away." if has_repo else
             "You have not published a repository yet. Create one, or add a "
             "local Git repository from the desktop app, then give it a "
             "mirror.")
    text = [
        "Hi %s," % name,
        first,
        "A mirror is a full Git copy on an independently operated node. When "
        "the source host is offline, a clone of the same URL is routed to a "
        "healthy mirror whose history matches what you published.",
        "Try it yourself:\n"
        "1. Install ForkMesh on a second machine: " + INSTALL + "\n"
        "2. Add your repository as a mirror there.\n"
        "3. Take the first host offline and clone again.",
        "The steps and what each one proves: " + PROMISE_URL,
        "What was confusing, missing, or most useful? Send feedback "
        "to " + FEEDBACK + ".",
    ]
    body = ("<p %s>%s</p>" % (_LAST, _e(text[2]))) + _section("Try it yourself", [
        "1. Install ForkMesh on a second machine: <code %s>%s</code>" % (
            _CODE, _e(INSTALL)),
        "2. Add your repository as a mirror there.",
        "3. Take the first host offline and clone again. %s." % _a(
            PROMISE_URL, "What each step proves"),
    ]) + _feedback_html()
    return {
        "subject": "Give your code a second home",
        "preheader": "Add a mirror, take the host offline, clone again.",
        "heading": "Give your code a second home",
        "intro": "Hi <strong class=\"fm-strong\" style=\"color:#f5f5f5\">%s"
                 "</strong>. %s" % (_e(name), _e(first)),
        "text": text,
        "body_html": body,
        "action_label": "Add a mirror" if has_repo else "Create a repository",
        "action_url": DESKTOP_URL if has_repo else NEW_REPO_URL,
    }


def _day_7(name):
    rows = [
        ("Signed collaboration", "issues, pull requests and reviews are "
         "signed with your own key and stored in the repository."),
        ("AI agents", "hand an agent an issue and follow it to a branch and "
         "a pull request."),
        ("Desktop CI", "shell-step checks run on your own machine."),
        ("Your phone", "review and merge pull requests, and run real Git, "
         "from the iPhone app or the Android APK."),
        ("Marketplace", "signed extensions, agents and themes, each with "
         "public source."),
    ]
    links = {"AI agents": FEATURES_URL, "Your phone": MOBILE_URL,
             "Marketplace": MARKET_URL}
    text = [
        "Hi %s," % name,
        "You have had ForkMesh for a week. Beyond mirrors, every repository "
        "gets a full forge:",
        "\n".join("- %s: %s%s" % (
            title, line, (" " + links[title]) if title in links else "")
            for title, line in rows),
        "The whole catalog: " + FEATURES_URL,
        "What was confusing, missing, or most useful in your first week? "
        "Send feedback to " + FEEDBACK + ".",
    ]
    body = _section("Every repository gets a full forge", [
        "<strong class=\"fm-strong\" style=\"color:#f5f5f5\">%s</strong>: %s"
        % (_a(links[title], title) if title in links else _e(title), _e(line))
        for title, line in rows
    ]) + _feedback_html()
    return {
        "subject": "Your first week on ForkMesh",
        "preheader": "Signed reviews, AI agents, desktop CI and your phone.",
        "heading": "Your first week on ForkMesh",
        "intro": "Hi <strong class=\"fm-strong\" style=\"color:#f5f5f5\">%s"
                 "</strong>. Beyond mirrors, here is what else ForkMesh does "
                 "for a repository." % _e(name),
        "text": text,
        "body_html": body,
        "action_label": "See every feature",
        "action_url": FEATURES_URL,
    }


def footer_lines(unsubscribe, settings_url=SETTINGS_URL):
    lines = ["You get this because you created a ForkMesh account. It is one "
             "of three getting-started emails."]
    if unsubscribe:
        lines.append("Stop getting-started emails: " + unsubscribe)
    lines.append("Email settings: " + settings_url)
    return lines


def footer_html(unsubscribe, settings_url=SETTINGS_URL):
    links = []
    if unsubscribe:
        links.append("<a href=\"%s\" style=\"color:#8a8a93\">Stop "
                     "getting-started emails</a>" % _e(unsubscribe))
    links.append("<a href=\"%s\" style=\"color:#8a8a93\">Email settings</a>"
                 % _e(settings_url))
    return ("<p class=\"fm-muted\" style=\"margin:22px 0 0;color:#8a8a93;"
            "font-size:12px\">You get this because you created a ForkMesh "
            "account. It is one of three getting-started emails. %s</p>"
            % " &middot; ".join(links))


def message(step, name, repo="", has_repo=True, unsubscribe="",
            settings_url=SETTINGS_URL):
    name = _name(name)
    if step == "first_repo":
        out = _first_repo(name, repo_name(repo))
    elif step == "day_3":
        out = _day_3(name, bool(has_repo))
    elif step == "day_7":
        out = _day_7(name)
    else:
        raise ValueError("unknown onboarding step: %r" % (step,))
    out["step"] = step
    out["text"] = "\n\n".join(out["text"]) + "\n\n--\n" + "\n".join(
        footer_lines(unsubscribe, settings_url))
    out["intro_html"] = out.pop("intro")
    out["footer_html"] = footer_html(unsubscribe, settings_url)
    return out
