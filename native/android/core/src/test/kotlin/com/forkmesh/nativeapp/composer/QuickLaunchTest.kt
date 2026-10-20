package com.forkmesh.nativeapp.composer

import com.forkmesh.nativeapp.agents.AgentLaunchException
import com.forkmesh.nativeapp.agents.AgentLaunchOptions
import com.forkmesh.nativeapp.agents.AgentLaunchSession
import com.forkmesh.nativeapp.agents.AgentLaunchTarget
import com.forkmesh.nativeapp.agents.AgentModelCatalog
import com.forkmesh.nativeapp.agents.AgentModelChoice
import com.forkmesh.nativeapp.agents.AgentQueueResult
import com.forkmesh.nativeapp.agents.AgentStartTarget
import com.forkmesh.nativeapp.agents.NodeModelPreference
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.localrepos.LocalRepoRecord
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

private data class Launched(
    val owner: String,
    val name: String,
    val prompt: String,
    val provider: String,
    val model: String,
    val node: String,
    val orgScope: Boolean,
    val photos: Int,
)

private class FakeBackend(
    var targets: List<AgentLaunchTarget>,
    var canQueue: Boolean = true,
    var signedIn: Boolean = true,
    var credentials: Boolean = false,
    val records: List<LocalRepoRecord> = emptyList(),
    var repos: List<Repository> = listOf(Repository("alice", "mobile"), Repository("bob", "worker")),
    var account: String = "",
) : QuickLaunchBackend {
    val launches = ArrayList<Launched>()
    var launchError: Exception? = null
    var queueResult = AgentQueueResult(
        queueState = "ready_for_claim",
        targetOnline = true,
        message = "Queued.",
        session = AgentLaunchSession(sessionId = "sess-1", owner = "alice", repo = "mobile", status = "queued"),
    )
    var optionsCalls = 0

    override fun signedIn(): Boolean = signedIn

    override fun loadTargets(): List<AgentStartTarget> =
        AgentStartTarget.fold(repos, records)

    override fun launchOptions(owner: String, name: String): AgentLaunchOptions {
        optionsCalls++
        return AgentLaunchOptions(canQueue = canQueue, targets = targets)
    }

    override fun launch(
        owner: String,
        name: String,
        prompt: String,
        provider: String,
        model: String,
        targetNode: String,
        title: String,
        orgScope: Boolean,
        attachments: List<PromptAttachment>,
    ): AgentQueueResult {
        launchError?.let { throw it }
        launches.add(Launched(owner, name, prompt, provider, model, targetNode, orgScope, attachments.size))
        return queueResult
    }

    override fun hasDeviceCredentials(): Boolean = credentials

    override fun account(): String = account

    var desktop = ""
    var desktopError: Exception? = null
    val desktopStarts = ArrayList<List<String>>()
    val desktopOptions = ArrayList<QuickLaunchRunOptions>()
    val desktopImages = ArrayList<List<PromptAttachment>>()

    override fun desktopName(): String = desktop

    override fun startOnDesktop(
        owner: String,
        name: String,
        task: String,
        provider: String,
        model: String,
        options: QuickLaunchRunOptions,
        images: List<PromptAttachment>,
    ): Int {
        desktopStarts.add(listOf(owner, name, task, provider, model))
        desktopOptions.add(options)
        desktopImages.add(images)
        desktopError?.let { throw it }
        return 7
    }
}

class QuickLaunchTest {
    private fun node(name: String, providers: List<String>, online: Boolean, mirror: Boolean = true) =
        AgentLaunchTarget(name, providers = providers, online = online, runtimeMode = if (mirror) "headless" else "")

    fun testSendQueuesOnTheFirstOnlineNodeForTheProviderAndOpensTheSession() {
        val backend = FakeBackend(
            listOf(
                node("desk-off", listOf("claude-code"), online = false, mirror = false),
                node("mirror-codex", listOf("codex"), online = true),
                node("mirror35", listOf("claude-code"), online = true),
            ),
        )
        val quick = QuickAgentLauncher(backend, MemoryPrefStore())
        quick.prepare()
        assertEq("alice/mobile · mirror35", quick.contextLine)
        val outcome = quick.send("Fix the crash", emptyList())
        expect(outcome is QuickLaunchOutcome.Queued, "$outcome")
        outcome as QuickLaunchOutcome.Queued
        assertEq("alice/mobile", outcome.repo.fullName)
        assertEq("sess-1", outcome.session.sessionId)
        assertEq("mirror35", backend.launches.last().node)
        assertEq("auto", backend.launches.last().provider)
        assertEq("opus", backend.launches.last().model)
    }

