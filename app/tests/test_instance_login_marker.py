import ast
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _module_names(tree):
    names = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Import, ast.ImportFrom)):
            for alias in node.names:
                names.add((alias.asname or alias.name).split(".")[0])
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            names.add(node.name)
        elif isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    names.add(target.id)
    return names


def test_first_admin_login_marker_uses_an_imported_path():
    source = (ROOT / "src" / "entry.py").read_text()
    tree = ast.parse(source)
    defined = _module_names(tree)
    for node in ast.walk(tree):
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
                and node.func.id == "Path"):
            assert "Path" in defined, "entry.py calls Path() without importing it"
    assert "_MarkerPath(marker).touch(exist_ok=True)" in source
