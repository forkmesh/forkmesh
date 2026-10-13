package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.code.diff.DiffFile
import com.forkmesh.nativeapp.code.diff.DiffLine
import com.forkmesh.nativeapp.code.diff.parseUnifiedDiff
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.model.RepoBlob
import com.forkmesh.nativeapp.model.RepoBranch
import com.forkmesh.nativeapp.model.RepoComparison
import com.forkmesh.nativeapp.model.RepoTree
import com.forkmesh.nativeapp.model.RepoTreeEntry
import com.forkmesh.nativeapp.security.DeployApproval
import com.forkmesh.nativeapp.security.DeployApprover
import java.security.SecureRandom

interface ReviewLedgerReader {
    fun tree(owner: String, name: String, path: String, ref: String): RepoTree
    fun blob(owner: String, name: String, path: String, ref: String): RepoBlob
    fun compare(owner: String, name: String, base: String, head: String): RepoComparison
}

class ApiReviewLedgerReader(private val api: ApiService) : ReviewLedgerReader {
    override fun tree(owner: String, name: String, path: String, ref: String) = api.tree(owner, name, path, ref)
    override fun blob(owner: String, name: String, path: String, ref: String) = api.blob(owner, name, path, ref)
    override fun compare(owner: String, name: String, base: String, head: String) = api.compare(owner, name, base, head)
}

class ReviewLedger(private val reader: ReviewLedgerReader, private val owner: String, private val name: String) {
    fun load(number: Int): PullReviewRecord {
        val folder = ".forkmesh/pulls/$number"
        val listing = try {
            reader.tree(owner, name, folder, REF)
        } catch (_: Exception) {
            throw CollabException("Pull request #$number could not be read from the pull ledger.")
        }
        if (listing.entries.none { it.name == "pull.md" }) throw CollabException("Pull request #$number was not found.")
        val header = reader.blob(owner, name, "$folder/pull.md", REF)
        val events = ArrayList<ReviewEvent>()
        for (file in eventFileNames(listing.entries)) {
            try {
                events.add(ReviewEvent.fromMarkdown(reader.blob(owner, name, file.path, REF).content))
            } catch (_: Exception) {
            }
        }
        var storedPatch = ""
        if (listing.entries.any { it.name == "changes.patch" }) {
            storedPatch = try {
                reader.blob(owner, name, "$folder/changes.patch", REF).content
            } catch (_: Exception) {
                ""
            }
        }
        var record = PullReviewRecord.fromMarkdown(number, header.content, events, storedPatch)
        if (record.patch.isEmpty()) record = record.copy(patch = comparePatch(record))
        if (record.patch.toByteArray(Charsets.UTF_8).size > MAX_PATCH_BYTES) record = record.copy(patch = "")
        return record
    }

    private fun comparePatch(record: PullReviewRecord): String {
        val pairs = listOf(record.creationBaseOid to record.creationHeadOid, record.base to record.head)
            .filter { it.first.isNotEmpty() && it.second.isNotEmpty() }
        for ((base, head) in pairs) {
            try {
                val comparison = reader.compare(owner, name, base, head)
                if (comparison.patch.isNotEmpty()) return comparison.patch
            } catch (_: Exception) {
            }
        }
        return ""
    }

    companion object {
        const val REF = "forkmesh/pulls"
        const val MAX_EVENTS = 400
        const val MAX_PATCH_BYTES = 4 * 1024 * 1024

        fun eventFileNames(entries: List<RepoTreeEntry>): List<RepoTreeEntry> =
            entries.filter { !it.isDirectory && it.name.endsWith(".md") && it.name != "pull.md" }
                .sortedBy { it.name }
                .takeLast(MAX_EVENTS)
    }
}

