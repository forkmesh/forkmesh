package com.forkmesh.nativeapp.apps

import com.forkmesh.nativeapp.chat.ChatSlashCommand
import com.forkmesh.nativeapp.model.ChatMessage
import java.util.concurrent.atomic.AtomicInteger

class ChatApps(
    private val api: AppsApi,
    private val background: (() -> Unit) -> Unit = { task -> Thread(task, "ChatApps").apply { isDaemon = true }.start() },
    private val now: () -> Long = System::currentTimeMillis,
) {
    companion object {
        const val MAX_EPHEMERAL_PER_CONVERSATION = 50
        const val FALLBACK_BOT_NAME = "App"
    }

    private val lock = Any()
    private val ephemeral = HashMap<String, List<ChatMessage>>()
    private val sequence = AtomicInteger()

    @Volatile
    var commands: List<AppCommand> = emptyList()
        private set

    @Volatile
    var mentionable: List<String> = emptyList()
        private set

    @Volatile
    var loaded: Boolean = false
        private set

    @Volatile
    var onChange: (() -> Unit)? = null

    @Volatile
    var onNotice: ((String) -> Unit)? = null

    @Volatile
    var enabled: () -> Boolean = { true }

    private fun notice(text: String) {
        if (text.isNotEmpty()) onNotice?.invoke(text)
    }

    fun load(): Boolean = if (!enabled()) false else try {
        val fetched = api.commands()
        commands = fetched.commands
        mentionable = fetched.mentionable
        loaded = true
        onChange?.invoke()
        true
    } catch (_: Exception) {
        false
    }

    fun ensureLoaded() {
        if (!loaded) load()
    }

    fun reset() {
        commands = emptyList()
        mentionable = emptyList()
        loaded = false
        synchronized(lock) { ephemeral.clear() }
        onChange?.invoke()
    }

    fun suggestions(text: String): List<ChatSlashCommand> {
        if (!enabled() || !text.startsWith("/") || !AppsText.isCommandPrefix(text)) return emptyList()
        val prefix = text.lowercase()
        val all = ArrayList<ChatSlashCommand>()
        all.add(ChatSlashCommand("/${AppsText.APPS_COMMAND}", "/${AppsText.APPS_COMMAND}", "List your app commands"))
        for (entry in commands) {
            val usage = if (entry.usageHint.isEmpty()) "/${entry.command}" else "/${entry.command} ${entry.usageHint}"
            val summary = listOf(entry.description, entry.appName).filter { it.isNotEmpty() }.joinToString(" · ")
            all.add(ChatSlashCommand("/${entry.command}", usage, summary))
        }
        return all.filter { it.command.startsWith(prefix) }
    }

    fun handle(text: String, conversation: String, channelId: String): Boolean {
        if (!enabled()) return false
        val slash = AppsText.parseSlash(text) ?: return false
        if (slash.command == AppsText.APPS_COMMAND) {
            ensureLoaded()
            notice(AppsText.commandList(commands))
            return true
        }
        ensureLoaded()
        if (commands.none { it.command == slash.command }) return false
        val reply = try {
            api.run(text.trim(), channelId)
        } catch (_: Exception) {
            AppReply(ok = false)
        }
        show(reply, conversation)
        return true
    }

    fun interact(message: ChatMessage, actionId: String, value: String, channelId: String): AppReply? {
        if (message.appMessageId.isEmpty() || actionId.isEmpty()) return null
        val reply = try {
            api.interact(message.appMessageId, channelId, actionId, value)
        } catch (_: Exception) {
            AppReply(ok = false)
        }
        show(reply, message.conversation)
        return reply
    }

    private fun show(reply: AppReply, conversation: String) {
        if (reply.isEphemeral) {
            addEphemeral(reply, conversation)
        } else if (!reply.ok) {
            notice(reply.errorText)
        }
    }

    fun addEphemeral(reply: AppReply, conversation: String): ChatMessage? {
        if (conversation.isEmpty() || (reply.text.isEmpty() && reply.blocks.isEmpty())) return null
        val at = now()
        val message = ChatMessage(
            id = "app-ephemeral-$at-${sequence.incrementAndGet()}",
            conversation = conversation,
            senderId = "app:${reply.appSlug}",
            senderName = reply.botName.ifEmpty { FALLBACK_BOT_NAME },
            text = reply.text,
            timestamp = at,
            blocks = reply.blocks,
            appMessageId = reply.appMessageId,
            ephemeral = true,
        )
        synchronized(lock) {
            ephemeral[conversation] = (ephemeral[conversation].orEmpty() + message).takeLast(MAX_EPHEMERAL_PER_CONVERSATION)
        }
        onChange?.invoke()
        return message
    }

    fun ephemerals(conversation: String): List<ChatMessage> = synchronized(lock) { ephemeral[conversation].orEmpty() }

    fun forwardMentions(text: String, channelId: String, messageId: String) {
        if (!enabled() || channelId.isEmpty() || messageId.isEmpty() || !AppsText.mentionsApp(text)) return
        background {
            ensureLoaded()
            if (!AppsText.shouldForward(text, mentionable)) return@background
            try {
                api.forwardMentions(channelId, messageId, text)
            } catch (_: Exception) {
            }
        }
    }

    fun merge(messages: List<ChatMessage>, conversation: String): List<ChatMessage> {
        val extra = ephemerals(conversation)
        if (extra.isEmpty()) return messages
        val out = ArrayList<ChatMessage>(messages.size + extra.size)
        var next = 0
        for (message in messages) {
            while (next < extra.size && extra[next].timestamp < message.timestamp) out.add(extra[next++])
            out.add(message)
        }
        while (next < extra.size) out.add(extra[next++])
        return out
    }
}