    fun testWithNothingRememberedTheAccountsMostRecentRepositoryIsUsed() {
        val backend = FakeBackend(
            listOf(node("mirror35", listOf("claude-code"), online = true)),
            repos = listOf(
                Repository("alice", "mobile", updatedMs = 300),
                Repository("bob", "old", updatedMs = 100),
                Repository("bob", "worker", updatedMs = 200),
            ),
            account = "Bob",
        )
        val mine = QuickAgentLauncher(backend, MemoryPrefStore())
        mine.prepare()
        assertEq("bob/worker", mine.repository?.label)

        backend.account = ""
        val anyone = QuickAgentLauncher(backend, MemoryPrefStore())
        anyone.prepare()
        assertEq("alice/mobile", anyone.repository?.label)
    }

    fun testRemembersRepositoryAndTargetPerRepository() {
        val prefs = MemoryPrefStore()
        val targets = listOf(node("mirror35", listOf("claude-code"), true), node("mirror36", listOf("claude-code"), true))
        val first = QuickAgentLauncher(FakeBackend(targets), prefs)
        first.prepare()
        val worker = first.targets!!.first { it.name == "worker" }
        first.choose(worker)
        first.choose(QuickLaunchDestination.Node("mirror36"))

        val again = QuickAgentLauncher(FakeBackend(targets), prefs)
        again.prepare()
        assertEq("bob/worker · mirror36", again.contextLine)
        again.prepare(currentRepo = "alice/mobile")
        assertEq("alice/mobile · mirror35", again.contextLine)
    }

    fun testNoUsableNodeOffersTheProviderSignInAndSendsNothing() {
        val backend = FakeBackend(listOf(node("mirror35", listOf("codex"), true)))
        val quick = QuickAgentLauncher(backend, MemoryPrefStore())
        quick.prepare()
        val outcome = quick.send("Fix it", emptyList())
        assertEq(
            QuickLaunchOutcome.NeedsFix(QuickLaunchFix.SignInProvider("mirror35", "claude-code"), "mirror35 has no Claude Code signed in."),
            outcome,
        )
        assertEq("Sign in to Claude Code on mirror35", QuickLaunchFix.SignInProvider("mirror35", "claude-code").action())
        expect(backend.launches.isEmpty())
    }

    fun testAnExpiredRunnerOffersTheRunnerSignInNotTheNodeTerminal() {
        val runner = AgentLaunchTarget(
            node = "fmnext-n1",
            runtimeMode = "runner-pool",
            runner = "fmnext-r1",
            needsLogin = listOf("claude-code"),
        )
        val backend = FakeBackend(listOf(runner))
        val quick = QuickAgentLauncher(backend, MemoryPrefStore())
        quick.prepare()
        val outcome = quick.send("Fix it", emptyList())
        assertEq(
            QuickLaunchOutcome.NeedsFix(QuickLaunchFix.SignInRunner("fmnext-r1", "claude-code"), "fmnext-r1 has no Claude Code signed in."),
            outcome,
        )
        assertEq("Sign in to Claude Code on fmnext-r1", QuickLaunchFix.SignInRunner("fmnext-r1", "claude-code").action())
        expect(backend.launches.isEmpty())
    }

