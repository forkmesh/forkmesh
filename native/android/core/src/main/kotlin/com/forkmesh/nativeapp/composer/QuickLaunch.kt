package com.forkmesh.nativeapp.composer

import com.forkmesh.nativeapp.agents.AGENT_LOGIN_PROVIDERS
import com.forkmesh.nativeapp.agents.AgentLaunchException
import com.forkmesh.nativeapp.agents.AgentLaunchOptions
import com.forkmesh.nativeapp.agents.AgentLaunchSession
import com.forkmesh.nativeapp.agents.AgentLaunchTarget
import com.forkmesh.nativeapp.agents.AgentModelCatalog
import com.forkmesh.nativeapp.agents.AgentModelChoice
import com.forkmesh.nativeapp.agents.AgentQueueResult
import com.forkmesh.nativeapp.agents.AgentStartPlan
import com.forkmesh.nativeapp.agents.AgentStartTarget
import com.forkmesh.nativeapp.agents.NodeModelPreference
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.agents.PromptAttachmentLimits
import com.forkmesh.nativeapp.agents.agentProviderLabel
import com.forkmesh.nativeapp.agents.promptOrPhotoNote
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.control.ControlErrorText
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.localrepos.LocalRepoRecord
import com.forkmesh.nativeapp.model.Repository

sealed class QuickLaunchDestination {
    data class Node(val name: String) : QuickLaunchDestination()

    data object Device : QuickLaunchDestination()

    data class Desktop(val name: String) : QuickLaunchDestination()

    val stored: String
        get() = when (this) {
            is Node -> "node:$name"
            Device -> "device"
            is Desktop -> "desktop:$name"
        }

    companion object {
        fun parse(stored: String?): QuickLaunchDestination? = when {
            stored == null -> null
            stored == "device" -> Device
            stored.startsWith("desktop:") -> Desktop(stored.substring(8))
            stored.startsWith("node:") && stored.length > 5 -> Node(stored.substring(5))
            else -> null
        }
    }
}

class QuickLaunchMemory(private val prefs: PrefStore?) {
    val lastRepo: String get() = prefs?.getString(LAST_REPO_KEY) ?: ""

    fun destination(repoKey: String): QuickLaunchDestination? =
        if (repoKey.isEmpty()) null else QuickLaunchDestination.parse(prefs?.getString(destinationKey(repoKey)))

    fun rememberRepo(repoKey: String) {
        if (repoKey.isNotEmpty()) prefs?.putString(LAST_REPO_KEY, repoKey.lowercase())
    }

    fun rememberDestination(repoKey: String, destination: QuickLaunchDestination) {
        if (repoKey.isNotEmpty()) prefs?.putString(destinationKey(repoKey), destination.stored)
    }

    companion object {
        const val LAST_REPO_KEY = "agent/quick/last-repo"

        fun destinationKey(repoKey: String): String = "agent/quick/target/${repoKey.lowercase()}"
    }
}

sealed class QuickLaunchFix {
    data object SignInAccount : QuickLaunchFix()

    data object ChooseRepository : QuickLaunchFix()

    data class SignInProvider(val node: String, val provider: String) : QuickLaunchFix()

    data class SignInRunner(val runner: String, val provider: String) : QuickLaunchFix()

    data object SetUpDevice : QuickLaunchFix()

    data class MirrorToDevice(val owner: String, val name: String) : QuickLaunchFix()

    fun action(deviceName: String = "this phone"): String = when (this) {
        SignInAccount -> "Sign in"
        ChooseRepository -> "Choose a repository"
        is SignInProvider -> "Sign in to ${agentProviderLabel(provider)} on $node"
        is SignInRunner -> "Sign in to ${agentProviderLabel(provider)} on $runner"
        SetUpDevice -> "Set up the agent on $deviceName"
        is MirrorToDevice -> "Mirror to $deviceName"
    }

