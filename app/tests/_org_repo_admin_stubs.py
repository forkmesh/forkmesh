import ast
import importlib
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"

REPO_ADMIN_FUNCTIONS = (
    "team_permission_rank",
    "_org_role",
    "_org_permission",
    "_org_linked_repo_permission",
    "_org_admin_of_linked_repo",
    "_repo_admin_allowed",
)


def org_database():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript(
        "CREATE TABLE org_repos (org_bi TEXT, repo TEXT, node_owner TEXT,"
        " created_at INTEGER DEFAULT 0);"
        "CREATE TABLE org_members (org_bi TEXT, member_bi TEXT, role TEXT);"
        "CREATE TABLE org_teams (org_bi TEXT, team TEXT, permission TEXT);"
        "CREATE TABLE org_team_members (org_bi TEXT, team TEXT,"
        " member_bi TEXT);")
    return db


def d1_for(db):
    async def d1_all(_env, sql, *args):
        return [dict(row) for row in db.execute(sql, args).fetchall()]

    async def d1_first(_env, sql, *args):
        row = db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(_env, sql, *args):
        db.execute(sql, args)
        db.commit()

    return d1_all, d1_first, d1_run


async def blind_index(_env, value):
    return "bi:" + str(value or "").strip().lower()


def entry_functions(names, extra_globals):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    wanted = set(names)
    selected = [
        node for node in tree.body
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in wanted)
        or (isinstance(node, ast.Assign)
            and any(isinstance(target, ast.Name)
                    and target.id == "TEAM_PERMISSIONS"
                    for target in node.targets))
    ]
    found = {node.name for node in selected if hasattr(node, "name")}
    assert found == wanted, "missing: %s" % sorted(wanted - found)
    sys.path.insert(0, str(ROOT / "src"))
    try:
        rbac = importlib.import_module("organization_rbac")
    finally:
        sys.path.pop(0)
    namespace = {"_org_rbac": rbac, "blind_index": blind_index}
    namespace.update(extra_globals)
    exec(compile(ast.Module(body=selected, type_ignores=[]), str(ENTRY),
                 "exec"), namespace)
    return namespace


def link(db, org, node, repo):
    db.execute("INSERT INTO org_repos (org_bi, repo, node_owner)"
               " VALUES (?,?,?)", ("org:" + org, repo.lower(), node.lower()))


def member(db, org, account, role):
    db.execute("INSERT INTO org_members VALUES (?,?,?)",
               ("org:" + org, "bi:" + account, role))


def team(db, org, name, permission, accounts):
    db.execute("INSERT INTO org_teams VALUES (?,?,?)",
               ("org:" + org, name, permission))
    for account in accounts:
        db.execute("INSERT INTO org_team_members VALUES (?,?,?)",
                   ("org:" + org, name, "bi:" + account))