    fun testARunnerSignedInAfterTheOptionsLoadedTakesTheSendWithoutAFix() {
        val expired = AgentLaunchTarget(
            node = "fmnext-n1",
            online = true,
            runtimeMode = "runner-pool",
            runner = "fmnext-r1",
            needsLogin = listOf("claude-code"),
            queueNode = "fmnext-n1",
        )
        val backend = FakeBackend(listOf(expired))
        val quick = QuickAgentLauncher(backend, MemoryPrefStore())
        quick.prepare()
        assertEq(QuickLaunchDestination.Node("fmnext-n1"), quick.destination)
        backend.targets = listOf(
            AgentLaunchTarget(
                node = "fmnext-r1",
                providers = listOf("claude-code"),
                online = true,
                runtimeMode = "runner-pool",
                runner = "fmnext-r1",
                queueNode = "fmnext-n1",
            ),
        )
        val outcome = quick.send("Fix it", emptyList())
        expect(outcome is QuickLaunchOutcome.Queued)
        assertEq("fmnext-n1", backend.launches.last().node)
    }

    fun testOpeningTheComposerAgainRereadsOptionsOnceTheyAreOld() {
        var now = 1_000L
        val backend = FakeBackend(listOf(node("mirror35", listOf("codex"), true)))
        val quick = QuickAgentLauncher(backend, MemoryPrefStore(), clock = { now })
        quick.prepare()
        val first = backend.optionsCalls
        backend.targets = listOf(node("mirror35", listOf("claude-code"), true))
        quick.prepare()
        assertEq(first, backend.optionsCalls)
        now += QuickAgentLauncher.OPTIONS_FRESH_MS
        quick.prepare()
        assertEq(first + 1, backend.optionsCalls)
        assertEq("Mirror · online", quick.detail(QuickLaunchDestination.Node("mirror35")))
    }

    fun testTheCoreNameFindsThePoolTargetNotAPersonalRunner() {
        val options = AgentLaunchOptions(
            canQueue = true,
            targets = listOf(
                AgentLaunchTarget("my-box", runtimeMode = "runner-pool", runner = "my-box", queueNode = "fmnext-n1", runnerPin = "my-box"),
                AgentLaunchTarget("fmnext-r1", runtimeMode = "runner-pool", runner = "fmnext-r1", queueNode = "fmnext-n1"),
            ),
        )
        assertEq("fmnext-r1", options.targetNamed("fmnext-n1")?.node)
        assertEq("my-box", options.targetNamed("my-box")?.node)
        assertEq(null, options.targetNamed(""))
    }

    fun testRunnerTargetsReadTheRunnerAndItsExpiredLogins() {
        val target = AgentLaunchTarget.fromJson(
            mapOf(
                "node" to "fmnext-n1",
                "runtimeMode" to "runner-pool",
                "runner" to "fmnext-r1",
                "providers" to emptyList<String>(),
                "needsLogin" to mapOf("claude-code" to listOf("fmnext-r1")),
            ),
        )
        expect(target.isRunner)
        assertEq("fmnext-r1", target.signInNode)
        assertEq(listOf("claude-code"), target.needsLogin)
        assertEq(QuickLaunchFix.SignInRunner("fmnext-r1", "claude-code"), QuickLaunchFix.signIn(target, "claude-code"))
        val mirror = AgentLaunchTarget(node = "mirror35", runtimeMode = "headless", runner = "ignored")
        assertEq("mirror35", mirror.signInNode)
        assertEq(QuickLaunchFix.SignInProvider("mirror35", "claude-code"), QuickLaunchFix.signIn(mirror, "claude-code"))
    }

    fun testSignedOutAsksForAnAccount() {
        val backend = FakeBackend(listOf(node("m", listOf("claude-code"), true)), signedIn = false)
        val outcome = QuickAgentLauncher(backend, MemoryPrefStore()).send("Go", emptyList())
        expect(outcome is QuickLaunchOutcome.NeedsFix && outcome.fix == QuickLaunchFix.SignInAccount, "$outcome")
    }

    fun testTheChipModelIsSentAndRemembered() {
        val prefs = MemoryPrefStore()
        val backend = FakeBackend(listOf(node("mirror35", listOf("claude-code", "codex"), true)))
        val quick = QuickAgentLauncher(backend, prefs)
        quick.prepare()
        assertEq("Opus 5.5", quick.model.label)
        quick.chooseModel(AgentModelCatalog.NODE_MODELS.first { it.alias == "terra" })
        quick.send("Go", emptyList())
        assertEq("codex", backend.launches.last().provider)
        assertEq("terra", backend.launches.last().model)
        assertEq("terra", NodeModelPreference(prefs).read().alias)
    }

