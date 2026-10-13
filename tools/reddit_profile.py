#!/usr/bin/env python3
import argparse
import base64
import json
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import reddit_scout

PROFILE = Path(__file__).resolve().parent / "reddit_profile.json"
FIELDS = {"title": 30, "public_description": 200}
LABELS = {"title": "display name", "public_description": "about"}
USERNAME = re.compile(r"[A-Za-z0-9_-]{3,20}")
SETTING_NAMES = {
    "default_set": "allow_top",
    "header_hover_text": "header-title",
    "language": "lang",
    "content_options": "link_type",
    "subreddit_id": "sr",
    "subreddit_type": "type",
}
SECRET_KEYS = (
    "REDDIT_CLIENT_ID",
    "REDDIT_CLIENT_SECRET",
    "REDDIT_REFRESH_TOKEN",
    "REDDIT_USERNAME",
    "REDDIT_PASSWORD",
)
SETUP = (
    "Reddit write access is not set up. Put REDDIT_CLIENT_ID and REDDIT_CLIENT_SECRET (a script app owned by "
    "u/{user}) plus REDDIT_REFRESH_TOKEN, or REDDIT_USERNAME and REDDIT_PASSWORD, in the environment or the fleet "
    "secrets.env. See docs/community/reddit.md, section 'The u/forkmesh profile'."
)


def load_profile(path=PROFILE):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    user = str(data.get("user", "")).removeprefix("u/").strip()
    if not USERNAME.fullmatch(user):
        raise ValueError(f"{path}: user must be a Reddit username, got {user!r}")
    fields = data.get("profile") or {}
    unknown = sorted(set(fields) - set(FIELDS))
    if unknown:
        raise ValueError(f"{path}: only {', '.join(FIELDS)} can be set, not {', '.join(unknown)}")
    for name, limit in FIELDS.items():
        if name not in fields:
            continue
        value = fields[name]
        if not isinstance(value, str) or not value.strip():
            raise ValueError(f"{path}: {name} must be non-empty text")
        if len(value) > limit:
            raise ValueError(f"{path}: {name} is {len(value)} characters; Reddit allows {limit}")
    return {"user": user, "profile": {name: fields[name] for name in FIELDS if name in fields}}


def changes(desired, current):
    return {
        name: {"from": current.get(name), "to": value}
        for name, value in desired.items()
        if current.get(name) != value
    }


def site_admin_form(settings, updates):
    form = {}
    for key, value in settings.items():
        if value is None or isinstance(value, (list, dict)):
            continue
        if isinstance(value, bool):
            value = "true" if value else "false"
        form[SETTING_NAMES.get(key, key)] = str(value)
    if not form.get("sr"):
        raise RuntimeError("Reddit returned the profile settings without its id; nothing was changed")
    form.update(updates)
    form["api_type"] = "json"
    return form


def feed_profile(raw):
    root = ET.fromstring(raw)
    about = root.findtext(f"{reddit_scout.ATOM}subtitle")
    return {"title": None, "public_description": about.strip() if about is not None else None}


class ProfileClient(reddit_scout.Reddit):
    def __init__(self, client_id="", client_secret="", refresh_token="", username="", password=""):
        super().__init__(client_id, client_secret)
        self._refresh_token = refresh_token
        self._username = username
        self._password = password

    @property
    def can_write(self):
        return self.mode == "oauth" and bool(self._refresh_token or (self._username and self._password))

    def _grant(self):
        if self._refresh_token:
            return {"grant_type": "refresh_token", "refresh_token": self._refresh_token}
        if self._username and self._password:
            return {"grant_type": "password", "username": self._username, "password": self._password}
        return {"grant_type": "client_credentials"}

    def _authorize(self):
        if self._token:
            return self._token
        basic = base64.b64encode(f"{self._client_id}:{self._client_secret}".encode()).decode()
        request = urllib.request.Request(
            reddit_scout.TOKEN_URL,
            data=urllib.parse.urlencode(self._grant()).encode(),
            method="POST",
            headers={"Authorization": f"Basic {basic}", "User-Agent": reddit_scout.USER_AGENT},
        )
        self._token = json.loads(self._open(request)).get("access_token", "")
        if not self._token:
            raise RuntimeError("Reddit returned no access token; check the app and account credentials")
        return self._token

    def current(self, user):
        if self.mode == "oauth":
            profile = self._get_json(f"/user/{user}/about", {}).get("data", {}).get("subreddit") or {}
            return {name: profile.get(name) for name in FIELDS}
        request = urllib.request.Request(
            f"{reddit_scout.WWW}/user/{user}/.rss", headers={"User-Agent": reddit_scout.USER_AGENT}
        )
        return feed_profile(self._open(request))

    def signed_in_as(self):
        return self._get_json("/api/v1/me", {}).get("name", "")

    def settings(self, user):
        return self._get_json(f"/r/u_{user}/about/edit", {}).get("data") or {}

    def save(self, form):
        request = urllib.request.Request(
            f"{reddit_scout.OAUTH}/api/site_admin",
            data=urllib.parse.urlencode(form).encode(),
            method="POST",
            headers={"Authorization": f"Bearer {self._authorize()}", "User-Agent": reddit_scout.USER_AGENT},
        )
        reply = json.loads(self._open(request) or b"{}")
        errors = (reply.get("json") or {}).get("errors") or []
        if errors:
            raise RuntimeError("Reddit refused the profile update: " + "; ".join(" ".join(map(str, e)) for e in errors))


