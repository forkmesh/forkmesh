package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ChatComposerToolsTest {
    fun testMentionTokenFollowsTheDesktopRules() {
        assertEq("pa", ChatComposerTools.mentionToken("hi @pa")?.query)
        assertEq("", ChatComposerTools.mentionToken("@")?.query)
        assertEq(null, ChatComposerTools.mentionToken("mail@pa"))
        assertEq(null, ChatComposerTools.mentionToken("hi @pa there"))
        assertEq("hi @pat ", ChatComposerTools.insertMention("hi @pa", "pat"))
        assertEq("no token", ChatComposerTools.insertMention("no token", "pat"))
        assertEq(listOf("Pat", "Paul"), ChatComposerTools.mentionCandidates(listOf("Pat", "pat", "Paul", "me", "Zed"), "me", "pa"))
    }

    fun testSlashCommands() {
        assertEq(listOf("/issue"), ChatComposerTools.slashCommands("/", canSetTopic = false).map { it.command })
        assertEq(listOf("/topic"), ChatComposerTools.slashCommands("/t", canSetTopic = true).map { it.command })
        expect(ChatComposerTools.slashCommands("/issue x", canSetTopic = true).isEmpty())
        assertEq(ChatSlash.Issue("fix it"), ChatComposerTools.parseSlash("/issue  fix it "))
        assertEq(ChatSlash.Topic("new"), ChatComposerTools.parseSlash("/TOPIC new"))
        assertEq(null, ChatComposerTools.parseSlash("/shrug"))
        assertEq(null, ChatComposerTools.parseSlash("plain"))
    }

    fun testLinkInsertionMatchesTheDesktop() {
        val plain = ChatComposerTools.linkInsertion("see ", 4, 4)
        assertEq("see [link text](https://)", plain.text)
        assertEq(4 + 1 + 9 + 2, plain.selectionStart)
        assertEq(8, plain.selectionLength)
        val clip = ChatComposerTools.linkInsertion("", 0, 0, clipboard = " https://forkmesh.com ")
        assertEq("[link text](https://forkmesh.com)", clip.text)
        assertEq(1, clip.selectionStart)
        assertEq(9, clip.selectionLength)
        val both = ChatComposerTools.linkInsertion("docs", 0, 4, clipboard = "https://a.b")
        assertEq("[docs](https://a.b)", both.text)
        assertEq(0, both.selectionLength)
        assertEq("", ChatComposerTools.sanitizedHref("javascript:alert(1)"))
        assertEq("", ChatComposerTools.sanitizedHref("https://no spaces"))
    }

    fun testForkbotMentionAndContext() {
        expect(ChatComposerTools.mentionsForkbot("hey @ForkBot help"))
        expect(ChatComposerTools.mentionsForkbot("forkbot"))
        expect(!ChatComposerTools.mentionsForkbot("forkbots rule"))
        expect(!ChatComposerTools.mentionsForkbot("myforkbot"))
        val history = (0 until 20).map {
            ChatMessage(id = "m$it", conversation = "#general", senderId = if (it == 3) "forkbot" else "a", senderName = "A", text = "t$it", timestamp = it.toLong())
        }
        val context = ChatComposerTools.forkbotContext(history)
        assertEq(12, context.size)
        assertEq("t18", context.last()["text"])
        val thread = ChatComposerTools.forkbotContext(history, threadRootId = "m0", replies = listOf(history[5], history[6]))
        assertEq(listOf("t0", "t5"), thread.map { it["text"] })
    }

    fun testIssueDraftParsing() {
        val repos = listOf(RepoSlug("alice", "tool"), RepoSlug("forkmesh", "forkmesh"))
        val draft = (ChatIssueDraft.parse("alice/tool Crash on start\nmore detail", repos, null, "#general", "pat", 0) as ChatIssueDraft.Result.Ok).draft
        assertEq("alice/tool", draft.repo.slug)
        assertEq("Crash on start", draft.title)
        expect(draft.body.endsWith("Filed from a #general chat message by pat at 1970-01-01T00:00:00Z."), draft.body)
        val fallback = (ChatIssueDraft.parse("Just a title", repos, null, "", "", 0) as ChatIssueDraft.Result.Ok).draft
        assertEq("forkmesh/forkmesh", fallback.repo.slug)
        expect(fallback.body.contains("Filed from a chat message by someone"))
        assertEq(ChatIssueDraft.Result.Error(ChatIssueDraft.Failure.Usage), ChatIssueDraft.parse("", repos, null, "", "", 0))
        assertEq(
            ChatIssueDraft.Result.Error(ChatIssueDraft.Failure.UnknownRepository("bob/none")),
            ChatIssueDraft.parse("bob/none title", repos, null, "", "", 0),
        )
        assertEq(ChatIssueDraft.Result.Error(ChatIssueDraft.Failure.NoRepositories), ChatIssueDraft.parse("x", emptyList(), null, "", "", 0))
        val message = ChatMessage(id = "m", conversation = "#general", senderId = "a", senderName = "Pat", text = "line one\nline two", timestamp = 0)
        val quoted = ChatIssueDraft.fromMessage(message, message.text, RepoSlug("alice", "tool"), "#general")
        assertEq("line one", quoted.title)
        assertEq("> line one\n> line two\n\n---\nFrom a #general chat message by Pat at 1970-01-01T00:00:00Z.", quoted.body)
        expect(ChatIssueDraft.isRepoToken("a-b/c.d"))
        expect(!ChatIssueDraft.isRepoToken("Alice/tool"))
        assertEq(RepoSlug("a", "b"), RepoSlug.parse(" a/b "))
        assertEq(null, RepoSlug.parse("a/b/c"))
    }

    fun testVisitorsAndWelcome() {
        expect(ChatVisitorPresence.isTransientVisitor(name = "Guest 42"))
        expect(ChatVisitorPresence.isTransientVisitor(name = "World visitor"))
        expect(ChatVisitorPresence.isTransientVisitor(accountKind = "guest", name = "anyone"))
        expect(!ChatVisitorPresence.isTransientVisitor(name = "guesthouse"))
        expect(ChatVisitorPresence.isIdle(0, 600_001))
        expect(!ChatVisitorPresence.isIdle(null, 600_001))
    }

    fun testWelcomeIsOnlyForFreshVerifiedAccounts() {
        val prefs = MemoryPrefStore()
        val announcer = ChatWelcomeAnnouncer(prefs)
        val now = 10_000_000_000L
        expect(!announcer.shouldAnnounce("old", isGuest = false, emailVerified = true, createdAtMs = 1, nowMs = now, alreadyInRoom = false))
        expect(!announcer.shouldAnnounce("g", isGuest = true, emailVerified = true, createdAtMs = now, nowMs = now, alreadyInRoom = false))
        expect(announcer.shouldAnnounce("new", isGuest = false, emailVerified = true, createdAtMs = now - 1000, nowMs = now, alreadyInRoom = false))
        expect(announcer.shouldAnnounce("secs", isGuest = false, emailVerified = true, createdAtMs = now / 1000 - 5, nowMs = now, alreadyInRoom = false))
        announcer.markAnnounced("new")
        expect(!announcer.shouldAnnounce("new", isGuest = false, emailVerified = true, createdAtMs = now - 1000, nowMs = now, alreadyInRoom = false))
    }
}