    fun testEmptySendIsIgnored() {
        val backend = FakeBackend(listOf(node("mirror35", listOf("claude-code"), true)))
        assertEq(QuickLaunchOutcome.Ignored, QuickAgentLauncher(backend, MemoryPrefStore()).send("   ", emptyList()))
        assertEq(0, backend.optionsCalls)
    }

    fun testPhotosOnlyPromptIsSentWithItsPhotos() {
        val backend = FakeBackend(listOf(node("mirror35", listOf("claude-code"), true)))
        val quick = QuickAgentLauncher(backend, MemoryPrefStore())
        val photo = PromptAttachment("a", "a.jpg", byteArrayOf(1, 2, 3))
        val outcome = quick.send("", listOf(photo))
        expect(outcome is QuickLaunchOutcome.Queued, "$outcome")
        assertEq(1, backend.launches.last().photos)
    }

    fun testMirroredRepositoryRunsOnTheDeviceWhenNoNodeIsOnline() {
        val record = LocalRepoRecord("mobile", "/x/mobile", 0, publishedAs = "alice/mobile")
        val backend = FakeBackend(emptyList(), canQueue = false, credentials = true, records = listOf(record))
        val quick = QuickAgentLauncher(backend, MemoryPrefStore())
        quick.prepare()
        assertEq("alice/mobile · this phone", quick.contextLine)
        val outcome = quick.send("Tidy", emptyList())
        expect(outcome is QuickLaunchOutcome.Device && outcome.record.name == "mobile", "$outcome")
    }

    fun testDeviceWithoutKeyAsksForSetup() {
        val record = LocalRepoRecord("mobile", "/x/mobile", 0, publishedAs = "alice/mobile")
        val backend = FakeBackend(emptyList(), canQueue = false, credentials = false, records = listOf(record))
        val quick = QuickAgentLauncher(backend, MemoryPrefStore())
        quick.prepare()
        val outcome = quick.send("Tidy", emptyList())
        expect(outcome is QuickLaunchOutcome.NeedsFix && outcome.fix == QuickLaunchFix.SetUpDevice, "$outcome")
    }

    fun testChoosingDeviceForAMeshRepositoryAsksToMirrorIt() {
        val backend = FakeBackend(listOf(node("mirror35", listOf("claude-code"), true)), credentials = true)
        val quick = QuickAgentLauncher(backend, MemoryPrefStore())
        quick.prepare()
        expect(QuickLaunchDestination.Device in quick.destinations)
        quick.choose(QuickLaunchDestination.Device)
        val outcome = quick.send("Go", emptyList())
        assertEq(
            QuickLaunchOutcome.NeedsFix(QuickLaunchFix.MirrorToDevice("alice", "mobile"), "alice/mobile is not on this phone yet."),
            outcome,
        )
    }

    fun testOfflineNodeIsStillChosenSoTheRunQueues() {
        val backend = FakeBackend(listOf(node("mirror35", listOf("claude-code"), online = false)))
        val quick = QuickAgentLauncher(backend, MemoryPrefStore())
        quick.prepare()
        assertEq("alice/mobile · mirror35", quick.contextLine)
    }

    fun testRelayRefusalIsReportedAndNothingElseHappens() {
        val backend = FakeBackend(listOf(node("mirror35", listOf("claude-code"), true)))
        backend.launchError = AgentLaunchException("That prompt is longer than the relay accepts.")
        val quick = QuickAgentLauncher(backend, MemoryPrefStore())
        assertEq(QuickLaunchOutcome.Failed("That prompt is longer than the relay accepts."), quick.send("Go", emptyList()))
    }

    fun testQueuedWithoutSessionFromAnOlderRelay() {
        val backend = FakeBackend(listOf(node("mirror35", listOf("claude-code"), true)))
        backend.queueResult = AgentQueueResult(queueState = "ready_for_claim", targetOnline = true, message = "Queued on mirror35.")
        val outcome = QuickAgentLauncher(backend, MemoryPrefStore()).send("Go", emptyList())
        assertEq(QuickLaunchOutcome.QueuedWithoutSession("Queued on mirror35."), outcome)
    }

