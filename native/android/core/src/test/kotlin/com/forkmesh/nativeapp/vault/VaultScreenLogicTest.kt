package com.forkmesh.nativeapp.vault

import com.forkmesh.nativeapp.testing.assertEq

class VaultScreenLogicTest {
    private fun item(id: String, title: String, owned: Boolean, owner: String = "alice", url: String = "", error: String = "") =
        VaultItem(id, owner, owned, if (owned) "write" else "read", 1, 0, owner, fields = mapOf("title" to title, "url" to url), error = error)

    fun testErrorsReadAsSentences() {
        assertEq("That is not the master password.", VaultScreenLogic.errorText(VaultException("wrong_password")))
        assertEq("A recovery code is 13 groups of 4 letters and digits.", VaultScreenLogic.errorText(VaultCryptoException("invalid_recovery_code")))
        assertEq("brand new code", VaultScreenLogic.errorText(VaultException("brand_new_code")))
        assertEq("Something went wrong. Check your connection and try again.", VaultScreenLogic.errorText(IllegalStateException()))
    }

    fun testNewPasswordRules() {
        assertEq("Use at least 12 characters for the master password.", VaultScreenLogic.newPasswordProblem("short", "short"))
        assertEq("The two passwords do not match.", VaultScreenLogic.newPasswordProblem("long enough pw", "long enough pX"))
        assertEq(null, VaultScreenLogic.newPasswordProblem("long enough pw", "long enough pw"))
    }

    fun testFilterAndSearch() {
        val items = listOf(item("1", "Bank", true, url = "https://bank.example"), item("2", "Mail", false, owner = "bob"))
        assertEq(listOf("1"), VaultScreenLogic.filter(items, "mine", "").map { it.id })
        assertEq(listOf("2"), VaultScreenLogic.filter(items, "shared", "").map { it.id })
        assertEq(listOf("1"), VaultScreenLogic.filter(items, "all", "BANK.ex").map { it.id })
        assertEq(listOf("2"), VaultScreenLogic.filter(items, "all", "bob").map { it.id })
    }

    fun testShareCopy() {
        val members = listOf(VaultRecipient("bob", "new", "f", null), VaultRecipient("carol", "no_vault", "", null))
        assertEq(
            "First share with this person. To be certain these keys are theirs, ask them to read their fingerprint (Vault settings) to you. 1 member has no vault yet. They get access the next time you unlock after they create one.",
            VaultScreenLogic.shareAdvice("user", emptyList(), members),
        )
        assertEq("Shared with acme (2 sealed, skipped: dan).", VaultScreenLogic.shareOutcome("acme", VaultShareOutcome(listOf("a", "b"), listOf("dan"), emptyList())))
        assertEq("Shared by alice, last changed by bob.", VaultScreenLogic.sharedBy(item("1", "x", false).copy(updatedBy = "bob")))
        assertEq("ForkMesh is your autofill provider. 1 login ready. In a browser a login is offered only on its own site; in other apps you pick one, and the choice is remembered for that app.", VaultScreenLogic.autofillSummary(1))
    }
}