    companion object {
        fun signIn(target: AgentLaunchTarget, provider: String): QuickLaunchFix =
            if (target.isRunner) SignInRunner(target.signInNode, provider) else SignInProvider(target.node, provider)
    }
}

sealed class QuickLaunchOutcome {
    data object Ignored : QuickLaunchOutcome()

    data class Queued(
        val repo: Repository,
        val session: AgentLaunchSession,
        val message: String,
        val result: AgentQueueResult,
    ) : QuickLaunchOutcome()

    data class QueuedWithoutSession(val message: String) : QuickLaunchOutcome()

    data class Device(val record: LocalRepoRecord, val prompt: String, val images: List<PromptAttachment>) :
        QuickLaunchOutcome()

    data class Desktop(val desktop: String, val agentId: Int, val message: String) : QuickLaunchOutcome()

    data class NeedsFix(val fix: QuickLaunchFix, val message: String) : QuickLaunchOutcome()

    data class Failed(val message: String) : QuickLaunchOutcome()

    data class Done(val message: String) : QuickLaunchOutcome()
}

data class QuickLaunchRunOptions(val yolo: Boolean = false, val worktree: Boolean = true) {
    companion object {
        const val YOLO_KEY = "agent/desktop/yolo"

        fun read(prefs: PrefStore?): QuickLaunchRunOptions = QuickLaunchRunOptions(yolo = prefs?.getBool(YOLO_KEY) ?: false)

        fun cutsOwnWorktree(provider: String): Boolean = provider != "claude-code" && provider != "codex"
    }
}

interface QuickLaunchBackend {
    fun signedIn(): Boolean

    fun loadTargets(): List<AgentStartTarget>

    fun launchOptions(owner: String, name: String): AgentLaunchOptions

    fun launch(
        owner: String,
        name: String,
        prompt: String,
        provider: String,
        model: String,
        targetNode: String,
        title: String,
        orgScope: Boolean,
        attachments: List<PromptAttachment>,
    ): AgentQueueResult

    fun hasDeviceCredentials(): Boolean

    fun account(): String = ""

    fun desktopName(): String = ""

    fun desktopDetail(): String = ""

    fun startOnDesktop(
        owner: String,
        name: String,
        task: String,
        provider: String,
        model: String,
        options: QuickLaunchRunOptions = QuickLaunchRunOptions(),
        images: List<PromptAttachment> = emptyList(),
    ): Int = throw LocalControlException("not_connected")
}

