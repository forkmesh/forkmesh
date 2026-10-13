package com.forkmesh.nativeapp.coves

data class CoveRepo(val owner: String, val name: String) {
    val slug: String get() = "$owner/$name"
}

data class CoveFileRef(val path: String, val size: Long = 0)

data class CoveAccountCheck(val exists: Boolean, val active: Boolean)

interface CoveRepositoryIO {
    fun listFiles(repo: CoveRepo): List<CoveFileRef>
    fun readFile(repo: CoveRepo, path: String, maxBytes: Int): ByteArray
    fun propose(repo: CoveRepo, title: String, head: String, patch: String, description: String)
    fun checkAccount(name: String): CoveAccountCheck? = null
}

class CoveEntry(val cove: Cove, val servedBytes: ByteArray?, val pendingReview: Boolean) {
    val id: String get() = cove.id

    fun with(cove: Cove = this.cove, servedBytes: ByteArray? = this.servedBytes, pendingReview: Boolean = this.pendingReview) =
        CoveEntry(cove, servedBytes, pendingReview)
}

sealed class CovesPhase {
    data object Idle : CovesPhase()
    data object Loading : CovesPhase()
    data object Loaded : CovesPhase()
    data class Failed(val message: String) : CovesPhase()
}

class CoveActionException(message: String) : Exception(message) {
    companion object {
        val signInRequired get() = CoveActionException("Sign in with a ForkMesh account.")
        val notFound get() = CoveActionException("That cove is no longer in this repository.")
        val locked get() = CoveActionException("Unlock the cove first.")
        val onlyCreatorShares get() = CoveActionException("Only the cove creator can change sharing.")
        val notAccountCove get() = CoveActionException("Password coves are shared by passing the password out of band.")
        fun alreadyMember(name: String) = CoveActionException("@$name already has access to this cove.")
        fun invalidAccount(name: String) = CoveActionException("$name is not a ForkMesh account.")
        fun inactiveAccount(name: String) = CoveActionException("$name is not an active ForkMesh account.")
        fun documentExists(name: String) = CoveActionException("A document named $name already exists.")
    }
}

