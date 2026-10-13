package com.forkmesh.nativeapp.coves

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

private class FakeCoveRepository : CoveRepositoryIO {
    val files = LinkedHashMap<String, ByteArray>()
    val proposals = ArrayList<Triple<String, String, String>>()
    val accounts = HashMap<String, CoveAccountCheck>()
    var failPropose = false

    override fun listFiles(repo: CoveRepo): List<CoveFileRef> =
        files.keys.sorted().map { CoveFileRef(it, files.getValue(it).size.toLong()) } + CoveFileRef(".forkmesh/coves/README.md")

    override fun readFile(repo: CoveRepo, path: String, maxBytes: Int): ByteArray = files[path] ?: throw IllegalStateException("missing")

    override fun propose(repo: CoveRepo, title: String, head: String, patch: String, description: String) {
        if (failPropose) throw IllegalStateException("inbox_full")
        proposals.add(Triple(title, head, patch))
    }

    override fun checkAccount(name: String): CoveAccountCheck = accounts[name] ?: CoveAccountCheck(exists = false, active = false)
}

private fun addedText(patch: String): String =
    patch.split("\n").filter { it.startsWith("+") && !it.startsWith("+++") }.joinToString("\n") { it.drop(1) }

class CovesModelTest {
    private val repo = CoveRepo("alice", "plans")

    fun testLoadsDesktopCovesAndOpensAccountCovesAutomatically() {
        val fake = FakeCoveRepository()
        fake.files[".forkmesh/coves/aaaa.cove"] = DesktopCoveVectors.account.toByteArray()
        fake.files[".forkmesh/coves/bbbb.cove"] = DesktopCoveVectors.password.toByteArray()
        fake.files[".forkmesh/coves/cccc.cove"] = "garbage".toByteArray()
        val model = CovesModel(repo, "Bob", io = fake, rounds = 20)
        model.load()
        assertEq(CovesPhase.Loaded, model.phase)
        assertEq(2, model.entries.size)
        assertEq(1, model.skipped)
        assertEq(listOf("Investor list"), model.unlockedCoves.map { it.name })
        assertEq(1, model.lockedCoves.size)
        val passwordId = model.lockedCoves[0].id
        expect(!model.unlock(passwordId, "nope"))
        expect(model.unlock(passwordId, "correct horse battery staple"))
        assertEq(2, model.cove(passwordId)?.documents?.size)
        model.lock(passwordId)
        assertEq(false, model.cove(passwordId)?.unlocked)
        assertEq(emptyList<CoveDocument>(), model.cove(passwordId)?.documents)
        assertEq("forkmesh://cove/alice/plans/x", model.shareLink("x"))
    }

    fun testSavingProposesAReplacementEnvelope() {
        val fake = FakeCoveRepository()
        val path = ".forkmesh/coves/bbbb.cove"
        fake.files[path] = DesktopCoveVectors.password.toByteArray()
        val model = CovesModel(repo, "Bob", "bobKey", fake, rounds = 20)
        model.load()
        val id = model.entries[0].cove.id
        expect(model.unlock(id, "correct horse battery staple"))
        model.saveDocument(id, "notes.md", "changed")
        assertEq(1, fake.proposals.size)
        assertEq("cove: bbbb", fake.proposals[0].first)
        expect(fake.proposals[0].second.startsWith("bob:mobile-cove/bbbb-"))
        val patch = fake.proposals[0].third
        expect(patch.startsWith("diff --git a/$path b/$path\nindex ${CovePatch.gitBlobId(fake.files.getValue(path))}.."))
        expect(!patch.contains("changed"))
        expect(model.entries[0].pendingReview)
        val reopened = CoveStore.unlock(CoveStore.parseEnvelope((addedText(patch) + "\n").toByteArray()) ?: error("parse"), "correct horse battery staple")
            ?: error("unlock")
        assertEq("changed", reopened.document("notes.md")?.body)
        assertEq("edit", reopened.accessLog.last().action)
        assertEq("bobKey", reopened.accessLog.last().who)
        assertEq("Launch plans — Q4", reopened.name)
    }

    fun testCreateAddDocumentAndInvite() {
        val fake = FakeCoveRepository()
        fake.accounts["carol"] = CoveAccountCheck(exists = true, active = true)
        fake.accounts["dave"] = CoveAccountCheck(exists = true, active = false)
        val model = CovesModel(repo, "Alice", io = fake, rounds = 20)
        val created = model.createAccountCove("Board")
        expect(fake.proposals[0].third.contains("new file mode 100644"))
        val template = CoveFiles.template("lead-pipeline") ?: error("template")
        assertEq("leads.pipeline.json", model.addDocument(created.id, template))
        assertEq("leads-2.pipeline.json", model.addDocument(created.id, template))
        expect(fake.proposals[2].third.contains("new file mode 100644"))
        assertEq("zed is not a ForkMesh account.", assertFails<CoveActionException> { model.invite(created.id, "zed") }.message)
        assertEq("dave is not an active ForkMesh account.", assertFails<CoveActionException> { model.invite(created.id, "dave") }.message)
        assertEq("@alice already has access to this cove.", assertFails<CoveActionException> { model.invite(created.id, "alice") }.message)
        model.invite(created.id, " Carol")
        assertEq(listOf("alice", "carol"), model.cove(created.id)?.members)
        val text = addedText(fake.proposals.last().third)
        val asCarol = CoveStore.unlockForAccount(CoveStore.parseEnvelope(text.toByteArray()) ?: error("parse"), "carol") ?: error("carol")
        assertEq(2, asCarol.documents.size)

        val other = CovesModel(repo, "carol", io = fake, rounds = 20)
        fake.files[created.relPath] = (text + "\n").toByteArray()
        other.load()
        assertEq("Only the cove creator can change sharing.", assertFails<CoveActionException> { other.invite(created.id, "carol2") }.message)
    }

    fun testFailuresLeaveStateAlone() {
        val fake = FakeCoveRepository()
        fake.failPropose = true
        val model = CovesModel(repo, "Alice", io = fake, rounds = 20)
        assertFails<CoveActionException> { model.createPasswordCove("X", "pw") }
        expect(model.entries.isEmpty())
        val signedOut = CovesModel(repo, "", io = fake, rounds = 20)
        assertEq("Sign in with a ForkMesh account.", assertFails<CoveActionException> { signedOut.createAccountCove("X") }.message)
    }
}