    fun testLinkedDesktopIsOfferedAndTakesThePromptDirectly() {
        val backend = FakeBackend(listOf(node("fmnext-r1", listOf("claude-code"), true)))
        val prefs = MemoryPrefStore()
        val quick = QuickAgentLauncher(backend, prefs)
        quick.prepare()
        expect(QuickLaunchDestination.Desktop("studio") !in quick.destinations)
        backend.desktop = "studio"
        expect(QuickLaunchDestination.Desktop("studio") in quick.destinations)
        assertEq("Your desktop, directly", quick.detail(QuickLaunchDestination.Desktop("studio")))
        quick.choose(QuickLaunchDestination.Desktop("studio"))
        assertEq("alice/mobile · studio", quick.contextLine)
        val outcome = quick.send(" Fix the build ", emptyList())
        assertEq(QuickLaunchOutcome.Desktop("studio", 7, "Started agent #7 on studio."), outcome)
        assertEq(listOf("alice", "mobile", "Fix the build", "claude-code", "claude-opus-5-5"), backend.desktopStarts.single())
        assertEq(0, backend.launches.size)
        val again = QuickAgentLauncher(backend, prefs)
        again.prepare()
        assertEq(QuickLaunchDestination.Desktop("studio"), again.destination)
    }

    fun testYoloAndNoWorktreeGoToTheDesktopOnly() {
        val backend = FakeBackend(listOf(node("fmnext-r1", listOf("claude-code"), true)))
        backend.desktop = "studio"
        val prefs = MemoryPrefStore()
        val quick = QuickAgentLauncher(backend, prefs)
        quick.prepare()
        quick.choose(QuickLaunchDestination.Node("fmnext-r1"))
        expect(!quick.takesRunOptions)
        quick.choose(QuickLaunchDestination.Desktop("studio"))
        expect(quick.takesRunOptions)
        expect(quick.takesNoWorktree)

        quick.send("Plain", emptyList())
        assertEq(QuickLaunchRunOptions(yolo = false, worktree = true), backend.desktopOptions.last())

        quick.toggleYolo()
        quick.send("Ship it", emptyList())
        assertEq(QuickLaunchRunOptions(yolo = true, worktree = true), backend.desktopOptions.last())
        expect(QuickLaunchRunOptions.read(prefs).yolo)

        quick.toggleWorktree()
        expect(!quick.sendsYolo)
        quick.send("In the folder", emptyList())
        assertEq(QuickLaunchRunOptions(yolo = false, worktree = false), backend.desktopOptions.last())
        assertEq(QuickLaunchRunOptions(yolo = true, worktree = true), quick.runOptions)

        assertEq(QuickLaunchRunOptions(yolo = true, worktree = true), QuickAgentLauncher(backend, prefs).runOptions)
    }

    fun testNoWorktreeIsNotOfferedForAProviderThatAlwaysCutsOne() {
        val backend = FakeBackend(listOf(node("fmnext-r1", listOf("claude-code", "local-llama"), true)))
        backend.desktop = "studio"
        val quick = QuickAgentLauncher(backend, MemoryPrefStore())
        quick.prepare()
        quick.choose(QuickLaunchDestination.Desktop("studio"))
        quick.chooseModel(AgentModelChoice("local-llama", "gemma4:12b", "gemma4:12b", "Gemma 4"))
        expect(quick.takesRunOptions)
        expect(!quick.takesNoWorktree)
        quick.toggleYolo()
        quick.toggleWorktree()
        quick.send("Go", emptyList())
        assertEq(QuickLaunchRunOptions(yolo = true, worktree = true), backend.desktopOptions.last())
    }

    fun testDesktopRefusalIsExplained() {
        val backend = FakeBackend(listOf(node("fmnext-r1", listOf("claude-code"), true)))
        backend.desktop = "studio"
        backend.desktopError = LocalControlException("no_checkout")
        val quick = QuickAgentLauncher(backend, MemoryPrefStore())
        quick.prepare()
        quick.choose(QuickLaunchDestination.Desktop("studio"))
        assertEq(QuickLaunchOutcome.Failed("The desktop has no local checkout of that repository."), quick.send("Go", emptyList()))
        val photo = PromptAttachment("a", "a.jpg", byteArrayOf(1))
        expect(quick.send("Go", listOf(photo)) is QuickLaunchOutcome.Failed)
    }