class ChannelGroupsTest {
    fun testLayoutPutsUngroupedFirstThenGroups() {
        var state = ChannelGroups.createGroup(ChannelGroups.State(), "  Work  stuff ")!!
        assertEq(null, ChannelGroups.createGroup(state, "Work stuff"))
        val rows0 = ChannelGroups.layout(state, listOf("#general", "#dev", "#discord/1")) {
            if (it.startsWith("#discord/")) "Discord · Server" else ""
        }
        assertEq(listOf("channel:#general", "channel:#dev", "group:Work stuff", "group:Discord · Server", "channel:#discord/1"), rows0.map { it.id })
        state = ChannelGroups.assign(state, "#dev", "Work stuff", -1, emptyList(), false)!!
        val rows = ChannelGroups.layout(state, listOf("#general", "#dev"))
        assertEq(listOf("#dev"), ChannelGroups.renderedMembers(rows, "Work stuff"))
        state = ChannelGroups.setCollapsed(state, "Work stuff", collapsed = true, automatic = false)
        val visible = ChannelGroups.visibleRows(ChannelGroups.layout(state, listOf("#general", "#dev")))
        assertEq(listOf("channel:#general", "group:Work stuff"), visible.map { it.id })
        assertEq(listOf("Work stuff"), ChannelGroups.renderedGroupNames(rows))
    }

