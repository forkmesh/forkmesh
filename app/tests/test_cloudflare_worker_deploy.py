import json
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import cloudflare_worker_deploy as cf  # noqa: E402


TOKEN = "cf-token-abcdefghijklmnop"
ACCOUNT = "0123456789abcdef0123456789abcdef"


def git_repo(path):
    subprocess.run(["git", "init", "-q", str(path)], check=True)
    return path


def worker(path, name="shop", envs=("production",), fmt="toml"):
    path.mkdir(parents=True, exist_ok=True)
    if fmt == "toml":
        text = f'name = "{name}"\nmain = "src/index.js"\n'
        for env in envs:
            text += f'\n[env.{env}]\nname = "{name}-live"\n'
        (path / "wrangler.toml").write_text(text)
    else:
        body = {"name": name, "main": "src/index.js",
                "env": {env: {"vars": {"MODE": env}} for env in envs}}
        text = "// Worker config\n" + json.dumps(body, indent=2)
        (path / "wrangler.jsonc").write_text(text.replace('"main"', '/* x */ "main"'))
    (path / "package.json").write_text(json.dumps({"devDependencies": {"wrangler": "^4"}}))
    (path / "package-lock.json").write_text("{}")
    return path


def fake_tools(tmp_path, monkeypatch, output="", node_version="v22.4.0"):
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    log = tmp_path / "calls.log"
    for tool in ("npm", "npx"):
        script = bin_dir / tool
        script.write_text(
            "#!/bin/sh\n"
            f'echo "{tool} $*" >> "{log}"\n'
            f'echo "{tool} sees $CLOUDFLARE_ACCOUNT_ID $CLOUDFLARE_API_TOKEN"\n'
            + (f"cat <<'EOF'\n{output}\nEOF\n" if tool == "npx" else ""))
        script.chmod(0o755)
    node = bin_dir / "node"
    node.write_text(f"#!/bin/sh\necho {node_version}\n")
    node.chmod(0o755)
    monkeypatch.setenv("PATH", f"{bin_dir}:/usr/bin:/bin")
    return log


def test_jsonc_comments_and_trailing_commas_are_removed_but_strings_kept():
    text = '{\n // note\n "url": "https://a.example//x", /* block */ "list": [1, 2,],\n}'
    assert json.loads(cf.strip_jsonc(text)) == {"url": "https://a.example//x",
                                                "list": [1, 2]}


def test_toml_fallback_reads_name_and_environments():
    data = cf.parse_toml_fallback(
        'name = "shop" # main\n[env.production]\nname = "shop-live"\n[[routes]]\n')
    assert data["name"] == "shop"
    assert data["env"]["production"]["name"] == "shop-live"


@pytest.mark.parametrize("fmt", ["toml", "jsonc"])
def test_connect_scaffolds_config_script_and_workflows(tmp_path, fmt):
    repo = git_repo(tmp_path / "repo")
    worker(repo / "worker", fmt=fmt)
    result = cf.connect(repo=repo, worker_dir=Path("worker"), environment="production")

    config = json.loads((repo / cf.CONFIG_REL).read_text())
    assert config["schema"] == cf.SCHEMA
    assert config["workerDir"] == "worker"
    assert config["wranglerConfig"] == ("wrangler.toml" if fmt == "toml" else "wrangler.jsonc")
    assert config["keepVars"] is True
    assert (repo / cf.SCRIPT_REL).read_bytes() == (ROOT / "tools/cloudflare_worker_deploy.py").read_bytes()
    assert set(result["written"]) == {cf.CONFIG_REL, cf.SCRIPT_REL,
                                      cf.DEPLOY_WORKFLOW_REL, cf.ROLLBACK_WORKFLOW_REL}
    assert result["worker"] == ("shop-live" if fmt == "toml" else "shop-production")
    assert any("Node labels" in step for step in result["nextSteps"])
    again = cf.connect(repo=repo, worker_dir=Path("worker"), environment="production")
    assert again["written"] == []


