package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.code.diff.DiffLine
import com.forkmesh.nativeapp.code.diff.DiffLineKind
import com.forkmesh.nativeapp.model.RepoTreeEntry
import java.security.SecureRandom

object BranchReviewLedger {
    const val ROOT = ".forkmesh/branch-reviews"
    const val KEY_CHARS = 24
    private val EVENT_FILE = Regex("^\\d{4}-[a-z-]+\\.md$")

    fun key(branch: String): String = CollabSigner.sha256Hex(branch).take(KEY_CHARS)

    fun folder(branch: String): String = "$ROOT/${key(branch)}"

    fun eventFiles(entries: List<RepoTreeEntry>): List<RepoTreeEntry> =
        entries.filter { !it.isDirectory && EVENT_FILE.matches(it.name) }
            .sortedBy { it.name }
            .takeLast(ReviewLedger.MAX_EVENTS)

    fun load(reader: ReviewLedgerReader, owner: String, name: String, branch: String): List<ReviewEvent> {
        val folder = folder(branch)
        val listing = try {
            reader.tree(owner, name, folder, ReviewLedger.REF)
        } catch (_: Exception) {
            return emptyList()
        }
        val events = ArrayList<ReviewEvent>()
        for (file in eventFiles(listing.entries)) {
            val path = file.path.ifEmpty { "$folder/${file.name}" }
            try {
                val event = ReviewEvent.fromMarkdown(reader.blob(owner, name, path, ReviewLedger.REF).content)
                if (event.type in CollabContent.BRANCH_EVENT_TYPES) events.add(event)
            } catch (_: Exception) {
            }
        }
        return events
    }

    fun canComment(head: String, base: String): Boolean {
        val branch = head.trim()
        return branch.isNotEmpty() && branch != base.trim() && ReviewRecordRules.commitOid(branch).isEmpty()
    }
}

class BranchReviewSession(
    override val owner: String,
    override val name: String,
    val branch: String,
    private val reader: ReviewLedgerReader,
    private val writer: CollabRecordWriter,
    private val now: () -> Long = System::currentTimeMillis,
) : ReviewThreadActions {
    private var counter = 0
    private var byLine: Map<String, Map<DiffSide, Map<Int, List<ReviewThread>>>> = emptyMap()

    @Volatile var patch: String = ""
        private set
    @Volatile var patchset: String = ""
        private set
    @Volatile var events: List<ReviewEvent> = emptyList()
        private set
    @Volatile var pending: List<ReviewEvent> = emptyList()
        private set
    @Volatile var snapshot: ReviewSnapshot = ReviewSnapshot()
        private set
    @Volatile var loaded: Boolean = false
        private set
    @Volatile var busy: Boolean = false
        private set
    @Volatile var selection: DiffLineSelection? = null

    override val canWrite: Boolean get() = writer.signer != null

    @Synchronized
    fun attach(patch: String, headOid: String) {
        this.patch = patch
        patchset = ReviewRecordRules.commitOid(headOid)
        selection = null
        rebuild()
    }

    fun load() {
        val fresh = BranchReviewLedger.load(reader, owner, name, branch)
        synchronized(this) {
            events = fresh
            val known = fresh.mapTo(HashSet()) { it.id }
            pending = pending.filter { it.id.isEmpty() || it.id !in known }
            loaded = true
            rebuild()
        }
    }

    private fun rebuild() {
        val built = ReviewSnapshotBuilder.build(events + pending, patch, patchset)
        val index = HashMap<String, HashMap<DiffSide, HashMap<Int, ArrayList<ReviewThread>>>>()
        for (thread in built.threads) {
            val line = thread.paintedLine ?: continue
            index.getOrPut(thread.path) { HashMap() }.getOrPut(thread.side) { HashMap() }.getOrPut(line) { ArrayList() }.add(thread)
        }
        byLine = index
        snapshot = built
    }

    fun threadsAt(path: String, side: DiffSide, line: Int): List<ReviewThread> =
        if (line <= 0) emptyList() else byLine[path]?.get(side)?.get(line).orEmpty()

    fun threadsUnder(path: String, line: DiffLine): List<ReviewThread> {
        val old = if (line.kind != DiffLineKind.Added) threadsAt(path, DiffSide.Old, line.oldNumber) else emptyList()
        val new = if (line.kind != DiffLineKind.Removed) threadsAt(path, DiffSide.New, line.newNumber) else emptyList()
        return old + new
    }

    fun threadsUnder(path: String, left: DiffLine?, right: DiffLine?): List<ReviewThread> {
        val old = left?.takeIf { it.kind != DiffLineKind.Added }?.let { threadsAt(path, DiffSide.Old, it.oldNumber) }.orEmpty()
        val new = right?.takeIf { it.kind != DiffLineKind.Removed }?.let { threadsAt(path, DiffSide.New, it.newNumber) }.orEmpty()
        return old + new
    }

    fun fileThreads(path: String): List<ReviewThread> = snapshot.fileThreads(path)

    fun outdatedThreads(path: String): List<ReviewThread> = snapshot.outdatedThreads(path)

    fun isSelected(path: String, line: DiffLine): Boolean = selection?.let { it.path == path && it.contains(line) } ?: false

    fun tap(path: String, line: DiffLine) {
        if (!canWrite) return
        selection = DiffLineSelection.next(selection, path, line)
    }

    fun clearSelection() {
        selection = null
    }

    private fun nextId(type: String): String {
        counter++
        val bytes = ByteArray(4)
        random.nextBytes(bytes)
        return ReviewComposer.eventId(type, now(), "$counter-" + bytes.joinToString("") { "%02x".format(it.toInt() and 0xff) })
    }

    fun draft(body: String): ReviewEventFields? {
        val current = selection ?: return null
        return ReviewComposer.branchComment(current, body, patch, patchset) { nextId("thread") }
    }

    private fun send(fields: ReviewEventFields, ok: String): CollabNotice {
        if (busy) return CollabNotice("Still sending.", true)
        busy = true
        return try {
            val event = writer.sendBranch(branch, fields)
            synchronized(this) {
                pending = pending + ReviewEvent.fromJson(event)
                rebuild()
            }
            CollabNotice(ok)
        } catch (failure: Exception) {
            CollabNotice("Failed: ${failure.message ?: failure}", true)
        } finally {
            busy = false
        }
    }

    fun commentNow(body: String): CollabNotice {
        ReviewComposer.validate(body)?.let { return CollabNotice(it, true) }
        val fields = draft(body) ?: return CollabNotice("Pick a line first.", true)
        val notice = send(fields, "Comment sent. Everyone viewing $branch sees it once the ledger is updated.")
        if (!notice.isError) selection = null
        return notice
    }

    override fun reply(thread: ReviewThread, body: String): CollabNotice {
        ReviewComposer.validate(body)?.let { return CollabNotice(it, true) }
        return send(ReviewComposer.reply(thread, body), "Reply sent.")
    }

    override fun setResolved(thread: ReviewThread, resolved: Boolean): CollabNotice =
        send(
            ReviewComposer.setResolved(thread, resolved, reopened = UNRESOLVED),
            if (resolved) "Thread resolved." else "Thread reopened.",
        )

    override fun isPending(event: ReviewEvent): Boolean = event in pending

    companion object {
        const val UNRESOLVED = "unresolved"
        private val random = SecureRandom()
    }
}