    fun testRenameRemoveAndRoundTrip() {
        var state = ChannelGroups.createGroup(ChannelGroups.State(), "A")!!
        state = ChannelGroups.assign(state, "#x", "A", 0, emptyList(), false)!!
        state = ChannelGroups.assign(state, "#y", "A", 0, listOf("#x"), false)!!
        assertEq(listOf("#y", "#x"), state.groups[0].channels)
        state = ChannelGroups.renameGroup(state, "A", "B", listOf("#y", "#x"))!!
        assertEq(state, ChannelGroups.fromJson(ChannelGroups.toJson(state)))
        state = ChannelGroups.removeGroup(state, "B", listOf("#y", "#x"))
        expect(state.groups.isEmpty())
        assertEq(listOf("#y", "#x"), state.ungrouped)
        state = ChannelGroups.purge(state, { it == "#x" }, { true })
        assertEq(listOf("#y"), state.ungrouped)
        assertEq(ChannelGroups.State(), ChannelGroups.fromJson("garbage"))
    }

    fun testStorePersists() {
        val prefs = MemoryPrefStore()
        val store = ChannelGroupsStore(prefs, "relay")
        store.update { ChannelGroups.createGroup(it, "Mine") }
        assertEq(listOf("Mine"), ChannelGroupsStore(prefs, "relay").state.groups.map { it.name })
        store.update { ChannelGroups.removeGroup(it, "Mine", emptyList()) }
        expect(!prefs.contains("chat/channelGroups/relay"))
    }

    fun testGroupsSurviveAProjectUrlChange() {
        val prefs = MemoryPrefStore()
        ChannelGroupsStore.forProject(prefs, "relay.example", "https://relay.example", "lobby")
            .update { ChannelGroups.assign(ChannelGroups.createGroup(it, "Ops")!!, "#deploy", "Ops", 0, emptyList(), false) }
        val moved = ChannelGroupsStore.forProject(prefs, "relay.example", "https://relay.example/relay/", "lobby")
        assertEq(listOf("Ops"), moved.state.groups.map { it.name })
        assertEq(listOf("#deploy"), moved.state.groups[0].channels)
        expect(ChannelGroupsStore.forProject(prefs, "other.example", "https://relay.example", "lobby").state.isEmpty)
        expect(ChannelGroupsStore.forProject(prefs, "relay.example", "https://relay.example", "annex").state.isEmpty)
    }

    fun testGroupsSavedUnderTheUrlAreAdopted() {
        val prefs = MemoryPrefStore()
        ChannelGroupsStore(prefs, ChatHistoryCodec.scopeName("https://relay.example", "lobby")).update { ChannelGroups.createGroup(it, "Old") }
        val store = ChannelGroupsStore.forProject(prefs, "relay.example", "https://relay.example", "lobby")
        assertEq(listOf("Old"), store.state.groups.map { it.name })
        expect(!prefs.contains(ChannelGroupsStore.keyFor(ChatHistoryCodec.scopeName("https://relay.example", "lobby"))))
        expect(prefs.contains(ChannelGroupsStore.keyFor(ChannelGroupsStore.projectScope("relay.example", "lobby"))))
        assertEq(listOf("Old"), ChannelGroupsStore.forProject(prefs, "relay.example", "https://relay.example/new", "lobby").state.groups.map { it.name })
    }
}