class PullReviewSession(
    override val owner: String,
    override val name: String,
    val number: Int,
    reader: ReviewLedgerReader,
    private val writer: CollabRecordWriter,
    private val now: () -> Long = System::currentTimeMillis,
) : ReviewThreadActions {
    private val ledger = ReviewLedger(reader, owner, name)
    private var counter = 0

    @Volatile var record: PullReviewRecord? = null
        private set
    @Volatile var snapshot: ReviewSnapshot = ReviewSnapshot()
        private set
    @Volatile var files: List<DiffFile> = emptyList()
        private set
    @Volatile var loading: Boolean = false
        private set
    @Volatile var loadError: String? = null
        private set
    @Volatile var pending: List<ReviewEvent> = emptyList()
        private set
    @Volatile var busy: Boolean = false
        private set
    @Volatile var selection: DiffLineSelection? = null
    val drafts = ReviewDraftBook()
    @Volatile var viewed: Set<String> = emptySet()

    override val canWrite: Boolean get() = writer.signer != null
    val patchset: String get() = record?.currentPatchset ?: ""
    val patch: String get() = record?.patch ?: ""

    fun load() {
        loading = true
        try {
            val loaded = ledger.load(number)
            record = loaded
            files = parseUnifiedDiff(loaded.patch)
            loadError = null
            rebuild()
        } catch (failure: Exception) {
            loadError = failure.message ?: "$failure"
        } finally {
            loading = false
        }
    }

    private fun rebuild() {
        val current = record ?: return
        snapshot = ReviewSnapshotBuilder.build(current.events + pending, current.patch, current.currentPatchset)
    }

    fun file(path: String): DiffFile? = files.firstOrNull { it.path == path }

    fun threadCount(path: String): Int = snapshot.files[path]?.totalThreads ?: 0

    fun tap(path: String, line: DiffLine) {
        selection = DiffLineSelection.next(selection, path, line)
    }

    fun clearSelection() {
        selection = null
    }

    fun toggleViewed(path: String) {
        viewed = if (path in viewed) viewed - path else viewed + path
    }

    val viewedSummary: String get() = "${viewed.intersect(files.map { it.path }.toSet()).size} of ${files.size} viewed"

    private fun nextId(type: String): String {
        counter++
        val bytes = ByteArray(4)
        random.nextBytes(bytes)
        return ReviewComposer.eventId(type, now(), "$counter-" + bytes.joinToString("") { "%02x".format(it.toInt() and 0xff) })
    }

    fun addDraft(body: String, suggestion: String = ""): String? {
        val current = selection ?: return "Pick a line first."
        val problem = drafts.add(current, body, nextId("draft"), suggestion)
        if (problem == null) selection = null
        return problem
    }

    private fun send(fields: List<ReviewEventFields>, ok: String): CollabNotice {
        if (busy) return CollabNotice("Still sending.", true)
        busy = true
        return try {
            for (item in fields) {
                val event = writer.sendPull(number, item)
                pending = pending + ReviewEvent.fromJson(event)
            }
            rebuild()
            CollabNotice(ok)
        } catch (failure: Exception) {
            rebuild()
            CollabNotice("Failed: ${failure.message ?: failure}", true)
        } finally {
            busy = false
        }
    }

    fun commentNow(body: String, suggestion: String = ""): CollabNotice {
        ReviewComposer.validate(body)?.let { return CollabNotice(it, true) }
        val current = selection ?: return CollabNotice("Pick a line first.", true)
        val fields = ReviewComposer.inlineComment(current, body, patch, patchset, suggestion) { nextId("line") }
        val notice = send(listOf(fields), "Comment sent. It appears for everyone once the owner's node applies it.")
        if (!notice.isError) selection = null
        return notice
    }

    override fun reply(thread: ReviewThread, body: String): CollabNotice {
        ReviewComposer.validate(body)?.let { return CollabNotice(it, true) }
        return send(listOf(ReviewComposer.reply(thread, body)), "Reply sent.")
    }

    override fun setResolved(thread: ReviewThread, resolved: Boolean): CollabNotice =
        send(listOf(ReviewComposer.setResolved(thread, resolved)), if (resolved) "Thread resolved." else "Thread reopened.")

    fun submit(verdict: ReviewVerdict, body: String): CollabNotice {
        ReviewComposer.validateReview(verdict, body, drafts.drafts)?.let { return CollabNotice(it, true) }
        val events = ReviewComposer.submission(verdict, body, drafts.drafts, patch, patchset) { nextId("thread") }
        val before = pending.size
        val notice = send(events, if (verdict == ReviewVerdict.Comment) "Review sent." else "${verdict.title} sent.")
        val sentDrafts = minOf(maxOf(0, pending.size - before), drafts.count)
        for (draft in drafts.drafts.take(sentDrafts)) drafts.remove(draft.id)
        return notice
    }

    override fun isPending(event: ReviewEvent): Boolean = event in pending

    private companion object {
        val random = SecureRandom()
    }
}

data class PullMergePlan(
    val number: Int,
    val expectedBaseOid: String,
    val expectedHeadOid: String,
    val expectedPullsOid: String,
    val branchBacked: Boolean = true,
    val clean: Boolean = true,
    val conflicts: List<String> = emptyList(),
) {
    val isConsistent: Boolean
        get() = expectedBaseOid.isNotEmpty() && expectedHeadOid.isNotEmpty() && expectedPullsOid.isNotEmpty() &&
            setOf(expectedBaseOid.length, expectedHeadOid.length, expectedPullsOid.length).size == 1

    val isReady: Boolean get() = isConsistent && clean
}

