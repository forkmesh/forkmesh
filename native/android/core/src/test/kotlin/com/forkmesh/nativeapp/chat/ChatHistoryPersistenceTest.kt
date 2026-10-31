package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AppEnv
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.ChatReceipt
import com.forkmesh.nativeapp.model.ChatReceiptTimes
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.nio.file.Files

class ChatHistoryPersistenceTest {
    private val legacyUrl = "wss://forkmesh.com/api/repo/mainnode/forkmesh/rooms/general/ws"
    private val nextUrl = "wss://app.next.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws"
    private val directConversation = DirectMessageFrames.PREFIX + "d".repeat(32)

    private fun note(id: String, conversation: String, ts: Long = System.currentTimeMillis()) =
        ChatMessage(id = id, conversation = conversation, senderId = "peer", senderName = "Pat", text = id, timestamp = ts)

    private fun snapshot(messages: List<ChatMessage>) = ChatHistorySnapshot(messages = ChatHistoryCodec.bounded(messages.groupBy { it.conversation }))

    private fun ids(load: ChatHistoryLoad): List<String>? = (load as? ChatHistoryLoad.Loaded)?.snapshot?.messages?.map { it.id }

    private fun withDir(name: String, body: (File) -> Unit) {
        val dir = Files.createTempDirectory(name).toFile()
        try {
            body(dir)
        } finally {
            dir.listFiles()?.forEach {
                it.setReadable(true)
                it.delete()
            }
            dir.delete()
        }
    }

    private fun direct() = DirectMessageService(
        ChatAccountCalling { _, _, _ -> emptyMap() }, FakeOfficeConnector(), MemorySecretStore(),
        { "https://app.forkmesh.com" }, { "me-node" }, { "Me" },
        loop = EventLoop("history-test", virtualTime = true),
    )

    private fun office() = OfficeChannelService(
        ChatAccountCalling { _, _, _ -> emptyMap() }, FakeOfficeConnector(), MemorySecretStore(),
        { "https://app.forkmesh.com" }, { "me-node" }, { "Me" },
    )

    fun testRestoresMergeOnceInOrderAndKeepTheNewest() {
        assertEq(
            listOf("a", "b", "c", "d", "e"),
            mergeSorted(listOf(note("b", "#g", 20), note("d", "#g", 40)), listOf(note("e", "#g", 40), note("c", "#g", 30), note("a", "#g", 10))).map { it.id },
        )
        val direct = direct()
        val count = DirectMessageFrames.MAX_MESSAGES_PER_ROOM + 50
        direct.restore((0 until count).reversed().map { note("d$it", directConversation, 1_000L + it) } + note("d7", directConversation, 1_007), emptyMap())
        assertEq((50 until count).map { "d$it" }, direct.messages(directConversation).map { it.id })
        val office = office()
        val room = "#office/general"
        val rooms = OfficeFrames.MAX_MESSAGES_PER_ROOM + 10
        office.restore((0 until rooms).reversed().map { note("o$it", room, 5_000L + it) })
        office.restore(listOf(note("o${rooms - 1}", room, 5_000L + rooms - 1)))
        assertEq((10 until rooms).map { "o$it" }, office.messages(room).map { it.id })
    }

    fun testAFileFromANewerOrOlderBuildStillLoads() {
        val root = LinkedHashMap(Json.parseObject(ChatHistoryCodec.encode(snapshot(listOf(note("a", "#general"))))))
        root["v"] = 7L
        root["somethingNew"] = mapOf("x" to 1L)
        assertEq(listOf("a"), ChatHistoryCodec.decode(Json.stringify(root))?.messages?.map { it.id })
        root.remove("v")
        assertEq(null, ChatHistoryCodec.decode(Json.stringify(root)))
        assertEq(null, ChatHistoryCodec.decode("not json"))
    }