def test_deploy_workflow_pins_the_desktop_label_and_maps_secrets_through_env(tmp_path):
    repo = git_repo(tmp_path / "repo")
    worker(repo)
    cf.connect(repo=repo, needs=[".forkmesh/ci.yml"])
    text = (repo / cf.DEPLOY_WORKFLOW_REL).read_text()

    assert "runs-on: cloudflare-deploy\n" in text
    assert "on:\n  push:\n  workflow_dispatch:\n" in text
    assert "needs: [.forkmesh/ci.yml]\n" in text
    assert "  CLOUDFLARE_API_TOKEN: ${{ vars.CLOUDFLARE_API_TOKEN }}\n" in text
    assert "  CLOUDFLARE_ACCOUNT_ID: ${{ vars.CLOUDFLARE_ACCOUNT_ID }}\n" in text
    run_lines = [line for line in text.splitlines() if "run:" in line]
    assert run_lines and all("vars." not in line for line in run_lines)
    rollback = (repo / cf.ROLLBACK_WORKFLOW_REL).read_text()
    assert "on: [workflow_dispatch]\n" in rollback and "rollback" in rollback


def test_manual_only_and_custom_variables(tmp_path):
    repo = git_repo(tmp_path / "repo")
    worker(repo)
    cf.connect(repo=repo, on_push=False, runs_on="studio-pc", rollback=False,
               token_variable="SHOP_CF_TOKEN", account_variable="SHOP_CF_ACCOUNT")
    text = (repo / cf.DEPLOY_WORKFLOW_REL).read_text()
    assert "on: [workflow_dispatch]\n" in text
    assert "runs-on: studio-pc\n" in text
    assert "${{ vars.SHOP_CF_TOKEN }}" in text and "${{ vars.SHOP_CF_ACCOUNT }}" in text
    assert not (repo / cf.ROLLBACK_WORKFLOW_REL).exists()


def test_connect_warns_about_existing_github_deploys(tmp_path):
    repo = git_repo(tmp_path / "repo")
    worker(repo)
    workflows = repo / ".github" / "workflows"
    workflows.mkdir(parents=True)
    (workflows / "deploy.yml").write_text("steps:\n  - uses: cloudflare/wrangler-action@v3\n")
    (workflows / "test.yml").write_text("steps:\n  - run: npm test\n")
    result = cf.connect(repo=repo)
    assert result["existingGitHubDeploys"] == [".github/workflows/deploy.yml"]
    assert "Turn off the GitHub deploys" in result["nextSteps"][0]


@pytest.mark.parametrize("setup, message", [
    (lambda repo: None, "no wrangler.jsonc"),
    (lambda repo: worker(repo, envs=()), "has no [env.production]"),
    (lambda repo: (repo / "wrangler.toml").write_text('main = "x.js"\n'), "no top-level name"),
])
def test_connect_refuses_unusable_workers(tmp_path, setup, message):
    repo = git_repo(tmp_path / "repo")
    setup(repo)
    with pytest.raises(cf.DeployError, match=message.replace("[", r"\[").replace("]", r"\]")) as info:
        cf.connect(repo=repo, environment="production")
    assert info.value.code == cf.EXIT_CONFIG


def test_connect_refuses_bad_labels_and_paths_outside_the_repository(tmp_path):
    repo = git_repo(tmp_path / "repo")
    worker(repo)
    worker(tmp_path / "elsewhere")
    with pytest.raises(cf.DeployError, match="runsOn"):
        cf.connect(repo=repo, runs_on="Bad Label")
    with pytest.raises(cf.DeployError, match="inside the repository"):
        cf.connect(repo=repo, worker_dir=Path("../elsewhere"))


