package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.Member
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.nio.file.Files
import java.util.Base64

class ChatExtrasTest {
    fun testAPeerThreadReplyBuildsTheThreadAndStaysOutOfTheChannel() {
        val (hub, relay) = hubWith()
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "root"), id = "root-1"))
        relay.handle(peerFrame("thread-reply", mapOf("channel" to "#general", "rootId" to "root-1", "text" to "first", "ts" to 10L), id = "tr-1"))
        relay.handle(peerFrame("thread-reply", mapOf("channel" to "#general", "rootId" to "root-1", "text" to "second", "ts" to 20L), id = "tr-2", sender = "peer-2"))
        val rows = hub.rows("#general")
        assertEq(1, rows.size)
        val summary = rows[0].thread!!
        assertEq(2, summary.replyCount)
        assertEq("2 replies", summary.label)
        assertEq(listOf("Pat"), summary.faces)
        assertEq(listOf("first", "second"), hub.threadRows("root-1").map { it.text })
    }

    fun testReplyingInAThreadSendsAThreadReplyFrame() {
        val (hub, relay) = hubWith()
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "question"), id = "root-2"))
        expect(hub.replyInThread("#general", "root-2", "answer"))
        assertEq(listOf("answer"), hub.threadRows("root-2").map { it.text })
        expect(hub.threadRows("root-2")[0].deletable)
        val frame = relay.sentOfType("thread-reply").single()
        assertEq("root-2", frame["rootId"])
        assertEq(true, frame["persist"])
        relay.online = false
        expect(!hub.replyInThread("#general", "root-2", "offline"))
        assertEq(1, relay.connects)
    }

    fun testDeleteForMeHidesMessagesOnThisDeviceOnly() {
        val (hub, relay) = hubWith()
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "keep"), id = "keep-1"))
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "root"), id = "root-3"))
        relay.handle(peerFrame("thread-reply", mapOf("channel" to "#general", "rootId" to "root-3", "text" to "reply", "ts" to 10L), id = "reply-3"))
        assertEq(1, hub.rows("#general").firstOrNull { it.message.id == "root-3" }?.thread?.replyCount)
        hub.hideMessage(hub.threadRows("root-3").single().message)
        expect(hub.threadRows("root-3").isEmpty())
        assertEq(null, hub.rows("#general").firstOrNull { it.message.id == "root-3" }?.thread)
        hub.hideMessage(hub.rows("#general").first { it.message.id == "root-3" }.message)
        assertEq(listOf("keep"), hub.rows("#general").map { it.text })
        assertEq(listOf("reply-3", "root-3"), hub.historySnapshot().hiddenMessages)
        expect(relay.sentOfType("delete").isEmpty())
    }

    fun testHiddenMessagesRestoreAndStayBounded() {
        val extras = ChatExtras()
        extras.restoreHiddenMessages(listOf("a", "b", "a", ""))
        assertEq(listOf("a", "b"), extras.snapshotHiddenMessages())
        val revision = extras.revision
        extras.hideMessage("b")
        assertEq(revision, extras.revision)
        repeat(ChatLimits.HIDDEN_MESSAGES) { extras.hideMessage("m$it") }
        assertEq(ChatLimits.HIDDEN_MESSAGES, extras.snapshotHiddenMessages().size)
        expect(!extras.isHidden("a"))
        expect(!extras.isHidden("b"))
        expect(extras.isHidden("m0"))
    }

    fun testReactionsToggleAndRefuseASpoofedReactor() {
        val (hub, relay) = hubWith()
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "ship it"), id = "m-1"))
        val message = hub.rows("#general")[0].message
        hub.toggleReaction(message, "like")
        assertEq(listOf("like"), hub.rows("#general")[0].reactions.map { it.value })
        expect(hub.rows("#general")[0].reactions[0].mine)
        assertEq("👍", hub.rows("#general")[0].reactions[0].emoji)
        hub.toggleReaction(message, "like")
        expect(hub.rows("#general")[0].reactions.isEmpty())
        relay.handle(peerFrame("reaction", mapOf("conversation" to "#general", "target" to "m-1", "emoji" to "❤", "reactorId" to "someone-else", "added" to true)))
        relay.handle(
            peerFrame(
                "reaction",
                mapOf("conversation" to "#general", "target" to "m-1", "emoji" to "❤️", "reactorId" to "peer-1", "reactorName" to "Pat", "added" to true),
            ),
        )
        assertEq(listOf("love"), hub.rows("#general")[0].reactions.map { it.value })
        assertEq(listOf("Pat"), hub.rows("#general")[0].reactions[0].names)
    }

    fun testAFailedReactionSendIsUndone() {
        val (hub, relay) = hubWith()
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "x"), id = "m-9"))
        relay.online = false
        hub.toggleReaction(hub.rows("#general")[0].message, "rocket")
        expect(hub.rows("#general")[0].reactions.isEmpty())
    }

    private fun sticker(slug: String, emoji: String, added: Boolean = true, reactor: String = "peer-1", target: String = "m-1"): Map<String, Any?> =
        peerFrame(
            "reaction",
            mapOf(
                "conversation" to "#general", "target" to target, "emoji" to emoji, "sticker" to slug,
                "reactorId" to reactor, "reactorName" to "Pat", "added" to added,
            ),
            sender = reactor,
        )

    fun testAPeerStickerArrivesAndComesOffAgain() {
        val (hub, relay) = hubWith()
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "ready"), id = "m-1"))
        relay.handle(sticker("lgtm", "✅"))
        val chip = hub.rows("#general")[0].stickers.single()
        assertEq("lgtm", chip.slug)
        assertEq("LGTM", chip.text)
        assertEq(-6f, chip.entry?.tilt)
        assertEq(0xFF8250DF, chip.entry?.color)
        assertEq(listOf("Pat"), chip.names)
        expect(!chip.mine)
        expect(hub.rows("#general")[0].reactions.isEmpty())
        relay.handle(sticker("lgtm", "✅"))
        assertEq(1, hub.rows("#general")[0].stickers.single().count)
        relay.handle(sticker("lgtm", "✅", reactor = "intruder").plus("senderId" to "peer-1"))
        assertEq(1, hub.rows("#general")[0].stickers.single().count)
        relay.handle(sticker("lgtm", "✅", added = false))
        expect(hub.rows("#general")[0].stickers.isEmpty())
    }

    fun testTogglingAStickerSendsItsFallbackAndTurnsItOff() {
        val (hub, relay) = hubWith()
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "ship"), id = "m-1"))
        relay.handle(sticker("lgtm", "✅"))
        val message = hub.rows("#general")[0].message
        hub.toggleSticker(message, "lgtm")
        val sent = relay.sentOfType("reaction").single()
        assertEq("m-1", sent["target"])
        assertEq("✅", sent["emoji"])
        assertEq("lgtm", sent["sticker"])
        assertEq("me-node", sent["reactorId"])
        assertEq(true, sent["added"])
        assertEq(true, sent["persist"])
        assertEq("#general", sent["conversation"])
        val chip = hub.rows("#general")[0].stickers.single()
        assertEq(2, chip.count)
        expect(chip.mine)
        assertEq("LGTM ×2", chip.label)
        assertEq(listOf("Me", "Pat"), chip.names)
        hub.toggleSticker(message, "lgtm")
        assertEq(false, relay.sentOfType("reaction").last()["added"])
        assertEq(1, hub.rows("#general")[0].stickers.single().count)
        expect(!hub.rows("#general")[0].stickers.single().mine)
        hub.toggleSticker(message, "not-a-sticker")
        assertEq(2, relay.sentOfType("reaction").size)
        relay.online = false
        hub.toggleSticker(message, "party")
        assertEq(listOf("lgtm"), hub.rows("#general")[0].stickers.map { it.slug })
    }

    fun testAStickerAndTheSameEmojiReactionNeverUndoEachOther() {
        val (hub, relay) = hubWith()
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "launch"), id = "m-1"))
        relay.handle(sticker("party", "🎉"))
        relay.handle(peerFrame("reaction", mapOf("conversation" to "#general", "target" to "m-1", "emoji" to "🎉", "reactorId" to "peer-1", "added" to true)))
        assertEq(listOf("party"), hub.rows("#general")[0].stickers.map { it.slug })
        assertEq(listOf("celebrate"), hub.rows("#general")[0].reactions.map { it.value })
        relay.handle(peerFrame("reaction", mapOf("conversation" to "#general", "target" to "m-1", "emoji" to "🎉", "reactorId" to "peer-1", "added" to false)))
        assertEq(listOf("party"), hub.rows("#general")[0].stickers.map { it.slug })
        expect(hub.rows("#general")[0].reactions.isEmpty())
        relay.handle(peerFrame("reaction", mapOf("conversation" to "#general", "target" to "m-1", "emoji" to "🎉", "reactorId" to "peer-1", "added" to true)))
        relay.handle(sticker("party", "🎉", added = false))
        expect(hub.rows("#general")[0].stickers.isEmpty())
        assertEq(listOf("celebrate"), hub.rows("#general")[0].reactions.map { it.value })
    }

    fun testAnUnknownStickerShowsAsItsFallbackReaction() {
        val (hub, relay) = hubWith()
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "x"), id = "m-1"))
        relay.handle(sticker("moon-shot", "🚀"))
        val row = hub.rows("#general")[0]
        expect(row.stickers.isEmpty())
        assertEq(listOf("rocket"), row.reactions.map { it.value })
        relay.handle(sticker("moon-shot", "🚀", added = false))
        expect(hub.rows("#general")[0].reactions.isEmpty())
    }

    fun testStickersShowInCatalogOrder() {
        val (hub, relay) = hubWith()
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "x"), id = "m-1"))
        relay.handle(sticker("party", "🎉"))
        relay.handle(sticker("wow", "😮", reactor = "peer-2"))
        relay.handle(sticker("lgtm", "✅", reactor = "peer-3"))
        assertEq(listOf("lgtm", "wow", "party"), hub.rows("#general")[0].stickers.map { it.slug })
        assertEq(
            listOf("lgtm", "ship-it", "nice", "thanks", "on-it", "wow", "love-it", "party"),
            StickerCatalog.entries.map { it.slug },
        )
        assertEq("❤️", StickerCatalog.entry("love-it")?.fallback)
        for (entry in StickerCatalog.entries) expect(ReactionCatalog.hasEmoji(ReactionCatalog.valueForEmoji(entry.fallback)), entry.slug)
    }

    fun testStickersAreOfferedOnlyInRelayRoomsAndDirectMessages() {
        val (hub, _) = hubWith()
        expect(hub.canSticker("#general"))
        expect(hub.canSticker("@peer-1"))
        expect(!hub.canSticker(MESH_CONVERSATION))
        expect(!hub.canSticker(OfficeFrames.conversation("design")))
        expect(!hub.canSticker(""))
    }

    fun testStickersPersistWithTheHistory() {
        val dir = Files.createTempDirectory("chat-history").toFile()
        try {
            val (hub, relay) = hubWith()
            hub.attachHistory(ChatHistoryStore(dir)) { "scope-s" }
            relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "keep me"), id = "m-1"))
            relay.handle(sticker("thanks", "🙏"))
            hub.toggleSticker(hub.rows("#general")[0].message, "nice")
            hub.saveHistory()
            val (other, _) = hubWith()
            other.attachHistory(ChatHistoryStore(dir)) { "scope-s" }
            val row = other.rows("#general").single()
            assertEq(listOf("nice", "thanks"), row.stickers.map { it.slug })
            expect(row.stickers.first().mine)
            expect(row.reactions.isEmpty())
        } finally {
            dir.listFiles()?.forEach { it.delete() }
            dir.delete()
        }
    }

    fun testOnlyTheAuthorCanEdit() {
        val (hub, relay) = hubWith()
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "original"), id = "m-2"))
        relay.handle(peerFrame("edit", mapOf("conversation" to "#general", "target" to "m-2", "text" to "forged"), sender = "intruder"))
        relay.handle(peerFrame("edit", mapOf("conversation" to "#general", "target" to "m-2", "text" to "fixed")))
        val first = hub.rows("#general")[0]
        expect(first.edited)
        assertEq("fixed", first.text)
        expect(!first.editable)
        assertEq(ChatHubSend.Sent, hub.sendText("#general", "mine"))
        val mine = hub.rows("#general").last()
        expect(mine.editable)
        expect(hub.edit(mine.message, "mine, edited"))
        assertEq("mine, edited", hub.rows("#general").last().text)
        expect(!hub.edit(first.message, "not mine"))
    }

    fun testDeletesFollowTheAuthor() {
        val (hub, relay) = hubWith()
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "root"), id = "r-1"))
        relay.handle(peerFrame("thread-reply", mapOf("channel" to "#general", "rootId" to "r-1", "text" to "a"), id = "t-1"))
        relay.handle(peerFrame("delete", mapOf("conversation" to "#general", "target" to "t-1"), sender = "intruder"))
        assertEq(1, hub.extras.replies("r-1").size)
        relay.handle(peerFrame("delete", mapOf("conversation" to "#general", "target" to "t-1")))
        expect(hub.extras.replies("r-1").isEmpty())
        expect(hub.rows("#general")[0].thread == null)
    }

    fun testTypingShowsAndExpires() {
        val clock = TestClock()
        val (hub, relay) = hubWith(clock = clock)
        relay.handle(peerFrame("typing", mapOf("conversation" to "#general", "active" to true)))
        assertEq("Pat is typing...", hub.typingLine("#general"))
        clock.ms += ChatLimits.TYPING_TIMEOUT_MS + 1
        assertEq("", hub.typingLine("#general"))
        assertEq("A and B are typing...", ChatExtras.typingLabel(listOf("A", "B")))
        assertEq("3 people are typing...", ChatExtras.typingLabel(listOf("A", "B", "C")))
        hub.composerChanged("hello")
        hub.composerChanged("hello again")
        assertEq(1, relay.sentOfType("typing").size)
        hub.composerChanged("")
        assertEq(2, relay.sentOfType("typing").size)
        assertEq(false, relay.sentOfType("typing").last()["active"])
        expect(relay.sentOfType("typing").none { it.containsKey("channel") })
    }

    fun testAnInviteJoinsAPrivateChannelAndItsMessagesAreMarkedPrivate() {
        val (hub, relay) = hubWith()
        relay.handle(peerFrame("invite", mapOf("to" to relay.selfId, "channel" to "secret")))
        expect("#secret" in hub.channels)
        expect(hub.extras.isPrivate("#secret"))
        expect(hub.extras.inviteNotice != null)
        hub.select("#secret")
        expect(hub.isPrivateRoom("#secret"))
        assertEq("end-to-end", hub.badge("#secret"))
        expect(!hub.canAskForkbot("#secret"))
        assertEq(ChatHubSend.Sent, hub.sendText("#secret", "psst"))
        assertEq(listOf("psst"), hub.rows("#secret").map { it.text })
        expect(relay.sent.any { it["private"] == true && it["text"] == "psst" })
        hub.extras.removeChannel("#secret")
        expect("#secret" !in hub.channels)
        relay.handle(peerFrame("invite", mapOf("to" to "someone-else", "channel" to "other")))
        expect("#other" !in hub.channels)
    }

    fun testChannelNamesAndCreation() {
        val (hub, relay) = hubWith()
        expect(ChatExtras.validChannelName("team-notes"))
        expect(!ChatExtras.validChannelName("#team"))
        expect(!ChatExtras.validChannelName("has space"))
        assertEq("#plans", hub.extras.createPrivateChannel("Plans"))
        expect("#plans" in hub.channels)
        assertEq("#lobby", hub.extras.addPublicChannel("lobby"))
        expect("#lobby" in hub.channels)
        assertEq("#lobby", relay.sentOfType("channel").single()["name"])
        expect(hub.extras.invite("peer-1", "#plans"))
        expect(!hub.extras.invite("peer-1", "#lobby"))
        hub.extras.removeChannel("#general")
        expect("#general" in hub.channels)
    }

    fun testFilesArriveAsAttachmentsWithinBudget() {
        val (hub, relay) = hubWith()
        val png = byteArrayOf(0x89.toByte(), 0x50, 0x4E, 0x47)
        relay.handle(
            peerFrame(
                "chat",
                mapOf("channel" to "#general", "fileName" to "shot.png", "fileMime" to "image/png", "file" to Base64.getEncoder().encodeToString(png)),
                id = "f-1",
            ),
        )
        val attachment = hub.rows("#general")[0].attachment!!
        assertEq(ChatAttachmentKind.IMAGE, attachment.kind)
        expect(attachment.data.contentEquals(png))
        assertEq(ChatAttachmentKind.AUDIO, ChatAttachmentKind.of("note.m4a", ""))
        assertEq(ChatAttachmentKind.VIDEO, ChatAttachmentKind.of("clip.mov", ""))
        expect(!hub.sendAttachment("#general", "big.bin", "", ByteArray(ChatLimits.MAX_SEND_FILE_BYTES + 1)))
        expect(hub.sendAttachment("#general", "small.txt", "text/plain", "hi".toByteArray()))
        assertEq("small.txt", hub.rows("#general").last().attachment?.fileName)
        expect(!hub.sendAttachment(MESH_CONVERSATION, "x.txt", "text/plain", "hi".toByteArray()))
    }

    fun testHistoryFramesFromAPeerFillTheChannel() {
        val (hub, relay) = hubWith()
        val entries = listOf(
            peerFrame("chat", mapOf("channel" to "#random", "text" to "old news"), id = "h-1"),
            peerFrame("chat", mapOf("channel" to "#hidden-room", "text" to "nope", "private" to true), id = "h-2"),
            peerFrame("chat", mapOf("channel" to "#team", "text" to "team"), id = "h-3"),
        )
        relay.handle(peerFrame("history", mapOf("to" to relay.selfId, "entries" to entries)))
        assertEq(1, relay.messages("#random").size)
        expect(relay.messages("#hidden-room").isEmpty())
        expect(relay.messages("#team").isEmpty())
        expect(hub.rows("#random").isNotEmpty())
    }

    fun testHistoryPersistsAndRestores() {
        val dir = Files.createTempDirectory("chat-history").toFile()
        try {
            val (hub, relay) = hubWith()
            hub.attachHistory(ChatHistoryStore(dir)) { "scope-a" }
            relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "keep me"), id = "p-1"))
            relay.handle(peerFrame("thread-reply", mapOf("channel" to "#general", "rootId" to "p-1", "text" to "reply"), id = "p-2"))
            hub.toggleReaction(hub.rows("#general")[0].message, "rocket")
            hub.extras.createPrivateChannel("vault")
            hub.saveHistory()

            val (other, _) = hubWith()
            other.attachHistory(ChatHistoryStore(dir)) { "scope-a" }
            assertEq(listOf("keep me"), other.rows("#general").map { it.text })
            assertEq(listOf("rocket"), other.rows("#general").first().reactions.map { it.value })
            assertEq(1, other.rows("#general").first().thread?.replyCount)
            expect(other.extras.isPrivate("#vault"))
            expect("#vault" in other.channels)
        } finally {
            dir.listFiles()?.forEach { it.delete() }
            dir.delete()
        }
    }

    fun testANotificationOpenAsksTheScreenToRevealTheConversation() {
        val (hub, relay) = hubWith()
        var reveals = 0
        hub.onReveal = { reveals++ }
        assertEq(0, hub.revealRequests)
        hub.select("#random")
        assertEq(0, hub.revealRequests, "picking a room inside Chat reveals nothing")
        hub.switchConversation("#random")
        assertEq(1, hub.revealRequests, "the same room opened from a notification still closes what covers it")
        hub.switchConversation("#ops")
        assertEq(2, hub.revealRequests)
        assertEq(2, reveals)
        assertEq("#ops", relay.currentConversation)
    }

    fun testSlashCommandsAreHandledBeforeSending() {
        val (hub, _) = hubWith()
        val notices = ArrayList<String>()
        hub.onNotice = { notices.add(it) }
        assertEq(ChatHubSend.Issue("alice/tool Broken build"), hub.sendText("#general", "/issue alice/tool Broken build"))
        expect(hub.rows("#general").isEmpty())
        expect(hub.sendText("#general", "/topic hello") is ChatHubSend.Failed)
        assertEq("/topic is available in managed Office channels.", notices.last())
    }

    fun testHistoryCodecBoundsEachConversation() {
        val many = (0 until ChatLimits.HISTORY_ENTRIES_PER_CHANNEL + 20).map {
            ChatMessage(id = "m$it", conversation = "#general", senderId = "a", senderName = "A", text = "x", timestamp = it.toLong())
        }
        val mesh = ChatMessage(id = "mesh", conversation = MESH_CONVERSATION, senderId = "a", senderName = "A", text = "x", timestamp = 1)
        val kept = ChatHistoryCodec.bounded(mapOf("#general" to many, MESH_CONVERSATION to listOf(mesh)))
        assertEq(ChatLimits.HISTORY_ENTRIES_PER_CHANNEL, kept.size)
        assertEq("m${ChatLimits.HISTORY_ENTRIES_PER_CHANNEL + 19}", kept.last().id)
        val snapshot = ChatHistorySnapshot(
            messages = kept.take(2),
            edits = mapOf("m0" to "edited"),
            reactions = mapOf("m0" to mapOf("like" to mapOf("a" to "A"))),
            reactionOrder = mapOf("m0" to listOf("like")),
            privateChannels = listOf("#p"),
            hiddenMessages = listOf("m1"),
        )
        assertEq(snapshot, ChatHistoryCodec.decode(ChatHistoryCodec.encode(snapshot)))
        expect(ChatHistoryCodec.scopeName("a", "b") != ChatHistoryCodec.scopeName("a", "c"))
        assertEq("af63f14c8602103b", ChatHistoryCodec.scopeName("", ""))
        assertEq(null, ChatHistoryCodec.decode("{\"v\":0}"))
    }

    fun testReactionCatalogTranslatesBothWays() {
        assertEq("love", ReactionCatalog.valueForEmoji("❤"))
        assertEq("love", ReactionCatalog.valueForEmoji("❤️"))
        assertEq("✅", ReactionCatalog.emoji("check"))
        assertEq("🦄", ReactionCatalog.valueForEmoji("🦄"))
        assertEq("Done", ReactionCatalog.label("check"))
    }

    fun testReactionGroupsOfferEveryCatalogEmojiOnce() {
        val all = ReactionCatalog.groups.flatMap { it.emoji }
        expect(all.size >= 300, "the picker offers a few hundred reactions")
        assertEq(all.size, all.map(ReactionCatalog::withoutVariationSelectors).toSet().size)
        for (emoji in all) expect(emoji.codePointCount(0, emoji.length) in 1..16, emoji)
        val bare = all.map(ReactionCatalog::withoutVariationSelectors).toSet()
        for (entry in ReactionCatalog.entries) expect(ReactionCatalog.withoutVariationSelectors(entry.emoji) in bare, entry.value)
        assertEq("love", ReactionCatalog.valueForEmoji(all.first { ReactionCatalog.withoutVariationSelectors(it) == "❤" }))
    }

    fun testReplyTargetsResolveWithoutSwitching() {
        val (hub, relay) = hubWith()
        relay.peers.add(Member(id = "peer-9", name = "pat", online = true, owner = "pat", nodeName = "pat-laptop"))
        hub.reply("pat", "on my way")
        assertEq("#general", relay.currentConversation)
        assertEq(listOf("on my way"), relay.messages("@peer-9").map { it.text })
        hub.reply("random", "hi all")
        assertEq(listOf("hi all"), relay.messages("#random").map { it.text })
        try {
            hub.reply("nobody", "x")
            expect(false, "an unknown conversation must throw")
        } catch (error: ChatReplyException) {
            assertEq(ChatReplyException.Kind.UNKNOWN_CONVERSATION, error.kind)
        }
        assertEq("#x", ChatReplyTarget.resolve("#x", emptyList(), emptyList()))
        assertEq("@peer-9", ChatReplyTarget.resolve("@pat", emptyList(), relay.roster()))
        assertEq("@stranger", ChatReplyTarget.resolve("@stranger", emptyList(), relay.roster()))
        assertEq("#office/design", ChatReplyTarget.resolve("design", emptyList(), emptyList(), listOf("#office/design")))
        hub.switchConversation("@pat")
        assertEq("@peer-9", hub.currentConversation)
        relay.online = false
        try {
            hub.reply("random", "x")
            expect(false, "offline must throw")
        } catch (error: ChatReplyException) {
            assertEq(ChatReplyException.Kind.OFFLINE, error.kind)
        }
    }

    fun testForkbotAnswersOnlyInPublicChannels() {
        val (hub, relay) = hubWith()
        val api = FakeAccountApi()
        api.replies["POST /api/forkbot/chat"] = mapOf("botMessage" to "Hello from ForkBot")
        hub.forkbot = api
        val notices = ArrayList<String>()
        hub.onNotice = { notices.add(it) }
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "context line"), id = "c-1"))
        assertEq(ChatHubSend.Sent, hub.sendText("#general", "@forkbot what is up"))
        expect(hub.rows("#general").any { it.message.senderId == "forkbot" })
        val body = api.calls.first().third!!
        assertEq(1, (body["context"] as List<*>).size)
        assertEq("@forkbot what is up", body["message"])
        assertEq("Me", body["sender"])
        hub.extras.createPrivateChannel("quiet")
        hub.select("#quiet")
        hub.sendText("#quiet", "forkbot?")
        assertEq(1, api.calls.size)
        assertEq(ChatHub.FORKBOT_PRIVATE_NOTICE, notices.last())
        hub.sendText("#general", "no bot here")
        assertEq(1, api.calls.size)
    }

    fun testRowsFlagVisitorsAndSealSummaries() {
        val (hub, relay) = hubWith()
        relay.handle(linkedMapOf("type" to "chat", "id" to "v-1", "senderId" to "g1", "sender" to "Guest 4411", "ts" to 5L, "channel" to "#general", "text" to "hi"))
        expect(hub.rows("#general")[0].visitor)
        assertEq(ChatHub.PUBLIC_SEAL_SUMMARY, hub.sealSummary("#general"))
        assertEq(ChatHub.PRIVATE_SEAL_SUMMARY, hub.sealSummary("@peer"))
        assertEq("", hub.sealSummary(MESH_CONVERSATION))
        assertEq("public", hub.badge("#general"))
        val names = hub.mentionNames("#general")
        expect("Guest 4411" in names)
        expect("Me" !in names)
        expect(jsonText(relay.makeFrame("x")["type"]) == "x")
    }
}