    fun testReceiptsUseTheIosWireNames() {
        val saved = ChatHistorySnapshot(
            receipts = mapOf("m1" to ChatReceipt.DELIVERED, "m2" to ChatReceipt.READ),
            receiptTimes = mapOf("m1" to ChatReceiptTimes(delivered = 1000L), "m2" to ChatReceiptTimes(delivered = 1000L, read = 2000L), "m3" to ChatReceiptTimes()),
        )
        val text = ChatHistoryCodec.encode(saved)
        expect(text.contains("\"m1\":\"delivered\""))
        expect(text.contains("\"m2\":\"read\""))
        expect(text.contains("\"m2\":{\"delivered\":1000,\"read\":2000}"))
        val decoded = ChatHistoryCodec.decode(text)
        assertEq(saved.receipts, decoded?.receipts)
        assertEq(saved.receiptTimes - "m3", decoded?.receiptTimes)
    }

    fun testACorruptFileIsSetAsideInsteadOfOverwritten() = withDir("chat-history-corrupt") { dir ->
        val store = ChatHistoryStore(dir)
        store.file("s").writeText("{\"truncated")
        assertEq(ChatHistoryLoad.Missing, store.read("s"))
        store.save(snapshot(listOf(note("new", "#general"))), "s")
        val aside = dir.list().orEmpty().filter { it.contains(".unreadable-") }
        assertEq(1, aside.size)
        assertEq("{\"truncated", File(dir, aside[0]).readText())
        assertEq(listOf("new"), store.load("s")?.messages?.map { it.id })
    }

    fun testErasingAProjectRemovesEveryHistoryFileAndNothingElse() = withDir("chat-history-erase") { dir ->
        val store = ChatHistoryStore(dir)
        store.save(snapshot(listOf(note("a", "#general"))), "project-1")
        store.save(snapshot(listOf(note("b", "#general"))), "former")
        File(dir, "keep.txt").writeText("other")
        assertEq(2, store.eraseAll())
        assertEq(listOf("keep.txt"), dir.list().orEmpty().toList())
        assertEq(ChatHistoryLoad.Missing, store.read("project-1"))
        assertEq(0, ChatHistoryStore(File(dir, "missing")).eraseAll())
    }

    fun testAFileThatCannotBeReadYetIsNeverOverwritten() = withDir("chat-history-locked") { dir ->
        if (System.getProperty("user.name") == "root") return@withDir
        ChatHistoryStore(dir).save(snapshot(listOf(note("kept", "#general"))), "s")
        val store = ChatHistoryStore(dir)
        val target = store.file("s")
        val original = target.readText()
        expect(target.setReadable(false))
        assertEq(ChatHistoryLoad.Unreadable, store.read("s"))
        store.save(snapshot(emptyList()), "s")
        store.save(snapshot(listOf(note("other", "#general"))), "s")
        assertEq(0, store.writes)
        target.setReadable(true)
        assertEq(original, target.readText())
        assertEq(listOf("kept"), ids(store.read("s")))
        store.save(snapshot(listOf(note("kept", "#general"), note("later", "#general"))), "s")
        assertEq(listOf("kept", "later"), store.load("s")?.messages?.map { it.id })
    }

    fun testFormerScopesCoverTheRetiredRoomPathAndTheProductionAliases() {
        val scopes = ChatHistoryCodec.formerScopes(AppEnv.PROD_SERVER_URL, "world").toSet()
        for (url in listOf(AppEnv.PROD_SERVER_URL, legacyUrl, nextUrl, "wss://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/general/ws")) {
            expect(ChatHistoryCodec.scopeName(url, "world") in scopes)
        }
        expect(ChatHistoryCodec.scopeName(AppEnv.PROD_SERVER_URL, "other") !in scopes)
        val custom = ChatHistoryCodec.formerScopes("wss://relay.example/custom/ws", "world")
        assertEq(listOf(ChatHistoryCodec.scopeName("wss://relay.example/custom/ws", "world")), custom)
        assertEq("project-" + ChatHistoryCodec.scopeName("app.forkmesh.com", "world"), ChatHistoryCodec.projectScope("app.forkmesh.com", "world"))
    }