data class PullMergeOutcome(val merged: Boolean, val message: String = "", val pending: Boolean = false)

sealed class PullMergeState {
    data object Merged : PullMergeState()
    data object Closed : PullMergeState()
    data object Checking : PullMergeState()
    data object NotOwner : PullMergeState()
    data object NotPinned : PullMergeState()
    data class Conflicts(val files: List<String>) : PullMergeState()
    data class ChangesRequested(val who: List<String>) : PullMergeState()
    data object NeedsIndependentReview : PullMergeState()
    data object Ready : PullMergeState()

    val canMerge: Boolean get() = this == Ready || this == NeedsIndependentReview
    val needsWaiver: Boolean get() = this == NeedsIndependentReview

    val tone: CollabTone
        get() = when (this) {
            Merged -> CollabTone.AccentEdge
            Ready -> CollabTone.Success
            Closed, is Conflicts, is ChangesRequested -> CollabTone.Danger
            else -> CollabTone.Secondary
        }
}

data class PullMergeInputs(
    val status: String,
    val author: String,
    val events: List<ReviewEvent>,
    val isOwner: Boolean?,
    val plan: PullMergePlan?,
    val planLoaded: Boolean,
)

object PullMergeRules {
    const val CHECKING = "Checking whether this can be merged from here…"
    const val NOT_PINNED =
        "This pull request could not be pinned to exact commits, so it is not offered for merge here. Use the desktop request below."
    const val WAIVER_TITLE = "Merge without an independent review?"
    const val WAIVER_COPY =
        "This pull request has not been reviewed by anyone else. As the repository owner you can merge it anyway; the audit log records that the review gate was waived."

    fun state(input: PullMergeInputs): PullMergeState {
        val status = input.status.trim().lowercase()
        if (status == "merged") return PullMergeState.Merged
        if (status == "closed") return PullMergeState.Closed
        val isOwner = input.isOwner ?: return PullMergeState.Checking
        if (!isOwner) return PullMergeState.NotOwner
        if (!input.planLoaded) return PullMergeState.Checking
        val plan = input.plan
        if (plan == null || !plan.isConsistent) return PullMergeState.NotPinned
        if (!plan.clean) return PullMergeState.Conflicts(plan.conflicts)
        val blockers = ReviewGate.changesRequestedBy(input.events, input.author)
        if (blockers.isNotEmpty()) return PullMergeState.ChangesRequested(blockers)
        if (ReviewGate.independentApprovals(input.events, input.author) == 0) return PullMergeState.NeedsIndependentReview
        return PullMergeState.Ready
    }

    fun headline(state: PullMergeState, base: String): String {
        val target = base.ifEmpty { "the base branch" }
        return when (state) {
            PullMergeState.Merged -> "Merged."
            PullMergeState.Closed -> "This pull request is closed."
            PullMergeState.Checking -> CHECKING
            PullMergeState.NotOwner -> "Only an owner of this repository can merge it."
            PullMergeState.NotPinned -> NOT_PINNED
            is PullMergeState.Conflicts -> {
                if (state.files.isEmpty()) {
                    "This branch conflicts with $target."
                } else {
                    val shown = state.files.take(8).joinToString(", ")
                    val more = if (state.files.size > 8) " and ${state.files.size - 8} more" else ""
                    "${state.files.size} file${if (state.files.size == 1) "" else "s"} conflict with $target: $shown$more."
                }
            }
            is PullMergeState.ChangesRequested ->
                "Changes requested by ${state.who.joinToString(", ")}. Resolve the review before merging."
            PullMergeState.NeedsIndependentReview -> "No one else has approved this yet. You can merge anyway as the owner."
            PullMergeState.Ready -> "Ready to merge into $target."
        }
    }

    fun blockers(state: PullMergeState, base: String): List<String> = when (state) {
        is PullMergeState.Conflicts, is PullMergeState.ChangesRequested, PullMergeState.NotPinned, PullMergeState.Closed ->
            listOf(headline(state, base))
        else -> emptyList()
    }

