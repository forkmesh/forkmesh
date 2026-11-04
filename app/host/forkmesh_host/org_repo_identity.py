from __future__ import annotations

import asyncio
import json
import os

from .instance_repo import APP_USER, NAME_RE, RepoError, _forked

VIEW_TABLES = (
    ("repo_view_totals", "repo_views", ()),
    ("issue_view_totals", "issue_views", ("number",)),
)
SET_TABLES = (
    ("repo_stars", ("account_bi",), ""),
    ("pull_batch_sent", ("target_bi", "branch", "head"), ""),
    ("repo_remote_forks", ("url_bi",), "remote-forks:"),
)


async def linked_repositories(entry, env, organization=""):
    rows = await entry.d1_all(
        env, "SELECT o.name AS org, r.repo AS repo, r.node_owner AS node "
        "FROM org_repos r JOIN orgs o ON o.org_bi=r.org_bi "
        "ORDER BY o.name, r.repo")
    linked = []
    for row in rows or []:
        org = str(row.get("org") or "").strip().lower()
        repo = str(row.get("repo") or "").strip()
        node = str(row.get("node") or "").strip().lower()
        if not org or not repo or not node or org == node:
            continue
        if organization and org != organization:
            continue
        if await entry._repo_organization(env, node, repo) != org:
            continue
        record = await entry._repo_catalog_record(env, node, repo)
        if not isinstance(record, dict) or record.get(
                "visibility") != "public":
            continue
        linked.append({"organization": org, "member": node,
                       "name": str(record.get("name") or repo)})
    return linked


def _signed(record):
    return bool(record.get("signature") or record.get("stateSig"))


async def _edge_parent(entry, env, key_bi):
    row = await entry.d1_first(
        env, "SELECT parent_bi FROM repo_forks WHERE fork_bi=?", key_bi)
    return str((row or {}).get("parent_bi") or "")


async def fork_changes(entry, env, linked):
    by_member = {("%s/%s" % (item["member"], item["name"])).lower(): item
                 for item in linked}
    if not by_member:
        return []
    rows = await entry.d1_all(
        env, "SELECT key_bi, owner_bi, data FROM repositories")
    changes = []
    for row in rows or []:
        record = await entry.decrypt_row(env, row.get("data"))
        if not isinstance(record, dict):
            continue
        parent = str(record.get("forkedFrom") or "").strip().lower()
        item = by_member.get(parent)
        if not item:
            item = next((value for value in linked if (
                "%s/%s" % (value["organization"], value["name"])).lower()
                == parent), None)
        if not item:
            continue
        target = ("%s/%s" % (item["organization"], item["name"])).lower()
        fork = "%s/%s" % (record.get("owner") or "", record.get("name") or "")
        if parent == target:
            change = "unchanged"
        elif _signed(record):
            change = "signed-kept"
        elif record.get("source") != "hosted-web":
            change = "not-hosted-kept"
        else:
            change = "rewrite"
        edge = "none"
        if record.get("visibility") == "public":
            wanted = await entry.blind_index(env, target)
            edge = "ok" if await _edge_parent(
                entry, env, row.get("key_bi")) == wanted else "rekey"
        if change == "unchanged" and edge in ("ok", "none"):
            continue
        changes.append({
            "fork": fork, "from": parent, "to": target,
            "record": change, "edge": edge,
            "_row": row, "_record": record})
    return changes


async def apply_fork(entry, env, change):
    hosted_git = entry._hosted_git._load()
    row, record = change["_row"], change["_record"]
    prior = dict(record)
    owner = str(record.get("owner") or "")
    name = str(record.get("name") or "")
    if change["record"] == "rewrite":
        record["forkedFrom"] = change["to"]
        await hosted_git._pin_hosted_state(
            env, owner, name, row.get("key_bi"), row, record,
            str(record.get("stateHash") or ""), notify=False, pusher=owner)
    await entry._index_repo_fork_edge(
        env, record, prior, row.get("owner_bi"), row.get("key_bi"))


async def _count(entry, env, sql, *args):
    row = await entry.d1_first(env, sql, *args)
    return int((row or {}).get("n") or 0)


async def table_changes(entry, env, item):
    old = ("%s/%s" % (item["member"], item["name"])).lower()
    new = ("%s/%s" % (item["organization"], item["name"])).lower()
    found = []
    for table, _columns, prefix in SET_TABLES:
        source = await entry.blind_index(env, prefix + old)
        rows = await _count(
            entry, env, "SELECT COUNT(*) AS n FROM %s WHERE repo_bi=?"
            % table, source)
        if rows:
            found.append({"table": table, "from": old, "to": new,
                          "rows": rows})
    source = await entry.blind_index(env, old)
    for totals, views, _keys in VIEW_TABLES:
        rows = await _count(
            entry, env, "SELECT COUNT(*) AS n FROM %s WHERE repo_bi=?"
            % views, source)
        total = await _count(
            entry, env, "SELECT COUNT(*) AS n FROM %s WHERE repo_bi=?"
            % totals, source)
        if rows or total:
            found.append({"table": views, "from": old, "to": new,
                          "rows": rows, "totals": total})
    return found


def _set_statements(table, columns, source, target):
    same = " AND ".join("n.%s=%s.%s" % (column, table, column)
                        for column in columns)
    return [
        ("UPDATE OR IGNORE %s SET repo_bi=? WHERE repo_bi=?" % table,
         (target, source)),
        ("DELETE FROM %s WHERE repo_bi=? AND EXISTS (SELECT 1 FROM %s n "
         "WHERE n.repo_bi=? AND %s)" % (table, table, same),
         (source, target)),
    ]