    fun testPhotosGoStraightToTheDesktop() {
        val backend = FakeBackend(listOf(node("fmnext-r1", listOf("claude-code"), true)))
        backend.desktop = "studio"
        val quick = QuickAgentLauncher(backend, MemoryPrefStore())
        quick.prepare()
        quick.choose(QuickLaunchDestination.Desktop("studio"))
        val photo = PromptAttachment("a", "a.png", byteArrayOf(0x89.toByte(), 0x50, 0x4E, 0x47), "image/png")
        assertEq(QuickLaunchOutcome.Desktop("studio", 7, "Started agent #7 on studio."), quick.send("", listOf(photo)))
        assertEq("See the attached photos.", backend.desktopStarts.single()[2])
        assertEq(listOf(listOf(photo)), backend.desktopImages)
        assertEq(0, backend.launches.size)
    }

    fun testDestinationRoundTrip() {
        assertEq(QuickLaunchDestination.Node("mirror35"), QuickLaunchDestination.parse("node:mirror35"))
        assertEq(QuickLaunchDestination.Device, QuickLaunchDestination.parse("device"))
        assertEq(QuickLaunchDestination.Desktop("studio"), QuickLaunchDestination.parse(QuickLaunchDestination.Desktop("studio").stored))
        assertEq(null, QuickLaunchDestination.parse("node:"))
        assertEq(null, QuickLaunchDestination.parse("elsewhere"))
        assertEq("agent/quick/target/alice/mobile", QuickLaunchMemory.destinationKey("Alice/Mobile"))
    }

    fun testSwitchingToAModelTheNodeCannotRunMovesTheDefault() {
        val backend = FakeBackend(
            listOf(node("claude-only", listOf("claude-code"), true), node("codex-box", listOf("codex"), true)),
        )
        val quick = QuickAgentLauncher(backend, MemoryPrefStore())
        quick.prepare()
        assertEq("claude-only", quick.destinationLabel)
        quick.chooseModel(AgentModelCatalog.NODE_MODELS.first { it.provider == "codex" })
        assertEq("codex-box", quick.destinationLabel)
        assertEq("Mirror · online · no Codex", quick.detail(QuickLaunchDestination.Node("claude-only")))
        assertEq("Mirror · online", quick.detail(QuickLaunchDestination.Node("codex-box")))
    }
}

private class FakePromptActions : QuickPromptActions {
    var error: Exception? = null
    var people = QuickTaskPeople("alice", emptyList())
    val bugs = ArrayList<Pair<String, List<PromptAttachment>>>()
    val tasks = ArrayList<List<String>>()
    val issues = ArrayList<List<String>>()
    val posts = ArrayList<List<String>>()
    val notes = ArrayList<List<String>>()

    override fun reportBug(text: String, photos: List<PromptAttachment>) {
        error?.let { throw it }
        bugs += text to photos
    }

    override fun taskPeople(): QuickTaskPeople = people

    override fun createTask(title: String, details: String, assignee: String, repository: String) {
        error?.let { throw it }
        tasks += listOf(title, details, assignee, repository)
    }

    override fun createIssue(owner: String, name: String, title: String, body: String) {
        error?.let { throw it }
        issues += listOf(owner, name, title, body)
    }

    override fun postToActivityPub(owner: String, name: String, title: String, body: String) {
        error?.let { throw it }
        posts += listOf(owner, name, title, body)
    }

    override fun takeNote(title: String, markdown: String) {
        error?.let { throw it }
        notes += listOf(title, markdown)
    }
}

class QuickPromptActionsTest {
    private val online = listOf(AgentLaunchTarget("mirror35", providers = listOf("claude-code"), online = true, runtimeMode = "headless"))

    private fun launcher(
        backend: FakeBackend,
        prefs: MemoryPrefStore = MemoryPrefStore(),
        actions: FakePromptActions? = FakePromptActions(),
    ): QuickAgentLauncher = QuickAgentLauncher(backend, prefs).also { it.actions = actions }