    fun testHistoryKeptUnderAnOlderRelayUrlMovesToTheProjectScope() = withDir("chat-history-migrate") { dir ->
        val now = System.currentTimeMillis()
        val writer = ChatHistoryStore(dir)
        val old = ChatHistoryCodec.scopeName(legacyUrl, "world")
        val recent = ChatHistoryCodec.scopeName(AppEnv.PROD_SERVER_URL, "world")
        writer.save(snapshot(listOf(note("a", "#general", now - 3000), note("dm", directConversation, now - 2500))), old)
        writer.save(snapshot(listOf(note("a", "#general", now - 3000), note("b", "#general", now - 1000))), recent)
        writer.file(old).setLastModified(now - 60_000)

        val store = ChatHistoryStore(dir)
        val scope = ChatHistoryCodec.projectScope(AppEnv.PROD_HOST, "world")
        val former = ChatHistoryCodec.formerScopes(AppEnv.PROD_SERVER_URL, "world")
        assertEq(listOf("a", "dm", "b"), ids(store.read(scope, former)))
        expect(!store.file(old).exists())
        expect(!store.file(recent).exists())
        expect(File(dir, store.file(old).name + ".migrated").exists())
        assertEq(listOf("a", "dm", "b"), ChatHistoryStore(dir).load(scope)?.messages?.map { it.id })
        assertEq(ChatHistoryLoad.Missing, ChatHistoryStore(dir).read("elsewhere", former))
    }

    fun testGroupChannelMessagesSurviveARestart() = withDir("chat-history-office") { dir ->
        val (hub, _) = hubWith()
        val first = office()
        hub.office = first
        val now = System.currentTimeMillis()
        first.restore(listOf(note("o1", "#office/general", now - 2000), note("o2", "#office/general", now - 1000)))
        ChatHistoryStore(dir).save(hub.historySnapshot(), "s")

        val (fresh, relay) = hubWith()
        fresh.attachHistory(ChatHistoryStore(dir)) { "s" }
        val restarted = office()
        fresh.office = restarted
        assertEq(listOf("o1", "o2"), restarted.messages("#office/general").map { it.id })
        expect(relay.messages("#office/general").isEmpty())
        assertEq(listOf("o1", "o2"), fresh.historySnapshot().messages.map { it.id })
    }

    fun testDirectMessagesAndTheirReceiptsSurviveARestart() = withDir("chat-history-direct") { dir ->
        val (hub, _) = hubWith()
        val service = direct()
        hub.direct = service
        service.restore(
            listOf(note("d1", directConversation), note("d2", directConversation)),
            mapOf("d2" to ChatReceipt.READ),
            mapOf("d2" to ChatReceiptTimes(delivered = 1000L, read = 2000L)),
        )
        hub.attachHistory(ChatHistoryStore(dir)) { "s" }
        hub.saveHistory()

        val (fresh, relay) = hubWith()
        fresh.attachHistory(ChatHistoryStore(dir)) { "s" }
        val restarted = direct()
        fresh.direct = restarted
        assertEq(listOf("d1", "d2"), restarted.messages(directConversation).map { it.id })
        assertEq(ChatReceipt.READ, restarted.receipt("d2"))
        assertEq(ChatReceiptTimes(delivered = 1000L, read = 2000L), restarted.receiptTime("d2"), "a restart keeps when it arrived")
        expect(relay.messages(directConversation).isEmpty())
    }

    fun testDirectMessagesHeldAcrossASignOutComeBackForTheSameAccount() {
        val (hub, _) = hubWith()
        val service = direct()
        hub.direct = service
        service.restore(listOf(note("d1", directConversation)), emptyMap())
        hub.directSessionChanged("pat")

        hub.directSessionChanged("")
        service.clear()
        expect(service.messages(directConversation).isEmpty())
        assertEq(listOf("d1"), hub.historySnapshot().messages.map { it.id })

        hub.directSessionChanged("Pat")
        assertEq(listOf("d1"), service.messages(directConversation).map { it.id })

        hub.directSessionChanged("")
        service.clear()
        hub.directSessionChanged("sam")
        expect(service.messages(directConversation).isEmpty())
        expect(hub.historySnapshot().messages.isEmpty())
    }
}