    fun mergeMessage(body: Map<String, Any?>, status: Int): String {
        val message = body.str("message").trim()
        if (message.isNotEmpty()) return message
        return when (val code = body.str("error").trim()) {
            "forbidden" -> "Only an owner of this repository can merge it from here."
            "invalid_session" -> "Sign in again before merging."
            "merge_conflict" -> "The branches conflict. Resolve the conflict, then merge."
            "review_required" -> "This pull request needs an independent review before it can merge."
            "checks_required" -> "Required checks have not passed yet."
            "stale_base", "stale_head", "stale_pull_metadata" -> "The repository moved while this was being prepared. Refresh and merge again."
            "pull_not_open" -> "This pull request is no longer open."
            "unsupported_pull" -> "This pull request cannot be merged from the app; merge it from the ForkMesh desktop app."
            "merge_node_unavailable" -> when (body.str("reason").trim()) {
                "refs_catching_up" -> "The available nodes have not agreed on this repository's latest refs yet. ForkMesh will retry while they catch up."
                "mirror_unverified" -> "The available nodes could not verify this repository's signed merge state. ForkMesh will retry shortly."
                "no_merge_executor" -> "The available nodes do not run the merge executor for this repository. Merge from an updated desktop or bring an executor online."
                "job_node_offline" -> "The node already handling this merge is temporarily offline. ForkMesh will keep checking it."
                else -> "No healthy mirror could verify this repository for the merge yet. ForkMesh will retry shortly."
            }
            "mirror_unavailable" -> "The relay could not load this repository's current mirror route. ForkMesh will retry shortly."
            "merge_queue_full" -> "Too many merges are queued for this repository. Try again shortly."
            "" -> "The merge was refused (HTTP $status)."
            else -> code.replace('_', ' ')
        }
    }

    fun mergeBody(
        plan: PullMergePlan,
        requestId: String,
        overrideReview: Boolean,
        deployApproval: Map<String, Any?>? = null,
    ): Map<String, Any?> {
        val body = linkedMapOf<String, Any?>(
            "schemaVersion" to 1L,
            "type" to "forkmesh.pull-merge-v1",
            "pullNumber" to plan.number.toLong(),
            "requestId" to requestId,
            "expectedBaseOid" to plan.expectedBaseOid,
            "expectedHeadOid" to plan.expectedHeadOid,
            "expectedPullsOid" to plan.expectedPullsOid,
        )
        if (overrideReview) body["overrideReview"] = true
        if (deployApproval != null) body["deployApproval"] = deployApproval
        return body
    }

    fun mergeRequestId(): String {
        val bytes = ByteArray(18)
        SecureRandom().nextBytes(bytes)
        return "mobile_" + bytes.joinToString("") { "%02x".format(it.toInt() and 0xff) }
    }
}

data class PullBranchState(val ahead: Int = 0, val behind: Int = 0, val conflicts: List<String> = emptyList(), val clean: Boolean = true) {
    val label: String
        get() {
            if (!clean) return if (conflicts.isEmpty()) "Conflicts" else "${conflicts.size} conflict${if (conflicts.size == 1) "" else "s"}"
            val parts = ArrayList<String>()
            if (ahead > 0) parts.add("$ahead ahead")
            if (behind > 0) parts.add("$behind behind")
            return if (parts.isEmpty()) "Up to date" else parts.joinToString(" · ")
        }

    val tone: CollabTone get() = if (!clean) CollabTone.Danger else if (behind > 0) CollabTone.Secondary else CollabTone.Success
}

interface PullMergeReader {
    fun treeCommit(owner: String, name: String, path: String, ref: String): String
    fun branches(owner: String, name: String): List<RepoBranch>
    fun mergePreview(owner: String, name: String, base: String, head: String): Map<String, Any?>?
    fun hasPullPayload(owner: String, name: String, number: Int): Boolean = true
    fun routeOwner(owner: String, name: String): String = owner
    fun postMerge(owner: String, name: String, number: Int, body: Map<String, Any?>): Pair<Int, Map<String, Any?>>
    fun merged(owner: String, name: String) {}
}

class ApiPullMergeReader(private val api: ApiService) : PullMergeReader {
    override fun treeCommit(owner: String, name: String, path: String, ref: String): String =
        api.tree(owner, name, path, ref).commit

    override fun branches(owner: String, name: String): List<RepoBranch> = api.branches(owner, name)

    override fun hasPullPayload(owner: String, name: String, number: Int): Boolean = try {
        api.tree(owner, name, ".forkmesh/pulls/$number", ReviewLedger.REF).entries
            .any { !it.isDirectory && (it.name == "commits.mbox" || it.name == "changes.patch") }
    } catch (_: Exception) {
        false
    }

    override fun merged(owner: String, name: String) = api.clearRepoCache(owner, name)

    override fun routeOwner(owner: String, name: String): String = api.readOwner(owner, name)

    override fun mergePreview(owner: String, name: String, base: String, head: String): Map<String, Any?>? {
        val result = api.jsonExchange(
            "GET",
            "/api/repo/${api.readOwner(owner, name)}/$name/merge-preview",
            listOf("base" to base, "head" to head, "ref" to ""),
        )
        return if (result.ok && result.body.isNotEmpty()) result.body else null
    }