def _view_statements(totals, views, keys, source, target):
    match = " AND ".join(["o.viewer_key=n.viewer_key"] + [
        "o.%s=n.%s" % (key, key) for key in keys])
    per = "".join(" AND o.%s=%s.%s" % (key, totals, key) for key in keys)
    key_list = ", ".join(("repo_bi",) + keys)
    key_select = ", ".join(("?",) + keys)
    conflict = ", ".join(("repo_bi",) + keys)
    same_view = " AND ".join(["n.viewer_key=%s.viewer_key" % views] + [
        "n.%s=%s.%s" % (key, views, key) for key in keys])
    old_view = " AND ".join(["o.viewer_key=%s.viewer_key" % views] + [
        "o.%s=%s.%s" % (key, views, key) for key in keys])
    return [
        ("INSERT INTO %s (%s, views, viewers, last_viewed_at) "
         "SELECT %s, views, MAX(0, viewers - (SELECT COUNT(*) FROM %s o "
         "JOIN %s n ON n.repo_bi=? AND %s WHERE o.repo_bi=?%s)), "
         "last_viewed_at FROM %s WHERE repo_bi=? "
         "ON CONFLICT(%s) DO UPDATE SET views=%s.views+excluded.views, "
         "viewers=%s.viewers+excluded.viewers, "
         "last_viewed_at=MAX(%s.last_viewed_at, excluded.last_viewed_at)"
         % (totals, key_list, key_select, views, views, match, per, totals,
            conflict, totals, totals, totals),
         (target, target, source, source)),
        ("DELETE FROM %s WHERE repo_bi=?" % totals, (source,)),
        ("UPDATE %s SET hits=hits+(SELECT o.hits FROM %s o WHERE "
         "o.repo_bi=? AND %s), first_at=MIN(first_at, (SELECT o.first_at "
         "FROM %s o WHERE o.repo_bi=? AND %s)), last_at=MAX(last_at, "
         "(SELECT o.last_at FROM %s o WHERE o.repo_bi=? AND %s)) WHERE "
         "repo_bi=? AND EXISTS (SELECT 1 FROM %s o WHERE o.repo_bi=? AND %s)"
         % (views, views, old_view, views, old_view, views, old_view, views,
            old_view),
         (source, source, source, target, source)),
        ("UPDATE OR IGNORE %s SET repo_bi=? WHERE repo_bi=?" % views,
         (target, source)),
        ("DELETE FROM %s WHERE repo_bi=? AND EXISTS (SELECT 1 FROM %s n "
         "WHERE n.repo_bi=? AND %s)" % (views, views, same_view),
         (source, target)),
    ]


async def apply_tables(entry, env, item):
    old = ("%s/%s" % (item["member"], item["name"])).lower()
    new = ("%s/%s" % (item["organization"], item["name"])).lower()
    for table, columns, prefix in SET_TABLES:
        await entry.d1_run_batch(env, _set_statements(
            table, columns, await entry.blind_index(env, prefix + old),
            await entry.blind_index(env, prefix + new)))
    source = await entry.blind_index(env, old)
    target = await entry.blind_index(env, new)
    for totals, views, keys in VIEW_TABLES:
        await entry.d1_run_batch(env, _view_statements(
            totals, views, keys, source, target))


async def plan_and_apply(runtime, options):
    entry, env = runtime.entry, runtime.env
    await entry.ensure_schema(env)
    linked = await linked_repositories(
        entry, env, options.get("organization", ""))
    forks = await fork_changes(entry, env, linked)
    tables = []
    for item in linked:
        tables.extend(await table_changes(entry, env, item))
    if options.get("apply"):
        for change in forks:
            await apply_fork(entry, env, change)
        for item in linked:
            await apply_tables(entry, env, item)
        entry._ORG_ALIAS_MEMO.clear()
    return {
        "ok": True,
        "dryRun": not options.get("apply"),
        "repositories": linked,
        "forks": [{key: value for key, value in change.items()
                   if not key.startswith("_")} for change in forks],
        "tables": tables,
    }


def run(args):
    from .ops import _runtime
    organization = str(args.organization or "").strip().lower()
    if organization and not NAME_RE.fullmatch(organization):
        raise RepoError("invalid --organization")
    options = {"apply": bool(args.apply), "organization": organization}
    root = os.geteuid() == 0
    user = args.app_user if args.app_user is not None else (
        APP_USER if root else "")
    if user and not root:
        raise RepoError("run as root to act as %s" % user)

    def work():
        return asyncio.run(plan_and_apply(_runtime(args), options))
    return _forked(user, work) if user else work()


def cmd_org_repo_identity(args):
    from . import vault
    try:
        if vault.enabled(os.environ):
            raise RepoError("the app vault is on; this command needs the "
                            "data key in the app settings")
        result = run(args)
    except RepoError as error:
        result = {"ok": False, "error": str(error)}
    print(json.dumps(result, sort_keys=True))
    return 0 if result.get("ok") else 1


def add_parser(sub, common):
    command = sub.add_parser(
        "org-repo-identity",
        help="move fork links, fork edges, stars, views and sent pull "
             "batches of organization repositories from the member's "
             "address to the organization's (dry run unless --apply)")
    common(command)
    command.add_argument("--organization", default="",
                         help="only this organization's repositories")
    command.add_argument("--apply", action="store_true",
                         help="write the changes (default: print them)")
    command.add_argument("--app-user", default=None,
                         help="user for database work (root: forkmesh-app)")
    command.set_defaults(func=cmd_org_repo_identity)
