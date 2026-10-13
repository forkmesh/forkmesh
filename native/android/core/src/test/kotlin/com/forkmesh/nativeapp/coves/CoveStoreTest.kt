package com.forkmesh.nativeapp.coves

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class CoveStoreTest {
    fun testNewPasswordCoveRoundTripsAndHidesEverything() {
        val doc = CoveDocument("d1", "plan.md", body = "secret launch plan", updatedAtMs = 5)
        val sealed = CoveStore.newPasswordCove("  Launch  ", "pw", "creator-account-one", true, listOf(doc), 42, 50)
        assertEq("Launch", sealed.name)
        assertEq(32, sealed.slug.length)
        assertEq(".forkmesh/coves/${sealed.slug}.cove", sealed.relPath)
        assertEq(4, sealed.grants.size)
        val text = CoveStore.envelopeText(sealed)
        expect(text.endsWith("}\n"))
        for (leak in listOf("Launch", "secret launch plan", "creator-account-one", "password", "notify")) expect(!text.contains(leak), leak)
        val reopened = CoveStore.unlock(CoveStore.parseEnvelope(text.toByteArray()) ?: error("parse"), "pw") ?: error("unlock")
        assertEq("Launch", reopened.name)
        assertEq(listOf(doc), reopened.documents)
        assertEq(42L, reopened.createdAtMs)
        expect(reopened.notifyOnOpen)
        assertFails<CoveStoreException> { CoveStore.newPasswordCove(" ", "pw", "", false, nowMs = 0) }
        assertFails<CoveStoreException> { CoveStore.newPasswordCove("x", "", "", false, nowMs = 0) }
    }

    fun testEnvelopeTextIsSortedFourSpaceJson() {
        val sealed = CoveStore.newPasswordCove("x", "pw", "", false, nowMs = 0, rounds = 5)
        val text = CoveStore.envelopeText(sealed)
        expect(text.startsWith("{\n    \"cipher\": {\n        \"body\": "))
        val keys = Regex("^    \"(\\w+)\":", RegexOption.MULTILINE).findAll(text).map { it.groupValues[1] }.toList()
        assertEq(listOf("cipher", "grants", "id", "kdf", "kind", "v"), keys)
    }

    fun testAccountCoveGrantsArePaddedToBlocksAndShuffled() {
        val sealed = CoveStore.newAccountCove("Team", "Alice", listOf("b", "c", "d", "e"), "k", false, nowMs = 1, rounds = 20)
        assertEq(listOf("alice", "b", "c", "d", "e"), sealed.invitedAccounts)
        assertEq(8, sealed.grants.size)
        val text = CoveStore.envelopeText(sealed)
        for (leak in listOf("alice", "Team", "account")) expect(!text.contains("\"$leak\""), leak)
        for (member in listOf("alice", "b", "e")) {
            val copy = CoveStore.unlockForAccount(CoveStore.parseEnvelope(text.toByteArray()) ?: error("parse"), member) ?: error(member)
            expect(copy.contentKey.contentEquals(sealed.contentKey))
        }
        assertEq(null, CoveStore.unlockForAccount(CoveStore.parseEnvelope(text.toByteArray()) ?: error("parse"), "f"))
        for (grant in sealed.grants) {
            assertEq(12, CoveBase64.decode(grant.nonce).size)
            assertEq(16, CoveBase64.decode(grant.tag).size)
            assertEq(32, CoveBase64.decode(grant.body).size)
        }
    }

    fun testSealRulesMatchDesktop() {
        val account = Cove(accessMode = "account")
        assertEq("Account-scoped coves are saved without a password.", assertFails<CoveStoreException> { CoveStore.sealPassword(account, "x") }.message)
        assertEq("Unlock the cove before saving it.", assertFails<CoveStoreException> { CoveStore.sealAccount(account) }.message)
        val password = Cove(accessMode = "password")
        assertEq("This cove is not account-scoped.", assertFails<CoveStoreException> { CoveStore.sealAccount(password) }.message)
        assertEq("The cove is missing its encryption parameters.", assertFails<CoveStoreException> { CoveStore.sealPassword(password, "x") }.message)
    }

    fun testAccessLogIsBounded() {
        var cove = Cove()
        for (i in 0 until 510) cove = CoveStore.appendAccess(cove, CoveAccessEntry(ts = i.toLong()))
        assertEq(500, cove.accessLog.size)
        assertEq(10L, cove.accessLog.first().ts)
    }

    fun testAccountHelpers() {
        assertEq(listOf("alice", "bob"), CoveStore.normalizedAccounts(listOf(" Bob", "alice", "BOB", "")))
        assertEq("a/b/d.md", CoveStore.cleanPath("a//b/./c/../d.md"))
        assertEq("0123", CoveStore.slugFromPath(".forkmesh/coves/0123.cove"))
        val cove = Cove(accessMode = "account", creatorAccount = "Alice", invitedAccounts = listOf("bob"))
        assertEq(listOf("alice", "bob"), cove.members)
        expect(cove.createdBy(" ALICE"))
        expect(!cove.createdBy("bob"))
        assertEq("forkmesh-cove-open-v1\nc\nk\no\n9", String(CoveStore.openCanonical("c", "k", "o", 9)))
    }
}