class QuickAgentLauncher(
    private val backend: QuickLaunchBackend,
    private val prefs: PrefStore?,
    private val deviceName: String = "this phone",
    private val clock: () -> Long = System::currentTimeMillis,
) {
    private val memory = QuickLaunchMemory(prefs)
    private var prepared = false
    private var probe = 0
    private var optionsCheckedAt = 0L

    var targets: List<AgentStartTarget>? = null
        private set
    var plan: AgentStartPlan? = null
        private set
    var destination: QuickLaunchDestination? = null
        private set
    var model: AgentModelChoice = prefs?.let { NodeModelPreference(it).read() } ?: AgentModelCatalog.DEFAULT_NODE_MODEL
        private set
    var runOptions: QuickLaunchRunOptions = QuickLaunchRunOptions.read(prefs)
        private set

    var actions: QuickPromptActions? = null
    private val storedAction = prefs?.getString(QuickPromptAction.STORE_KEY)
    var actionChosen: Boolean = storedAction != null
        private set
    var chosenAction: QuickPromptAction? = QuickPromptAction.of(storedAction)
        private set
    var defaultsToBugReport: Boolean = false
        private set
    var taskPeople: QuickTaskPeople? = null
        private set
    var taskAssignee: String = prefs?.getString(QuickPromptAction.ASSIGNEE_KEY) ?: ""
        private set
    var loadingTaskPeople: Boolean = false
        private set

    val action: QuickPromptAction?
        get() {
            if (actions == null) return null
            if (actionChosen) return chosenAction
            return if (defaultsToBugReport) QuickPromptAction.BugReport else null
        }

    val chip: AgentModelChoice get() = action?.choice ?: model

    val hasAgent: Boolean
        get() {
            if (!backend.signedIn()) return false
            return when (destination) {
                null -> false
                QuickLaunchDestination.Device -> backend.hasDeviceCredentials()
                else -> true
            }
        }

    private fun settleDefault() {
        if (plan?.loading == true) return
        defaultsToBugReport = !hasAgent
    }

    fun chooseAction(action: QuickPromptAction) {
        rememberAction(action)
        if (action == QuickPromptAction.Task && taskPeople == null) loadTaskPeople()
    }

    private fun rememberAction(action: QuickPromptAction?) {
        chosenAction = action
        actionChosen = true
        prefs?.putString(QuickPromptAction.STORE_KEY, action?.raw ?: "")
    }

    fun loadTaskPeople() {
        val actions = actions ?: return
        if (loadingTaskPeople) return
        loadingTaskPeople = true
        try {
            runCatching { actions.taskPeople() }.getOrNull()?.let { taskPeople = it }
        } finally {
            loadingTaskPeople = false
        }
    }

    val taskAssignees: List<String>
        get() = listOf("") + (taskPeople?.names ?: if (taskAssignee.isEmpty()) emptyList() else listOf(taskAssignee))

    fun taskAssigneeLabel(name: String): String = when {
        name.isEmpty() -> "Unassigned"
        name == taskPeople?.actor -> "@$name (you)"
        else -> "@$name"
    }

    fun chooseAssignee(name: String) {
        taskAssignee = name
        prefs?.putString(QuickPromptAction.ASSIGNEE_KEY, name)
    }

    val takesRunOptions: Boolean get() = action == null && destination is QuickLaunchDestination.Desktop

    val takesNoWorktree: Boolean get() = takesRunOptions && !QuickLaunchRunOptions.cutsOwnWorktree(model.provider)

    val sendsYolo: Boolean get() = runOptions.yolo && (runOptions.worktree || !takesNoWorktree)

    fun toggleYolo() {
        runOptions = runOptions.copy(yolo = !runOptions.yolo)
        prefs?.putBool(QuickLaunchRunOptions.YOLO_KEY, runOptions.yolo)
    }

    fun toggleWorktree() {
        runOptions = runOptions.copy(worktree = !runOptions.worktree)
    }

    val repository: AgentStartTarget? get() = plan?.target

    val contextLine: String
        get() {
            when (action) {
                QuickPromptAction.BugReport -> return "Bug report · ForkMesh team"
                QuickPromptAction.Task -> return "Task for ${taskAssigneeLabel(taskAssignee)}"
                QuickPromptAction.Note -> return "Note · your account"
                QuickPromptAction.Issue, QuickPromptAction.ActivityPub -> {
                    if (targets == null) return "Finding a repository…"
                    return repository?.label ?: "Choose a repository"
                }
                null -> {}
            }
            val target = repository
            if (targets == null) return "Finding a repository…"
            if (target == null) return "Choose a repository"
            if (plan?.loading == true) return "${target.label} · checking…"
            return "${target.label} · $destinationLabel"
        }

    val destinationLabel: String
        get() = when (val chosen = destination) {
            is QuickLaunchDestination.Node -> chosen.name
            QuickLaunchDestination.Device -> deviceName
            is QuickLaunchDestination.Desktop -> chosen.name.ifEmpty { "your desktop" }
            null -> "nowhere to run yet"
        }

    val destinations: List<QuickLaunchDestination>
        get() {
            val all = ArrayList<QuickLaunchDestination>()
            val options = plan?.options
            if (options?.canQueue == true) all.addAll(options.targets.map { QuickLaunchDestination.Node(it.node) })
            desktop()?.let { all.add(it) }
            if (repository?.canRunOnDevice == true) all.add(QuickLaunchDestination.Device)
            return all
        }

    val modelChoices: List<AgentModelChoice>
        get() = (if (actions == null) emptyList() else QuickPromptAction.entries.map { it.choice }) +
            AgentModelCatalog.nodeChoices(plan?.options?.models ?: emptyMap())

    private fun desktop(): QuickLaunchDestination.Desktop? {
        if (repository?.isOnMesh != true) return null
        val name = runCatching { backend.desktopName() }.getOrDefault("").trim()
        return if (name.isEmpty()) null else QuickLaunchDestination.Desktop(name)
    }

    fun detail(destination: QuickLaunchDestination): String = when (destination) {
        is QuickLaunchDestination.Desktop -> listOf("Your desktop, directly", runCatching { backend.desktopDetail() }.getOrDefault(""))
            .filter { it.isNotEmpty() }
            .joinToString(" · ")
        QuickLaunchDestination.Device -> when {
            !backend.hasDeviceCredentials() -> "Needs an API key"
            repository?.isMirrored == true -> "Runs on the copy on $deviceName"
            else -> "Mirrors the repository to $deviceName first"
        }
        is QuickLaunchDestination.Node -> {
            val node = plan?.options?.targetNamed(destination.name)
            if (node == null) {
                ""
            } else {
                val kind = if (node.isMirror) "Mirror" else "Source node"
                val runs = if (node.runs(model.provider)) "" else " · no ${agentProviderLabel(model.provider)}"
                "$kind · ${node.statusLabel}$runs"
            }
        }
    }

    fun prepare(currentRepo: String? = null) {
        if (prepared && currentRepo == null) {
            if (clock() - optionsCheckedAt >= OPTIONS_FRESH_MS) refresh()
            return
        }
        prepared = true
        if (targets == null) targets = runCatching { backend.loadTargets() }.getOrDefault(emptyList())
        val all = targets ?: emptyList()
        val wanted = listOf(currentRepo ?: "", memory.lastRepo).map { it.lowercase() }.filter { it.isNotEmpty() }
        val chosen = wanted.firstNotNullOfOrNull { key -> all.firstOrNull { matches(it, key) } }
            ?: mostRecent(all, backend.account())
        if (chosen != null && chosen != plan?.target) {
            select(chosen)
        } else if (clock() - optionsCheckedAt >= OPTIONS_FRESH_MS) {
            refreshOptions()
        }
        destination = resolveDestination()
        settleDefault()
        if (action == QuickPromptAction.Task && taskPeople == null) loadTaskPeople()
    }

    fun refresh() {
        if (!refreshOptions()) return
        val kept = when (val current = destination) {
            null -> false
            is QuickLaunchDestination.Node -> plan?.options?.targetNamed(current.name) != null
            else -> true
        }
        if (!kept) destination = resolveDestination()
        settleDefault()
    }

    private fun refreshOptions(): Boolean {
        val current = plan ?: return false
        if (current.loading || !current.target.isOnMesh) return false
        val generation = probe
        val options = runCatching { backend.launchOptions(current.target.owner, current.target.name) }.getOrNull()
            ?: return false
        if (generation != probe || plan?.target != current.target) return false
        plan = AgentStartPlan(current.target, options)
        optionsCheckedAt = clock()
        return true
    }

    fun choose(target: AgentStartTarget) {
        memory.rememberRepo(target.key)
        select(target)
        destination = resolveDestination()
        settleDefault()
    }

    fun choose(destination: QuickLaunchDestination) {
        this.destination = destination
        repository?.key?.let { memory.rememberDestination(it, destination) }
        settleDefault()
    }

    fun chooseModel(choice: AgentModelChoice) {
        QuickPromptAction.of(choice)?.let {
            chooseAction(it)
            return
        }
        rememberAction(null)
        model = choice
        prefs?.let { NodeModelPreference(it).write(choice) }
        val current = destination
        if (current is QuickLaunchDestination.Node) {
            val node = plan?.options?.targetNamed(current.name)
            if (node != null && !node.runs(choice.provider) && memory.destination(repository?.key ?: "") != current) {
                destination = resolveDestination()
            }
        }
    }

    fun credentialsChanged() {
        if (destination == null) destination = resolveDestination()
        settleDefault()
    }

    fun resolveDestination(): QuickLaunchDestination? {
        val current = plan ?: return null
        if (current.loading) return null
        val options = current.options
        val canQueue = options?.canQueue == true
        val target = current.target
        when (val remembered = memory.destination(target.key)) {
            is QuickLaunchDestination.Node -> if (canQueue && options?.targetNamed(remembered.name) != null) return remembered
            QuickLaunchDestination.Device -> if (target.canRunOnDevice) return remembered
            is QuickLaunchDestination.Desktop -> desktop()?.let { return it }
            null -> {}
        }
        if (canQueue) {
            options?.targets?.firstOrNull { it.online && it.runs(model.provider) }?.let { return QuickLaunchDestination.Node(it.node) }
        }
        if (target.isMirrored && backend.hasDeviceCredentials()) return QuickLaunchDestination.Device
        if (canQueue) {
            val node = options?.targets?.firstOrNull { it.runs(model.provider) } ?: options?.preferredTarget
            if (node != null) return QuickLaunchDestination.Node(node.node)
        }
        if (target.isMirrored) return QuickLaunchDestination.Device
        return null
    }

    fun send(prompt: String, images: List<PromptAttachment>): QuickLaunchOutcome {
        if (prompt.isBlank() && images.isEmpty()) return QuickLaunchOutcome.Ignored
        if (action == null && (!prepared || targets == null)) prepare()
        action?.let { return send(it, prompt, images) }
        return sendAgent(prompt, images)
    }

    private fun send(action: QuickPromptAction, prompt: String, images: List<PromptAttachment>): QuickLaunchOutcome {
        val actions = actions ?: return QuickLaunchOutcome.Failed("${action.label} is unavailable in this build.")
        val text = prompt.trim()
        if (text.isEmpty()) {
            return QuickLaunchOutcome.Failed(if (action == QuickPromptAction.BugReport) "Describe what went wrong." else "Type something first.")
        }
        if (!action.takesPhotos && images.isNotEmpty()) {
            return QuickLaunchOutcome.Failed("${action.label} cannot include photos yet. Remove them, or send a bug report.")
        }
        if (action.needsAccount && !backend.signedIn()) {
            return QuickLaunchOutcome.NeedsFix(QuickLaunchFix.SignInAccount, "Sign in to use ${action.label}.")
        }
        val (title, body) = QuickPromptAction.titleAndBody(text)
        return try {
            when (action) {
                QuickPromptAction.BugReport -> {
                    actions.reportBug(text, images)
                    QuickLaunchOutcome.Done("Sent to the ForkMesh team as a bug report.")
                }
                QuickPromptAction.Task -> {
                    val repo = repository?.let { "${it.owner}/${it.name}" } ?: ""
                    actions.createTask(title, text, taskAssignee, repo)
                    QuickLaunchOutcome.Done(if (taskAssignee.isEmpty()) "Created an unassigned task on General." else "Created a task for @$taskAssignee.")
                }
                QuickPromptAction.Note -> {
                    actions.takeNote(title, body)
                    QuickLaunchOutcome.Done("Saved as a note.")
                }
                QuickPromptAction.Issue, QuickPromptAction.ActivityPub -> {
                    if (!prepared || targets == null) prepare()
                    val repo = repository?.repo ?: return QuickLaunchOutcome.NeedsFix(
                        QuickLaunchFix.ChooseRepository,
                        "Choose a published repository for ${action.label}.",
                    )
                    if (action == QuickPromptAction.Issue) {
                        actions.createIssue(repo.owner, repo.name, title, body)
                        QuickLaunchOutcome.Done("Filed an issue on ${repo.owner}/${repo.name}.")
                    } else {
                        actions.postToActivityPub(repo.owner, repo.name, title.take(240), body.take(4000))
                        QuickLaunchOutcome.Done("Published to ActivityPub for ${repo.owner}/${repo.name}.")
                    }
                }
            }
        } catch (error: QuickActionError) {
            QuickLaunchOutcome.Failed(error.message)
        } catch (error: Exception) {
            QuickLaunchOutcome.Failed("${action.label} did not go through. Try again.")
        }
    }

    fun sendAgent(prompt: String, images: List<PromptAttachment>): QuickLaunchOutcome {
        if (prompt.isBlank() && images.isEmpty()) return QuickLaunchOutcome.Ignored
        if (!prepared || targets == null) prepare()
        if (!backend.signedIn()) {
            return QuickLaunchOutcome.NeedsFix(
                QuickLaunchFix.SignInAccount,
                "Sign in and the prompt goes straight to a mirror of the repository.",
            )
        }
        var current = plan ?: return QuickLaunchOutcome.NeedsFix(
            QuickLaunchFix.ChooseRepository,
            "There is no repository to point the agent at yet.",
        )
        if (current.loading) {
            select(current.target)
            destination = resolveDestination()
            current = plan ?: return QuickLaunchOutcome.Failed("The repository could not be checked.")
        }
        if (destination == null) {
            refresh()
            current = plan ?: current
        }
        val chosen = destination ?: return noDestination(current)
        memory.rememberRepo(current.target.key)
        return when (chosen) {
            is QuickLaunchDestination.Node -> sendToNode(current, chosen.name, prompt, images)
            QuickLaunchDestination.Device -> sendToDevice(current, prompt, images)
            is QuickLaunchDestination.Desktop -> sendToDesktop(current, chosen.name, prompt, images)
        }
    }

    private fun sendToDesktop(current: AgentStartPlan, desktop: String, prompt: String, images: List<PromptAttachment>): QuickLaunchOutcome {
        try {
            PromptAttachmentLimits.check(images)
        } catch (error: AgentLaunchException) {
            return QuickLaunchOutcome.Failed(error.message)
        }
        val target = current.target
        val owners = listOf(target.publicOwner, target.owner).filter { it.isNotEmpty() }.distinct()
        var failure: Throwable? = null
        for (owner in owners) {
            try {
                val options = QuickLaunchRunOptions(yolo = sendsYolo, worktree = runOptions.worktree || !takesNoWorktree)
                val id = backend.startOnDesktop(owner, target.name, promptOrPhotoNote(prompt, images), model.provider, model.id, options, images)
                runOptions = runOptions.copy(worktree = true)
                val where = desktop.ifEmpty { "your desktop" }
                return QuickLaunchOutcome.Desktop(desktop, id, if (id > 0) "Started agent #$id on $where." else "Sent to $where.")
            } catch (error: LocalControlException) {
                failure = error
                if (error.code != "no_checkout") break
            } catch (error: Exception) {
                failure = error
                break
            }
        }
        return QuickLaunchOutcome.Failed(failure?.let { ControlErrorText.describe(it) } ?: "The desktop did not take the prompt.")
    }

    private fun sendToNode(checked: AgentStartPlan, name: String, prompt: String, images: List<PromptAttachment>): QuickLaunchOutcome {
        var current = checked
        if (blocks(current.options?.targetNamed(name))) {
            refreshOptions()
            current = plan ?: current
        }
        val node = current.options?.targetNamed(name)
        if (node != null && blocks(node)) {
            return QuickLaunchOutcome.NeedsFix(
                QuickLaunchFix.signIn(node, model.provider),
                "${node.signInNode} has no ${agentProviderLabel(model.provider)} signed in.",
            )
        }
        val target = current.target
        val result = try {
            backend.launch(
                owner = target.owner,
                name = target.name,
                prompt = prompt,
                provider = AgentStartPlan.providerFor(model),
                model = model.alias,
                targetNode = name,
                title = AgentStartPlan.titleFor(prompt),
                orgScope = current.options?.orgScope ?: false,
                attachments = images,
            )
        } catch (error: AgentLaunchException) {
            return QuickLaunchOutcome.Failed(error.message)
        } catch (error: Exception) {
            return QuickLaunchOutcome.Failed(error.message ?: "The relay did not take the prompt.")
        }
        val repo = target.repo ?: Repository(owner = target.owner, name = target.name)
        val session = result.session
        if (session == null || session.sessionId.isEmpty()) return QuickLaunchOutcome.QueuedWithoutSession(result.label)
        val opened = session.copy(orgScope = current.options?.orgScope ?: false)
        return QuickLaunchOutcome.Queued(repo, opened, result.label, result)
    }

    private fun blocks(node: AgentLaunchTarget?): Boolean =
        node != null && !node.runs(model.provider) && model.provider in AGENT_LOGIN_PROVIDERS

    private fun sendToDevice(current: AgentStartPlan, prompt: String, images: List<PromptAttachment>): QuickLaunchOutcome {
        if (!backend.hasDeviceCredentials()) {
            return QuickLaunchOutcome.NeedsFix(
                QuickLaunchFix.SetUpDevice,
                "Add an API key for agents on $deviceName, then send again.",
            )
        }
        val target = current.target
        val record = target.record
        if (record != null) return QuickLaunchOutcome.Device(record, prompt, images)
        if (target.isOnMesh) {
            return QuickLaunchOutcome.NeedsFix(
                QuickLaunchFix.MirrorToDevice(target.publicOwner, target.name),
                "${target.publicOwner}/${target.name} is not on $deviceName yet.",
            )
        }
        return QuickLaunchOutcome.NeedsFix(QuickLaunchFix.ChooseRepository, "There is nothing to run here.")
    }

    private fun noDestination(current: AgentStartPlan): QuickLaunchOutcome {
        val first = current.options?.targets?.firstOrNull()
        if (first != null && model.provider in AGENT_LOGIN_PROVIDERS) {
            return QuickLaunchOutcome.NeedsFix(
                QuickLaunchFix.signIn(first, model.provider),
                "No node can run ${agentProviderLabel(model.provider)} for ${current.target.label} yet.",
            )
        }
        if (current.error.isNotEmpty()) return QuickLaunchOutcome.Failed(current.error)
        return QuickLaunchOutcome.NeedsFix(
            QuickLaunchFix.ChooseRepository,
            "Nothing can run agents for ${current.target.label}. Pick another repository.",
        )
    }

    private fun select(target: AgentStartTarget) {
        val generation = ++probe
        plan = AgentStartPlan(target, loading = target.isOnMesh)
        if (!target.isOnMesh) {
            plan = AgentStartPlan(target)
            return
        }
        var options: AgentLaunchOptions? = null
        var failure = ""
        try {
            options = backend.launchOptions(target.owner, target.name)
        } catch (error: AgentLaunchException) {
            failure = if (error.status == 403 || error.status == 401) "" else error.message
        } catch (error: Exception) {
            failure = error.message ?: error.toString()
        }
        if (generation != probe) return
        plan = AgentStartPlan(target, options, failure)
        optionsCheckedAt = clock()
    }

    companion object {
        const val OPTIONS_FRESH_MS = 15_000L

        fun mostRecent(targets: List<AgentStartTarget>, account: String): AgentStartTarget? {
            val me = account.trim().lowercase()
            fun mine(target: AgentStartTarget): Boolean {
                val repo = target.repo ?: return false
                return me.isNotEmpty() && listOf(repo.owner, repo.displayOwner, repo.organization).any { it.lowercase() == me }
            }
            fun newest(list: List<AgentStartTarget>) = list.maxByOrNull { it.repo?.updatedMs ?: 0L }
            return newest(targets.filter(::mine)) ?: targets.firstOrNull { it.isMirrored } ?: newest(targets)
        }

        fun matches(target: AgentStartTarget, key: String): Boolean =
            target.key == key || "${target.owner}/${target.name}".lowercase() == key || (target.record?.name?.lowercase() ?: "") == key
    }
}
