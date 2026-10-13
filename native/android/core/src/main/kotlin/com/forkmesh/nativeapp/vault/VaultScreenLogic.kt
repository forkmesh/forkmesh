package com.forkmesh.nativeapp.vault

object VaultScreenLogic {
    const val MINIMUM_MASTER_PASSWORD = 12
    const val CLIPBOARD_CLEAR_MS = 30_000L

    private val WORDS = mapOf(
        "wrong_password" to "That is not the master password.",
        "wrong_recovery_code" to "That recovery code does not open this vault.",
        "invalid_recovery_code" to "A recovery code is 13 groups of 4 letters and digits.",
        "unsafe_kdf_parameters" to "The server returned key-stretching settings weaker than this vault allows. Nothing was unlocked.",
        "keyset_mismatch" to "The keys stored for this account do not match its published keys. Nothing was unlocked.",
        "normalization_unavailable" to "This device could not normalize the password's special characters, so it was not used.",
        "recipient_has_no_vault" to "That person has not created a vault yet, so there are no keys to seal to.",
        "recipient_not_found" to "No ForkMesh user has that name.",
        "recipient_keys_changed" to "That person's keys changed since you last shared with them. Compare fingerprints before sharing.",
        "cannot_share_with_yourself" to "That is your own account.",
        "organization_not_found" to "You are not a member of that organization.",
        "revision_conflict" to "This login changed somewhere else. It was reloaded; make the change again.",
        "read_only" to "This login was shared with you to view, not to edit.",
        "vault_locked" to "The vault is locked.",
        "vault_full" to "This vault is full.",
        "title_required" to "Give the login a title.",
        "invalid_session" to "Sign in to ForkMesh to open your vault.",
    )

    fun errorText(error: Throwable): String {
        val code = when (error) {
            is VaultException -> error.code
            is VaultCryptoException -> error.code
            else -> ""
        }
        if (code.isEmpty()) return "Something went wrong. Check your connection and try again."
        return WORDS[code] ?: code.replace('_', ' ')
    }

    fun newPasswordProblem(password: String, confirmation: String): String? {
        if (password.length < MINIMUM_MASTER_PASSWORD) return "Use at least 12 characters for the master password."
        if (password != confirmation) return "The two passwords do not match."
        return null
    }

    fun filter(items: List<VaultItem>, filter: String, query: String): List<VaultItem> {
        val q = query.trim().lowercase()
        return items.filter { item ->
            (filter != "mine" || item.owned) &&
                (filter != "shared" || !item.owned) &&
                (q.isEmpty() || listOf(item.title, item.username, item.host, item.owner).any { it.lowercase().contains(q) })
        }
    }

    fun copiedNotice(label: String): String = "$label copied. The clipboard is cleared in 30 seconds."

    fun sharedBy(item: VaultItem): String {
        val changed = if (item.updatedBy.isNotEmpty() && item.updatedBy != item.owner) ", last changed by ${item.updatedBy}" else ""
        return "Shared by ${item.owner}$changed."
    }

    fun shareAdvice(kind: String, changed: List<String>, members: List<VaultRecipient>): String {
        val notes = ArrayList<String>()
        if (changed.isNotEmpty()) {
            notes.add(
                "${changed.joinToString(", ")} now presents different keys than you pinned. That happens when someone deletes and recreates their vault, and it is also what an attack looks like. Compare the fingerprint with them before going on.",
            )
        }
        if (kind == "user" && members.any { it.state == "new" }) {
            notes.add("First share with this person. To be certain these keys are theirs, ask them to read their fingerprint (Vault settings) to you.")
        }
        val without = members.count { it.state == "no_vault" }
        if (without > 0) {
            notes.add("$without member${if (without == 1) " has" else "s have"} no vault yet. They get access the next time you unlock after they create one.")
        }
        return notes.joinToString(" ")
    }

    fun shareOutcome(target: String, outcome: VaultShareOutcome): String {
        val skipped = if (outcome.skipped.isEmpty()) "" else ", skipped: ${outcome.skipped.joinToString(", ")}"
        return "Shared with $target (${outcome.sealedTo.size} sealed$skipped)."
    }

    fun autofillSummary(logins: Int): String =
        "ForkMesh is your autofill provider. $logins login${if (logins == 1) "" else "s"} ready. In a browser a login is offered only on its own site; in other apps you pick one, and the choice is remembered for that app."
}
