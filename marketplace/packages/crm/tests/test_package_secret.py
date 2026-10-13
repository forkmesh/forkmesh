import ast
import re
from pathlib import Path
from types import SimpleNamespace


ENTRY = Path(__file__).resolve().parents[4] / "app" / "src" / "entry.py"


def _package_secret():
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"))
    runtime = next(node for node in tree.body
                   if isinstance(node, ast.ClassDef) and node.name == "_PluginApiRuntime")
    method = next(node for node in runtime.body
                  if isinstance(node, ast.FunctionDef) and node.name == "package_secret")
    namespace = {"re": re}
    exec(compile(ast.Module(body=[method], type_ignores=[]), str(ENTRY), "exec"), namespace)
    return namespace["package_secret"]


def test_a_package_reads_only_secrets_under_its_own_prefix():
    secret = _package_secret()
    env = SimpleNamespace(PACKAGE_CRM_ZOHO_OAUTH_CLIENT_ID=" client ",
                          PACKAGE_OTHER_TOKEN="other", DATA_KEY="platform")
    crm = SimpleNamespace(env=env, _plugin="crm")
    assert secret(crm, "ZOHO_OAUTH_CLIENT_ID") == "client"
    assert secret(crm, "MISSING") == ""
    for name in ("DATA_KEY", "../DATA_KEY", "zoho_oauth_client_id", "", "A" * 65):
        assert secret(crm, name) == ""
    assert secret(SimpleNamespace(env=env, _plugin="other"), "TOKEN") == "other"
    assert secret(SimpleNamespace(env=env, _plugin="other"), "ZOHO_OAUTH_CLIENT_ID") == ""
    assert secret(SimpleNamespace(env=env, _plugin=""), "DATA_KEY") == ""