    override fun postMerge(owner: String, name: String, number: Int, body: Map<String, Any?>): Pair<Int, Map<String, Any?>> {
        val result = api.jsonExchange(
            "POST",
            "/api/repo/${routeOwner(owner, name)}/$name/pulls/$number/merge",
            body = body,
            extraHeaders = mapOf("Origin" to api.gitOrigin),
        )
        return result.statusCode to result.body
    }
}

class PullMergeClient(
    private val reader: PullMergeReader,
    private val sleep: (Long) -> Unit = { Thread.sleep(it) },
    private val requestId: () -> String = PullMergeRules::mergeRequestId,
    private val approver: DeployApprover? = null,
) {
    fun plan(owner: String, name: String, pull: PublishedPull): PullMergePlan? {
        if (pull.status.trim().lowercase() != "open") return null
        val pullsOid = try {
            reader.treeCommit(owner, name, ".forkmesh/pulls", ReviewLedger.REF).trim().lowercase()
        } catch (_: Exception) {
            ""
        }
        if (pullsOid.isEmpty()) return null
        val baseName = pull.base.trim()
        if (baseName.isEmpty()) return null
        val baseTip = try {
            reader.branches(owner, name).firstOrNull { it.name == baseName }?.sha?.trim()?.lowercase().orEmpty()
        } catch (_: Exception) {
            ""
        }
        if (baseTip.isEmpty()) return null
        val head = pull.head.trim()
        if (!pull.branchBacked || head.isEmpty()) {
            if (!reader.hasPullPayload(owner, name, pull.number)) return null
            return PullMergePlan(pull.number, baseTip, pullsOid, pullsOid, branchBacked = false)
        }
        val preview = reader.mergePreview(owner, name, baseTip, head) ?: return null
        val baseOid = preview.str("baseOid").trim().lowercase()
        val headOid = preview.str("headOid").trim().lowercase()
        if (baseOid.isEmpty() || headOid.isEmpty() || baseOid != baseTip) return null
        val conflicts = (preview["conflicts"] as? List<*>).orEmpty().map { it?.toString().orEmpty() }.filter { it.isNotEmpty() }
        return PullMergePlan(pull.number, baseOid, headOid, pullsOid, clean = preview["clean"] == true, conflicts = conflicts.take(200))
    }

    fun merge(owner: String, name: String, plan: PullMergePlan, overrideReview: Boolean = false): PullMergeOutcome {
        if (!plan.isConsistent) {
            throw CollabException("This pull request could not be pinned to exact commits, so it was not merged. Refresh and try again.")
        }
        val id = requestId()
        val approval = approver?.approve(
            DeployApproval.PULL,
            reader.routeOwner(owner, name),
            name,
            plan.number.toString(),
            plan.expectedBaseOid,
            plan.expectedHeadOid,
            id,
        )
        val body = PullMergeRules.mergeBody(plan, id, overrideReview, approval)
        var pollAttempts = 0
        var routeAttempts = 0
        while (true) {
            val (status, decoded) = reader.postMerge(owner, name, plan.number, body)
            val state = decoded.str("status").trim().lowercase()
            val ok = status in 200..299
            val code = decoded.str("error").trim()
            val retryRoute = status == 503 && (
                code == "mirror_unavailable" ||
                    (code == "merge_node_unavailable" && decoded.str("reason").trim() != "no_merge_executor")
                )
            if (retryRoute && routeAttempts < ROUTE_RETRY_ATTEMPTS - 1) {
                routeAttempts += 1
                sleep(ROUTE_RETRY_DELAY_MS)
                continue
            }
            if (ok && (status == 202 || state == "processing")) {
                pollAttempts += 1
                if (pollAttempts == POLL_ATTEMPTS) break
                sleep(POLL_DELAY_MS)
                continue
            }
            if (ok && state == "merged") {
                reader.merged(owner, name)
                return PullMergeOutcome(true, "Merged.")
            }
            throw CollabException(PullMergeRules.mergeMessage(decoded, status))
        }
        return PullMergeOutcome(
            false,
            "The node is still merging. It finishes on its own; reopen this pull request in a moment to see the result.",
            pending = true,
        )
    }

    companion object {
        const val POLL_ATTEMPTS = 8
        const val POLL_DELAY_MS = 750L
        const val ROUTE_RETRY_ATTEMPTS = 75
        const val ROUTE_RETRY_DELAY_MS = 2_000L
    }
}
