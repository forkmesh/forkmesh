package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.control.ControlTransport
import com.forkmesh.nativeapp.control.DesktopListCover
import com.forkmesh.nativeapp.control.LocalAgentRow
import com.forkmesh.nativeapp.control.LocalAgentsSnapshot
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.localrepos.LocalRepoRecord
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.model.AgentSessionPage
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class AgentsListTest {
    private val app = Repository(owner = "alice", name = "app")
    private val lib = Repository(owner = "bob", name = "lib")

    fun testGroupsFlattenRunningFirst() {
        val options = AgentLaunchOptions(
            targets = listOf(AgentLaunchTarget("mirror35", runtimeMode = "headless")),
            sessions = listOf(
                AgentLaunchSession("old", status = "completed", updatedAtMs = 5, targetNode = "mirror35"),
                AgentLaunchSession("new", status = "running", updatedAtMs = 1, targetNode = "mirror35"),
            ),
        )
        val g1 = AgentsList.group(app, AgentSessionPage(listOf(AgentSession(1, status = "done"), AgentSession(2, status = "running"))), options, emptyList())
        val g2 = AgentsList.group(lib, AgentSessionPage(sealedForOthers = 2), AgentLaunchOptions(), emptyList())
        val g3 = AgentsList.group(Repository(owner = "c", name = "empty"), AgentSessionPage(), AgentLaunchOptions(), emptyList())
        val visible = AgentsList.visible(listOf(g2, g1, g3))
        assertEq(listOf("alice/app", "bob/lib"), visible.map { it.repo.fullName })
        assertEq(listOf(2, 1), g1.sessions.map { it.id })
        assertEq(listOf("new", "old"), g1.launched.map { it.sessionId })
        val entries = AgentsList.flatten(visible)
        assertEq(2, entries.takeWhile { it.isActive }.size)
        expect((entries.first { it is AgentEntry.Launched } as AgentEntry.Launched).cloud)
        assertEq(4, AgentsList.sessionCount(visible))
        assertEq(2, AgentsList.running(visible))
        assertEq(1, AgentsList.cloudRunning(visible))
        assertEq(2, AgentsList.sealed(visible))
        assertEq("2 sessions are encrypted for another device", AgentsList.sealedLabel(2))
        assertEq("Watch agents", AgentsList.watchChoice(visible).first)
        expect(AgentsList.needsPulls(AgentLaunchOptions(sessions = listOf(AgentLaunchSession("x", branchName = "b")))))
    }

    fun testFinishedRunsBeyondTheListedWindowStay() {
        val cut = 1_000L
        expect(AgentsList.beyondListedWindow(AgentLaunchSession("a", status = "completed", updatedAtMs = 900), cut))
        expect(AgentsList.beyondListedWindow(AgentLaunchSession("a", status = "failed", updatedAtMs = cut), cut))
        expect(!AgentsList.beyondListedWindow(AgentLaunchSession("a", status = "completed", updatedAtMs = 1_100), cut))
        expect(!AgentsList.beyondListedWindow(AgentLaunchSession("a", status = "completed", updatedAtMs = 0), cut))
        expect(!AgentsList.beyondListedWindow(AgentLaunchSession("a", status = "running", updatedAtMs = 900), cut))
        expect(!AgentsList.beyondListedWindow(AgentLaunchSession("a", status = "security_pending", updatedAtMs = 900), cut))
        expect(!AgentsList.beyondListedWindow(AgentLaunchSession("a", status = "completed", updatedAtMs = 900), 0))

        val previous = listOf(
            AgentLaunchSession("old", status = "completed", updatedAtMs = 500),
            AgentLaunchSession("deleted", status = "completed", updatedAtMs = 1_500),
            AgentLaunchSession("queued", status = "queued", updatedAtMs = 400),
            AgentLaunchSession("kept", status = "completed", updatedAtMs = 800),
        )
        val fresh = listOf(AgentLaunchSession("kept", status = "completed", updatedAtMs = 1_000, title = "new"))
        val merged = AgentsList.keptCloudRuns(previous, fresh, cut)
        assertEq(listOf("kept", "old"), merged.map { it.sessionId })
        assertEq("new", merged.first().title)
        assertEq(listOf("kept"), AgentsList.keptCloudRuns(previous, fresh, 0).map { it.sessionId })
    }

    fun testRegroupKeepsRowsWhenARequestFails() {
        val node = AgentLaunchTarget("mirror35", runtimeMode = "headless")
        val first = AgentsList.group(
            app,
            AgentSessionPage(listOf(AgentSession(1, status = "done")), sealedForOthers = 1),
            AgentLaunchOptions(
                targets = listOf(node),
                sessions = listOf(
                    AgentLaunchSession("a", status = "completed", updatedAtMs = 100, targetNode = "mirror35"),
                    AgentLaunchSession("b", status = "completed", updatedAtMs = 300, targetNode = "mirror35"),
                ),
            ),
            emptyList(),
        )
        val offline = AgentsList.regroup(app, AgentsList.groupFor(listOf(first), app), null, null, null)
        assertEq(listOf(1), offline.sessions.map { it.id })
        assertEq(1, offline.sealedForOthers)
        assertEq(listOf("b", "a"), offline.launched.map { it.sessionId })
        assertEq(2, offline.cloud.size)
        val windowed = AgentsList.regroup(
            app,
            offline,
            AgentSessionPage(),
            AgentLaunchOptions(
                targets = listOf(node),
                sessions = listOf(AgentLaunchSession("c", status = "running", updatedAtMs = 400, targetNode = "mirror35")),
                finishedBefore = 200,
            ),
            emptyList(),
        )
        assertEq(listOf("c", "a"), windowed.launched.map { it.sessionId })
        expect(windowed.sessions.isEmpty())
        assertEq(0, windowed.sealedForOthers)
        val whole = AgentsList.regroup(app, windowed, AgentSessionPage(), AgentLaunchOptions(targets = listOf(node)), emptyList())
        expect(whole.launched.isEmpty())
        val other = AgentsList.regroup(lib, first, null, null, null)
        expect(other.launched.isEmpty() && other.sessions.isEmpty())
        expect(AgentsList.groupFor(listOf(first), lib) == null)
    }

    fun testAPushedSessionTheWholeDesktopListLacksIsDropped() {
        val live = UnifiedAgentRow.fromDesktop(
            LocalAgentRow("alice", "App", AgentSession(7, status = "stopped", createdAtMs = 500)),
            ControlTransport.Relay,
            "studio",
        )
        val group = AgentRepoGroup(
            app,
            listOf(
                AgentSession(7, status = "done", createdAtMs = 500),
                AgentSession(8, status = "done", createdAtMs = 900),
                AgentSession(2, status = "done", createdAtMs = 100),
            ),
        )
        val merged = AgentRepoGroup(lib, listOf(AgentSession(3, status = "done", createdAtMs = 900)))
        val whole = AgentsList.covered(listOf(group, merged), listOf(live), DesktopListCover())
        assertEq(listOf("alice/app"), whole.map { it.repo.fullName })
        assertEq(listOf(7), whole.single().sessions.map { it.id })
        val capped = AgentsList.covered(listOf(group), listOf(live), DesktopListCover(oldestCreatedAtMs = 400))
        assertEq(listOf(7, 2), capped.single().sessions.map { it.id })
        val unreached = AgentsList.covered(listOf(group, merged), listOf(live), null)
        assertEq(4, AgentsList.sessionCount(unreached))
    }

    fun testDesktopListCoverIsWholeBelowTheCap() {
        val few = LocalAgentsSnapshot(sessions = listOf(LocalAgentRow("a", "b", AgentSession(1, createdAtMs = 50))))
        assertEq(DesktopListCover(), DesktopListCover.of(few))
        val many = LocalAgentsSnapshot(
            sessions = (1..DesktopListCover.LIST_CAP).map { LocalAgentRow("a", "b", AgentSession(it, createdAtMs = it * 10L)) },
        )
        assertEq(10L, DesktopListCover.of(many).oldestCreatedAtMs)
        expect(DesktopListCover.of(many).covers(10))
        expect(!DesktopListCover.of(many).covers(9))
    }

    fun testTonesAndDetails() {
        assertEq(AgentTone.Success, AgentsList.statusTone(AgentSession(1, merged = true)))
        assertEq(AgentTone.Danger, AgentsList.badgeTone("Failed: boom"))
        assertEq(AgentTone.Accent, AgentsList.badgeTone("queued"))
        val detail = AgentsList.desktopDetail(app, AgentSession(1, issueNumber = 4, provider = "codex", numTurns = 3), 0)
        assertEq("alice/app · Issue #4 · Codex · 3 turns", detail)
        val launched = AgentsList.launchedDetail(
            app,
            AgentLaunchSession("s", targetNode = "m", createPr = true, history = listOf(AgentLaunchMessage("agent", text = "Working\nmore"))),
            emptyList(),
            cloud = true,
        )
        assertEq("alice/app · cloud · m · opens PR · Working", launched)
    }

    fun testStartTargetsFoldMirrorsFirst() {
        val mirrored = LocalRepoRecord("app", "/x/app", 0, publishedAs = "alice/app")
        val orphan = LocalRepoRecord("scratch", "/x/scratch", 0)
        val published = LocalRepoRecord("side", "/x/side", 0, publishedAs = "carol/side")
        val targets = AgentStartTarget.fold(listOf(app, lib), listOf(mirrored, orphan, published))
        assertEq(listOf("alice/app", "carol/side", "scratch", "bob/lib"), targets.map { it.label })
        assertEq("Mirrored on this device", targets[0].detail)
        assertEq("On this device · not published", targets[2].detail)
        assertEq("On the mesh · 1 mirror · not on this device yet", targets[3].detail)
        assertEq("carol", targets[1].owner)
    }

    fun testAMirroredMeshRepositoryIsNamedAndClonedByItsPublicOwner() {
        val flagship = AgentStartTarget(
            repo = Repository(owner = "mirror35", name = "forkmesh", mirrors = 4, publicOwner = "forkmesh"),
        )
        assertEq("forkmesh/forkmesh", flagship.label)
        assertEq("mirror35", flagship.owner)
        assertEq("forkmesh", flagship.publicOwner)
        assertEq("On the mesh · 4 mirrors · not on this device yet", flagship.detail)
        assertEq(
            "On the mesh · not on this device yet",
            AgentStartTarget(repo = Repository(owner = "bob", name = "x", mirrors = 0)).detail,
        )
    }

    fun testStartPlanTexts() {
        val node = AgentLaunchTarget("mirror35", online = true, runtimeMode = "headless")
        val plan = AgentStartPlan(AgentStartTarget(repo = app), AgentLaunchOptions(canQueue = true, targets = listOf(node)))
        expect(plan.canRunOnNode)
        assertEq(AgentRunMode.Node, plan.defaultMode)
        assertEq("Sent to mirror mirror35, which is online now.", plan.runsOnText(AgentRunMode.Node, node, true))
        assertEq(
            "Clones alice/app from its mirrors to this device first, then runs here on your own API key.",
            plan.runsOnText(AgentRunMode.Device, node, true),
        )
        val none = AgentStartPlan(AgentStartTarget(repo = app))
        expect(none.runsOnText(AgentRunMode.Device, null, false).startsWith("No node on this account can run agents"))
        assertEq("Photo prompt", AgentStartPlan.titleFor("  "))
        assertEq("codex", AgentStartPlan.providerFor(AgentModelCatalog.NODE_MODELS.last()))
    }

    fun testAttachmentHelpers() {
        val metas = AgentAttachmentMeta.list(
            listOf(
                mapOf("id" to "0123456789ABCDEF0123456789abcdef", "name" to "C:\\tmp\\a.JPG", "size" to 5),
                mapOf("id" to "0123456789abcdef0123456789abcdef", "name" to "dup"),
                mapOf("id" to "nothex", "name" to "x"),
            ),
        )
        assertEq(1, metas.size)
        assertEq("a.JPG", metas[0].name)
        assertEq("image/jpeg", metas[0].mediaType)
        val transcript = "Attached image: /home/u/shot.png\nAttached images (open them with your file-reading tool):\n- .forkmesh/attachments/a.jpg\n- b.gif\nend"
        assertEq(listOf("/home/u/shot.png", ".forkmesh/attachments/a.jpg", "b.gif"), AgentTranscriptAttachments.paths(transcript))
        val refs = AgentTranscriptAttachments.references(transcript)
        expect(!refs[0].canFetch)
        assertEq("on the machine that ran the agent", refs[0].rowDetail)
        assertEq("image/gif", AgentAttachmentMedia.sniff("GIF89a....".toByteArray()))
        assertEq(null, AgentAttachmentMedia.sniff("hello".toByteArray()))
        val photo = PromptAttachment("p", "p.jpg", ByteArray(5))
        assertEq(listOf(metas[0] to photo), AgentAttachmentReconcile.pair(listOf(photo), metas))
        assertEq(null, AgentAttachmentReconcile.pair(listOf(PromptAttachment("q", "q", ByteArray(6))), metas))
        val sent = AgentSentPhotos(sessionLimit = 1)
        sent.record("s1", "t", listOf(photo))
        sent.record("s2", "t", listOf(photo))
        assertEq(null, sent.photos("s1", "t"))
        sent.rename("s2", "s3")
        expect(sent.photos("s3", "t") != null)
        sent.clear()
        assertEq(null, sent.photos("s3", "t"))
        assertEq(".forkmesh/attachments/image.png", attachmentRelativePath(PromptAttachment("../", "x", ByteArray(0), "image/png")))
        assertEq("do\n\nAttached images (open them with your file-reading tool):\n- a", promptWithAttachmentPaths("do", listOf("a")))
    }
}
