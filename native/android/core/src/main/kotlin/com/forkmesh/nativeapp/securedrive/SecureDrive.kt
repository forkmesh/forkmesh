package com.forkmesh.nativeapp.securedrive

import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.securedrive.SecureDriveLayout.CHUNK_BYTES
import com.forkmesh.nativeapp.securedrive.SecureDriveLayout.DATA_PAGE
import com.forkmesh.nativeapp.securedrive.SecureDriveLayout.INITIAL_DATA_PAGES
import com.forkmesh.nativeapp.securedrive.SecureDriveLayout.LEASE_PAGE
import com.forkmesh.nativeapp.securedrive.SecureDriveLayout.MAILBOX_SLOTS
import com.forkmesh.nativeapp.securedrive.SecureDriveLayout.PAGE_BYTES
import com.forkmesh.nativeapp.securedrive.SecureDriveLayout.ROOT_A_PAGE
import com.forkmesh.nativeapp.securedrive.SecureDriveLayout.ROOT_B_PAGE
import com.forkmesh.nativeapp.securedrive.SecureDriveLayout.ROOT_PAGES
import com.forkmesh.nativeapp.securedrive.SecureDriveLayout.SLOT_BYTES
import com.forkmesh.nativeapp.securedrive.SecureDriveLayout.UNIT_OVERHEAD
import com.forkmesh.nativeapp.securedrive.SecureDriveLayout.mailboxOffset
import java.io.ByteArrayInputStream
import java.io.InputStream
import java.io.OutputStream
import java.security.MessageDigest
import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import java.util.TreeMap

interface BlobSource {
    val length: Long
    fun open(): InputStream
}

class BytesSource(private val bytes: ByteArray) : BlobSource {
    override val length: Long get() = bytes.size.toLong()
    override fun open(): InputStream = ByteArrayInputStream(bytes)
}

class SecureBuffer : OutputStream() {
    private var data = ByteArray(4096)
    private var size = 0

    override fun write(b: Int) {
        ensure(1)
        data[size++] = b.toByte()
    }

    override fun write(b: ByteArray, off: Int, len: Int) {
        ensure(len)
        System.arraycopy(b, off, data, size, len)
        size += len
    }

    private fun ensure(extra: Int) {
        if (size + extra <= data.size) return
        var capacity = data.size
        while (capacity < size + extra) capacity *= 2
        val next = data.copyOf(capacity)
        data.fill(0)
        data = next
    }

    fun toByteArray(): ByteArray = data.copyOf(size)

    fun wipe() {
        data.fill(0)
        size = 0
    }
}

