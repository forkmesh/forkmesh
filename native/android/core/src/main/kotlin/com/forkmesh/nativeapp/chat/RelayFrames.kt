package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.account.guestChatName
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.ChatMessage

object RelayFrames {
    const val INTEGRATION_KEY = "integration"
    const val INTEGRATION_KIND = "integration"
    const val BOT_PREFIX = "bot:"
    const val SERVER_PREFIX = "srv:"
    private val PRIVATE_TYPES = setOf("dm", "invite", "cove-invite", "cove-open")
    private val CLAIM_KEYS = listOf("ownerUser", "owner", "acct", "accountKind", "bot", "solana", "dg", "userStatus", "mirrors", "nodeName")

    fun isPrivate(frame: Map<String, Any?>): Boolean {
        if (jsonText(frame["type"]) in PRIVATE_TYPES) return true
        if (frame["private"] == true) return true
        return ChatFrameText.conversation(frame).startsWith("@")
    }

    fun authenticated(plain: Map<String, Any?>, senderNode: String?): Map<String, Any?>? {
        if (isPrivate(plain)) return null
        val frame = LinkedHashMap(plain)
        frame.remove(INTEGRATION_KEY)
        if (!senderNode.isNullOrEmpty()) {
            val claimed = jsonText(frame["senderId"])
            if (claimed.isNotEmpty() && claimed != senderNode) return null
            frame["senderId"] = senderNode
        }
        return frame
    }

    fun serverFrame(envelope: Map<String, Any?>, plain: Map<String, Any?>): Map<String, Any?>? {
        if (envelope["srv"] != true) return null
        if (jsonText(plain["type"]) != "chat" || isPrivate(plain)) return null
        val channel = jsonText(plain["channel"])
        if (!channel.startsWith("#")) return null
        val frame = LinkedHashMap(plain)
        for (key in CLAIM_KEYS) frame.remove(key)
        frame["senderId"] = integrationId(jsonText(plain["senderId"]))
        frame["accountKind"] = INTEGRATION_KIND
        frame[INTEGRATION_KEY] = true
        if (jsonText(frame["sender"]).isEmpty()) frame["sender"] = "Integration"
        return frame
    }

    fun integrationId(senderId: String): String {
        val id = senderId.trim().take(120)
        if (id.isEmpty()) return SERVER_PREFIX + "integration"
        return if (':' in id) id else SERVER_PREFIX + id
    }

    fun isIntegration(frame: Map<String, Any?>): Boolean = frame[INTEGRATION_KEY] == true

    fun botName(frame: Map<String, Any?>): String = jsonText(frame["bot"]).trim().replace(":", "").take(ChatLimits.MAX_DISPLAY_NAME_CHARS)

    fun shownSender(frame: Map<String, Any?>): String {
        val bot = botName(frame)
        val sender = jsonText(frame["senderId"])
        return if (bot.isNotEmpty()) "$BOT_PREFIX$sender:$bot" else sender
    }

    fun botLabel(bot: String, node: String, account: String): String =
        "$bot \u00b7 via ${account.trim().ifEmpty { guestChatName(node) }}"

    fun shownName(frame: Map<String, Any?>, account: (String) -> String = { "" }): String {
        val bot = botName(frame)
        if (bot.isEmpty()) return jsonText(frame["sender"])
        val node = jsonText(frame["senderId"])
        return botLabel(bot, node, account(node))
    }

    fun botParts(senderId: String): Pair<String, String>? {
        if (!senderId.startsWith(BOT_PREFIX)) return null
        val rest = senderId.removePrefix(BOT_PREFIX)
        val node = rest.substringBefore(':')
        val bot = rest.substringAfter(':', "")
        return if (node.isEmpty() || bot.isEmpty()) null else node to bot
    }

    fun relabel(message: ChatMessage, account: (String) -> String): ChatMessage {
        val (node, bot) = botParts(message.senderId) ?: return message
        val label = botLabel(bot, node, account(node))
        return if (label == message.senderName) message else message.copy(senderName = label)
    }

    fun serverVisible(frame: Map<String, Any?>): Boolean = isIntegration(frame) && frame["serverVisible"] == true

    fun actsForPerson(frame: Map<String, Any?>): Boolean = !isIntegration(frame) && jsonText(frame["bot"]).isEmpty()
}