def test_deploy_installs_runs_wrangler_redacts_the_token_and_reports_urls(
        tmp_path, monkeypatch, capsys):
    repo = git_repo(tmp_path / "repo")
    worker(repo, envs=("production",))
    cf.connect(repo=repo, environment="production")
    log = fake_tools(tmp_path, monkeypatch, output=(
        "Uploaded shop-live (1.2 sec)\n"
        "Deployed shop-live triggers (0.4 sec)\n"
        "  \x1b[36mhttps://shop-live.example.workers.dev\x1b[39m\n"
        "  shop.example.com (custom domain)\n"
        "  example.com/api/* (zone name: example.com)\n"
        "  schedule: */5 * * * *\n"
        "  https://shop-live.example.workers.dev\n"
        "Current Version ID: 1b2c3d4e-0000-4000-8000-123456789abc\n"
        "See https://developers.cloudflare.com/workers/ for more"))
    monkeypatch.setenv("CLOUDFLARE_API_TOKEN", TOKEN)
    monkeypatch.setenv("CLOUDFLARE_ACCOUNT_ID", ACCOUNT)
    monkeypatch.setenv("FORKMESH_COMMIT", "a" * 40)
    monkeypatch.setenv("FORKMESH_REF", "refs/heads/main")

    assert cf.main(["deploy", "--repo", str(repo)]) == 0
    out = capsys.readouterr().out
    calls = log.read_text().splitlines()
    assert calls[0] == "npm ci --no-audit --no-fund"
    assert calls[1] == ("npx --no-install wrangler deploy --config wrangler.toml --env production "
                        "--keep-vars --message ForkMesh aaaaaaaaaaaa")
    assert TOKEN not in out
    assert f"npx sees {ACCOUNT} ***" in out
    assert ("::cloudflare-deploy:: ok worker=shop version=1b2c3d4e-0000-4000-8000-123456789abc "
            "url=https://shop-live.example.workers.dev url=shop.example.com "
            "url=example.com/api/*\n") in out


def test_deploy_skips_pushes_to_other_branches(tmp_path, monkeypatch, capsys):
    repo = git_repo(tmp_path / "repo")
    worker(repo)
    cf.connect(repo=repo)
    log = fake_tools(tmp_path, monkeypatch)
    monkeypatch.setenv("FORKMESH_REF", "refs/heads/feature")
    assert cf.main(["deploy", "--repo", str(repo)]) == 0
    assert "skipped refs/heads/feature is not refs/heads/main" in capsys.readouterr().out
    assert not log.exists()


def test_deploy_without_credentials_is_a_configuration_error(tmp_path, monkeypatch, capsys):
    repo = git_repo(tmp_path / "repo")
    worker(repo)
    cf.connect(repo=repo)
    fake_tools(tmp_path, monkeypatch)
    monkeypatch.delenv("CLOUDFLARE_API_TOKEN", raising=False)
    monkeypatch.setenv("CLOUDFLARE_ACCOUNT_ID", ACCOUNT)
    monkeypatch.delenv("FORKMESH_REF", raising=False)
    assert cf.main(["deploy", "--repo", str(repo)]) == cf.EXIT_CONFIG
    assert "Settings > Secrets" in capsys.readouterr().err
    monkeypatch.setenv("CLOUDFLARE_API_TOKEN", TOKEN)
    monkeypatch.setenv("CLOUDFLARE_ACCOUNT_ID", "not-an-account")
    assert cf.main(["deploy", "--repo", str(repo)]) == cf.EXIT_CONFIG


def test_rollback_runs_wrangler_rollback(tmp_path, monkeypatch, capsys):
    repo = git_repo(tmp_path / "repo")
    worker(repo)
    cf.connect(repo=repo)
    log = fake_tools(tmp_path, monkeypatch)
    monkeypatch.setenv("CLOUDFLARE_API_TOKEN", TOKEN)
    monkeypatch.setenv("CLOUDFLARE_ACCOUNT_ID", ACCOUNT)
    assert cf.main(["rollback", "--repo", str(repo)]) == 0
    assert log.read_text().splitlines()[-1] == (
        "npx --no-install wrangler rollback --config wrangler.toml "
        "--message Rolled back from ForkMesh")
    assert "::cloudflare-rollback:: ok worker=shop" in capsys.readouterr().out


def test_check_offline_reads_the_worker_and_node(tmp_path, monkeypatch, capsys):
    repo = git_repo(tmp_path / "repo")
    worker(repo)
    cf.connect(repo=repo, environment="production")
    fake_tools(tmp_path, monkeypatch)
    assert cf.main(["check", "--repo", str(repo)]) == 0
    out = capsys.readouterr().out
    assert "worker: shop-live" in out and "node: 22" in out and "::cloudflare-check:: ok" in out