    private val photo = PromptAttachment("a", "shot.png", byteArrayOf(1), "image/png")

    fun testTheActionsAreOfferedBeforeTheModelsOnlyWhenTheAppProvidesThem() {
        val quick = launcher(FakeBackend(online))
        assertEq(
            listOf("Bug report", "Task · assign", "Manual · create issue", "ActivityPub · post", "Take a note"),
            quick.modelChoices.take(5).map { it.label },
        )
        assertEq(AgentModelCatalog.DEFAULT_NODE_MODEL, quick.modelChoices.drop(5).first())
        val bare = launcher(FakeBackend(online), actions = null)
        expect(bare.modelChoices.none { QuickPromptAction.of(it) != null })
    }

    fun testWithNoAgentAnywhereTheChipDefaultsToABugReport() {
        val quick = launcher(FakeBackend(emptyList(), canQueue = false))
        quick.prepare()
        expect(!quick.hasAgent)
        assertEq(QuickPromptAction.BugReport, quick.action)
        assertEq("Bug report", quick.chip.label)
        assertEq("Bug report · ForkMesh team", quick.contextLine)
        expect(!quick.takesRunOptions)
    }

    fun testSignedOutPeopleCanStillSendABugReport() {
        val actions = FakePromptActions()
        val quick = launcher(FakeBackend(online, signedIn = false), actions = actions)
        quick.prepare()
        assertEq(QuickPromptAction.BugReport, quick.action)
        val outcome = quick.send("The calendar said Invalid json.", listOf(photo))
        assertEq(QuickLaunchOutcome.Done("Sent to the ForkMesh team as a bug report."), outcome)
        assertEq(listOf("The calendar said Invalid json."), actions.bugs.map { it.first })
        assertEq(listOf(photo), actions.bugs.first().second)
    }

    fun testWithAnAgentTheModelStaysTheDefault() {
        val quick = launcher(FakeBackend(online))
        quick.prepare()
        expect(quick.hasAgent)
        assertEq(null, quick.action)
        assertEq(AgentModelCatalog.DEFAULT_NODE_MODEL, quick.chip)
    }

    fun testPickingAModelOverridesTheBugReportDefaultAndIsRemembered() {
        val prefs = MemoryPrefStore()
        val quick = launcher(FakeBackend(emptyList(), canQueue = false), prefs)
        quick.prepare()
        quick.chooseModel(AgentModelCatalog.DEFAULT_NODE_MODEL)
        assertEq(null, quick.action)
        val again = launcher(FakeBackend(emptyList(), canQueue = false), prefs)
        again.prepare()
        assertEq(null, again.action)
    }

    fun testAChosenActionIsRememberedAndBlocksNoAgentRun() {
        val prefs = MemoryPrefStore()
        val quick = launcher(FakeBackend(online), prefs)
        quick.prepare()
        quick.chooseModel(QuickPromptAction.Note.choice)
        assertEq("Take a note", quick.chip.label)
        val again = launcher(FakeBackend(online), prefs)
        assertEq(QuickPromptAction.Note, again.action)
        assertEq(AgentModelCatalog.DEFAULT_NODE_MODEL, again.model)
    }

    fun testATaskGoesToThePickedMemberWithTheFirstLineAsItsTitle() {
        val actions = FakePromptActions()
        actions.people = QuickTaskPeople("alice", listOf("bob", "alice", "carol"))
        val prefs = MemoryPrefStore()
        val backend = FakeBackend(online)
        val quick = launcher(backend, prefs, actions)
        quick.prepare()
        quick.chooseAction(QuickPromptAction.Task)
        assertEq(listOf("", "alice", "bob", "carol"), quick.taskAssignees)
        assertEq("@alice (you)", quick.taskAssigneeLabel("alice"))
        assertEq("Unassigned", quick.taskAssigneeLabel(""))
        quick.chooseAssignee("bob")
        assertEq("Task for @bob", quick.contextLine)
        val outcome = quick.send("Ship the calendar fix\nIt fails on save.", emptyList())
        assertEq(QuickLaunchOutcome.Done("Created a task for @bob."), outcome)
        assertEq(listOf("Ship the calendar fix", "Ship the calendar fix\nIt fails on save.", "bob", "alice/mobile"), actions.tasks.first())
        expect(backend.launches.isEmpty())
        assertEq("bob", launcher(FakeBackend(online), prefs).taskAssignee)
    }

