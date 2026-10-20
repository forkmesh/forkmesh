package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.ChatMessage

object ChatLimits {
    const val MAX_TEXT_CHARS = 16_000
    const val MAX_FILE_BYTES = 16 * 1024 * 1024
    const val MAX_FILE_NAME_CHARS = 180
    const val MAX_MIME_CHARS = 100
    const val MAX_ROOT_ID_CHARS = 96
    const val MAX_DISPLAY_NAME_CHARS = 32
    const val HISTORY_ENTRIES_PER_CHANNEL = 200
    const val HISTORY_CHARS_PER_CHANNEL = 2 * 1024 * 1024
    const val ATTACHMENT_BUDGET_BYTES = 48 * 1024 * 1024
    const val PERSISTED_FILE_BYTES = 512 * 1024
    const val TYPING_TIMEOUT_MS = 6_000L
    const val TYPING_RESEND_MS = 2_500L
    const val MAX_SEND_FILE_BYTES = 2 * 1024 * 1024
    const val HIDDEN_MESSAGES = 2_000
}

data class ReactionEntry(val value: String, val label: String, val emoji: String)

data class ReactionGroup(val title: String, val emoji: List<String>)

object ReactionCatalog {
    val entries: List<ReactionEntry> = listOf(
        ReactionEntry("like", "Like", "👍"),
        ReactionEntry("love", "Love", "❤️"),
        ReactionEntry("laugh", "Laugh", "😂"),
        ReactionEntry("celebrate", "Celebrate", "🎉"),
        ReactionEntry("hot", "Hot", "🔥"),
        ReactionEntry("thanks", "Thanks", "🙏"),
        ReactionEntry("smile", "Smile", "🙂"),
        ReactionEntry("wink", "Wink", "😉"),
        ReactionEntry("cool", "Cool", "😎"),
        ReactionEntry("thinking", "Thinking", "🤔"),
        ReactionEntry("surprised", "Surprised", "😮"),
        ReactionEntry("sad", "Sad", "😢"),
        ReactionEntry("angry", "Angry", "😡"),
        ReactionEntry("dislike", "Dislike", "👎"),
        ReactionEntry("eyes", "Eyes", "👀"),
        ReactionEntry("star", "Star", "⭐"),
        ReactionEntry("rocket", "Rocket", "🚀"),
        ReactionEntry("check", "Done", "✅"),
    )

    val quick: List<String> = listOf("like", "love", "laugh", "celebrate", "eyes", "check")

    val groups: List<ReactionGroup> = listOf(
        ReactionGroup(
            "Smileys",
            listOf(
                "😀", "😃", "😄", "😁", "😆", "😅", "🤣", "😂", "🙂", "🙃",
                "😉", "😊", "😇", "🥰", "😍", "🤩", "😘", "😋", "😛", "😜",
                "🤪", "😝", "🤑", "🤗", "🤭", "🤫", "🤔", "🤐", "🤨", "😐",
                "😑", "😶", "😏", "😒", "🙄", "😬", "😌", "😔", "😪", "🤤",
                "😴", "😷", "🤒", "🤕", "🤢", "🤮", "🥵", "🥶", "🥴", "😵",
                "🤯", "🤠", "🥳", "😎", "🤓", "🧐", "😕", "😟", "🙁", "😮",
                "😯", "😲", "😳", "🥺", "😦", "😧", "😨", "😰", "😥", "😢",
                "😭", "😱", "😖", "😣", "😞", "😓", "😩", "😫", "🥱", "😤",
                "😡", "😠", "🤬", "😈", "💀", "💩", "🤡", "👻", "👽", "🤖",
            ),
        ),
        ReactionGroup(
            "People",
            listOf(
                "👍", "👎", "👏", "🙌", "👐", "🤲", "🤝", "🙏", "✌️", "🤞",
                "🤟", "🤘", "👌", "🤌", "🤏", "👈", "👉", "👆", "👇", "☝️",
                "✋", "🤚", "🖐️", "🖖", "👋", "🤙", "💪", "👀", "🧠", "🙋",
                "🙆", "🙅", "🤷", "🤦", "🙇", "💃", "🕺", "🏃",
            ),
        ),
        ReactionGroup(
            "Hearts",
            listOf(
                "❤️", "🧡", "💛", "💚", "💙", "💜", "🖤", "🤍", "🤎", "💔",
                "❣️", "💕", "💞", "💓", "💗", "💖", "💘", "💝", "💯", "💢",
                "💥", "💫", "💦", "💨", "💬", "💭", "💤", "✨", "⭐", "🌟",
                "🔥", "⚡",
            ),
        ),
        ReactionGroup(
            "Animals & nature",
            listOf(
                "🐶", "🐱", "🐭", "🐹", "🐰", "🦊", "🐻", "🐼", "🐨", "🐯",
                "🦁", "🐮", "🐷", "🐸", "🐵", "🙈", "🙉", "🙊", "🐔", "🐧",
                "🐦", "🦆", "🦉", "🦄", "🐝", "🦋", "🐌", "🐞", "🐢", "🐍",
                "🐙", "🦀", "🐬", "🐳", "🦈", "🌱", "🌲", "🌵", "🍀", "🌸",
                "🌻", "🌹", "🌈", "☀️", "🌙", "☁️", "❄️", "🌊",
            ),
        ),
        ReactionGroup(
            "Food & drink",
            listOf(
                "🍎", "🍊", "🍋", "🍌", "🍉", "🍇", "🍓", "🍒", "🍑", "🥑",
                "🌶️", "🌽", "🥕", "🍞", "🧀", "🍔", "🍟", "🍕", "🌮", "🍣",
                "🍜", "🍩", "🍪", "🎂", "🍰", "🍫", "🍿", "☕", "🍵", "🍺",
                "🍻", "🥂", "🍷", "🧃",
            ),
        ),
        ReactionGroup(
            "Activities & objects",
            listOf(
                "🎉", "🎊", "🎈", "🎁", "🏆", "🥇", "🏅", "🎯", "🎮", "🎲",
                "🎵", "🎶", "🎤", "🎧", "📷", "💡", "📌", "📎", "🔒", "🔑",
                "🔔", "📣", "⏰", "⌛", "💻", "📱", "🛠️", "🔧", "🔨", "⚙️",
                "🧪", "🐛", "📝", "📚", "💰", "💸", "📈", "📉", "🚀", "🛸",
            ),
        ),
        ReactionGroup(
            "Symbols",
            listOf(
                "✅", "☑️", "✔️", "❌", "❎", "➕", "➖", "❓", "❗", "‼️",
                "⁉️", "⚠️", "🚫", "⛔", "🆗", "🆕", "🆒", "🆙", "🔝", "♻️",
                "🔄", "⏩", "⏪", "⏸️", "▶️", "⏹️", "🔴", "🟠", "🟡", "🟢",
                "🔵", "🟣", "⚫", "⚪", "🏁", "🚩",
            ),
        ),
    )