def test_check_online_verifies_the_token_and_the_worker(tmp_path, monkeypatch):
    repo = git_repo(tmp_path / "repo")
    worker(repo)
    cf.connect(repo=repo)
    fake_tools(tmp_path, monkeypatch)
    monkeypatch.setenv("CLOUDFLARE_API_TOKEN", TOKEN)
    monkeypatch.setenv("CLOUDFLARE_ACCOUNT_ID", ACCOUNT)
    seen = []

    def fake_get(path, token, timeout=20):
        seen.append(path)
        assert token == TOKEN
        if path == "/user/tokens/verify":
            return 401, {}
        if path.endswith("/tokens/verify"):
            return 200, {"result": {"status": "active"}}
        return 404, {}

    monkeypatch.setattr(cf, "cloudflare_get", fake_get)
    report = cf.check(repo, online=True)
    assert report["token"] == "active"
    assert report["workerExists"] is False
    assert seen == ["/user/tokens/verify", f"/accounts/{ACCOUNT}/tokens/verify",
                    f"/accounts/{ACCOUNT}/workers/scripts/shop/settings"]


def test_unreachable_api_points_at_the_network_variable(monkeypatch):
    def refuse(*args, **kwargs):
        raise cf.URLError("Network is unreachable")

    monkeypatch.setattr(cf, "urlopen", refuse)
    with pytest.raises(cf.DeployError, match=cf.NETWORK_VARIABLE) as info:
        cf.cloudflare_get("/user/tokens/verify", TOKEN)
    assert info.value.code == cf.EXIT_NETWORK


def test_connect_with_github_scaffolds_the_bridge(tmp_path):
    pytest.importorskip("cryptography")
    repo = git_repo(tmp_path / "repo")
    worker(repo)
    result = cf.connect(repo=repo, github="octo/shop", bridge_key_out=tmp_path / "key.pem")
    assert (repo / ".github/workflows/forkmesh-github-sync.yml").is_file()
    assert result["github"]["forkmeshRemote"] == "ssh://git@ssh.forkmesh.com/octo/shop.git"
    assert any("FORKMESH_SYNC_SSH_KEY" in step for step in result["nextSteps"])


def test_check_refuses_old_node_and_a_small_address_space(tmp_path, monkeypatch):
    repo = git_repo(tmp_path / "repo")
    worker(repo)
    cf.connect(repo=repo)
    fake_tools(tmp_path, monkeypatch, node_version="v20.11.1")
    with pytest.raises(cf.DeployError, match="Node.js 20 is too old"):
        cf.check(repo)
    (tmp_path / "bin" / "node").write_text("#!/bin/sh\necho v22.4.0\n")
    monkeypatch.setattr(cf, "address_space_limit", lambda: 24 * 1024 ** 3)
    with pytest.raises(cf.DeployError, match="24 GiB"):
        cf.check(repo)


def test_monorepo_installs_at_the_root_lockfile_and_uses_the_hoisted_wrangler(
        tmp_path, monkeypatch, capsys):
    repo = git_repo(tmp_path / "repo")
    app = repo / "apps" / "shop"
    app.mkdir(parents=True)
    (app / "wrangler.toml").write_text('name = "shop"\n')
    (app / "package.json").write_text("{}")
    (repo / "pnpm-lock.yaml").write_text("lockfileVersion: 9\n")
    hoisted = repo / "node_modules" / ".bin" / "wrangler"
    hoisted.parent.mkdir(parents=True)
    log = fake_tools(tmp_path, monkeypatch)
    hoisted.write_text(f'#!/bin/sh\necho "wrangler $* in $(pwd)" >> "{log}"\n')
    hoisted.chmod(0o755)
    pnpm = tmp_path / "bin" / "pnpm"
    pnpm.write_text(f'#!/bin/sh\necho "pnpm $* in $(pwd)" >> "{log}"\n')
    pnpm.chmod(0o755)
    cf.connect(repo=repo, worker_dir=Path("apps/shop"))
    monkeypatch.setenv("CLOUDFLARE_API_TOKEN", TOKEN)
    monkeypatch.setenv("CLOUDFLARE_ACCOUNT_ID", ACCOUNT)
    monkeypatch.delenv("FORKMESH_REF", raising=False)
    monkeypatch.delenv("FORKMESH_COMMIT", raising=False)

    assert cf.main(["deploy", "--repo", str(repo)]) == 0
    assert log.read_text().splitlines() == [
        f"pnpm install --frozen-lockfile in {repo.resolve()}",
        f"wrangler deploy --config wrangler.toml --keep-vars in {app.resolve()}",
    ]
    capsys.readouterr()
    assert cf.main(["check", "--repo", str(repo)]) == 0
    assert "install: pnpm install --frozen-lockfile" in capsys.readouterr().out