    fun testAnUnassignedTaskSaysSo() {
        val actions = FakePromptActions()
        val quick = launcher(FakeBackend(online), actions = actions)
        quick.chooseAction(QuickPromptAction.Task)
        assertEq(QuickLaunchOutcome.Done("Created an unassigned task on General."), quick.send("Plan the offsite", emptyList()))
        assertEq("", actions.tasks.first()[2])
    }

    fun testIssuesAndPostsGoToThePublishedRepository() {
        val actions = FakePromptActions()
        val quick = launcher(FakeBackend(online), actions = actions)
        quick.prepare()
        quick.chooseAction(QuickPromptAction.Issue)
        assertEq("alice/mobile", quick.contextLine)
        assertEq(QuickLaunchOutcome.Done("Filed an issue on alice/mobile."), quick.send("Calendar save fails\nSteps: open, save.", emptyList()))
        assertEq(listOf("alice", "mobile", "Calendar save fails", "Steps: open, save."), actions.issues.first())
        quick.chooseAction(QuickPromptAction.ActivityPub)
        assertEq(QuickLaunchOutcome.Done("Published to ActivityPub for alice/mobile."), quick.send("Calendars work again", emptyList()))
        assertEq(listOf("alice", "mobile", "Calendars work again", ""), actions.posts.first())
    }

    fun testANoteKeepsTheRestAsItsBody() {
        val actions = FakePromptActions()
        val quick = launcher(FakeBackend(online), actions = actions)
        quick.chooseAction(QuickPromptAction.Note)
        assertEq(QuickLaunchOutcome.Done("Saved as a note."), quick.send("Groceries\n- milk\n- eggs", emptyList()))
        assertEq(listOf("Groceries", "- milk\n- eggs"), actions.notes.first())
    }

    fun testOnlyBugReportsTakePhotosAndAccountActionsNeedAnAccount() {
        val quick = launcher(FakeBackend(online))
        quick.chooseAction(QuickPromptAction.Note)
        assertEq(
            QuickLaunchOutcome.Failed("Take a note cannot include photos yet. Remove them, or send a bug report."),
            quick.send("Hi", listOf(photo)),
        )
        val signedOut = launcher(FakeBackend(online, signedIn = false))
        signedOut.chooseAction(QuickPromptAction.Task)
        val outcome = signedOut.send("Hi", emptyList())
        expect(outcome is QuickLaunchOutcome.NeedsFix && outcome.fix == QuickLaunchFix.SignInAccount, "$outcome")
    }

    fun testARefusalIsShownAndTheTextIsKept() {
        val actions = FakePromptActions()
        actions.error = QuickActionError("Only the assignee can do that.")
        val quick = launcher(FakeBackend(online), actions = actions)
        quick.chooseAction(QuickPromptAction.Task)
        assertEq(QuickLaunchOutcome.Failed("Only the assignee can do that."), quick.send("Do it", emptyList()))
    }

    fun testAnAgentOnlySendStillStartsAnAgentWhateverTheChipSays() {
        val actions = FakePromptActions()
        val quick = launcher(FakeBackend(online), actions = actions)
        quick.chooseAction(QuickPromptAction.BugReport)
        val outcome = quick.sendAgent("Fix the crash", emptyList())
        expect(outcome is QuickLaunchOutcome.Queued, "$outcome")
        expect(actions.bugs.isEmpty())
    }

    fun testTitlesAreTheFirstLineCappedLikeTheDesktop() {
        assertEq("One two" to "three", QuickPromptAction.titleAndBody("  One   two \n three "))
        assertEq(160, QuickPromptAction.titleAndBody("a".repeat(200)).first.length)
        assertEq("body only", QuickPromptAction.titleAndBody("\n\nbody only").first)
    }
}
