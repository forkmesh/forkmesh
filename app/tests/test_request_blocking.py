from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
DEPLOY = (ROOT / "deploy.sh").read_text(encoding="utf-8")


def test_entrypoint_has_no_database_work_before_application_dispatch():
    handler = ENTRY.split("class Default(WorkerEntrypoint):", 1)[1]
    handler = handler.split("\n\nasync def chat_history_recent", 1)[0]
    assert "request_blocking" not in handler
    assert "blocked_response" not in handler
    assert handler.count("await ") == 3
    assert "return await self._runtime().fetch(request)" in handler


def test_deploy_syncs_database_rules_to_edge_before_worker_upload():
    deploy = DEPLOY.split("deploy_app_target() {", 1)[1]
    assert deploy.index("./sync_request_block_rules.sh") < deploy.index(
        "deploy_app_version")
    assert deploy.index("./sync_request_rate_limit.sh") < deploy.index(
        "deploy_app_version")


def test_no_obsolete_python_request_policy_module_is_shipped():
    assert not (ROOT / "src" / "request_blocking.py").exists()