class Drive(
    private val state: LocalState,
    val id: String,
    private val opener: ContainerOpener = FileContainerOpener,
    var timing: LeaseTiming = LeaseTiming(),
) {
    var inviteLifetimeMs: Long = INVITE_LIFETIME

    private class Txn(val file: ContainerIo) {
        val records = mutableListOf<Pair<String, Map<String, Any?>>>()
        val reserved = HashSet<Long>()
        val freed = mutableListOf<Run>()
        val afterCommit = mutableListOf<() -> Boolean>()
        var renew: (() -> Unit)? = null
        var bothRoots = false
    }

    private class RootCandidate(
        val generation: Long,
        val keyNumber: Int,
        val keyId: String,
        val body: Map<String, Any?>,
        val bodyBytes: ByteArray,
        val signature: ByteArray,
        val hash: String,
    )

    private class Envelope(val body: ByteArray, val signature: ByteArray, val parsed: Map<String, Any?>)

    private class Ring(
        val body: Map<String, Any?>,
        val bodyBytes: ByteArray,
        val signature: ByteArray,
        val epochs: List<EpochKey>,
    )

    var scanned = false
        private set
    private var opened = false
    var joinRequestMissing = false
        private set
    var removedFromDrive = false
        private set
    private var repairKeyring = false
    private var fingerprintBytes: ByteArray? = null
    private var leaseBytes: ByteArray? = null
    private var rootABytes: ByteArray? = null
    private var rootBBytes: ByteArray? = null
    private var rootGeneration = -1L
    private var rootHash = ""
    private var totalPages = 0L
    private var logs = TreeMap<String, List<Run>>()
    private var myLastRecords: List<Any?> = emptyList()
    private var recordList: List<Record> = emptyList()
    private var deviceMap = TreeMap<String, DeviceInfo>()
    private var fileMap = TreeMap<String, FileEntry>()
    private var inviteMap = TreeMap<String, Invite>()
    private var requestList = mutableListOf<JoinRequest>()
    private var issueList = mutableListOf<Issue>()
    private var driveLabel = ""

    val membership: Membership? get() = state.membership(id)

    val records: List<Record> get() = recordList
    val requests: List<JoinRequest> get() = requestList.toList()
    val issues: List<Issue> get() = issueList.toList()
    val containerBytes: Long get() = totalPages * PAGE_BYTES

    fun changedSinceScan(): Boolean = !same(fingerprint(), fingerprintBytes)

    fun joinSafetyNumber(): String {
        val m = membership ?: return ""
        if (m.joinSecret.isEmpty()) return ""
        return SdCrypto.safetyNumber(inviteIdBytes(m.joinSecret), state.publicKeys)
    }

    fun files(): List<FileEntry> =
        fileMap.values.filter { !it.deleted }.sortedWith { a, b -> a.name.compareTo(b.name, ignoreCase = true) }

    fun file(id: String): FileEntry? = fileMap[id]?.takeIf { !it.deleted }

    fun devices(): List<DeviceInfo> = deviceMap.values.toList()

    fun openInvites(): List<Invite> {
        val moment = now()
        return inviteMap.values.filter { it.open(moment) }
    }

    fun deviceName(id: String): String {
        if (id == state.deviceId && id.isNotEmpty()) return state.deviceName + " (this device)"
        val found = deviceMap[id]
        if (found != null && found.name.isNotEmpty()) return found.name
        return if (id.isEmpty()) "A device" else "Device ${id.take(8)}"
    }

    fun deviceSafetyNumber(id: String): String {
        val keys = deviceMap[id]?.keys ?: return ""
        if (!keys.isValid()) return ""
        return SdCrypto.safetyNumber(SdCrypto.utf8("device"), keys)
    }

    fun usedBytes(): Long {
        var pages = DATA_PAGE
        for (runs in logs.values) pages += runPages(runs)
        for (entry in fileMap.values) {
            if (entry.deleted) continue
            val seen = HashSet<Long>()
            for (version in entry.versions) for (run in version.pages) if (seen.add(run.start)) pages += run.count
        }
        return pages * PAGE_BYTES
    }

    fun dataRunsInUse(): List<Run> {
        val runs = mutableListOf<Run>()
        for (log in logs.values) runs += log
        for (entry in fileMap.values) if (!entry.deleted) for (version in entry.versions) runs += version.pages
        return runs
    }

    fun mailboxesInUse(): List<Int> = usedMailboxes().sorted()

    fun activityCsv(): String {
        val sorted = recordList.sortedBy { it.time }
        val format = DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'").withZone(ZoneOffset.UTC)
        val lines = mutableListOf("time,device,activity,record,hash")
        for (record in sorted) {
            lines += listOf(
                format.format(Instant.ofEpochMilli(record.time)),
                csvField(deviceName(record.device)),
                csvField(SecureDriveFormat.describeRecord(record, ::deviceName)),
                (record.seq + 1).toString(),
                record.hash,
            ).joinToString(",")
        }
        return lines.joinToString("\n") + "\n"
    }

    fun describe(record: Record): String = SecureDriveFormat.describeRecord(record, ::deviceName)

    fun issueText(issue: Issue): String = SecureDriveFormat.issueText(issue, ::deviceName)

    private fun mutableMembership(): Membership? = state.membership(id)

    private fun fingerprint(): ByteArray? {
        val m = membership ?: return null
        val file = opener.open(m.container, false) ?: return null
        file.use {
            val digest = MessageDigest.getInstance("SHA-256")
            digest.update(SdCrypto.utf8(it.size().toString()))
            it.readAt(ROOT_A_PAGE * PAGE_BYTES, ((DATA_PAGE - ROOT_A_PAGE) * PAGE_BYTES).toInt())?.let(digest::update)
            return digest.digest()
        }
    }

    fun cancelJoin() {
        val m = membership
        if (m == null || !m.isPending) fail("not_pending")
        opener.open(m.container, true)?.use { container ->
            val requestKey = SdCrypto.hkdf(m.joinSecret, "request")
            if (openUnit(requestKey, requestAad(m.mailbox), container.readAt(mailboxOffset(m.mailbox), SLOT_BYTES.toInt())) != null) {
                container.writeAt(mailboxOffset(m.mailbox), SdCrypto.random(SLOT_BYTES.toInt()))
                container.sync()
            }
            SdCrypto.wipe(requestKey)
        }
        state.forget(id)
        state.save()
    }

    fun scan() {
        val m = membership ?: fail("unknown_drive")
        val print = fingerprint()
        val file = opener.open(m.container, false) ?: fail("container_unavailable")
        file.use { load(it) }
        fingerprintBytes = print
        val after = membership
        if (!opened && after != null && after.isMember && !removedFromDrive) {
            opened = true
            note("drive_opened", emptyMap())
        }
    }

    private fun loadPending(file: ContainerIo) {
        val m = mutableMembership() ?: fail("unknown_drive")
        val slot = file.readAt(mailboxOffset(m.mailbox), SLOT_BYTES.toInt())
        val approveKey = SdCrypto.hkdf(m.joinSecret, "approve")
        val requestKey = SdCrypto.hkdf(m.joinSecret, "request")
        val inviteId = SdCrypto.toB64(inviteIdBytes(m.joinSecret))
        var adopted = false
        val plain = slot?.let { SdCrypto.openFromDevice(state.keys, mailboxLabel(m.mailbox), it) }
        if (plain != null) {
            val outer = J.parse(stripPadding(plain))
            val envelope = outer?.let(::openEnvelope)
            val mac = outer?.let { SdCrypto.fromB64(it["m"]) }
            if (envelope != null && mac != null) {
                val body = envelope.parsed
                val issuer = PublicKeys.fromJson(body["issuerKeys"])
                val rootEd = SdCrypto.fromB64(J.str(J.obj(body["root"])["ed25519"]))
                if (SdCrypto.constantTimeEqual(mac, SdCrypto.hmac(approveKey, envelope.body)) &&
                    J.str(body["device"]) == state.deviceId &&
                    J.str(body["invite"]) == inviteId &&
                    issuer != null &&
                    SdCrypto.verify(issuer.ed25519, keyringMessage(envelope.body), envelope.signature) &&
                    rootEd != null && rootEd.size == 32
                ) {
                    val epochs = SecureDriveFormat.epochsFrom(J.arr(body["epochs"]))
                    if (epochs.isNotEmpty()) {
                        m.driveId = J.str(body["drive"])
                        m.rootDevice = J.str(J.obj(body["root"])["device"])
                        m.rootEd25519 = rootEd
                        m.epochs = epochs.toMutableList()
                        SdCrypto.wipe(m.joinSecret)
                        m.joinSecret = ByteArray(0)
                        m.joinedAt = now()
                        adopted = true
                    }
                }
            }
            SdCrypto.wipe(plain)
        }
        SdCrypto.wipe(approveKey)
        if (adopted) {
            SdCrypto.wipe(requestKey)
            state.save()
            load(file)
            return
        }
        joinRequestMissing = openUnit(requestKey, requestAad(m.mailbox), slot) == null
        SdCrypto.wipe(requestKey)
        scanned = true
    }

    private fun load(file: ContainerIo) {
        val m = mutableMembership() ?: fail("unknown_drive")
        issueList.clear()
        requestList.clear()
        joinRequestMissing = false
        removedFromDrive = false
        totalPages = file.pages()
        if (totalPages < DATA_PAGE) fail("not_a_drive")
        if (m.isPending) {
            loadPending(file)
            return
        }
        if (!m.isMember) fail("unknown_drive")
        val stateBefore = Json.stringify(m.toJson())
        val me = state.deviceId

        var ring: Ring? = null
        if (m.mailbox >= 0) {
            val slot = file.readAt(mailboxOffset(m.mailbox), SLOT_BYTES.toInt())
            val plain = slot?.let { SdCrypto.openFromDevice(state.keys, mailboxLabel(m.mailbox), it) }
            if (plain != null) {
                val envelope = J.parse(stripPadding(plain))?.let(::openEnvelope)
                if (envelope != null && J.str(envelope.parsed["drive"]) == m.driveId && J.str(envelope.parsed["device"]) == me) {
                    ring = Ring(envelope.parsed, envelope.body, envelope.signature, SecureDriveFormat.epochsFrom(J.arr(envelope.parsed["epochs"])))
                }
                SdCrypto.wipe(plain)
            }
        }

        rootABytes = file.readAt(ROOT_A_PAGE * PAGE_BYTES, (ROOT_PAGES * PAGE_BYTES).toInt())
        rootBBytes = file.readAt(ROOT_B_PAGE * PAGE_BYTES, (ROOT_PAGES * PAGE_BYTES).toInt())

        var chains = TreeMap<String, TreeMap<Long, Record>>()
        val accepted = mutableListOf<Record>()
        val usedKeys = HashSet<String>()
        var unreadable = 0
        var chosen: RootCandidate? = null
        var restored = false

        fun attempt(keys: List<EpochKey>) {
            issueList.clear()
            chains = TreeMap()
            accepted.clear()
            usedKeys.clear()
            unreadable = 0
            chosen = null
            for (slotA in listOf(true, false)) {
                val bytes = if (slotA) rootABytes else rootBBytes
                for (epoch in keys) {
                    val plain = openUnit(SdCrypto.hkdf(epoch.key, "root"), rootAad(slotA), bytes) ?: continue
                    val envelope = J.parse(stripPadding(plain))?.let(::openEnvelope)
                    if (envelope != null && J.str(envelope.parsed["drive"]) == m.driveId && J.int(envelope.parsed["v"], 0) == 2) {
                        val candidate = RootCandidate(
                            J.long(envelope.parsed["gen"], -1),
                            epoch.number,
                            epoch.id,
                            envelope.parsed,
                            envelope.body,
                            envelope.signature,
                            SdCrypto.toB64(SdCrypto.sha256(rootMessage(envelope.body), envelope.signature)),
                        )
                        val writer = J.str(candidate.body["writer"])
                        if (m.removedDevices.contains(writer)) {
                            addIssue("untrusted_root", writer, "")
                            break
                        }
                        val best = chosen
                        if (best == null || candidate.keyNumber > best.keyNumber ||
                            (candidate.keyNumber == best.keyNumber && candidate.generation > best.generation)
                        ) {
                            chosen = candidate
                        }
                    }
                    break
                }
            }
            restored = false
            val cachedBytes = if (m.cachedRoot.isNotEmpty()) SdCrypto.fromB64(m.cachedRoot) else null
            val cachedEnvelope = cachedBytes?.let { J.parse(it) }?.let(::openEnvelope)
            if (cachedEnvelope != null && J.str(cachedEnvelope.parsed["drive"]) == m.driveId) {
                val cached = RootCandidate(
                    J.long(cachedEnvelope.parsed["gen"], -1),
                    -1,
                    "",
                    cachedEnvelope.parsed,
                    cachedEnvelope.body,
                    cachedEnvelope.signature,
                    SdCrypto.toB64(SdCrypto.sha256(rootMessage(cachedEnvelope.body), cachedEnvelope.signature)),
                )
                val best = chosen
                if (cached.hash == m.rootHash && (best == null || cached.generation > best.generation)) {
                    chosen = cached
                    restored = true
                    addIssue("index_restored", "", "")
                }
            }
            logs = TreeMap()
            myLastRecords = emptyList()
            val picked = chosen ?: return
            usedKeys.add(picked.keyId)
            for ((device, value) in J.obj(picked.body["logs"])) {
                val runs = SecureDriveFormat.runsFromJson(J.arr(value))
                logs[device] = runs
                for ((r, run) in runs.withIndex()) {
                    if (run.start + run.count > totalPages) {
                        unreadable++
                        continue
                    }
                    val bytes = file.readAt(run.start * PAGE_BYTES, (run.count * PAGE_BYTES).toInt())
                    var payload: Map<String, Any?>? = null
                    for (epoch in keys) {
                        val plain = openUnit(SdCrypto.hkdf(epoch.key, "log"), logAad(m.driveId, run.start), bytes) ?: continue
                        payload = J.parse(stripPadding(plain))
                        usedKeys.add(epoch.id)
                        break
                    }
                    if (payload == null || J.str(payload["device"]) != device) {
                        unreadable++
                        continue
                    }
                    val stored = J.arr(payload["records"])
                    if (device == me && r == runs.size - 1) myLastRecords = stored
                    for (value2 in stored) {
                        val storedMap = J.obj(value2)
                        val envelope = openEnvelope(storedMap) ?: continue
                        val body = envelope.parsed
                        val record = Record(
                            device = J.str(body["d"]),
                            seq = J.long(body["s"], -1),
                            lamport = J.long(body["l"]),
                            time = J.long(body["t"]),
                            action = J.str(body["a"]),
                            data = J.obj(body["x"]),
                            prev = J.str(body["p"]),
                            hash = SdCrypto.toB64(SdCrypto.sha256(recordMessage(m.driveId, envelope.body), envelope.signature)),
                            witness = J.obj(body["w"]),
                            stored = storedMap,
                        )
                        if (record.device != device || record.seq < 0) continue
                        val chain = chains.getOrPut(device) { TreeMap() }
                        val found = chain[record.seq]
                        if (found != null) {
                            if (found.hash != record.hash) addIssue("log_forked", device, "")
                            continue
                        }
                        chain[record.seq] = record
                    }
                }
            }

            val trusted = TreeMap<String, ByteArray>()
            trusted[m.rootDevice] = m.rootEd25519
            val verified = HashSet<String>()
            var changed = true
            while (changed) {
                changed = false
                for (device in trusted.keys.toList()) {
                    if (!verified.add(device)) continue
                    changed = true
                    val ed = trusted[device] ?: continue
                    var expected = 0L
                    var lastHash = ""
                    val chain = chains[device] ?: TreeMap()
                    for (record in chain.values) {
                        val envelope = openEnvelope(record.stored)
                        if (envelope == null || !SdCrypto.verify(ed, recordMessage(m.driveId, envelope.body), envelope.signature)) {
                            addIssue("bad_signature", device, "")
                            continue
                        }
                        if (record.seq != expected) {
                            addIssue(
                                "records_missing",
                                device,
                                if (record.seq - expected == 1L) "record ${expected + 1}" else "records ${expected + 1} to ${record.seq}",
                            )
                        } else if (record.prev != lastHash) {
                            addIssue("chain_broken", device, "record ${record.seq + 1}")
                        }
                        expected = record.seq + 1
                        lastHash = record.hash
                        accepted.add(record)
                        if (record.action == "device_added") {
                            val keys2 = PublicKeys.fromJson(record.data["keys"])
                            val added = J.str(record.data["device"])
                            if (keys2 != null && !trusted.containsKey(added)) trusted[added] = keys2.ed25519
                        }
                    }
                }
            }
            for (device in chains.keys) if (!trusted.containsKey(device)) addIssue("unknown_device", device, "")
            fold(accepted)
        }

        val keys = m.epochs.toMutableList()
        val ringKeys = HashSet<String>()
        ring?.let {
            for (epoch in it.epochs) {
                if (m.epoch(epoch.id) == null) {
                    keys.add(epoch)
                    ringKeys.add(epoch.id)
                }
            }
        }
        keys.sortByDescending { it.number }
        attempt(keys)

        var ringValid = false
        ring?.let {
            val issuer = J.str(it.body["issuer"])
            val device = deviceMap[issuer]
            val issuerKeys = device?.keys
            ringValid = device != null && !device.removed && issuerKeys != null &&
                SdCrypto.verify(issuerKeys.ed25519, keyringMessage(it.bodyBytes), it.signature)
            if (!ringValid && ringKeys.isNotEmpty()) {
                val usedRingKey = ringKeys.any { k -> usedKeys.contains(k) }
                keys.removeAll { e -> ringKeys.contains(e.id) }
                if (usedRingKey) attempt(keys)
                addIssue("untrusted_keyring", issuer, "")
            }
        }
        if (ringValid) {
            m.keyringSeen = true
            for (epoch in ring!!.epochs) if (m.epoch(epoch.id) == null) m.epochs.add(epoch)
        }
        val slotEmpty = ring == null && m.keyringSeen
        val keyringGone = slotEmpty && (chosen == null || restored)
        repairKeyring = slotEmpty && !keyringGone
        if (keyringGone) addIssue("keyring_missing", me, "") else if (repairKeyring) addIssue("keyring_restored", me, "")

        val picked = chosen
        if (picked == null) {
            val fresh = m.rootGeneration < 0 && m.rootDevice == me && m.lastSeq < 0
            if (!fresh) fail("drive_unreadable")
            rootGeneration = -1
            rootHash = ""
        } else {
            rootGeneration = picked.generation
            rootHash = picked.hash
            val writer = J.str(picked.body["writer"])
            val device = deviceMap[writer]
            val writerKeys = device?.keys
            if (device == null || device.removed || writerKeys == null ||
                !SdCrypto.verify(writerKeys.ed25519, rootMessage(picked.bodyBytes), picked.signature)
            ) {
                addIssue("untrusted_root", writer, "")
            }
            if (J.long(picked.body["pages"]) > totalPages) addIssue("drive_truncated", "", "")
            if (picked.generation < m.rootGeneration) {
                addIssue("rolled_back", "", "index ${picked.generation}, this device saw ${m.rootGeneration}")
            } else if (picked.generation == m.rootGeneration && picked.hash != m.rootHash) {
                addIssue("log_forked", "", "")
            }
            if (picked.generation > m.rootGeneration) {
                m.rootGeneration = picked.generation
                m.rootHash = picked.hash
                m.cachedRoot = SdCrypto.toB64(J.compact(envelope(picked.bodyBytes, picked.signature)))
            }
        }
        if (unreadable > 0) addIssue("unreadable_objects", "", unreadable.toString())

        checkWitnesses(chains)
        val own = chains[me] ?: TreeMap()
        val ownMax = if (own.isEmpty()) -1L else own.lastKey()
        if (ownMax < m.lastSeq) addIssue("own_records_missing", me, "")
        if (ownMax > m.lastSeq && m.lastSeq >= 0) addIssue("log_forked", me, "")
        if (m.lastSeq < 0 && ownMax >= 0) {
            m.lastSeq = ownMax
            m.lastHash = own.lastEntry().value.hash
        }
        for (record in recordList) m.lamport = maxOf(m.lamport, record.lamport)
        for ((device, chain) in chains) {
            if (!deviceMap.containsKey(device) || chain.isEmpty()) continue
            val last = chain.lastEntry().value
            val head = m.heads[device]
            if (head == null || last.seq > head.seq) m.heads[device] = Head(last.seq, last.hash)
        }

        val moment = now()
        for (invite in inviteMap.values) {
            if (!invite.open(moment) || invite.mailbox < 0) continue
            val plain = openUnit(invite.requestKey, requestAad(invite.mailbox), file.readAt(mailboxOffset(invite.mailbox), SLOT_BYTES.toInt())) ?: continue
            val envelope = J.parse(stripPadding(plain))?.let(::openEnvelope) ?: continue
            val body = envelope.parsed
            val requestKeys = PublicKeys.fromJson(body["keys"]) ?: continue
            if (!SdCrypto.verify(requestKeys.ed25519, requestMessage(envelope.body), envelope.signature)) continue
            val inviteId = J.str(body["invite"])
            val device = J.str(body["device"])
            if (inviteId != invite.id || deviceMap.containsKey(device)) continue
            val inviteBytes = SdCrypto.fromB64(invite.id) ?: ByteArray(0)
            requestList.add(
                JoinRequest(
                    mailbox = invite.mailbox,
                    inviteId = inviteId,
                    device = device,
                    name = J.str(body["name"]).take(64),
                    keys = requestKeys,
                    time = J.long(body["time"]),
                    safety = SdCrypto.safetyNumber(inviteBytes, requestKeys),
                ),
            )
        }

        for (entry in fileMap.values) {
            val current = entry.current
            if (entry.deleted || current == null) continue
            if (current.pages.any { it.start + it.count > totalPages }) addIssue("missing_content", "", entry.name)
        }

        if (driveLabel.isNotEmpty()) m.label = driveLabel
        val self = deviceMap[me]
        removedFromDrive = (self != null && self.removed) || keyringGone
        for (device in deviceMap.values) {
            if (device.removed && !m.removedDevices.contains(device.id)) m.removedDevices.add(device.id)
        }
        if (Json.stringify(m.toJson()) != stateBefore) state.save()
        scanned = true
    }

    private fun addIssue(code: String, device: String, detail: String) {
        val perFile = code == "missing_content" || code == "content_damaged"
        for (issue in issueList) {
            if (issue.code == code && issue.device == device && (!perFile || issue.detail == detail)) return
        }
        issueList.add(Issue(code, device, detail))
    }

    private fun checkWitnesses(chains: Map<String, TreeMap<Long, Record>>) {
        val m = membership ?: return
        for (record in recordList) {
            for ((device, value) in record.witness) {
                val pair = J.arr(value)
                val seq = J.long(pair.getOrNull(0), -1)
                val hash = J.str(pair.getOrNull(1))
                if (seq < 0 || device == state.deviceId) continue
                val found = chains[device]?.get(seq)
                if (found == null) {
                    addIssue("records_missing", device, "${deviceName(record.device)} saw record ${seq + 1}")
                } else if (found.hash != hash) {
                    addIssue("log_forked", device, "")
                }
            }
        }
        for ((device, head) in m.heads) {
            if (device == state.deviceId || head.seq < 0) continue
            val found = chains[device]?.get(head.seq)
            if (found == null) {
                addIssue("rolled_back", device, "this device saw record ${head.seq + 1}")
            } else if (found.hash != head.hash) {
                addIssue("log_forked", device, "")
            }
        }
    }

    private fun fold(input: List<Record>) {
        val sorted = input.sortedWith(compareBy<Record>({ it.lamport }, { it.device }, { it.seq }))
        val m = membership ?: return
        val folded = mutableListOf<Record>()
        driveLabel = ""
        deviceMap = TreeMap()
        fileMap = TreeMap()
        inviteMap = TreeMap()
        val cutoff = HashMap<String, Long>()
        for (r in sorted) {
            if (r.action != "device_removed") continue
            val target = J.str(r.data["device"])
            val upTo = J.long(r.data["upTo"], -1)
            cutoff[target] = cutoff[target]?.let { minOf(it, upTo) } ?: upTo
        }
        for (r in sorted) {
            val x = r.data
            val limit = cutoff[r.device]
            if (limit != null && r.seq > limit) {
                addIssue("after_removal", r.device, "")
                continue
            }
            if (r.action == "drive_created") {
                if (r.device != m.rootDevice || r.seq != 0L || deviceMap.isNotEmpty()) continue
                val device = J.obj(x["device"])
                deviceMap[r.device] = DeviceInfo(
                    id = r.device,
                    name = J.str(device["name"]).take(64),
                    keys = PublicKeys.fromJson(device["keys"]),
                    addedBy = r.device,
                    addedAt = r.time,
                    mailbox = J.int(x["mailbox"], -1),
                    root = true,
                )
                driveLabel = J.str(x["label"]).take(80)
                folded.add(r)
                continue
            }
            if (!deviceMap.containsKey(r.device)) continue
            folded.add(r)
            val fileId = J.str(x["file"])
            fun version() = FileVersion(
                blob = J.str(x["blob"]),
                key = SdCrypto.fromB64(J.str(x["key"])) ?: ByteArray(0),
                pages = SecureDriveFormat.runsFromJson(J.arr(x["pages"])),
                size = J.long(x["size"]),
                sha256 = J.str(x["sha256"]),
                time = r.time,
                device = r.device,
            )
            when (r.action) {
                "device_added" -> {
                    val added = J.str(x["device"])
                    if (added.isEmpty() || deviceMap.containsKey(added)) continue
                    deviceMap[added] = DeviceInfo(
                        id = added,
                        name = J.str(x["name"]).take(64),
                        keys = PublicKeys.fromJson(x["keys"]),
                        addedBy = r.device,
                        addedAt = r.time,
                        mailbox = J.int(x["mailbox"], -1),
                    )
                }
                "device_removed" -> {
                    val target = deviceMap[J.str(x["device"])]
                    if (target != null && !target.removed) {
                        target.removed = true
                        target.removedBy = r.device
                        target.removedAt = r.time
                        target.removedSeq = J.long(x["upTo"], -1)
                    }
                }
                "invite_created" -> {
                    val invite = Invite(
                        id = J.str(x["invite"]),
                        requestKey = SdCrypto.fromB64(J.str(x["requestKey"])) ?: ByteArray(0),
                        approveKey = SdCrypto.fromB64(J.str(x["approveKey"])) ?: ByteArray(0),
                        mailbox = J.int(x["mailbox"], -1),
                        created = r.time,
                        expires = J.long(x["expires"]),
                        createdBy = r.device,
                    )
                    if (invite.id.isNotEmpty() && !inviteMap.containsKey(invite.id)) inviteMap[invite.id] = invite
                }
                "invite_used", "invite_revoked" -> {
                    val invite = inviteMap[J.str(x["invite"])]
                    if (invite != null) {
                        if (r.action == "invite_used") invite.used = true else invite.revoked = true
                    }
                }
                "file_created" -> {
                    if (fileId.isEmpty() || fileMap.containsKey(fileId)) continue
                    val entry = FileEntry(fileId, J.str(x["name"]), J.str(x["kind"]))
                    entry.versions.add(version())
                    entry.created = r.time
                    entry.modified = r.time
                    entry.createdBy = r.device
                    entry.modifiedBy = r.device
                    fileMap[fileId] = entry
                }
                "file_saved", "file_restored" -> {
                    val entry = fileMap[fileId]
                    if (entry == null || entry.deleted) continue
                    val current = entry.current
                    if (r.action == "file_saved" && current != null && J.str(x["base"]) != current.blob) entry.conflicts++
                    entry.versions.add(version())
                    entry.modified = r.time
                    entry.modifiedBy = r.device
                }
                "file_renamed" -> {
                    val entry = fileMap[fileId]
                    if (entry != null && !entry.deleted) entry.name = J.str(x["name"])
                }
                "file_deleted" -> fileMap[fileId]?.deleted = true
                "file_rekeyed" -> {
                    val entry = fileMap[fileId] ?: continue
                    for (value in J.arr(x["versions"])) {
                        val change = J.obj(value)
                        val old = J.str(change["old"])
                        for (v in entry.versions) {
                            if (v.blob != old) continue
                            v.blob = J.str(change["blob"])
                            v.key = SdCrypto.fromB64(J.str(change["key"])) ?: ByteArray(0)
                            v.pages = SecureDriveFormat.runsFromJson(J.arr(change["pages"]))
                        }
                    }
                }
                "file_opened" -> {
                    val entry = fileMap[fileId]
                    if (entry != null) {
                        entry.opens++
                        entry.lastOpened = r.time
                        entry.lastOpenedBy = r.device
                    }
                }
            }
        }
        recordList = folded
    }

    private fun usedMailboxes(): Set<Int> {
        val used = HashSet<Int>()
        for (device in deviceMap.values) if (!device.removed && device.mailbox >= 0) used.add(device.mailbox)
        val moment = now()
        for (invite in inviteMap.values) if (invite.open(moment) && invite.mailbox >= 0) used.add(invite.mailbox)
        return used
    }

    private fun freePages(txn: Txn): List<Long> {
        val total = txn.file.pages()
        val used = BooleanArray(maxOf(total, 0L).toInt())
        for (page in 0 until minOf(total, DATA_PAGE)) used[page.toInt()] = true
        fun mark(runs: List<Run>) {
            for (run in runs) for (i in 0 until run.count) if (run.start + i < total) used[(run.start + i).toInt()] = true
        }
        for (runs in logs.values) mark(runs)
        for (entry in fileMap.values) if (!entry.deleted) for (version in entry.versions) mark(version.pages)
        for (page in txn.reserved) if (page < total) used[page.toInt()] = true
        val free = mutableListOf<Long>()
        for (page in DATA_PAGE until total) if (!used[page.toInt()]) free.add(page)
        return free
    }

    private fun allocate(txn: Txn, count: Long, contiguous: Boolean): List<Run> {
        var free = freePages(txn)
        fun findRun(): Long {
            var length = 0L
            for (i in free.indices) {
                length = if (i > 0 && free[i] == free[i - 1] + 1) length + 1 else 1
                if (length >= count) return free[i] - count + 1
            }
            return -1
        }
        var start = if (contiguous) findRun() else 0L
        if ((contiguous && start < 0) || (!contiguous && free.size < count)) {
            val total = txn.file.pages()
            val shortfall = if (contiguous) count else count - free.size
            val grow = maxOf(shortfall, INITIAL_DATA_PAGES, total / 8)
            if (!txn.file.appendRandom(grow)) fail("drive_write_failed")
            txn.renew?.invoke()
            free = freePages(txn)
            start = if (contiguous) findRun() else 0L
            if ((contiguous && start < 0) || (!contiguous && free.size < count)) fail("drive_full")
        }
        val taken = if (contiguous) (0 until count).map { start + it } else free.subList(0, count.toInt())
        val runs = mutableListOf<Run>()
        for (page in taken) {
            txn.reserved.add(page)
            val last = runs.lastOrNull()
            if (last != null && last.start + last.count == page) runs[runs.size - 1] = Run(last.start, last.count + 1)
            else runs.add(Run(page, 1))
        }
        return runs
    }

    private fun leaseUnit(epoch: EpochKey): ByteArray? = sealUnit(
        SdCrypto.hkdf(epoch.key, "lease"),
        SdCrypto.label("lease"),
        J.compact(linkedMapOf("device" to state.deviceId, "token" to SdCrypto.randomId())),
        PAGE_BYTES,
    )

    private fun acquireLease(file: ContainerIo) {
        val m = membership ?: fail("unknown_drive")
        val epoch = m.currentEpoch() ?: fail("no_drive_key")
        var observed: ByteArray? = null
        var seenAt = 0L
        val started = System.nanoTime()
        while (true) {
            val bytes = file.readAt(LEASE_PAGE * PAGE_BYTES, PAGE_BYTES.toInt())
            var holder = ""
            for (key in m.epochs) {
                val plain = openUnit(SdCrypto.hkdf(key.key, "lease"), SdCrypto.label("lease"), bytes) ?: continue
                val lease = J.parse(stripPadding(plain)) ?: continue
                holder = J.str(lease["device"])
                break
            }
            val holderInfo = deviceMap[holder]
            var free = holder.isEmpty() || holder == state.deviceId || (holderInfo != null && holderInfo.removed)
            if (!free) {
                if (observed == null || !same(bytes, observed)) {
                    observed = bytes ?: ByteArray(0)
                    seenAt = System.nanoTime()
                } else if ((System.nanoTime() - seenAt) / 1_000_000 >= timing.staleMs) {
                    free = true
                }
            }
            if (free) {
                val mine = leaseUnit(epoch)
                if (mine == null || !file.writeAt(LEASE_PAGE * PAGE_BYTES, mine) || !file.sync()) fail("drive_write_failed")
                if (timing.settleMs > 0) Thread.sleep(timing.settleMs)
                if (same(file.readAt(LEASE_PAGE * PAGE_BYTES, PAGE_BYTES.toInt()), mine)) {
                    leaseBytes = mine
                    return
                }
                observed = null
            }
            if ((System.nanoTime() - started) / 1_000_000 > timing.staleMs * 2 + 10000) fail("drive_busy")
            Thread.sleep(maxOf(1L, timing.pollMs))
        }
    }

    private fun renewLease(file: ContainerIo): Boolean {
        val epoch = membership?.currentEpoch() ?: return false
        if (!same(file.readAt(LEASE_PAGE * PAGE_BYTES, PAGE_BYTES.toInt()), leaseBytes)) return false
        val mine = leaseUnit(epoch) ?: return false
        if (!file.writeAt(LEASE_PAGE * PAGE_BYTES, mine) || !file.sync()) return false
        leaseBytes = mine
        return true
    }

    private fun releaseLease(file: ContainerIo) {
        val held = leaseBytes
        if (held != null && same(file.readAt(LEASE_PAGE * PAGE_BYTES, PAGE_BYTES.toInt()), held)) {
            file.writeAt(LEASE_PAGE * PAGE_BYTES, SdCrypto.random(PAGE_BYTES.toInt()))
            file.sync()
        }
        leaseBytes = null
    }

    private fun transact(body: (Txn) -> Unit) {
        val m0 = mutableMembership() ?: fail("unknown_drive")
        if (!m0.isMember) fail("unknown_drive")
        val file = opener.open(m0.container, true) ?: fail("container_unavailable")
        file.use {
            load(file)
            if (removedFromDrive) fail("device_removed")
            acquireLease(file)
            var before: Membership? = null
            try {
                load(file)
                val m = mutableMembership() ?: fail("unknown_drive")
                before = m.copy()
                if (removedFromDrive) fail("device_removed")
                val txn = Txn(file)
                var lastRenew = System.nanoTime()
                txn.renew = {
                    if ((System.nanoTime() - lastRenew) / 1_000_000 > maxOf(1000L, timing.staleMs / 8)) {
                        renewLease(file)
                        lastRenew = System.nanoTime()
                    }
                }
                body(txn)
                commit(txn)
            } catch (error: Throwable) {
                val snapshot = before
                if (snapshot != null) mutableMembership()?.assign(snapshot)
                releaseLease(file)
                throw if (error is SecureDriveException || error is Error) error else SecureDriveException("drive_write_failed")
            }
            releaseLease(file)
            try {
                load(file)
            } catch (_: SecureDriveException) {
            }
        }
    }

    private fun commit(txn: Txn) {
        val m = mutableMembership() ?: fail("unknown_drive")
        val epoch = m.currentEpoch() ?: fail("no_drive_key")
        val me = state.deviceId
        val mine = (logs[me] ?: emptyList()).toMutableList()
        var reuse = myLastRecords.isNotEmpty() && mine.isNotEmpty()
        val added = mutableListOf<Any?>()
        for ((action, data) in txn.records) {
            val witness = linkedMapOf<String, Any?>()
            for ((device, head) in m.heads) if (device != me && head.seq >= 0) witness[device] = listOf(head.seq, head.hash)
            val bodyBytes = J.compact(
                linkedMapOf(
                    "d" to me,
                    "s" to m.lastSeq + 1,
                    "l" to m.lamport + 1,
                    "t" to now(),
                    "a" to action,
                    "x" to data,
                    "p" to m.lastHash,
                    "w" to witness,
                ),
            )
            val message = recordMessage(m.driveId, bodyBytes)
            val signature = SdCrypto.sign(state.keys.ed25519, message)
            val hash = SdCrypto.toB64(SdCrypto.sha256(message, signature))
            m.lastSeq += 1
            m.lastHash = hash
            m.lamport += 1
            m.heads[me] = Head(m.lastSeq, hash)
            added.add(envelope(bodyBytes, signature))
        }
        fun payloadFor(records: List<Any?>) = J.compact(linkedMapOf("device" to me, "records" to records))
        var payload = payloadFor(if (reuse) myLastRecords + added else added)
        if (reuse && (payload.size + UNIT_OVERHEAD + PAGE_BYTES - 1) / PAGE_BYTES > LOG_UNIT_LIMIT_PAGES) {
            reuse = false
            payload = payloadFor(added)
        }
        val pages = maxOf(1L, (payload.size + UNIT_OVERHEAD + PAGE_BYTES - 1) / PAGE_BYTES)
        val unit = allocate(txn, pages, true).first()
        val sealed = sealUnit(SdCrypto.hkdf(epoch.key, "log"), logAad(m.driveId, unit.start), payload, unit.count * PAGE_BYTES)
        if (sealed == null || !txn.file.writeAt(unit.start * PAGE_BYTES, sealed)) fail("drive_write_failed")
        if (reuse) txn.freed.add(mine.removeAt(mine.size - 1))
        mine.add(unit)
        val nextLogs = TreeMap(logs)
        nextLogs[me] = mine
        val logsJson = linkedMapOf<String, Any?>()
        for ((device, runs) in nextLogs) logsJson[device] = SecureDriveFormat.runsToJson(runs)
        val generation = rootGeneration + 1
        val rootBody = J.compact(
            linkedMapOf(
                "v" to 2L,
                "drive" to m.driveId,
                "gen" to generation,
                "pages" to txn.file.pages(),
                "logs" to logsJson,
                "writer" to me,
                "time" to now(),
                "prev" to rootHash,
            ),
        )
        val rootSignature = SdCrypto.sign(state.keys.ed25519, rootMessage(rootBody))
        val slotA = generation % 2 == 0L
        val rootUnit = sealUnit(
            SdCrypto.hkdf(epoch.key, "root"),
            rootAad(slotA),
            J.compact(envelope(rootBody, rootSignature)),
            ROOT_PAGES * PAGE_BYTES,
        ) ?: fail("drive_index_full")
        if (!txn.file.sync()) fail("drive_write_failed")
        if (!same(txn.file.readAt(LEASE_PAGE * PAGE_BYTES, PAGE_BYTES.toInt()), leaseBytes)) fail("drive_busy")
        if (!same(txn.file.readAt(ROOT_A_PAGE * PAGE_BYTES, (ROOT_PAGES * PAGE_BYTES).toInt()), rootABytes) ||
            !same(txn.file.readAt(ROOT_B_PAGE * PAGE_BYTES, (ROOT_PAGES * PAGE_BYTES).toInt()), rootBBytes)
        ) {
            fail("drive_changed")
        }
        val rootOffset = (if (slotA) ROOT_A_PAGE else ROOT_B_PAGE) * PAGE_BYTES
        if (!txn.file.writeAt(rootOffset, rootUnit) || !txn.file.sync() ||
            !same(txn.file.readAt(rootOffset, (ROOT_PAGES * PAGE_BYTES).toInt()), rootUnit)
        ) {
            fail("drive_write_failed")
        }
        if (txn.bothRoots) {
            val twin = sealUnit(
                SdCrypto.hkdf(epoch.key, "root"),
                rootAad(!slotA),
                J.compact(envelope(rootBody, rootSignature)),
                ROOT_PAGES * PAGE_BYTES,
            )
            val twinOffset = (if (slotA) ROOT_B_PAGE else ROOT_A_PAGE) * PAGE_BYTES
            if (twin == null || !txn.file.writeAt(twinOffset, twin) || !txn.file.sync()) fail("drive_write_failed")
        }
        if (repairKeyring && m.mailbox >= 0) {
            val unit = try {
                keyringUnit(me, state.publicKeys, m.mailbox, null, "")
            } catch (_: SecureDriveException) {
                null
            }
            val slot = m.mailbox
            if (unit != null) txn.afterCommit.add { txn.file.writeAt(mailboxOffset(slot), unit) && txn.file.sync() }
        }
        m.rootGeneration = generation
        m.rootHash = SdCrypto.toB64(SdCrypto.sha256(rootMessage(rootBody), rootSignature))
        m.cachedRoot = SdCrypto.toB64(J.compact(envelope(rootBody, rootSignature)))
        var after = true
        for (action in txn.afterCommit) after = action() && after
        for (run in txn.freed) txn.file.writeAt(run.start * PAGE_BYTES, SdCrypto.random((run.count * PAGE_BYTES).toInt()))
        txn.file.sync()
        state.save()
        if (!after) fail("drive_write_failed")
    }

    fun note(action: String, data: Map<String, Any?>) {
        transact { txn -> txn.records.add(action to data) }
    }

    class NewInvite(val code: String, val inviteId: String)

    fun createInvite(): NewInvite {
        var text = ""
        var inviteId = ""
        transact { txn ->
            val used = usedMailboxes()
            if (used.size >= MAILBOX_SLOTS) fail("drive_full")
            var secret = ByteArray(0)
            var mailbox = -1
            for (tries in 0 until 4096) {
                val invite = SdCrypto.newInviteCode()
                text = invite.code
                secret = invite.secret
                mailbox = SecureDriveFormat.mailboxForInvite(secret)
                if (!used.contains(mailbox)) break
                mailbox = -1
            }
            if (mailbox < 0) fail("drive_full")
            inviteId = SdCrypto.toB64(inviteIdBytes(secret))
            val requestKey = SdCrypto.hkdf(secret, "request")
            val approveKey = SdCrypto.hkdf(secret, "approve")
            SdCrypto.wipe(secret)
            txn.records.add(
                "invite_created" to linkedMapOf(
                    "invite" to inviteId,
                    "requestKey" to SdCrypto.toB64(requestKey),
                    "approveKey" to SdCrypto.toB64(approveKey),
                    "mailbox" to mailbox.toLong(),
                    "expires" to now() + inviteLifetimeMs,
                ),
            )
            val file = txn.file
            val slot = mailbox
            txn.afterCommit.add { file.writeAt(mailboxOffset(slot), SdCrypto.random(SLOT_BYTES.toInt())) && file.sync() }
        }
        return NewInvite(text, inviteId)
    }

    fun revokeInvite(inviteId: String) {
        val invite = inviteMap[inviteId] ?: fail("unknown_invite")
        val mailbox = invite.mailbox
        transact { txn ->
            txn.records.add("invite_revoked" to linkedMapOf("invite" to inviteId))
            val file = txn.file
            if (mailbox >= 0 && !usedMailboxes().contains(mailbox)) {
                txn.afterCommit.add { file.writeAt(mailboxOffset(mailbox), SdCrypto.random(SLOT_BYTES.toInt())) && file.sync() }
            }
        }
    }

    private fun keyringUnit(recipientId: String, recipient: PublicKeys, mailbox: Int, approveKey: ByteArray?, inviteId: String): ByteArray {
        val m = membership ?: fail("unknown_drive")
        val body = linkedMapOf<String, Any?>(
            "suite" to SdCrypto.SUITE,
            "drive" to m.driveId,
            "device" to recipientId,
            "mailbox" to mailbox.toLong(),
            "epochs" to SecureDriveFormat.epochsJson(m.epochs),
            "root" to linkedMapOf("device" to m.rootDevice, "ed25519" to SdCrypto.toB64(m.rootEd25519)),
            "issuer" to state.deviceId,
            "issuerKeys" to state.publicKeys.toJson(),
            "time" to now(),
        )
        if (inviteId.isNotEmpty()) body["invite"] = inviteId
        val bodyBytes = J.compact(body)
        val signature = SdCrypto.sign(state.keys.ed25519, keyringMessage(bodyBytes))
        val outer = envelope(bodyBytes, signature)
        if (approveKey != null && approveKey.isNotEmpty()) outer["m"] = SdCrypto.toB64(SdCrypto.hmac(approveKey, bodyBytes))
        val plain = J.compact(outer)
        val capacity = (SLOT_BYTES - 32 - SdCrypto.MLKEM_CIPHERTEXT_BYTES - UNIT_OVERHEAD).toInt()
        if (plain.size > capacity) fail("keyring_too_large")
        val padded = plain.copyOf(capacity)
        val sealed = SdCrypto.sealToDevice(recipient, mailboxLabel(mailbox), padded)
        SdCrypto.wipe(padded)
        SdCrypto.wipe(plain)
        if (sealed.size.toLong() != SLOT_BYTES) fail("encryption_failed")
        return sealed
    }

    fun approve(mailbox: Int) {
        val request = requestList.firstOrNull { it.mailbox == mailbox } ?: fail("unknown_request")
        transact { txn ->
            val invite = inviteMap[request.inviteId]
            if (invite == null || !invite.open(now())) fail("invite_expired")
            val unit = keyringUnit(request.device, request.keys, mailbox, invite.approveKey, invite.id)
            txn.records.add(
                "device_added" to linkedMapOf(
                    "device" to request.device,
                    "name" to request.name,
                    "keys" to request.keys.toJson(),
                    "invite" to request.inviteId,
                    "mailbox" to mailbox.toLong(),
                ),
            )
            txn.records.add(
                "invite_used" to linkedMapOf(
                    "invite" to request.inviteId,
                    "device" to request.device,
                    "name" to request.name,
                ),
            )
            val file = txn.file
            txn.afterCommit.add {
                file.writeAt(mailboxOffset(mailbox), unit) && file.sync() &&
                    same(file.readAt(mailboxOffset(mailbox), SLOT_BYTES.toInt()), unit)
            }
        }
    }

    fun deny(mailbox: Int) {
        val request = requestList.firstOrNull { it.mailbox == mailbox } ?: fail("unknown_request")
        transact { txn ->
            txn.records.add(
                "join_denied" to linkedMapOf(
                    "invite" to request.inviteId,
                    "device" to request.device,
                    "name" to request.name,
                ),
            )
            txn.records.add("invite_revoked" to linkedMapOf("invite" to request.inviteId))
            val file = txn.file
            txn.afterCommit.add { file.writeAt(mailboxOffset(mailbox), SdCrypto.random(SLOT_BYTES.toInt())) && file.sync() }
        }
    }

    private fun writeBlob(txn: Txn, source: BlobSource): FileVersion {
        val m = membership ?: fail("unknown_drive")
        val length = source.length
        if (length < 0) fail("read_failed")
        val chunks = chunkCount(length)
        val runs = allocate(txn, chunks, false)
        val pages = expand(runs)
        val key = SdCrypto.random(SdCrypto.KEY_BYTES)
        val blob = SdCrypto.randomId()
        val digest = MessageDigest.getInstance("SHA-256")
        var remaining = length
        val stream = try {
            source.open()
        } catch (_: Exception) {
            fail("read_failed")
        }
        stream.use { input ->
            val plain = ByteArray(CHUNK_BYTES.toInt())
            for (i in 0 until chunks) {
                val take = minOf(CHUNK_BYTES, maxOf(remaining, 0L)).toInt()
                plain.fill(0)
                var got = 0
                try {
                    while (got < take) {
                        val n = input.read(plain, got, take - got)
                        if (n < 0) break
                        got += n
                    }
                } catch (_: Exception) {
                    fail("read_failed")
                }
                if (got != take) fail("read_failed")
                digest.update(plain, 0, take)
                remaining -= take
                val page = pages[i.toInt()]
                val unit = sealUnit(key, blobAad(m.driveId, blob, i, i == chunks - 1, page), plain, PAGE_BYTES)
                if (unit == null || !txn.file.writeAt(page * PAGE_BYTES, unit)) fail("drive_write_failed")
                if (i % 256 == 255L) txn.renew?.invoke()
            }
            plain.fill(0)
        }
        return FileVersion(blob, key, runs, length, SdCrypto.hex(digest.digest()))
    }

    private fun readBlob(file: ContainerIo, version: FileVersion, sink: (ByteArray) -> Boolean) {
        val m = membership ?: fail("unknown_drive")
        val pages = expand(version.pages)
        val chunks = chunkCount(version.size)
        if (pages.size.toLong() != chunks) fail("content_damaged")
        val digest = MessageDigest.getInstance("SHA-256")
        var remaining = version.size
        for (i in 0 until chunks) {
            val page = pages[i.toInt()]
            val plain = openUnit(
                version.key,
                blobAad(m.driveId, version.blob, i, i == chunks - 1, page),
                file.readAt(page * PAGE_BYTES, PAGE_BYTES.toInt()),
            ) ?: fail("content_damaged")
            val take = minOf(remaining, plain.size.toLong()).toInt()
            if (take > 0) {
                val part = plain.copyOf(take)
                digest.update(part)
                val ok = try {
                    sink(part)
                } catch (_: Exception) {
                    false
                }
                SdCrypto.wipe(part)
                if (!ok) {
                    SdCrypto.wipe(plain)
                    fail("write_failed")
                }
            }
            SdCrypto.wipe(plain)
            remaining -= take
        }
        if (SdCrypto.hex(digest.digest()) != version.sha256) fail("content_damaged")
    }

    private fun cleanName(name: String): String {
        val cleaned = name.trim()
        if (cleaned.isEmpty() || cleaned.length > 255 || cleaned.contains('/') || cleaned.contains('\\')) fail("invalid_name")
        return cleaned
    }

    fun addFile(name: String, source: BlobSource, kind: String = ""): String {
        val cleaned = cleanName(name)
        val fileId = SdCrypto.randomId()
        transact { txn ->
            val version = writeBlob(txn, source)
            txn.records.add(
                "file_created" to linkedMapOf(
                    "file" to fileId,
                    "name" to cleaned,
                    "kind" to kind.ifEmpty { SecureDriveFormat.fileKindForName(cleaned) },
                    "blob" to version.blob,
                    "pages" to SecureDriveFormat.runsToJson(version.pages),
                    "key" to SdCrypto.toB64(version.key),
                    "size" to version.size,
                    "sha256" to version.sha256,
                ),
            )
        }
        return fileId
    }

    fun addFile(name: String, bytes: ByteArray, kind: String = ""): String = addFile(name, BytesSource(bytes), kind)

    fun saveFile(fileId: String, source: BlobSource, startedFrom: String = "") {
        transact { txn ->
            val entry = file(fileId) ?: fail("unknown_file")
            val current = entry.current ?: fail("unknown_file")
            val base = startedFrom.ifEmpty { current.blob }
            val name = entry.name
            val version = writeBlob(txn, source)
            txn.records.add(
                "file_saved" to linkedMapOf(
                    "file" to fileId,
                    "name" to name,
                    "blob" to version.blob,
                    "pages" to SecureDriveFormat.runsToJson(version.pages),
                    "key" to SdCrypto.toB64(version.key),
                    "size" to version.size,
                    "sha256" to version.sha256,
                    "base" to base,
                ),
            )
        }
    }

    fun saveFile(fileId: String, bytes: ByteArray, startedFrom: String = "") = saveFile(fileId, BytesSource(bytes), startedFrom)

    fun readFile(fileId: String, versionIndex: Int, out: OutputStream, purpose: String) {
        val entry = file(fileId) ?: fail("unknown_file")
        if (entry.versions.isEmpty()) fail("unknown_file")
        val index = if (versionIndex < 0) entry.versions.size - 1 else versionIndex
        if (index >= entry.versions.size) fail("unknown_version")
        val version = entry.versions[index]
        val name = entry.name
        val m = membership ?: fail("unknown_drive")
        val container = opener.open(m.container, false) ?: fail("container_unavailable")
        try {
            container.use { readBlob(it, version) { part -> out.write(part); true } }
        } catch (error: SecureDriveException) {
            if (error.code == "content_damaged") {
                try {
                    note("integrity_alert", linkedMapOf("code" to "content_damaged", "file" to fileId, "detail" to name))
                } catch (_: SecureDriveException) {
                }
                addIssue("content_damaged", "", name)
            }
            throw error
        }
        note(
            "file_opened",
            linkedMapOf(
                "file" to fileId,
                "name" to name,
                "version" to index.toLong(),
                "purpose" to purpose.ifEmpty { "view" },
            ),
        )
    }

    fun readBytes(fileId: String, versionIndex: Int = -1, purpose: String = "view"): ByteArray {
        val buffer = SecureBuffer()
        try {
            readFile(fileId, versionIndex, buffer, purpose)
            return buffer.toByteArray()
        } finally {
            buffer.wipe()
        }
    }

    fun renameFile(fileId: String, name: String) {
        val cleaned = cleanName(name)
        transact { txn ->
            val entry = file(fileId) ?: fail("unknown_file")
            txn.records.add("file_renamed" to linkedMapOf("file" to fileId, "name" to cleaned, "from" to entry.name))
        }
    }

    fun deleteFile(fileId: String) {
        transact { txn ->
            val entry = file(fileId) ?: fail("unknown_file")
            val seen = HashSet<Long>()
            for (version in entry.versions) for (run in version.pages) if (seen.add(run.start)) txn.freed.add(run)
            txn.records.add("file_deleted" to linkedMapOf("file" to fileId, "name" to entry.name))
        }
    }

    fun restoreVersion(fileId: String, index: Int) {
        transact { txn ->
            val entry = file(fileId) ?: fail("unknown_file")
            if (index < 0 || index >= entry.versions.size) fail("unknown_version")
            val version = entry.versions[index]
            txn.records.add(
                "file_restored" to linkedMapOf(
                    "file" to fileId,
                    "name" to entry.name,
                    "version" to index.toLong(),
                    "blob" to version.blob,
                    "pages" to SecureDriveFormat.runsToJson(version.pages),
                    "key" to SdCrypto.toB64(version.key),
                    "size" to version.size,
                    "sha256" to version.sha256,
                ),
            )
        }
    }

    fun removeDevice(deviceId: String, progress: ((Int, Int) -> Unit)? = null) {
        if (deviceId == state.deviceId) fail("cannot_remove_self")
        transact { txn ->
            val m = mutableMembership() ?: fail("unknown_drive")
            val target = deviceMap[deviceId]
            if (target == null || target.removed) fail("unknown_device")
            txn.bothRoots = true
            val nextKey = SdCrypto.random(SdCrypto.KEY_BYTES)
            val next = EpochKey(SecureDriveFormat.epochIdFor(nextKey), (m.currentEpoch()?.number ?: 0) + 1, nextKey)
            m.epochs.add(next)
            val file = txn.file
            for (device in deviceMap.values) {
                if (device.removed || device.id == deviceId || device.id == state.deviceId || device.mailbox < 0) continue
                val keys = device.keys ?: fail("invalid_public_keys")
                val unit = keyringUnit(device.id, keys, device.mailbox, null, "")
                val slot = device.mailbox
                txn.afterCommit.add { file.writeAt(mailboxOffset(slot), unit) && file.sync() }
            }
            if (target.mailbox >= 0) {
                val slot = target.mailbox
                txn.afterCommit.add { file.writeAt(mailboxOffset(slot), SdCrypto.random(SLOT_BYTES.toInt())) && file.sync() }
            }
            txn.records.add(
                "device_removed" to linkedMapOf(
                    "device" to deviceId,
                    "name" to target.name,
                    "epoch" to next.number.toLong(),
                    "keyId" to next.id,
                    "upTo" to (m.heads[deviceId]?.seq ?: -1L),
                ),
            )
        }
        if (files().isEmpty()) return
        transact { txn ->
            val m = membership ?: fail("unknown_drive")
            val current = files()
            val total = current.sumOf { it.versions.size }
            var done = 0
            for (entry in current) {
                val moved = HashSet<String>()
                val changes = mutableListOf<Any?>()
                for (version in entry.versions) {
                    progress?.invoke(++done, total)
                    if (moved.contains(version.blob)) continue
                    val runs = allocate(txn, runPages(version.pages), false)
                    val target = expand(runs)
                    val key = SdCrypto.random(SdCrypto.KEY_BYTES)
                    val blob = SdCrypto.randomId()
                    val chunks = chunkCount(version.size)
                    val source = expand(version.pages)
                    var intact = source.size.toLong() == chunks
                    var i = 0L
                    while (intact && i < chunks) {
                        val plain = openUnit(
                            version.key,
                            blobAad(m.driveId, version.blob, i, i == chunks - 1, source[i.toInt()]),
                            txn.file.readAt(source[i.toInt()] * PAGE_BYTES, PAGE_BYTES.toInt()),
                        )
                        if (plain == null) {
                            intact = false
                            break
                        }
                        val unit = sealUnit(key, blobAad(m.driveId, blob, i, i == chunks - 1, target[i.toInt()]), plain, PAGE_BYTES)
                        SdCrypto.wipe(plain)
                        if (unit == null || !txn.file.writeAt(target[i.toInt()] * PAGE_BYTES, unit)) fail("drive_write_failed")
                        if (i % 256 == 255L) txn.renew?.invoke()
                        i++
                    }
                    if (!intact) {
                        for (page in target) txn.reserved.remove(page)
                        addIssue("content_damaged", "", entry.name)
                        continue
                    }
                    moved.add(version.blob)
                    changes.add(
                        linkedMapOf(
                            "old" to version.blob,
                            "blob" to blob,
                            "pages" to SecureDriveFormat.runsToJson(runs),
                            "key" to SdCrypto.toB64(key),
                        ),
                    )
                    txn.freed.addAll(version.pages)
                    txn.renew?.invoke()
                }
                if (changes.isNotEmpty()) {
                    txn.records.add(
                        "file_rekeyed" to linkedMapOf("file" to entry.id, "name" to entry.name, "versions" to changes),
                    )
                }
            }
            if (txn.records.isEmpty()) txn.records.add("drive_rekeyed" to emptyMap())
        }
    }

    companion object {
        private const val INVITE_LIFETIME = 24L * 60 * 60 * 1000
        private const val LOG_UNIT_LIMIT_PAGES = 4L

        fun create(
            state: LocalState,
            container: String,
            label: String,
            opener: ContainerOpener = FileContainerOpener,
            timing: LeaseTiming = LeaseTiming(),
            discard: () -> Unit = {},
        ): String {
            if (!state.isUnlocked) fail("locked")
            if (state.drives.any { it.container == container }) fail("folder_already_connected")
            val io = opener.open(container, true) ?: fail("folder_unavailable")
            io.use {
                if (it.size() != 0L) fail("container_exists")
                if (!it.appendRandom(DATA_PAGE + INITIAL_DATA_PAGES)) {
                    discard()
                    fail("drive_write_failed")
                }
            }
            val m = Membership()
            m.id = SdCrypto.randomId()
            m.container = container
            m.label = label.trim().ifEmpty { baseName(container) }.take(80)
            m.driveId = SdCrypto.randomId()
            m.rootDevice = state.deviceId
            m.rootEd25519 = state.publicKeys.ed25519
            m.mailbox = (SdCrypto.random(1)[0].toInt() and 0xff) % MAILBOX_SLOTS
            val firstKey = SdCrypto.random(SdCrypto.KEY_BYTES)
            m.epochs.add(EpochKey(SecureDriveFormat.epochIdFor(firstKey), 0, firstKey))
            m.joinedAt = now()
            state.drives.add(m)
            val drive = Drive(state, m.id, opener, timing)
            val device = linkedMapOf<String, Any?>(
                "id" to state.deviceId,
                "name" to state.deviceName,
                "keys" to state.publicKeys.toJson(),
            )
            try {
                drive.transact { txn ->
                    txn.records.add(
                        "drive_created" to linkedMapOf("label" to m.label, "device" to device, "mailbox" to m.mailbox.toLong()),
                    )
                    val unit = drive.keyringUnit(state.deviceId, state.publicKeys, m.mailbox, null, "")
                    val file = txn.file
                    val slot = m.mailbox
                    txn.afterCommit.add { file.writeAt(mailboxOffset(slot), unit) && file.sync() }
                }
            } catch (error: SecureDriveException) {
                state.forget(m.id)
                discard()
                throw error
            }
            return m.id
        }

        class JoinStarted(val id: String, val safety: String)

        fun requestJoin(
            state: LocalState,
            container: String,
            code: String,
            label: String = "",
            opener: ContainerOpener = FileContainerOpener,
        ): JoinStarted {
            if (!state.isUnlocked) fail("locked")
            val secret = SdCrypto.parseInviteCode(code) ?: fail("invalid_invite_code")
            if (state.drives.any { it.container == container }) fail("folder_already_connected")
            val io = opener.open(container, true) ?: fail("container_unavailable")
            val inviteId = inviteIdBytes(secret)
            val mailbox = SecureDriveFormat.mailboxForInvite(secret)
            io.use {
                if (it.pages() < DATA_PAGE) fail("not_a_drive")
                val body = J.compact(
                    linkedMapOf(
                        "suite" to SdCrypto.SUITE,
                        "invite" to SdCrypto.toB64(inviteId),
                        "device" to state.deviceId,
                        "name" to state.deviceName,
                        "keys" to state.publicKeys.toJson(),
                        "time" to now(),
                    ),
                )
                val signature = SdCrypto.sign(state.keys.ed25519, requestMessage(body))
                val requestKey = SdCrypto.hkdf(secret, "request")
                val unit = sealUnit(requestKey, requestAad(mailbox), J.compact(envelope(body, signature)), SLOT_BYTES)
                SdCrypto.wipe(requestKey)
                unit ?: fail("encryption_failed")
                if (!it.writeAt(mailboxOffset(mailbox), unit) || !it.sync()) fail("drive_write_failed")
            }
            val m = Membership()
            m.id = SdCrypto.randomId()
            m.container = container
            m.label = label.trim().ifEmpty { baseName(container) }
            m.joinSecret = secret
            m.mailbox = mailbox
            state.drives.add(m)
            try {
                state.save()
            } catch (error: SecureDriveException) {
                state.forget(m.id)
                throw error
            }
            return JoinStarted(m.id, SdCrypto.safetyNumber(inviteId, state.publicKeys))
        }

        internal fun baseName(container: String): String {
            val name = container.substringAfterLast('/').substringAfterLast("%2F")
            val dot = name.indexOf('.')
            return if (dot > 0) name.substring(0, name.lastIndexOf('.')) else name
        }

        private fun now(): Long = System.currentTimeMillis()

        private fun same(a: ByteArray?, b: ByteArray?): Boolean = (a ?: EMPTY).contentEquals(b ?: EMPTY)

        private val EMPTY = ByteArray(0)

        private fun stripPadding(plain: ByteArray): ByteArray {
            val end = plain.indexOf(0.toByte())
            return if (end < 0) plain else plain.copyOf(end)
        }

        private fun sealUnit(key: ByteArray, aad: ByteArray, plain: ByteArray, size: Long): ByteArray? {
            if (plain.size > size - UNIT_OVERHEAD) return null
            val padded = plain.copyOf((size - UNIT_OVERHEAD).toInt())
            val nonce = SdCrypto.random(12)
            val sealed = SdCrypto.aeadSeal(key, nonce, aad, padded)
            SdCrypto.wipe(padded)
            return Bytes.concat(nonce, sealed)
        }

        private fun openUnit(key: ByteArray, aad: ByteArray, bytes: ByteArray?): ByteArray? {
            if (bytes == null || bytes.size < UNIT_OVERHEAD || key.size != SdCrypto.KEY_BYTES) return null
            return SdCrypto.aeadOpen(key, bytes.copyOfRange(0, 12), aad, bytes.copyOfRange(12, bytes.size))
        }

        private fun envelope(body: ByteArray, signature: ByteArray): LinkedHashMap<String, Any?> =
            linkedMapOf("b" to SdCrypto.toB64(body), "s" to SdCrypto.toB64(signature))

        private fun openEnvelope(json: Map<String, Any?>): Envelope? {
            val body = SdCrypto.fromB64(J.str(json["b"])) ?: return null
            val signature = SdCrypto.fromB64(J.str(json["s"])) ?: return null
            if (signature.size != 64) return null
            val parsed = J.parse(body) ?: return null
            return Envelope(body, signature, parsed)
        }

        private fun recordMessage(driveId: String, body: ByteArray): ByteArray =
            Bytes.concat(SdCrypto.label("record\n"), SdCrypto.utf8(driveId), byteArrayOf('\n'.code.toByte()), body)

        private fun rootMessage(body: ByteArray): ByteArray = Bytes.concat(SdCrypto.label("root\n"), body)
        private fun keyringMessage(body: ByteArray): ByteArray = Bytes.concat(SdCrypto.label("keyring\n"), body)
        private fun requestMessage(body: ByteArray): ByteArray = Bytes.concat(SdCrypto.label("request\n"), body)

        private fun mailboxLabel(mailbox: Int): ByteArray = SdCrypto.utf8("keyring/$mailbox")
        private fun requestAad(mailbox: Int): ByteArray = SdCrypto.label("request/$mailbox")
        private fun logAad(driveId: String, start: Long): ByteArray = SdCrypto.label("log/$driveId/$start")
        private fun blobAad(driveId: String, blob: String, chunk: Long, last: Boolean, page: Long): ByteArray =
            SdCrypto.label("blob/$driveId/$blob/$chunk/${if (last) "1" else "0"}/$page")
        private fun rootAad(slotA: Boolean): ByteArray = SdCrypto.label(if (slotA) "root/A" else "root/B")

        internal fun inviteIdBytes(secret: ByteArray): ByteArray = SdCrypto.hkdf(secret, "invite-id", 12)

        internal fun chunkCount(size: Long): Long = maxOf(1L, (SdCrypto.paddedSize(size) + CHUNK_BYTES - 1) / CHUNK_BYTES)

        private fun runPages(runs: List<Run>): Long = runs.sumOf { it.count }

        private fun expand(runs: List<Run>): List<Long> {
            val pages = mutableListOf<Long>()
            for (run in runs) for (i in 0 until run.count) pages.add(run.start + i)
            return pages
        }

        private fun csvField(text: String): String =
            if (text.contains(',') || text.contains('"') || text.contains('\n')) "\"" + text.replace("\"", "\"\"") + "\"" else text
    }
}
