package com.forkmesh.nativeapp.apps

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.FakeTransport
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.api.bodyJson
import com.forkmesh.nativeapp.api.headerValue
import com.forkmesh.nativeapp.api.jsonResponse
import com.forkmesh.nativeapp.api.path
import com.forkmesh.nativeapp.chat.OfficeFrames
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class AppsTest {
    private fun api(transport: FakeTransport) =
        AppsApi(ApiService(TestSettings(), transport).also { it.sessionTokenProvider = { "tok" } })

    private fun appJson(name: String, slug: String) = mapOf(
        "id" to "a1", "clientId" to "fmc_1", "slug" to slug, "name" to name, "botName" to "$name Bot",
        "description" to "Says hello", "icon" to mapOf("emoji" to "x"), "scopes" to listOf("commands", "chat:read"),
        "events" to listOf("app.mention"), "commands" to listOf(mapOf("command" to "hello", "description" to "Greets", "usageHint" to "[name]")),
        "bot" to true, "developer" to "alice", "organization" to "", "listing" to "public", "createdAt" to 5L,
    )

    private val installedPayload = mapOf(
        "ok" to true,
        "account" to "bob",
        "installs" to listOf(
            mapOf(
                "installId" to "i1", "app" to appJson("Hello App", "hello-app"), "target" to mapOf("kind" to "user", "name" to "bob"),
                "scopes" to listOf(mapOf("scope" to "commands", "label" to "Add slash commands", "hint" to "h")),
                "installedBy" to "bob", "createdAt" to 10L, "lastUsedAt" to 0L, "canRevoke" to true,
            ),
            mapOf("app" to appJson("Broken", "broken")),
        ),
        "organizations" to listOf(
            mapOf(
                "name" to "acme", "role" to "admin", "canManage" to true,
                "installs" to listOf(
                    mapOf(
                        "installId" to "i2", "app" to appJson("Deploy", "deploy"), "target" to mapOf("kind" to "org", "name" to "acme"),
                        "scopes" to emptyList<Any>(), "installedBy" to "", "createdAt" to 11L, "lastUsedAt" to 1_000L, "canRevoke" to true,
                    ),
                ),
                "requests" to listOf(
                    mapOf(
                        "id" to "r1", "app" to appJson("Pager", "pager"),
                        "scopes" to listOf(mapOf("scope" to "chat:write", "label" to "Post messages", "hint" to "")),
                        "requester" to "carol", "createdAt" to 12L,
                    ),
                ),
                "policy" to mapOf("requestPolicy" to "any"),
            ),
            mapOf("name" to "globex", "role" to "member", "canManage" to false, "installs" to emptyList<Any>(), "requests" to emptyList<Any>()),
        ),
    )

    fun testInstalledParsing() {
        val installed = InstalledApps.fromJson(installedPayload)
        assertEq("bob", installed.account)
        assertEq(listOf("i1"), installed.installs.map { it.installId })
        val mine = installed.installs.single()
        assertEq("Hello App", mine.app.name)
        assertEq("hello-app", mine.app.slug)
        assertEq("Hello App Bot", mine.app.botName)
        assertEq(listOf(AppCommandInfo("hello", "Greets", "[name]")), mine.app.commands)
        assertEq(true, mine.app.bot)
        assertEq("user", mine.targetKind)
        assertEq("Just me", mine.targetLabel)
        assertEq(listOf(AppScope("commands", "Add slash commands", "h")), mine.scopes)
        assertEq(true, mine.canRevoke)
        assertEq("Never used", mine.lastUsedText())
        val acme = installed.organizations.first()
        assertEq(true, acme.canManage)
        assertEq("Organization acme", acme.installs.single().targetLabel)
        assertEq("Last used 1m ago", acme.installs.single().lastUsedText(61_000L + 1_000L))
        assertEq("carol", acme.requests.single().requester)
        assertEq("Post messages", acme.requests.single().scopes.single().label)
        assertEq("any", acme.policy?.get("requestPolicy"))
        assertEq(null, installed.organizations[1].policy)
        expect(!installed.isEmpty)
        expect(InstalledApps.fromJson(mapOf("installs" to emptyList<Any>(), "organizations" to listOf(mapOf("name" to "x")))).isEmpty)
    }

    fun testCommandsParsing() {
        val parsed = AppCommands.fromJson(
            mapOf(
                "commands" to listOf(
                    mapOf("command" to "Hello", "description" to "Greets", "usageHint" to "[name]", "app" to mapOf("name" to "Hello App", "slug" to "hello-app"), "installId" to "i1"),
                    mapOf("description" to "no command"),
                ),
                "mentionable" to listOf("Hello-App", ""),
            ),
        )
        assertEq(listOf(AppCommand("hello", "Greets", "[name]", "Hello App", "hello-app", "i1")), parsed.commands)
        assertEq(listOf("hello-app"), parsed.mentionable)
        assertEq("Your app commands: /hello (Hello App), /x (Y)", AppsText.commandList(parsed.commands + AppCommand("x", "", "", "Y", "y", "i")))
        assertEq("You have no app commands. Install apps from /dashboard/apps.", AppsText.commandList(emptyList()))
    }

    fun testBlockParsingIsDefensive() {
        val blocks = AppBlocks.parse(
            listOf(
                mapOf("type" to "section", "text" to "Hi", "fields" to (1..14).map { "f$it" }),
                mapOf("type" to "context", "text" to mapOf("text" to "small")),
                mapOf("type" to "divider"),
                mapOf("type" to "image", "url" to "https://x"),
                "junk",
                mapOf("type" to "section", "text" to ""),
                mapOf(
                    "type" to "actions",
                    "elements" to listOf(
                        mapOf("type" to "button", "action_id" to "wave", "text" to "Wave", "value" to "w", "style" to "primary"),
                        mapOf("type" to "button", "action_id" to "stop", "text" to "Stop", "style" to "danger"),
                        mapOf("type" to "button", "action_id" to "odd", "text" to "Odd", "style" to "<b>"),
                        mapOf("type" to "button", "action_id" to "wave", "text" to "Duplicate"),
                        mapOf("type" to "button", "action_id" to "", "text" to "No id"),
                        mapOf("type" to "datepicker", "action_id" to "d"),
                        mapOf(
                            "type" to "static_select", "action_id" to "pick",
                            "options" to listOf(mapOf("text" to "One", "value" to "1"), mapOf("text" to "", "value" to "2"), mapOf("text" to "Three", "value" to "3")),
                        ),
                    ),
                ),
                mapOf("type" to "actions", "elements" to listOf(mapOf("type" to "static_select", "action_id" to "empty", "options" to emptyList<Any>()))),
            ),
        )
        assertEq(4, blocks.size)
        val section = blocks[0] as AppBlock.Section
        assertEq("Hi", section.text)
        assertEq(AppBlocks.MAX_FIELDS, section.fields.size)
        assertEq(AppBlock.Context("small"), blocks[1])
        assertEq(AppBlock.Divider, blocks[2])
        val actions = (blocks[3] as AppBlock.Actions).elements
        assertEq(listOf("wave", "stop", "odd", "pick"), actions.map { it.actionId })
        assertEq(AppElement.Button("wave", "Wave", "w", "primary"), actions[0])
        assertEq("danger", (actions[1] as AppElement.Button).style)
        assertEq("", (actions[2] as AppElement.Button).style)
        val select = actions[3] as AppElement.Select
        assertEq("Choose", select.placeholder)
        assertEq(listOf(AppOption("One", "1"), AppOption("Three", "3")), select.options)
        val many = AppBlocks.parse((1..30).map { mapOf("type" to "divider") })
        assertEq(AppBlocks.MAX_BLOCKS, many.size)
        assertEq(emptyList<AppBlock>(), AppBlocks.parse("not a list"))
        assertEq(emptyList<AppBlock>(), AppBlocks.parse(null))
    }

    fun testSlashParsingMatchesTheServer() {
        assertEq(SlashCommand("hello", "world  now"), AppsText.parseSlash("  /Hello world  now "))
        assertEq(SlashCommand("deploy-2", ""), AppsText.parseSlash("/deploy-2"))
        assertEq(SlashCommand("x1", "a\nb"), AppsText.parseSlash("/x1 a\nb"))
        assertEq(null, AppsText.parseSlash("/h"))
        assertEq(null, AppsText.parseSlash("/1abc"))
        assertEq(null, AppsText.parseSlash("/hello!"))
        assertEq(null, AppsText.parseSlash("hello"))
        assertEq(null, AppsText.parseSlash("/" + "a".repeat(33)))
        expect(AppsText.parseSlash("/" + "a".repeat(32)) != null)
    }

    fun testMentionDetectionMatchesTheWeb() {
        expect(AppsText.mentionsApp("hey @hello-app can you"))
        expect(AppsText.mentionsApp("@Hello-App"))
        expect(!AppsText.mentionsApp("mail bob@hello-app.com"))
        expect(!AppsText.mentionsApp("path/@hello-app"))
        expect(!AppsText.mentionsApp("@hello-app-"))
        expect(!AppsText.mentionsApp("@ab"))
        expect(!AppsText.mentionsApp("@hello_app"))
        assertEq(listOf("hello-app", "deploy"), AppsText.mentionedSlugs("(@Hello-App) and @deploy, @hello-app again"))
        expect(AppsText.shouldForward("ping @deploy", listOf("deploy")))
        expect(!AppsText.shouldForward("ping @deploy", listOf("other")))
        expect(!AppsText.shouldForward("ping @deploy", emptyList()))
    }

    fun testReplyAndErrorParsing() {
        val ephemeral = AppReply.fromJson(
            200,
            mapOf(
                "ok" to true, "responseType" to "ephemeral", "text" to "Hi bob", "appMessageId" to "m1",
                "blocks" to listOf(mapOf("type" to "section", "text" to "Hi bob")),
                "app" to mapOf("name" to "Hello App", "botName" to "Hello Bot", "slug" to "hello-app"),
            ),
        )
        expect(ephemeral.ok)
        expect(ephemeral.isEphemeral)
        assertEq("Hello Bot", ephemeral.botName)
        assertEq("m1", ephemeral.appMessageId)
        assertEq(1, ephemeral.blocks.size)
        val unavailable = AppReply.fromJson(502, mapOf("ok" to false, "error" to "app_unavailable", "app" to "Hello App"))
        expect(!unavailable.ok)
        assertEq("Hello App did not answer.", unavailable.errorText)
        assertEq("The app did not answer.", AppReply.fromJson(502, mapOf("ok" to false, "error" to "app_unavailable")).errorText)
        assertEq("No installed app has that command. Type /apps to see yours.", AppReply.fromJson(404, mapOf("error" to "unknown_command")).errorText)
        assertEq("Too many requests to that app; try again in a minute.", AppReply.fromJson(429, mapOf("error" to "rate_limited")).errorText)
        assertEq("That message is too old to use.", AppReply.fromJson(404, mapOf("error" to "message_expired")).errorText)
        assertEq("The app could not handle that (invalid_action).", AppReply.fromJson(400, mapOf("error" to "invalid_action")).errorText)
        assertEq("The app could not handle that (http_500).", AppReply.fromJson(500, emptyMap()).errorText)
        assertEq("The app could not handle that (error).", AppReply(ok = false).errorText)
        assertEq("Revoke Hello App? It loses access at once and every token it holds is revoked.", AppsText.revokeTitle("Hello App") + " " + AppsText.REVOKE_BODY)
        assertEq("You do not have permission to do that.", AppsText.manageError("forbidden"))
        assertEq("The server refused the change (http_500).", AppsText.manageError("http_500"))
        assertEq("Installed by bob", AppsText.installedBy("bob"))
        assertEq("", AppsText.installedBy(""))
    }

    fun testRequestsUseTheAccountSession() {
        val transport = FakeTransport { request ->
            when (request.path()) {
                "/api/apps/installed" -> jsonResponse(installedPayload)
                "/api/apps/installs/a%2Fb" -> jsonResponse(mapOf("ok" to true, "revoked" to "a/b"))
                "/api/apps/orgs/acme/requests/r1" -> jsonResponse(mapOf("ok" to true, "status" to "approved"))
                "/api/apps/commands" -> jsonResponse(mapOf("ok" to true, "commands" to emptyList<Any>(), "mentionable" to listOf("deploy")))
                "/api/apps/commands/run" -> jsonResponse(mapOf("ok" to false, "error" to "app_unavailable", "app" to "Hello App"), 502)
                "/api/apps/interact" -> jsonResponse(mapOf("ok" to true, "responseType" to "none"))
                "/api/apps/mentions" -> jsonResponse(mapOf("ok" to true, "forwarded" to 1))
                else -> jsonResponse(mapOf("ok" to false, "error" to "not_found"), 404)
            }
        }
        val apps = api(transport)
        assertEq("bob", apps.installed().account)
        assertEq("a/b", apps.revoke("a/b"))
        assertEq("approved", apps.decide("Acme", "r1", approve = true))
        assertEq(listOf("deploy"), apps.commands().mentionable)
        val run = apps.run("/hello there", "c".repeat(32))
        assertEq("Hello App did not answer.", run.errorText)
        assertEq("none", apps.interact("m".repeat(32), "", "wave", "w").responseType)
        assertEq(1, apps.forwardMentions("c1", "msg-1", "hi @deploy"))
        val requests = transport.requests
        assertEq(listOf("GET", "DELETE", "POST", "GET", "POST", "POST", "POST"), requests.map { it.method })
        for (request in requests) assertEq("Bearer tok", request.headerValue("Authorization"))
        assertEq(mapOf("action" to "approve"), requests[2].bodyJson())
        assertEq(mapOf("text" to "/hello there", "channelId" to "c".repeat(32)), requests[4].bodyJson())
        assertEq(mapOf("appMessageId" to "m".repeat(32), "channelId" to "", "actionId" to "wave", "value" to "w"), requests[5].bodyJson())
        assertEq(mapOf("channelId" to "c1", "messageId" to "msg-1", "text" to "hi @deploy"), requests[6].bodyJson())
        apps.decide("acme", "r1", approve = false)
        assertEq(mapOf("action" to "deny"), transport.requests.last().bodyJson())
        transport.handler = { jsonResponse(mapOf("ok" to false, "error" to "forbidden"), 403) }
        assertEq("forbidden", assertFails<OrgApiException> { apps.revoke("x") }.code)
    }

    private class FakeApps : AppsApi(ApiService(TestSettings())) {
        val calls = ArrayList<String>()
        var commandsReply = AppCommands(listOf(AppCommand("hello", "Greets", "[name]", "Hello App", "hello-app", "i1")), listOf("hello-app"))
        var runReply = AppReply(ok = true, responseType = "ephemeral", text = "Hi", botName = "Hello Bot", appSlug = "hello-app", appMessageId = "m1")
        var failCommands = false

        override fun commands(): AppCommands {
            calls.add("commands")
            if (failCommands) throw IllegalStateException("offline")
            return commandsReply
        }

        override fun run(text: String, channelId: String): AppReply {
            calls.add("run $text|$channelId")
            return runReply
        }

        override fun interact(appMessageId: String, channelId: String, actionId: String, value: String): AppReply {
            calls.add("interact $appMessageId|$channelId|$actionId|$value")
            return AppReply(ok = false, error = "message_expired", status = 404)
        }

        override fun forwardMentions(channelId: String, messageId: String, text: String): Int {
            calls.add("mentions $channelId|$messageId|$text")
            return 1
        }
    }

    fun testChatAppsRunsCommandsAndKeepsEphemeralsLocal() {
        val fake = FakeApps()
        val notices = ArrayList<String>()
        val chat = ChatApps(fake, background = { it() }, now = { 500L })
        chat.onNotice = { notices.add(it) }
        assertEq(false, chat.handle("hello", "#general", ""))
        assertEq(false, chat.handle("/unknown thing", "#general", ""))
        assertEq(listOf("commands"), fake.calls)
        assertEq(true, chat.handle("/apps", "#general", ""))
        assertEq("Your app commands: /hello (Hello App)", notices.last())
        assertEq(true, chat.handle(" /hello bob ", "#office/general", "c1"))
        assertEq("run /hello bob|c1", fake.calls.last())
        val shown = chat.ephemerals("#office/general").single()
        expect(shown.ephemeral)
        assertEq("Hello Bot", shown.senderName)
        assertEq("Hi", shown.text)
        assertEq("m1", shown.appMessageId)
        assertEq(emptyList<ChatMessage>(), chat.ephemerals("#general"))
        fake.runReply = AppReply(ok = true, responseType = "ephemeral", text = "Plain")
        chat.handle("/hello", "#general", "")
        assertEq("App", chat.ephemerals("#general").single().senderName)
        fake.runReply = AppReply(ok = false, error = "rate_limited", status = 429)
        chat.handle("/hello", "#general", "")
        assertEq("Too many requests to that app; try again in a minute.", notices.last())
        fake.runReply = AppReply(ok = true, responseType = "in_channel")
        val before = notices.size
        chat.handle("/hello", "#general", "")
        assertEq(before, notices.size)
        assertEq(1, chat.ephemerals("#general").size)
        chat.interact(shown, "wave", "w", "")
        assertEq("interact m1||wave|w", fake.calls.last())
        assertEq("That message is too old to use.", notices.last())
        val count = fake.calls.size
        assertEq(null, chat.interact(shown.copy(appMessageId = ""), "wave", "w", ""))
        assertEq(count, fake.calls.size)
        val stored = listOf(
            ChatMessage("a", "#office/general", "u", "u", "a", 100L),
            ChatMessage("b", "#office/general", "u", "u", "b", 900L),
        )
        assertEq(listOf("a", shown.id, "b"), chat.merge(stored, "#office/general").map { it.id })
        chat.reset()
        assertEq(emptyList<ChatMessage>(), chat.ephemerals("#office/general"))
        assertEq(false, chat.loaded)
    }

    fun testChatAppsSuggestionsAndMentions() {
        val fake = FakeApps()
        val chat = ChatApps(fake, background = { it() })
        chat.load()
        assertEq(listOf("/apps", "/hello"), chat.suggestions("/").map { it.command })
        val hello = chat.suggestions("/he").single()
        assertEq("/hello [name]", hello.usage)
        assertEq("Greets · Hello App", hello.summary)
        assertEq(emptyList<String>(), chat.suggestions("/hello there").map { it.command })
        assertEq(emptyList<String>(), chat.suggestions("hello").map { it.command })
        fake.calls.clear()
        chat.forwardMentions("hi @hello-app", "", "m1")
        chat.forwardMentions("hi @other-app", "c1", "m1")
        chat.forwardMentions("no mention", "c1", "m1")
        assertEq(emptyList<String>(), fake.calls)
        chat.forwardMentions("hi @Hello-App", "c1", "m1")
        assertEq(listOf("mentions c1|m1|hi @Hello-App"), fake.calls)
        val failing = FakeApps().apply { failCommands = true }
        val offline = ChatApps(failing, background = { it() })
        assertEq(false, offline.load())
        assertEq(false, offline.handle("/hello", "#general", ""))
        assertEq(listOf("commands", "commands"), failing.calls)
    }

    fun testChatAppsStayQuietForGuests() {
        val fake = FakeApps()
        val chat = ChatApps(fake, background = { it() })
        chat.enabled = { false }
        assertEq(false, chat.load())
        assertEq(false, chat.handle("/hello", "#general", ""))
        assertEq(emptyList<String>(), chat.suggestions("/").map { it.command })
        chat.forwardMentions("hi @hello-app", "c1", "m1")
        assertEq(emptyList<String>(), fake.calls)
    }

    fun testOfficeFramesKeepAppBlocksOnlyFromApps() {
        val frame = mapOf(
            "type" to "chat", "id" to "m1", "senderId" to "app:a1", "sender" to "Hello Bot", "ts" to 5L, "text" to "Pick",
            "appMessageId" to "abc", "blocks" to listOf(mapOf("type" to "actions", "elements" to listOf(mapOf("type" to "button", "action_id" to "go", "text" to "Go")))),
        )
        val message = OfficeFrames.chatMessage(frame, "#office/general", "me")!!
        assertEq("abc", message.appMessageId)
        assertEq(1, message.blocks.size)
        val human = OfficeFrames.chatMessage(frame + ("senderId" to "bob"), "#office/general", "me")!!
        assertEq(emptyList<AppBlock>(), human.blocks)
        assertEq("", human.appMessageId)
        val blocksOnly = OfficeFrames.chatMessage(frame + ("text" to ""), "#office/general", "me")
        assertEq(1, blocksOnly?.blocks?.size)
    }
}