def _client():
    found = reddit_scout.secret_values(SECRET_KEYS)
    if not (found["REDDIT_CLIENT_ID"] and found["REDDIT_CLIENT_SECRET"]):
        return ProfileClient()
    return ProfileClient(*(found[key] for key in SECRET_KEYS))


def render_changes(diff):
    lines = []
    for name, change in diff.items():
        before = "unknown (reading it needs Reddit credentials)" if change["from"] is None else json.dumps(change["from"], ensure_ascii=False)
        lines.append(f"{LABELS[name]}: {before} -> {json.dumps(change['to'], ensure_ascii=False)}")
    return lines


def command_show(args):
    wanted = load_profile(args.profile)
    client = _client()
    current = client.current(wanted["user"])
    if args.json:
        print(json.dumps({"user": wanted["user"], "mode": client.mode, "current": current}, indent=2, ensure_ascii=False))
        return 0
    print(f"u/{wanted['user']} as Reddit shows it ({client.mode}):")
    for name in FIELDS:
        value = current.get(name)
        print(f"{LABELS[name]}: " + ("unknown (reading it needs Reddit credentials)" if value is None else value))
    return 0


def command_plan(args):
    wanted = load_profile(args.profile)
    diff = changes(wanted["profile"], _client().current(wanted["user"]))
    if args.json:
        print(json.dumps({"user": wanted["user"], "changes": diff}, indent=2, ensure_ascii=False))
        return 0
    print("\n".join(render_changes(diff)) if diff else f"u/{wanted['user']} already matches {Path(args.profile).name}")
    return 0


def command_apply(args):
    wanted = load_profile(args.profile)
    user = wanted["user"]
    client = _client()
    if not client.can_write:
        print(SETUP.format(user=user), file=sys.stderr)
        return 2
    signed_in = client.signed_in_as()
    if signed_in.lower() != user.lower():
        print(f"the Reddit credentials sign in as u/{signed_in or '?'}, not u/{user}; nothing was changed", file=sys.stderr)
        return 2
    diff = changes(wanted["profile"], client.current(user))
    if not diff:
        print(f"u/{user} already matches {Path(args.profile).name}; nothing to change")
        return 0
    print("\n".join(render_changes(diff)))
    if not args.yes:
        print("dry run: pass --yes to save these changes to Reddit")
        return 0
    client.save(site_admin_form(client.settings(user), {name: change["to"] for name, change in diff.items()}))
    left = changes(wanted["profile"], client.current(user))
    if left:
        print("Reddit accepted the update but still shows:\n" + "\n".join(render_changes(left)), file=sys.stderr)
        return 1
    print(f"saved: u/{user} now matches {Path(args.profile).name}")
    return 0


def build_parser():
    parser = argparse.ArgumentParser(
        description="Keep the u/forkmesh Reddit profile (display name and about text) in line with tools/reddit_profile.json. It changes nothing else on Reddit."
    )
    parser.add_argument("--profile", default=str(PROFILE))
    sub = parser.add_subparsers(dest="command", required=True)

    show = sub.add_parser("show", help="read the profile as Reddit shows it now")
    show.add_argument("--json", action="store_true")
    show.set_defaults(func=command_show)

    plan = sub.add_parser("plan", help="list what apply would change")
    plan.add_argument("--json", action="store_true")
    plan.set_defaults(func=command_plan)

    apply = sub.add_parser("apply", help="save the display name and about text from the profile file to Reddit")
    apply.add_argument("--yes", action="store_true", help="save; without it apply only lists the changes")
    apply.set_defaults(func=command_apply)
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 2
    except urllib.error.HTTPError as error:
        print(f"Reddit answered HTTP {error.code} for {error.url}", file=sys.stderr)
        return 1
    except urllib.error.URLError as error:
        print(f"Reddit could not be reached: {error.reason}", file=sys.stderr)
        return 1
    except RuntimeError as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
