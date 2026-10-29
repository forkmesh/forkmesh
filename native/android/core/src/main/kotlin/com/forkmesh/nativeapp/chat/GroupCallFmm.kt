package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.facechanger.FaceChangerLook
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.jsonText

object GroupCallE2ee {
    const val SIGNING_PREFIX = "forkmesh-call-e2ee-"
    const val SEAL_KID = "sealKid"
    const val SEAL_ACCT = "sealAcct"
    const val MAX_SIGN_BYTES = 64 * 1024

    private val prefix = SIGNING_PREFIX.toByteArray(Charsets.UTF_8)

    fun annotate(plain: Map<String, Any?>, kid: String?, acct: String?): Map<String, Any?> {
        val live = jsonText(plain["type"]) in GroupCallFrames.LIVE_TYPES
        if (!live && !plain.containsKey(SEAL_KID) && !plain.containsKey(SEAL_ACCT)) return plain
        val out = LinkedHashMap(plain)
        out.remove(SEAL_KID)
        out.remove(SEAL_ACCT)
        if (!live) return out
        if (!kid.isNullOrEmpty()) out[SEAL_KID] = kid
        if (!acct.isNullOrEmpty()) out[SEAL_ACCT] = acct
        return out
    }

    fun signable(message: ByteArray): Boolean {
        if (message.size < prefix.size || message.size > MAX_SIGN_BYTES) return false
        for (i in prefix.indices) if (message[i] != prefix[i]) return false
        return true
    }
}

class GroupCallIdentity(
    val call: String,
    val acct: String,
    val kid: String,
    val idk: ByteArray,
    private val signer: (ByteArray) -> ByteArray,
) {
    fun sign(call: String, message: ByteArray): ByteArray? {
        if (call != this.call || !GroupCallE2ee.signable(message)) return null
        return runCatching { signer(message) }.getOrNull()
    }

    fun page(): Map<String, Any?> = linkedMapOf("call" to call, "acct" to acct, "kid" to kid, "idk" to B64.encodeUrlNoPadding(idk))
}

object GroupCallVerifyText {
    const val TITLE = "Compare these with everyone in the call"
    const val MATCH = "They match"
    const val NO_MATCH = "They don't match"
    const val MISMATCH = "Someone in this call may not be who they seem. Leave the call or ask everyone to check their devices."
    const val INTERFERENCE = "Verification keeps restarting. Someone may be interfering with this call. Scan each other's codes if you can."
    const val VERIFIED = "Everyone in this call is verified."
    const val PREPARING = "The verification code appears once everyone's devices have exchanged keys."
    const val LEAVE = "Leave the call"
    const val MAX_UNFINISHED = 3

    fun numbers(digits: String): String = "Or compare numbers: $digits"

    fun waiting(names: List<String>): String = "Waiting for ${list(names)} to confirm…"

    fun newcomer(name: String): String = "$name is not verified. Compare the new code to verify everyone."

    fun list(names: List<String>): String = when (names.size) {
        0 -> ""
        1 -> names[0]
        else -> names.dropLast(1).joinToString(", ") + " and " + names.last()
    }
}

data class GroupCallVerify(
    val hasCode: Boolean = false,
    val emoji: List<String> = emptyList(),
    val names: List<String> = emptyList(),
    val digits: String = "",
    val waiting: List<String> = emptyList(),
    val late: Boolean = false,
    val verified: Boolean = false,
    val newcomers: List<String> = emptyList(),
    val unfinished: Int = 0,
    val mismatch: Boolean = false,
) {
    val interfered: Boolean get() = unfinished >= GroupCallVerifyText.MAX_UNFINISHED

    val summary: String get() = when {
        interfered -> GroupCallVerifyText.INTERFERENCE
        verified -> GroupCallVerifyText.VERIFIED
        hasCode && newcomers.isNotEmpty() -> GroupCallVerifyText.newcomer(newcomers.first())
        waiting.isNotEmpty() && late -> GroupCallVerifyText.waiting(waiting)
        hasCode -> GroupCallVerifyText.TITLE
        else -> GroupCallVerifyText.PREPARING
    }

    val label: String get() = when {
        verified -> "Verified"
        hasCode -> "Compare the code"
        else -> "Verify"
    }

    val canConfirm: Boolean get() = hasCode && !verified && !mismatch

    companion object {
        private const val MAX_NAMES = GroupCallFrames.MAX_PEOPLE
        private const val MAX_TEXT = 64

        private fun texts(value: Any?, limit: Int = MAX_NAMES): List<String> =
            (value as? List<*>).orEmpty().asSequence().map { jsonText(it).trim().take(MAX_TEXT) }.filter { it.isNotEmpty() }.take(limit).toList()

        fun parse(value: Any?): GroupCallVerify? {
            if (value !is Map<*, *>) return null
            val view = value.asJsonMap()
            val code = view["code"] as? Map<*, *>
            val digits = jsonText(code?.get("digits")).filter { it.isDigit() || it == ' ' }.trim().take(MAX_TEXT)
            val emoji = texts(code?.get("emoji"), 8)
            val names = texts(code?.get("emojiNames"), 8)
            return GroupCallVerify(
                hasCode = code != null && digits.isNotEmpty() && emoji.isNotEmpty(),
                emoji = emoji,
                names = names,
                digits = digits,
                waiting = texts(view["waiting"]),
                late = view["late"] == true,
                verified = view["verified"] == true,
                newcomers = texts(view["newcomers"]),
                unfinished = ((view["unfinished"] as? Number)?.toInt() ?: 0).coerceIn(0, 1000),
                mismatch = view["mismatch"] == true,
            )
        }
    }
}

object GroupCallLook {
    fun page(look: FaceChangerLook, video: Boolean): Map<String, Any?> = linkedMapOf(
        "face" to if (video) look.face else "",
        "faceImage" to if (video) look.customFaceImage else "",
        "background" to if (video) look.background else "",
        "backgroundImage" to if (video) look.customBackgroundImage else "",
        "voice" to look.effectiveVoice,
    )
}

object GroupCallPageData {
    const val FOLDER = "call-data/"
    const val SOURCE = "direct-call.html"
    private const val CLOSE = "</script>"
    val BLOCKS = mapOf(
        "face-photos.js" to "fm-call-face-photos",
        "backgrounds.js" to "fm-call-backgrounds",
        "models.js" to "fm-call-models",
    )

    fun block(page: String, file: String): String? {
        val id = BLOCKS[file] ?: return null
        val open = "<script id=\"$id\">"
        val start = page.indexOf(open)
        if (start < 0) return null
        val body = start + open.length
        val end = page.indexOf(CLOSE, body)
        if (end < 0) return null
        return page.substring(body, end)
    }
}