    fun withoutVariationSelectors(emoji: String): String {
        val out = StringBuilder()
        var i = 0
        while (i < emoji.length) {
            val cp = emoji.codePointAt(i)
            if (cp != 0xFE0F && cp != 0xFE0E) out.appendCodePoint(cp)
            i += Character.charCount(cp)
        }
        return out.toString()
    }

    fun hasEmoji(value: String): Boolean = entries.any { it.value == value }

    fun emoji(value: String): String = entries.firstOrNull { it.value == value }?.emoji ?: value

    fun label(value: String): String = entries.firstOrNull { it.value == value }?.label ?: value

    fun valueForEmoji(emoji: String): String {
        val bare = withoutVariationSelectors(emoji)
        return entries.firstOrNull { withoutVariationSelectors(it.emoji) == bare }?.value ?: emoji
    }
}

data class StickerEntry(val slug: String, val text: String, val color: Long, val tilt: Float, val fallback: String)

object StickerCatalog {
    val entries: List<StickerEntry> = listOf(
        StickerEntry("lgtm", "LGTM", 0xFF8250DF, -6f, "✅"),
        StickerEntry("ship-it", "Ship it", 0xFF0969DA, 5f, "🚀"),
        StickerEntry("nice", "Nice!", 0xFFBF3989, -4f, "👍"),
        StickerEntry("thanks", "Thanks!", 0xFFBC4C00, 6f, "🙏"),
        StickerEntry("on-it", "On it", 0xFF57606A, -5f, "👀"),
        StickerEntry("wow", "Wow!", 0xFFCF222E, 4f, "😮"),
        StickerEntry("love-it", "Love it", 0xFFD6336C, -3f, "❤️"),
        StickerEntry("party", "Party!", 0xFF9A6700, 7f, "🎉"),
    )

    fun entry(slug: String): StickerEntry? = entries.firstOrNull { it.slug == slug }

    fun isKnown(slug: String): Boolean = entry(slug) != null

    fun index(slug: String): Int = entries.indexOfFirst { it.slug == slug }.let { if (it < 0) entries.size else it }

    fun canOffer(conversation: String): Boolean =
        (conversation.startsWith("#") || conversation.startsWith("@") || DirectMessageFrames.isDirect(conversation)) &&
            conversation != MESH_CONVERSATION && !OfficeFrames.isOffice(conversation)
}

interface ChatFrameSink {
    val selfId: String

    fun makeFrame(type: String): LinkedHashMap<String, Any?>

    fun sendFrame(frame: Map<String, Any?>): Boolean

    fun applyFrame(frame: Map<String, Any?>)

    fun sendMessage(text: String, to: String): Boolean
}

class RelayFrameSink(private val relay: RelayRoom) : ChatFrameSink {
    override val selfId: String get() = relay.selfId

    override fun makeFrame(type: String): LinkedHashMap<String, Any?> = relay.makeFrame(type)

    override fun sendFrame(frame: Map<String, Any?>): Boolean = relay.sendFrame(frame)

    override fun applyFrame(frame: Map<String, Any?>) = relay.applyFrame(frame)

    override fun sendMessage(text: String, to: String): Boolean = relay.sendMessage(text, to)
}

object ChatFrameText {
    fun bounded(value: Any?, max: Int): String = jsonText(value).take(max)

    fun conversation(frame: Map<String, Any?>): String {
        val conversation = jsonText(frame["conversation"])
        if (conversation.isNotEmpty()) return conversation
        return jsonText(frame["channel"])
    }

    fun isChannel(conversation: String): Boolean = conversation.startsWith("#")
}

fun compareChatMessages(a: ChatMessage, b: ChatMessage): Int {
    val byTimestamp = a.timestamp.compareTo(b.timestamp)
    if (byTimestamp != 0) return byTimestamp
    return a.id.compareTo(b.id)
}

internal fun insertSorted(list: List<ChatMessage>, message: ChatMessage): MutableList<ChatMessage> {
    val next = ArrayList<ChatMessage>(list.size + 1)
    next.addAll(list)
    var at = next.size
    while (at > 0 && compareChatMessages(next[at - 1], message) > 0) at--
    next.add(at, message)
    return next
}
