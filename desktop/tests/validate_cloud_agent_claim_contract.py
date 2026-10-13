#!/usr/bin/env python3
"""Static guardrails for "Continue on this desktop".

A run queued from the website is claimed by whichever node takes it off the
relay's FIFO. "Continue on this desktop" is the same claim asked for by name,
so the rules the drain obeys have to hold here too: the desktop leases the job
before it runs anything, starts it through the one path that reports the
result, and shows the run once — as a local session, not also as the relay's
shadow of it.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CLOUD = (ROOT / "src/MainWindowAgentCloud.cpp").read_text(encoding="utf-8")
PARSE = (ROOT / "src/AgentCloudSessions.cpp").read_text(encoding="utf-8")
STORE = (ROOT / "src/AgentStore.cpp").read_text(encoding="utf-8")
SYNC = (ROOT / "src/MainWindowAgentSync.cpp").read_text(encoding="utf-8")
RELAY = (ROOT.parent / "app/src/entry.py").read_text(encoding="utf-8")


def between(source: str, start: str, end: str | None = None) -> str:
    begin = source.index(start)
    finish = source.index(end, begin) if end is not None else len(source)
    return source[begin:finish]


def main() -> None:
    # A run aimed at this desktop is shown, not hidden: nothing claims it while
    # ForkMesh is closed, and the list is where a person sees it waiting.
    parse = between(
        PARSE,
        "QList<CloudAgentSession> parseCloudAgentRows(",
        "AgentSession cloudAgentListEntry(",
    )
    assert "cloud.targetsThisNode =" in parse
    assert "if (!self.isEmpty() && cloud.targetNode.toLower() == self)\n" \
           "            continue;" not in parse

    # The one thing that keeps a taken-over run from showing twice is the
    # durable link, so it must be written where a job is accepted, persisted,
    # and read before the relay's row is turned into a second row.
    binding = between(
        SYNC,
        "void MainWindow::persistOrgAgentBinding(",
        "void MainWindow::clearOrgAgentBinding(",
    )
    assert "session->webSessionId = webSessionId;" in binding
    assert 'obj["webSessionId"] = webSessionId;' in STORE
    assert 'obj.value("webSessionId")' in STORE
    apply_payload = between(
        CLOUD,
        "void MainWindow::applyCloudAgentSessions(",
        "void MainWindow::injectCloudAgentSessions()",
    )
    assert "localSessionForCloudSession(cloud.sessionId) > 0" in apply_payload

    # Refuse before claiming, never after: a lease taken on a machine that
    # cannot run it would fail somebody's queued session for them.
    claim = between(
        CLOUD,
        "void MainWindow::continueCloudAgentHere(",
        "void MainWindow::finishCloudAgentClaim(",
    )
    blocked = claim.index("cloudAgentContinueHereBlocker(sessionId)")
    availability = claim.index("agentProviderAvailability(cloud.provider)")
    posted = claim.index("m_networkAccess->post(")
    assert blocked < availability < posted
    # The claim carries the node proof the job queue answers to, not the
    # browser-facing list proofs.
    assert "signedInboxQuery(" in claim
    assert "cloudAgentClaimPath(" in claim
    assert "repoSegment(accountOwner().trimmed().toLower()" in claim
    assert "repoSegment(cloud.owner" not in claim
    assert "cloudAgentSessionsUrl(cloud.relayRepoOwner()" in claim
    assert "cloudAgentSessionsUrl(cloud.owner" not in CLOUD
    session_url = between(
        CLOUD,
        "QUrl MainWindow::cloudAgentSessionUrl(",
        "bool MainWindow::authenticateCloudAgentRequest(",
    )
    assert "cloud.owner" not in session_url
    assert session_url.count("cloud.relayRepoOwner()") == 2
    assert "cloudAgentListProof" not in claim

    # A leased job runs through the drain's own path, which is what reports the
    # result back and releases the lease.
    started = between(
        CLOUD,
        "void MainWindow::finishCloudAgentClaim(",
        "// A cloud conversation is short",
    )
    verified = started.index('job.value(QStringLiteral("sessionId")).toString() !=')
    dispatched = started.index("applyOrgAgentJobsPayload(repo, QJsonArray{job});")
    assert verified < dispatched
    assert "m_cloudAgentSessions.remove(sessionId);" in started

    # An ended run continues on its own branch: fetch the session branch from
    # the served mirror, then cut the new local agent branch from that tip.
    ended = between(
        CLOUD,
        "void MainWindow::continueCloudRunOnItsBranch(",
        "QString MainWindow::cloudAgentContinueHereTip(",
    )
    fetch = ended.index("fetchServedBranch(local, branch, /*update=*/true)")
    plan = ended.index("cloudAgentContinuePlan(cloud, fetched)")
    start = ended.index("startAdHocAgentForRepo(")
    assert fetch < plan < start
    assert "cloudAgentBranchContinuationPrompt(cloud, plan)" in ended
    assert "cloud.model" in ended[start:]
    assert "startBranch);" in ended[start:]
    assert "session->webSessionId = cloud.sessionId;" in ended

    # A started run still going is stopped on the relay first, with the
    # desktop's own key-signed stop, only after the person confirms, and only
    # a successful stop continues here.
    click = between(
        CLOUD,
        "void MainWindow::continueCloudAgentHere(",
        "void MainWindow::finishCloudAgentClaim(",
    )
    assert click.index("cloudAgentStoppable(cloud)") < click.index(
        "stopCloudRunAndContinueHere(sessionId)") < click.index(
        "continueFinishedCloudAgentHere(sessionId)")
    ask = between(
        CLOUD,
        "void MainWindow::stopCloudRunAndContinueHere(",
        "void MainWindow::postCloudRunStopThenContinue(",
    )
    assert "Stop the cloud run and continue here" in ask
    assert "postCloudRunStopThenContinue(sessionId)" in ask
    assert "m_networkAccess" not in ask
    stop = between(
        CLOUD,
        "void MainWindow::postCloudRunStopThenContinue(",
        "void MainWindow::readCloudAgentSessionThen(",
    )
    runnable = stop.index("agentProviderAvailability(provider)")
    signed = stop.index("cloudAgentStopProof()")
    assert runnable < signed
    posted = stop.index('QByteArrayLiteral("{\\"action\\":\\"stop\\"}")')
    refused = stop.index("so it was not ")
    resumed = stop.index("continueCloudRunOnItsBranch(sessionId, fresh, runner)")
    assert signed < posted < refused < resumed
    assert "readCloudAgentSessionThen(" in stop
    assert "forkmesh-owner-agent-stop-v1" in (
        ROOT / "src/AgentCloudSessions.h").read_text(encoding="utf-8")
    routes = (ROOT.parent / "app/src/route_handlers.py").read_text(
        encoding="utf-8")
    assert 'OWNER_AGENT_STOP_PROOF = "forkmesh-owner-agent-stop-v1"' in routes

    # The bulk move deletes the relay copies, so it asks first and says so.
    bulk = between(
        CLOUD,
        "void MainWindow::moveQueuedCloudAgentsHere(",
        "void MainWindow::moveAllCloudAgentsToDesktop(",
    )
    assert bulk.index("QMessageBox::question(") < bulk.index(
        "moveAllCloudAgentsToDesktop();")
    assert "on the relay is " in bulk and "deleted" in bulk

    # The relay half: a claim is a write, it is checked against the account
    # that queued the job, and a granted claim moves job and session into the
    # namespace the claimant reports results in.
    handler = between(
        RELAY,
        "async def repo_owner_agent_session_claim_handler(",
        "repo_org_agent_job_result_handler = ",
    )
    assert 'if method_name(request) != "POST":' in handler
    authorized = handler.index("_authorized_agent_claimant_key(env, request, owner, repo)")
    owned = handler.index('record.get("organization")')
    leased = handler.index("UPDATE org_agent_jobs SET status='leased'")
    assert authorized < owned < leased
    assert "UPDATE org_agent_sessions SET target_node=?" in handler
    # Only work nothing has started: the conditional update is the gate.
    assert "WHERE id=? AND status='queued'" in handler


if __name__ == "__main__":
    main()
    print("cloud agent claim contract: ok")
