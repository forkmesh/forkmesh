package com.forkmesh.nativeapp.coves

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.hexOf

class CoveVectorTest {
    private fun parse(text: String, path: String = ".forkmesh/coves/abc.cove"): Cove =
        CoveStore.parseEnvelope(text.toByteArray(Charsets.UTF_8), path) ?: error("envelope did not parse")

    fun testDesktopPasswordCoveUnlocks() {
        val cove = parse(DesktopCoveVectors.password)
        assertEq(2, cove.version)
        assertEq("", cove.name)
        assertEq("", cove.accessMode)
        assertEq(4, cove.grants.size)
        assertEq("abc", cove.slug)
        assertEq(null, CoveStore.unlock(cove, "correct horse battery stapl"))
        val opened = CoveStore.unlock(cove, "correct horse battery staple") ?: error("unlock")
        assertEq("Launch plans — Q4", opened.name)
        assertEq("password", opened.accessMode)
        assertEq("creatorPubKeyB64url", opened.creator)
        assertEq(1_758_700_000_000L, opened.createdAtMs)
        expect(opened.notifyOnOpen)
        assertEq(listOf("notes.md", "sheet.csv"), opened.documents.map { it.name })
        assertEq("# Notes\n\nSecret 🔑 plan\n", opened.documents[0].body)
        assertEq("text/csv", opened.documents[1].mime)
        assertEq("A,B\r\n1,2\r\n", opened.documents[1].body)
        assertEq(listOf(CoveAccessEntry("openerKey", "bob", "node1", 1_758_700_003_000L, "edit")), opened.accessLog)
    }

    fun testDesktopAccountCoveUnlocksForEveryMemberOnly() {
        val locked = parse(DesktopCoveVectors.account)
        assertEq(4, locked.grants.size)
        for (member in listOf("alice", " Bob", "CAROL ")) {
            val cove = CoveStore.unlockForAccount(locked, member) ?: error(member)
            assertEq("Investor list", cove.name)
            assertEq("account", cove.accessMode)
            assertEq("alice", cove.creatorAccount)
            assertEq(listOf("alice", "bob", "carol"), cove.invitedAccounts)
            assertEq("1f1e1d1c1b1a191817161514131211100f0e0d0c0b0a09080706050403020100", hexOf(cove.contentKey))
            assertEq("leads.pipeline.json", cove.documents.firstOrNull()?.name)
        }
        assertEq(null, CoveStore.unlockForAccount(locked, "zed"))
        assertEq(null, CoveStore.unlock(locked, "alice"))
    }

    fun testLegacyV1Envelopes() {
        val legacy = parse(DesktopCoveVectors.legacyAccount)
        assertEq(1, legacy.version)
        assertEq("Legacy account cove", legacy.name)
        assertEq("account", legacy.accessMode)
        expect(CoveStore.accountCanAccess(legacy, "Erin"))
        assertEq("Hufy1KlPzJD2c741SrzdVLJkR5m4mda3d08X7714ke8", CoveStore.accountSecretV1(legacy))
        val erin = CoveStore.unlockForAccount(legacy, "erin") ?: error("erin")
        assertEq("legacy body", erin.documents.firstOrNull()?.body)
        assertEq("Legacy account cove", erin.name)
        assertEq(null, CoveStore.unlockForAccount(legacy, "mallory"))

        val old = parse(DesktopCoveVectors.legacyPassword)
        assertEq("password", old.accessMode)
        expect(old.notifyOnOpen)
        val opened = CoveStore.unlock(old, "hunter2") ?: error("hunter2")
        assertEq("Old password cove", opened.name)
        assertEq("old.md", opened.documents.firstOrNull()?.name)
    }

    fun testLegacyAccountCoveUpgradesToAnonymousV2OnSave() {
        val erin = CoveStore.unlockForAccount(parse(DesktopCoveVectors.legacyAccount), "erin") ?: error("erin")
        expect(erin.contentKey.isEmpty())
        val sealed = CoveStore.sealAccount(erin)
        assertEq(32, sealed.contentKey.size)
        val text = CoveStore.envelopeText(sealed)
        expect(!text.contains("erin"))
        expect(!text.contains("Legacy"))
        val reopened = CoveStore.parseEnvelope(text.toByteArray()) ?: error("reparse")
        assertEq(2, reopened.version)
        val dave = CoveStore.unlockForAccount(reopened, "dave") ?: error("dave")
        assertEq("legacy body", dave.documents.firstOrNull()?.body)
    }

    fun testRejectsEnvelopesThatAreNotCoves() {
        assertEq(null, CoveStore.parseEnvelope("{\"kind\":\"note\",\"id\":\"x\"}".toByteArray()))
        assertEq(null, CoveStore.parseEnvelope("{\"kind\":\"cove\",\"id\":\"x\",\"kdf\":{\"rounds\":0,\"salt\":\"AA==\"}}".toByteArray()))
        assertEq(null, CoveStore.parseEnvelope("not json".toByteArray()))
    }
}