class CovesModel(
    val repo: CoveRepo,
    account: String,
    openerKey: String = "",
    private val io: CoveRepositoryIO,
    private val rounds: Int = CoveCrypto.DEFAULT_ROUNDS,
    private val clock: () -> Long = System::currentTimeMillis,
) {
    private val lock = Any()
    private val listeners = java.util.concurrent.CopyOnWriteArrayList<() -> Unit>()
    private val passwords = HashMap<String, String>()

    @Volatile var phase: CovesPhase = CovesPhase.Idle
        private set
    @Volatile var entries: List<CoveEntry> = emptyList()
        private set
    @Volatile var skipped: Int = 0
        private set
    @Volatile var busy: Boolean = false
        private set
    @Volatile var account: String = account
    @Volatile var openerKey: String = openerKey

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    private fun changed() {
        for (listener in listeners) listener()
    }

    val unlockedCoves: List<Cove> get() = entries.map { it.cove }.filter { it.unlocked }
    val lockedCoves: List<Cove> get() = entries.map { it.cove }.filter { !it.unlocked }

    fun entry(id: String): CoveEntry? = entries.firstOrNull { it.cove.id == id }
    fun cove(id: String): Cove? = entry(id)?.cove

    fun shareLink(id: String): String = CoveFiles.shareUrl(repo.owner, repo.name, id)

    private fun replace(id: String, transform: (CoveEntry) -> CoveEntry) {
        synchronized(lock) {
            entries = entries.map { if (it.cove.id == id) transform(it) else it }
        }
        changed()
    }

    fun load() {
        phase = CovesPhase.Loading
        changed()
        try {
            val files = io.listFiles(repo).filter { it.path.endsWith(".cove") }.take(MAX_COVES)
            val loaded = ArrayList<CoveEntry>()
            var skippedCount = 0
            val previousEntries = entries
            for (file in files) {
                if (file.size > MAX_ENVELOPE_BYTES) {
                    skippedCount++
                    continue
                }
                val bytes = try {
                    io.readFile(repo, file.path, MAX_ENVELOPE_BYTES)
                } catch (_: Exception) {
                    null
                }
                val cove = bytes?.let { CoveStore.parseEnvelope(it, file.path) }
                if (bytes == null || cove == null) {
                    skippedCount++
                    continue
                }
                val previous = previousEntries.firstOrNull { it.cove.id == cove.id }
                if (previous != null && previous.cove.unlocked && previous.servedBytes?.contentEquals(bytes) == true) {
                    loaded.add(previous)
                } else {
                    loaded.add(CoveEntry(cove, bytes, false))
                }
            }
            val listed = loaded.map { it.cove.id }.toSet()
            loaded += previousEntries.filter { it.servedBytes == null && it.pendingReview && it.cove.id !in listed }
            synchronized(lock) { entries = loaded }
            skipped = skippedCount
            phase = CovesPhase.Loaded
            changed()
            unlockAccountCoves()
            reopenPasswordCoves()
        } catch (error: Exception) {
            phase = CovesPhase.Failed(error.message ?: error.toString())
            changed()
        }
    }

    fun unlockAccountCoves() {
        val me = CoveStore.normalizedAccount(account)
        if (me.isEmpty()) return
        for (entry in entries) {
            if (entry.cove.unlocked) continue
            val opened = CoveStore.unlockForAccount(entry.cove, me) ?: continue
            replace(opened.id) { it.with(cove = opened) }
        }
    }

    private fun reopenPasswordCoves() {
        for (entry in entries) {
            if (entry.cove.unlocked) continue
            val password = synchronized(lock) { passwords[entry.cove.id] } ?: continue
            val opened = CoveStore.unlock(entry.cove, password) ?: continue
            replace(opened.id) { it.with(cove = opened) }
        }
    }

    fun unlock(id: String, password: String): Boolean {
        val candidate = entry(id)?.cove ?: return false
        if (password.isEmpty()) return false
        val opened = CoveStore.unlock(candidate, password) ?: return false
        synchronized(lock) { passwords[id] = password }
        replace(id) { it.with(cove = opened) }
        return true
    }

    fun lock(id: String) {
        val entry = entry(id) ?: return
        synchronized(lock) { passwords.remove(id) }
        val locked = CoveStore.parseEnvelope(entry.servedBytes ?: ByteArray(0), entry.cove.relPath)
        if (locked == null) {
            replace(id) {
                it.with(cove = it.cove.copy(documents = emptyList(), accessLog = emptyList(), contentKey = ByteArray(0), unlocked = false))
            }
            return
        }
        replace(id) { it.with(cove = locked.copy(unlocked = false)) }
    }

    fun lockAll() {
        for (entry in entries) lock(entry.cove.id)
        synchronized(lock) { passwords.clear() }
    }

    fun forget() {
        synchronized(lock) {
            passwords.clear()
            entries = emptyList()
        }
        phase = CovesPhase.Idle
        changed()
    }

    fun createAccountCove(name: String, notifyOnOpen: Boolean = false): Cove {
        val me = CoveStore.normalizedAccount(account)
        if (me.isEmpty()) throw CoveActionException.signInRequired
        val sealed = CoveStore.newAccountCove(
            name = name, creatorAccount = me, invitedAccounts = listOf(me), creator = openerKey,
            notifyOnOpen = notifyOnOpen, nowMs = clock(), rounds = rounds,
        )
        publish(sealed, previous = null, password = null)
        return sealed
    }

    fun createPasswordCove(name: String, password: String, notifyOnOpen: Boolean = true): Cove {
        if (CoveStore.normalizedAccount(account).isEmpty()) throw CoveActionException.signInRequired
        val sealed = CoveStore.newPasswordCove(
            name = name, password = password, creator = openerKey, notifyOnOpen = notifyOnOpen, nowMs = clock(), rounds = rounds,
        )
        publish(sealed, previous = null, password = password)
        synchronized(lock) { passwords[sealed.id] = password }
        return sealed
    }

    fun saveDocument(coveId: String, name: String, body: String) {
        mutate(coveId, "edit") { cove ->
            val wanted = CoveStore.cleanPath(name)
            val index = cove.documents.indexOfFirst { CoveStore.cleanPath(it.name) == wanted }
            if (index < 0) throw CoveActionException.notFound
            cove.copy(documents = cove.documents.toMutableList().also { it[index] = it[index].copy(body = body, updatedAtMs = clock()) })
        }
    }

    fun addDocument(coveId: String, template: CoveFileTemplate, name: String = ""): String {
        var created = ""
        mutate(coveId, "edit") { cove ->
            val wanted = name.trim().ifEmpty { template.defaultPath }
            val unique = CoveFiles.uniqueName(wanted, cove.documents.map { it.name })
            created = unique
            val now = clock()
            cove.copy(documents = cove.documents + CoveDocument("doc-$now", unique, template.mime, template.body, now))
        }
        return created
    }

    fun renameDocument(coveId: String, from: String, to: String) {
        mutate(coveId, "edit") { cove ->
            val source = CoveStore.cleanPath(from)
            val target = CoveStore.cleanPath(to.trim())
            if (target.isEmpty() || target == source) return@mutate cove
            if (cove.documents.any { CoveStore.cleanPath(it.name).lowercase() == target.lowercase() }) {
                throw CoveActionException.documentExists(target)
            }
            val index = cove.documents.indexOfFirst { CoveStore.cleanPath(it.name) == source }
            if (index < 0) throw CoveActionException.notFound
            cove.copy(documents = cove.documents.toMutableList().also { it[index] = it[index].copy(name = target, updatedAtMs = clock()) })
        }
    }

    fun deleteDocument(coveId: String, name: String) {
        mutate(coveId, "edit") { cove ->
            val wanted = CoveStore.cleanPath(name)
            cove.copy(documents = cove.documents.filter { CoveStore.cleanPath(it.name) != wanted })
        }
    }

    fun invite(coveId: String, invitee: String) {
        val me = CoveStore.normalizedAccount(account)
        val grantee = CoveStore.normalizedAccount(invitee)
        val cove = cove(coveId) ?: throw CoveActionException.notFound
        if (!cove.unlocked) throw CoveActionException.locked
        if (!cove.accountScoped) throw CoveActionException.notAccountCove
        if (!cove.createdBy(me)) throw CoveActionException.onlyCreatorShares
        if (grantee.isEmpty()) throw CoveActionException.invalidAccount(invitee)
        if (grantee == me || grantee in cove.members) throw CoveActionException.alreadyMember(grantee)
        io.checkAccount(grantee)?.let { result ->
            if (!result.exists) throw CoveActionException.invalidAccount(grantee)
            if (!result.active) throw CoveActionException.inactiveAccount(grantee)
        }
        mutate(coveId, null) { it.copy(invitedAccounts = CoveStore.normalizedAccounts(it.invitedAccounts + grantee)) }
    }

    private fun mutate(coveId: String, action: String?, change: (Cove) -> Cove) {
        val entry = entry(coveId) ?: throw CoveActionException.notFound
        if (!entry.cove.unlocked) throw CoveActionException.locked
        var updated = change(entry.cove)
        if (action != null) {
            updated = CoveStore.appendAccess(
                updated,
                CoveAccessEntry(who = openerKey, name = CoveStore.normalizedAccount(account), ts = clock(), action = action),
            )
        }
        val password = if (updated.accountScoped) null else synchronized(lock) { passwords[coveId] }
        if (!updated.accountScoped && password == null) throw CoveActionException.locked
        val sealed = try {
            if (password != null) CoveStore.sealPassword(updated, password) else CoveStore.sealAccount(updated)
        } catch (error: CoveStoreException) {
            throw CoveActionException(error.message ?: "")
        }
        publish(sealed, entry.servedBytes, password)
    }

    private fun publish(sealed: Cove, previous: ByteArray?, password: String?) {
        val text = CoveStore.envelopeText(sealed)
        val patch = CovePatch.replaceFile(sealed.relPath, previous, text.toByteArray(Charsets.UTF_8))
        if (patch.isEmpty()) return
        busy = true
        changed()
        try {
            io.propose(repo, "cove: ${sealed.slug}", CovePatch.head(account, sealed.slug, clock()), patch, PROPOSAL_DESCRIPTION)
        } catch (error: Exception) {
            throw CoveActionException(error.message ?: error.toString())
        } finally {
            busy = false
        }
        val unlocked = sealed.copy(unlocked = true)
        synchronized(lock) {
            entries = if (entries.any { it.cove.id == sealed.id }) {
                entries.map { if (it.cove.id == sealed.id) it.with(cove = unlocked, pendingReview = true) else it }
            } else {
                entries + CoveEntry(unlocked, previous, true)
            }
            if (password != null) passwords[sealed.id] = password
        }
        changed()
    }

    companion object {
        const val MAX_COVES = 200
        const val MAX_ENVELOPE_BYTES = 6 * 1024 * 1024
        const val PROPOSAL_DESCRIPTION = "An encrypted cove change proposed from ForkMesh on Android."
    }
}
