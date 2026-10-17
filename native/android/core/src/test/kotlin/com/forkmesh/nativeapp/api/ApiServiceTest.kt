package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import com.sun.net.httpserver.HttpServer
import java.io.IOException
import java.net.InetAddress
import java.net.InetSocketAddress
import com.forkmesh.nativeapp.model.RepoTree
import com.forkmesh.nativeapp.model.UserStatus
import java.util.concurrent.CopyOnWriteArrayList

class ApiServiceTest {
    private fun api(transport: FakeTransport, token: String = "", url: String = "ws://test.local/ws"): ApiService =
        ApiService(TestSettings(serverUrl = url), transport).also { if (token.isNotEmpty()) it.sessionTokenProvider = { token } }

    private fun routeByPath(routes: (path: String, query: Map<String, String>, request: HttpClient.Request) -> HttpClient.Response?) =
        FakeTransport { request -> routes(request.path(), request.query(), request) ?: textResponse("not found", 404) }

    fun testRepositoriesRetriesAfterAFailedCatalogLoad() {
        var count = 0
        val transport = FakeTransport { request ->
            count++
            assertEq("/api/repositories", request.path())
            if (count <= 2) {
                textResponse("temporary failure", 500)
            } else {
                jsonResponse(mapOf("ok" to true, "repositories" to listOf(mapOf("owner" to "owner", "name" to "forkmesh"))))
            }
        }
        val api = api(transport)
        assertFails<DartException> { api.repositories() }
        assertEq(2, count)
        val repos = api.repositories()
        assertEq(3, count)
        assertEq("owner/forkmesh", repos.single().fullName)
        api.repositories()
        assertEq(3, count)
    }

    fun testRoomChatKeyRequestIncludesTheConfiguredRoom() {
        val transport = FakeTransport { jsonResponse(mapOf("passphrase" to "room-passphrase")) }
        val api = api(transport)
        assertEq("room-passphrase", api.roomChatPassphrase())
        val request = transport.requests.single()
        assertEq("/api/chat/room-key", request.path())
        assertEq(mapOf("owner" to "mainnode", "repo" to "forkmesh", "room" to "world-general"), request.query())
        transport.handler = { jsonResponse(mapOf("passphrase" to 5)) }
        assertEq("Exception: room key unavailable", assertFails<DartException> { api.roomChatPassphrase() }.toString())
    }

    fun testAuthenticatedCatalogRequestsCarryTheBearerToken() {
        val transport = FakeTransport { jsonResponse(mapOf("repositories" to emptyList<Any>())) }
        api(transport, token = "session-token").repositories()
        assertEq("Bearer session-token", transport.requests.single().headerValue("Authorization"))
        assertEq("application/json", transport.requests.single().headerValue("Accept"))
    }

    fun testBearer401InvokesTheCallbacksAndFailsClosed() {
        val transport = FakeTransport { textResponse("", 401) }
        val api = api(transport, token = "expired-token")
        var unauthorized = 0
        val codes = ArrayList<String>()
        api.onUnauthorized = { unauthorized++ }
        api.onSessionInvalid = { codes.add(it) }
        val error = assertFails<DartException> { api.notifications("alice") }
        expect(error.toString().contains("session expired"), error.toString())
        assertEq(1, unauthorized)
        assertEq(listOf(""), codes)

        transport.handler = { jsonResponse(mapOf("error" to "device_revoked"), 401) }
        val revoked = assertFails<DartException> { api.notifications("alice") }
        assertEq("This device was signed out from the web. Please sign in again.", revoked.message)
        assertEq(listOf("", "device_revoked"), codes)

        transport.handler = { jsonResponse(mapOf("error" to "unauthorized"), 401) }
        val repo = assertFails<DartException> { api.issues("owner", "repo") }
        expect(repo.message!!.contains("private repo view token"))
        assertEq(listOf("", "device_revoked"), codes)
        assertEq(2, unauthorized)

        transport.handler = { jsonResponse(mapOf("error" to "invalid_session"), 401) }
        assertFails<DartException> { api.issues("owner", "repo") }
        assertEq(listOf("", "device_revoked", "invalid_session"), codes)
    }

    fun testSignedOut401DoesNotFireSessionCallbacks() {
        val transport = FakeTransport { textResponse("", 401) }
        val api = api(transport)
        var fired = 0
        api.onUnauthorized = { fired++ }
        api.onSessionInvalid = { fired++ }
        assertFails<DartException> { api.repositories() }
        assertEq(0, fired)
    }

    fun testNetworkStatsRequestsPayoutReadinessAndParsesPayoutNodes() {
        val transport = routeByPath { path, _, request ->
            if (request.method == "GET" && path == "/api/network/stats") {
                jsonResponse(
                    mapOf(
                        "clients" to 4, "hosts" to 1, "repos" to 3,
                        "onlineNodes" to listOf("mainnode-a", "mainnode-b"),
                        "payoutNodes" to listOf(
                            mapOf(
                                "name" to "mainnode-a",
                                "wallet" to "Wallet11111111111111111111111111111111",
                                "balanceLamports" to "1250000000",
                                "balanceSol" to "1.25",
                                "online" to true,
                                "payoutEligible" to true,
                                "eligibilityReason" to "eligible",
                            ),
                        ),
                    ),
                )
            } else {
                null
            }
        }
        val stats = api(transport).networkStats()
        assertEq("/api/network/stats", transport.requests.single().path())
        assertEq("1", transport.requests.single().query()["payouts"])
        assertEq(2, stats.nodesOnline)
        assertEq(1, stats.hostsOnline)
        assertEq(3, stats.repos)
        assertEq("mainnode-a", stats.payoutNodes.single().name)
        expect(stats.payoutNodes.single().payoutEligible)
        assertEq("1.25 SOL (1250000000 lamports)", stats.payoutNodes.single().balanceLabel)
    }

    fun testNetworkStatsFailuresStayRetryable() {
        val transport = FakeTransport { textResponse("", 503) }
        val api = api(transport)
        expect(assertFails<DartException> { api.networkStats() }.toString().contains("HTTP 503"))
        expect(assertFails<DartException> { api.networkStats() }.toString().contains("HTTP 503"))
        assertEq(2, transport.requests.size)
    }

    fun testNetworkActivityRejectsEmptyPartialAndMalformedPayloads() {
        val payloads = listOf(
            { _: String -> emptyMap<String, Any?>() },
            { path: String -> if (path == "/api/network/stats") mapOf("nodesOnline" to 1) else mapOf("fundsMainnodes" to emptyList<Any>()) },
            { path: String ->
                if (path == "/api/network/stats") {
                    mapOf("nodesOnline" to "not-a-number", "payoutNodes" to emptyMap<String, Any>())
                } else {
                    mapOf("fundsMainnodes" to emptyMap<String, Any>())
                }
            },
        )
        for (payload in payloads) {
            val api = api(FakeTransport { request -> jsonResponse(payload(request.path())) })
            expect(assertFails<DartException> { api.networkStats() }.message!!.contains("Invalid network stats response"))
            expect(
                assertFails<DartException> { api.networkLeaderboards() }.message!!
                    .contains("Invalid network leaderboards response"),
            )
        }
    }

    fun testNetworkLeaderboardsRejectMalformedNestedEntries() {
        val invalid = listOf(
            mapOf("name" to "", "lamports" to 1),
            mapOf("name" to "alice", "lamports" to "not-a-number"),
            mapOf("name" to "alice", "lamports" to 1, "sol" to "not-a-number"),
        )
        for (entry in invalid) {
            val api = api(
                FakeTransport {
                    jsonResponse(
                        mapOf(
                            "fundsMainnodes" to listOf(entry),
                            "fundsContributors" to emptyList<Any>(),
                            "fundsProjects" to emptyList<Any>(),
                        ),
                    )
                },
            )
            expect(
                assertFails<DartException> { api.networkLeaderboards() }.message!!
                    .contains("Invalid network leaderboards response"),
                "$entry",
            )
        }
    }

    fun testNetworkActivityRejectsFractionalAndNegativeCounters() {
        var onlineNodes: Any = 1.5
        val transport = FakeTransport {
            jsonResponse(mapOf("clients" to 1, "hosts" to 1, "repos" to 1, "onlineNodes" to onlineNodes, "payoutNodes" to emptyList<Any>()))
        }
        val api = api(transport)
        assertFails<DartException> { api.networkStats() }
        onlineNodes = -1
        assertFails<DartException> { api.networkStats() }
        val fractional = api(
            FakeTransport {
                jsonResponse(mapOf("clients" to 1.5, "hosts" to 1, "repos" to 1, "onlineNodes" to emptyList<Any>(), "payoutNodes" to emptyList<Any>()))
            },
        )
        assertFails<DartException> { fractional.networkStats() }
        val whole = api(
            FakeTransport {
                jsonResponse(mapOf("clients" to 2.0, "hosts" to "1", "repos" to 1, "onlineNodes" to emptyList<Any>(), "payoutNodes" to emptyList<Any>()))
            },
        )
        assertEq(0, whole.networkStats().nodesOnline)
    }

    fun testNetworkLeaderboardsParseFundsReceivedBoards() {
        val transport = routeByPath { path, _, _ ->
            if (path == "/api/network/leaderboards") {
                jsonResponse(
                    mapOf(
                        "fundsMainnodes" to listOf(mapOf("name" to "mainnode-a", "lamports" to "1250000000", "sol" to "1.25")),
                        "fundsContributors" to listOf(mapOf("name" to "alice", "lamports" to 500000000, "sol" to 0.5)),
                        "fundsProjects" to listOf(mapOf("name" to "forkmesh/mobile", "lamports" to "42")),
                    ),
                )
            } else {
                null
            }
        }
        val boards = api(transport).networkLeaderboards()
        assertEq("/api/network/leaderboards", transport.requests.single().path())
        assertEq("mainnode-a", boards.fundsMainnodes.single().name)
        assertEq("0.5 SOL (500000000 lamports)", boards.fundsContributors.single().amountLabel)
        assertEq("42 lamports", boards.fundsProjects.single().amountLabel)
    }

    fun testPublishedMarkdownIssuesIncludeBountyAndVoteCount() {
        val transport = routeByPath { path, query, _ ->
            val p = query["path"] ?: ""
            when {
                path.endsWith("/tree") && p == "issues" ->
                    jsonResponse(mapOf("entries" to listOf(mapOf("name" to "12", "path" to "issues/12", "type" to "dir"))))
                path.endsWith("/tree") && p == "issues/12" -> jsonResponse(
                    mapOf(
                        "entries" to listOf(
                            mapOf("name" to "issue.md", "path" to "issues/12/issue.md", "type" to "file"),
                            mapOf("name" to "001-vote.md", "path" to "issues/12/001-vote.md", "type" to "file"),
                            mapOf("name" to "002-vote.md", "path" to "issues/12/002-vote.md", "type" to "file"),
                        ),
                    ),
                )
                path.endsWith("/blob") && p == "issues/12/issue.md" -> jsonResponse(
                    mapOf(
                        "content" to """
                            ---
                            title: Stabilize signed inbox
                            status: open
                            authorName: Alice
                            labels: [mobile, signed]
                            milestone: v2 mobile
                            priority: 4
                            assignees: [mona, kai]
                            bountyUsd: 150
                            bountyAddress: escrow-md
                            bountyStatus: open
                            bountyRequiredLamports: 4500
                            bountyReceivedLamports: 1500
                            bountyAmountSol: 0.0000045
                            bountyPayUri: solana:escrow-md?amount=0.0000045
                            bountyPayee: maintainer-node
                            bountyPayoutSig: payout-md
                            ---
                            Mobile should make signed collaboration clear.
                        """.trimIndent(),
                    ),
                )
                path.endsWith("/blob") && (p == "issues/12/001-vote.md" || p == "issues/12/002-vote.md") ->
                    jsonResponse(mapOf("content" to "---\ntype: vote\nauthorName: Voter\nts: 10\n---"))
                else -> null
            }
        }
        val issue = api(transport, url = "ws://127.0.0.1:9/ws").publishedIssues("owner", "repo").single()
        assertEq("Stabilize signed inbox", issue.title)
        assertEq("Mobile should make signed collaboration clear.", issue.body)
        assertEq("Alice", issue.author)
        assertEq(listOf("mobile", "signed"), issue.labels)
        assertEq("v2 mobile", issue.milestone)
        assertEq(4, issue.priority)
        assertEq(listOf("mona", "kai"), issue.assignees)
        assertEq(2, issue.votes)
        assertEq(10L, issue.events.first().ts)
        assertEq(150.0, issue.bountyUsd)
        assertEq("escrow-md", issue.bountyAddress)
        assertEq("open", issue.bountyStatus)
        assertEq(4500L, issue.bountyRequiredLamports)
        assertEq(1500L, issue.bountyReceivedLamports)
        assertEq(0.0000045, issue.bountyAmountSol)
        assertEq("solana:escrow-md?amount=0.0000045", issue.bountyPayUri)
        assertEq("maintainer-node", issue.bountyPayee)
        assertEq("payout-md", issue.bountyPayoutSig)
    }

    fun testPublishedIssuesReadTheSplitJsonLayout() {
        fun record(number: Int, status: String, title: String, body: String) = Json.stringify(
            mapOf(
                "schema" to "forkmesh-issue-v1",
                "number" to number,
                "title" to title,
                "status" to status,
                "authorName" to "Alice",
                "labels" to listOf("mobile"),
                "votes" to 1,
                "events" to listOf(
                    mapOf(
                        "type" to "open", "id" to "open-$number", "authorName" to "Alice", "ts" to 1,
                        "title" to title, "body" to body, "attachments" to emptyList<Any>(),
                    ),
                ),
            ),
        )
        val transport = routeByPath { path, query, _ ->
            val p = query["path"] ?: ""
            when {
                path.endsWith("/tree") && p == ".forkmesh/issues" -> jsonResponse(
                    mapOf(
                        "entries" to listOf(
                            mapOf("name" to "open", "path" to ".forkmesh/issues/open", "type" to "dir"),
                            mapOf("name" to "closed", "path" to ".forkmesh/issues/closed", "type" to "dir"),
                            mapOf("name" to "2", "path" to ".forkmesh/issues/2", "type" to "dir"),
                        ),
                    ),
                )
                path.endsWith("/tree") && p == ".forkmesh/issues/open" ->
                    jsonResponse(mapOf("entries" to listOf(mapOf("name" to "3", "path" to ".forkmesh/issues/open/3", "type" to "dir"))))
                path.endsWith("/tree") && p == ".forkmesh/issues/closed" ->
                    jsonResponse(mapOf("entries" to listOf(mapOf("name" to "1", "path" to ".forkmesh/issues/closed/1", "type" to "dir"))))
                path.endsWith("/blob") && p == ".forkmesh/issues/open/3/issue-3.json" ->
                    jsonResponse(mapOf("content" to record(3, "open", "Split open", "From the open folder")))
                path.endsWith("/blob") && p == ".forkmesh/issues/closed/1/issue-1.json" ->
                    jsonResponse(mapOf("content" to record(1, "closed", "Split closed", "done")))
                path.endsWith("/blob") && p == ".forkmesh/issues/2/issue-2.json" ->
                    jsonResponse(mapOf("content" to record(2, "open", "Legacy spot", "old spot")))
                else -> null
            }
        }
        val issues = api(transport).publishedIssues("owner", "repo")
        assertEq(listOf(3, 2, 1), issues.map { it.number })
        assertEq("Split open", issues.first().title)
        assertEq("From the open folder", issues.first().body)
        assertEq("Alice", issues.first().author)
        assertEq(listOf("mobile"), issues.first().labels)
        assertEq(1, issues.first().votes)
        expect(issues.first().isOpen)
        assertEq("closed", issues.last().status)
    }

    fun testIssueRecordBodyFollowsTheLatestSignedEditOfTheOpenEvent() {
        val record = Json.stringify(
            mapOf(
                "number" to 5,
                "title" to "Edited",
                "events" to listOf(
                    mapOf("type" to "open", "id" to "o5", "body" to "first", "authorName" to "Ann"),
                    mapOf("type" to "edit", "target" to "o5", "body" to "second"),
                    mapOf("type" to "edit", "target" to "other", "body" to "ignored"),
                    mapOf("type" to "edit", "target" to "o5", "body" to "Depends on: #4"),
                ),
            ),
        )
        val transport = routeByPath { path, query, _ ->
            val p = query["path"] ?: ""
            when {
                path.endsWith("/tree") && p == ".forkmesh/issues/open" ->
                    jsonResponse(mapOf("entries" to listOf(mapOf("name" to "5", "type" to "dir"))))
                path.endsWith("/blob") && p == ".forkmesh/issues/open/5/issue-5.json" -> jsonResponse(mapOf("content" to record))
                else -> null
            }
        }
        val issue = api(transport).publishedIssues("owner", "repo").single()
        assertEq("Depends on: #4", issue.body)
        assertEq("Ann", issue.author)
    }

    fun testPublishedIssuesReadRecordsInBatchesInsteadOfOneRequestEach() {
        val numbers = (1..120).toList()
        fun record(number: Int) = Json.stringify(
            mapOf("number" to number, "title" to "Issue $number", "events" to listOf(mapOf("type" to "open", "id" to "o$number", "body" to "b"))),
        )
        val transport = routeByPath { path, query, request ->
            when {
                path.endsWith("/tree") && query["path"] == ".forkmesh/issues/open" ->
                    jsonResponse(mapOf("entries" to numbers.map { mapOf("name" to "$it", "type" to "dir") }))
                path.endsWith("/blobs") -> {
                    val wanted = java.net.URI(request.url).rawQuery.split("&").filter { it.startsWith("path=") }
                        .map { java.net.URLDecoder.decode(it.removePrefix("path="), "UTF-8") }
                    jsonResponse(mapOf("blobs" to wanted.map { it.split("/")[3].toInt() }.filter { it != 77 }
                        .associate { ".forkmesh/issues/open/$it/issue-$it.json" to mapOf("content" to record(it)) }))
                }
                path.endsWith("/blob") && query["path"] == ".forkmesh/issues/open/77/issue-77.json" ->
                    jsonResponse(mapOf("content" to record(77)))
                else -> null
            }
        }
        val issues = api(transport).publishedIssues("owner", "repo")
        assertEq(numbers.reversed(), issues.map { it.number })
        assertEq(3, transport.requests.count { it.path().endsWith("/blobs") })
        assertEq(listOf(".forkmesh/issues/open/77/issue-77.json"), transport.requests.filter { it.path().endsWith("/blob") }.map { it.query()["path"] })
    }

    fun testIssueBountyStatusPostsBodyAndParsesFundingState() {
        val transport = routeByPath { path, _, request ->
            if (request.method == "POST" && path == "/api/repo/owner/repo/bounty") {
                jsonResponse(
                    mapOf(
                        "ok" to true,
                        "bounty" to mapOf(
                            "address" to "escrow-status", "amountUsd" to 25, "requiredLamports" to 250000000,
                            "amountSol" to 0.25, "receivedLamports" to 125000000, "confirmed" to false,
                            "status" to "open", "payee" to "dev-node", "payoutSig" to "",
                            "uri" to "solana:escrow-status?amount=0.25",
                        ),
                    ),
                )
            } else {
                null
            }
        }
        val bounty = api(transport).issueBountyStatus("owner", "repo", 12)
        assertEq(mapOf("action" to "status", "number" to 12L), transport.requests.single().bodyJson())
        assertEq("escrow-status", bounty.address)
        assertEq(25.0, bounty.amountUsd)
        assertEq(250000000L, bounty.requiredLamports)
        assertEq(125000000L, bounty.receivedLamports)
        assertEq(0.25, bounty.amountSol)
        assertEq("open", bounty.status)
        assertEq("dev-node", bounty.payee)
        assertEq("solana:escrow-status?amount=0.25", bounty.payUri)

        val missing = api(FakeTransport { textResponse("", 404) })
        assertEq("", missing.issueBountyStatus("owner", "repo", 1).address)
        assertFails<DartException> { api(FakeTransport { textResponse("", 400) }).issueBountyStatus("owner", "repo", 1) }
    }

    fun testCustodialBountyCreationAndWalletPreparationFailLocally() {
        val transport = FakeTransport { error("no request expected") }
        val api = api(transport)
        assertFails<UnsupportedOperationException> {
            api.createIssueBounty("owner", "repo", number = 12, amountUsd = 150.0, payee = "owner", ts = "1770000000000", sig = "sig-create")
        }
        assertFails<UnsupportedOperationException> { api.bountyWallet("owner", "repo", ts = "1770000000000", sig = "wallet-sig") }
        assertEq(0, transport.requests.size)
    }

    fun testPublishedDiscussionsIncludeCategoryAndReplies() {
        val transport = routeByPath { path, query, _ ->
            val p = query["path"] ?: ""
            when {
                path.endsWith("/tree") && p == ".forkmesh/discussions" ->
                    jsonResponse(mapOf("entries" to listOf(mapOf("name" to "3", "path" to ".forkmesh/discussions/3", "type" to "dir"))))
                path.endsWith("/tree") && p == ".forkmesh/discussions/3" -> jsonResponse(
                    mapOf(
                        "entries" to listOf(
                            mapOf("name" to "discussion.md", "path" to ".forkmesh/discussions/3/discussion.md", "type" to "file"),
                            mapOf("name" to "001-comment.md", "path" to ".forkmesh/discussions/3/001-comment.md", "type" to "file"),
                        ),
                    ),
                )
                path.endsWith("/blob") && p == ".forkmesh/discussions/3/discussion.md" -> jsonResponse(
                    mapOf("content" to "---\ntitle: Mobile maintainer workflow\ncategory: ideas\nauthorName: Alice\n---\nHow should maintainers review from phones?"),
                )
                path.endsWith("/blob") && p == ".forkmesh/discussions/3/001-comment.md" -> jsonResponse(
                    mapOf("content" to "---\ntype: comment\nauthorName: Mona\nts: 10\n---\nKeep desktop node as the canonical apply surface."),
                )
                else -> null
            }
        }
        val discussion = api(transport).publishedDiscussions("owner", "repo").single()
        assertEq("Mobile maintainer workflow", discussion.title)
        assertEq("ideas", discussion.category)
        assertEq("Alice", discussion.author)
        assertEq("Keep desktop node as the canonical apply surface.", discussion.events.single().body)
        assertEq("Mona", discussion.events.single().displayAuthor)
    }

    fun testNotificationsFetchAndMarkReadThroughTheWorkerApiOverHttp() {
        val requests = CopyOnWriteArrayList<Map<String, Any?>>()
        val server = HttpServer.create(InetSocketAddress(InetAddress.getLoopbackAddress(), 0), 0)
        server.createContext("/") { exchange ->
            val body = exchange.requestBody.readBytes().toString(Charsets.UTF_8)
            requests.add(
                mapOf(
                    "method" to exchange.requestMethod,
                    "path" to exchange.requestURI.path,
                    "query" to (exchange.requestURI.rawQuery ?: ""),
                    "body" to body,
                ),
            )
            val (status, payload) = when {
                exchange.requestMethod == "GET" && exchange.requestURI.path == "/api/notifications" -> 200 to mapOf(
                    "ok" to true,
                    "unread" to 1,
                    "total" to 137,
                    "notifications" to listOf(
                        mapOf("id" to "n1", "kind" to "mention", "title" to "You were mentioned in mona/forkmesh", "repo" to "mona/forkmesh", "ts" to 1770000000000L),
                    ),
                )
                (exchange.requestMethod == "POST" || exchange.requestMethod == "DELETE") &&
                    exchange.requestURI.path == "/api/notifications" -> 200 to mapOf("ok" to true)
                else -> 404 to mapOf("error" to "not_found")
            }
            val bytes = Json.stringify(payload).toByteArray()
            exchange.responseHeaders.add("Content-Type", "application/json")
            exchange.sendResponseHeaders(status, bytes.size.toLong())
            exchange.responseBody.use { it.write(bytes) }
        }
        server.start()
        try {
            val api = ApiService(TestSettings(serverUrl = "ws://127.0.0.1:${server.address.port}/ws"))
            val page = api.notifications(" Mona ", limit = 25)
            api.markNotificationsRead("mona", ids = listOf("n1"))
            api.markNotificationsRead("mona", all = true)
            api.deleteAllNotifications(" Mona ")

            assertEq(1, page.unread)
            assertEq(137, page.total)
            assertEq("mention", page.notifications.single().kind)
            assertEq("node=mona&limit=25", requests[0]["query"])
            assertEq(mapOf("node" to "mona", "ids" to listOf("n1")), Json.parse(requests[1]["body"] as String))
            assertEq(mapOf("node" to "mona", "all" to true), Json.parse(requests[2]["body"] as String))
            assertEq("DELETE", requests[3]["method"])
            assertEq(mapOf("node" to "mona", "all" to true), Json.parse(requests[3]["body"] as String))
            assertEq(0, api.notifications("  ").unread)
            api.markNotificationsRead("")
            api.deleteAllNotifications(" ")
            assertEq(4, requests.size)
        } finally {
            server.stop(0)
        }
    }

    fun testPublishedPullsReadTheLedgerBranchWithTimelineEvents() {
        val transport = routeByPath { path, query, _ ->
            val p = query["path"] ?: ""
            if (p.startsWith(".forkmesh/pulls") && query["ref"] != "forkmesh/pulls") {
                return@routeByPath jsonResponse(mapOf("error" to "not_found"), 404)
            }
            when {
                path.endsWith("/tree") && p == ".forkmesh/pulls" ->
                    jsonResponse(mapOf("entries" to listOf(mapOf("name" to "7", "path" to ".forkmesh/pulls/7", "type" to "dir"))))
                path.endsWith("/tree") && p == ".forkmesh/pulls/7" -> jsonResponse(
                    mapOf(
                        "entries" to listOf(
                            mapOf("name" to "pull.md", "path" to ".forkmesh/pulls/7/pull.md", "type" to "file"),
                            mapOf("name" to "002-review.md", "path" to ".forkmesh/pulls/7/002-review.md", "type" to "file"),
                            mapOf("name" to "001-comment.md", "path" to ".forkmesh/pulls/7/001-comment.md", "type" to "file"),
                        ),
                    ),
                )
                path.endsWith("/blob") && p == ".forkmesh/pulls/7/pull.md" -> jsonResponse(
                    mapOf(
                        "content" to "---\ntitle: Add signed review flow\nstatus: open\nbase: main\nhead: review/mobile\nsig: abc\n---\n" +
                            "Adds mobile review controls.\n\n```diff\ndiff --git a/lib/a.dart b/lib/a.dart\n+review\n```",
                    ),
                )
                path.endsWith("/blob") && p == ".forkmesh/pulls/7/001-comment.md" ->
                    jsonResponse(mapOf("content" to "---\ntype: comment\nauthorName: Kai\nts: 10\n---\nReview changes before merge."))
                path.endsWith("/blob") && p == ".forkmesh/pulls/7/002-review.md" ->
                    jsonResponse(mapOf("content" to "---\ntype: review\nstate: approve\nauthorName: Mona\nts: 11\n---\nLGTM from mobile."))
                else -> null
            }
        }
        val pull = api(transport).publishedPulls("owner", "repo").single()
        assertEq("Add signed review flow", pull.title)
        expect(pull.signed)
        assertEq("main", pull.base)
        assertEq("review/mobile", pull.head)
        assertEq("diff --git a/lib/a.dart b/lib/a.dart\n+review", pull.patch)
        assertEq(2, pull.events.size)
        assertEq("Review changes before merge.", pull.events.first().body)
        assertEq("approve", pull.events.last().state)
    }

    private fun pullMd(title: String) = mapOf("content" to "---\ntitle: $title\nstatus: open\nbase: main\nhead: feature\n---\nBody.")

    fun testRepoTreeKeepsOnlyNumberedPullRefs() {
        val tree = RepoTree.fromJson(
            mapOf(
                "entries" to emptyList<Any>(),
                "pullRefs" to mapOf("9" to "abc123", "x1" to "def", "10" to "", "11" to 5, "12" to "fed456"),
            ),
            path = ".forkmesh/pulls",
        )
        assertEq(mapOf("9" to "abc123", "12" to "fed456"), tree.pullRefs)
        assertEq(emptyMap<String, String>(), RepoTree.fromJson(mapOf("entries" to emptyList<Any>(), "pullRefs" to "9")).pullRefs)
        assertEq(emptyMap<String, String>(), RepoTree.fromJson(emptyList<Any>()).pullRefs)
    }

    fun testPublishedPullsReadBranchOnlyPullsAtTheirCommit() {
        val transport = routeByPath { path, query, _ ->
            val p = query["path"] ?: ""
            val ref = query["ref"] ?: ""
            when {
                path.endsWith("/tree") && p == ".forkmesh/pulls" && ref == "forkmesh/pulls" -> jsonResponse(
                    mapOf(
                        "entries" to listOf(mapOf("name" to "7", "path" to ".forkmesh/pulls/7", "type" to "dir")),
                        "pullRefs" to mapOf("7" to "ledgersha", "8" to "branchsha8"),
                    ),
                )
                path.endsWith("/blob") && p == ".forkmesh/pulls/7/pull.md" && ref == "forkmesh/pulls" ->
                    jsonResponse(pullMd("Indexed pull"))
                path.endsWith("/blob") && p == ".forkmesh/pulls/8/pull.md" && ref == "branchsha8" ->
                    jsonResponse(pullMd("Branch-only pull"))
                path.endsWith("/tree") && p == ".forkmesh/pulls/8" && ref == "branchsha8" -> jsonResponse(
                    mapOf("entries" to listOf(mapOf("name" to "001-comment.md", "path" to ".forkmesh/pulls/8/001-comment.md", "type" to "file"))),
                )
                path.endsWith("/blob") && p == ".forkmesh/pulls/8/001-comment.md" && ref == "branchsha8" ->
                    jsonResponse(mapOf("content" to "---\ntype: comment\nauthorName: Kai\nts: 10\n---\nFrom the branch."))
                else -> null
            }
        }
        val pulls = api(transport).publishedPulls("owner", "repo")
        assertEq(listOf(8, 7), pulls.map { it.number })
        assertEq("Branch-only pull", pulls.first().title)
        assertEq("From the branch.", pulls.first().events.single().body)
        assertEq("Indexed pull", pulls.last().title)
        expect(transport.requests.none { it.query()["ref"] == "ledgersha" })
    }

    fun testPublishedPullReadsTheLedgerAndDropsTheStaleList() {
        var listed = listOf(mapOf("name" to "7", "path" to ".forkmesh/pulls/7", "type" to "dir"))
        val transport = routeByPath { path, query, _ ->
            val p = query["path"] ?: ""
            if (query["ref"] != "forkmesh/pulls") return@routeByPath null
            when {
                path.endsWith("/tree") && p == ".forkmesh/pulls" -> jsonResponse(mapOf("entries" to listed))
                path.endsWith("/blob") && p == ".forkmesh/pulls/7/pull.md" -> jsonResponse(pullMd("Old pull"))
                path.endsWith("/blob") && p == ".forkmesh/pulls/9/pull.md" -> jsonResponse(pullMd("New pull"))
                else -> null
            }
        }
        val api = api(transport)
        assertEq(listOf(7), api.publishedPulls("owner", "repo").map { it.number })
        val before = transport.requests.size
        assertEq("Old pull", api.publishedPull("owner", "repo", 7)?.title)
        assertEq(before, transport.requests.size)
        listed = listed + mapOf("name" to "9", "path" to ".forkmesh/pulls/9", "type" to "dir")
        assertEq(listOf(7), api.publishedPulls("owner", "repo").map { it.number })
        assertEq("New pull", api.publishedPull("owner", "repo", 9)?.title)
        expect(transport.requests.none { it.query()["ref"] == "forkmesh/pull/9" })
        assertEq(listOf(9, 7), api.publishedPulls("owner", "repo").map { it.number })
    }

    fun testPublishedPullFallsBackToThePullBranch() {
        val transport = routeByPath { path, query, _ ->
            val p = query["path"] ?: ""
            when {
                path.endsWith("/blob") && p == ".forkmesh/pulls/12/pull.md" && query["ref"] == "forkmesh/pull/12" ->
                    jsonResponse(pullMd("Fresh pull"))
                else -> null
            }
        }
        val pull = api(transport).publishedPull("owner", "repo", 12)
        assertEq(12, pull?.number)
        assertEq("Fresh pull", pull?.title)
        expect(transport.requests.any { it.query()["path"] == ".forkmesh/pulls/12/pull.md" && it.query()["ref"] == "forkmesh/pulls" })
    }

    fun testPublishedPullIsNullWhenNeitherRefHasIt() {
        val transport = routeByPath { _, _, _ -> null }
        assertEq(null, api(transport).publishedPull("owner", "repo", 3))
        val blobRefs = transport.requests
            .filter { it.path().endsWith("/blob") }
            .map { it.query()["ref"] }
        assertEq(listOf("forkmesh/pulls", "forkmesh/pull/3"), blobRefs)
    }

    fun testPublishedPullSurfacesAnOfflineRepo() {
        val transport = FakeTransport { jsonResponse(mapOf("error" to "offline"), 503) }
        val api = api(transport)
        val error = assertFails<DartException> { api.publishedPull("owner", "repo", 3) }
        expect(error.message!!.contains("Repo host is offline"), error.toString())
        assertFails<DartException> { api.publishedPull("owner", "repo", 3) }
    }

    fun testReleasesParseManifestsWithArtifactDownloadCounts() {
        val sha = "1938916325d5839850fbc39db05a3a1f836b10615ac4467725b9f49e864884fb"
        val secondSha = "2938916325d5839850fbc39db05a3a1f836b10615ac4467725b9f49e864884fc"
        val transport = routeByPath { path, _, _ ->
            when (path) {
                "/api/repo/owner/repo/tree" ->
                    jsonResponse(mapOf("entries" to listOf(mapOf("name" to "latest", "path" to ".forkmesh/releases/latest", "type" to "dir"))))
                "/api/repo/owner/repo/blob" -> jsonResponse(
                    mapOf(
                        "content" to Json.stringify(
                            mapOf(
                                "schema" to "forkmesh-release-v1",
                                "tag" to "v1.2.3",
                                "channel" to "latest",
                                "tag_commit" to "abcdef1234567890",
                                "created_at" to 1700000000000L,
                                "assets" to listOf(
                                    mapOf("name" to "forkmesh-linux-x86_64", "blob_sha256" to sha, "size" to 14949672, "os" to "linux", "arch" to "x86_64"),
                                    mapOf("name" to "forkmesh-darwin-arm64", "sha256" to secondSha, "size" to "2048", "os" to "darwin", "arch" to "arm64"),
                                ),
                            ),
                        ),
                    ),
                )
                "/api/repo/owner/repo/releases/downloads" -> jsonResponse(mapOf("ok" to true, "counts" to mapOf(sha to 3, secondSha to "5")))
                else -> jsonResponse(mapOf("error" to "not_found"), 404)
            }
        }
        val release = api(transport).releases("owner", "repo").single()
        assertEq("v1.2.3", release.tag)
        assertEq("latest", release.channel)
        assertEq("abcdef12", release.shortCommit)
        assertEq(listOf(sha, secondSha), release.assets.map { it.sha256 })
        assertEq(3, release.assets.first().downloads)
        assertEq("14.3 MB", release.assets.first().sizeLabel)
        assertEq(5, release.assets.last().downloads)
        assertEq("2.0 KB", release.assets.last().sizeLabel)
        expect(transport.requests.any { it.path() == "/api/repo/owner/repo/releases/downloads" })
        assertEq(".forkmesh/releases/latest/release.json", transport.requests.first { it.path().endsWith("/blob") }.query()["path"])
    }

    fun testReleasesReturnEmptyWhenTheReleaseTreeIsAbsent() {
        val transport = routeByPath { path, query, _ ->
            when (path) {
                "/api/repo/owner/repo/releases/downloads" -> jsonResponse(mapOf("error" to "offline"), 503)
                "/api/repo/owner/repo/tree" -> {
                    assertEq(".forkmesh/releases", query["path"])
                    jsonResponse(mapOf("error" to "not_found"), 404)
                }
                else -> jsonResponse(mapOf("error" to "not_found"), 404)
            }
        }
        val api = api(transport)
        assertEq(emptyList<Any>(), api.releases("owner", "repo"))
        val paths = transport.requests.map { it.path() }
        expect("/api/repo/owner/repo/releases/downloads" in paths)
        expect("/api/repo/owner/repo/tree" in paths)
        transport.handler = { jsonResponse(mapOf("entries" to emptyList<Any>())) }
        assertEq(0, api.tree("owner", "repo", path = "src").entries.size)
    }

    fun testAnOfflineRepoHostSkipsRefetchForAMinute() {
        var now = 1_000_000L
        val transport = FakeTransport { jsonResponse(mapOf("error" to "offline"), 503) }
        val api = ApiService(TestSettings(), transport) { now }
        val first = assertFails<DartException> { api.tree("owner", "repo") }
        expect(first.message!!.contains("Repo host is offline. Open the Qt desktop node"))
        val skipped = assertFails<DartException> { api.blob("owner", "repo", "README.md") }
        expect(skipped.message!!.contains("Skipping refetch for a minute"))
        assertEq(1, transport.requests.size)

        now += ApiService.OFFLINE_REPO_TTL_MS
        transport.handler = { jsonResponse(mapOf("content" to "hello")) }
        assertEq("hello", api.blob("owner", "repo", "README.md").content)
        assertEq(2, transport.requests.size)

        transport.handler = { jsonResponse(mapOf("error" to "offline"), 503) }
        assertFails<DartException> { api.tree("owner", "repo", path = "x") }
        api.clearRepoCache("owner", "repo")
        transport.handler = { jsonResponse(mapOf("entries" to emptyList<Any>())) }
        assertEq(0, api.tree("owner", "repo", path = "x").entries.size)
    }

    fun testRepoContentIsCachedForTheTtlAndA500IsRetriedOnce() {
        var now = 0L
        var calls = 0
        val transport = FakeTransport {
            calls++
            if (calls == 1) textResponse("boom", 500) else jsonResponse(mapOf("content" to "v$calls"))
        }
        val api = ApiService(TestSettings(), transport) { now }
        assertEq("v2", api.blob("o", "r", "a.txt").content)
        assertEq("v2", api.blob("o", "r", "a.txt").content)
        assertEq(2, calls)
        now += ApiService.CACHE_TTL_MS
        assertEq("v3", api.blob("o", "r", "a.txt").content)
        transport.handler = { textResponse("boom", 500) }
        val error = assertFails<DartException> { api.blob("o", "r", "b.txt") }
        expect(error.message!!.contains("Repo host returned an internal error"))
    }

    fun testAgentSessionListPostsOwnerAccountAndParsesAgents() {
        val transport = FakeTransport { request ->
            assertEq("POST", request.method)
            assertEq("/api/repo/owner/repo/agents/list", request.path())
            jsonResponse(
                mapOf(
                    "ok" to true,
                    "agents" to listOf(
                        mapOf("id" to 42, "status" to "done", "provider" to "claude-code", "prNumber" to 7, "diffStats" to mapOf("files" to 3, "ahead" to 2, "behind" to 1)),
                    ),
                ),
            )
        }
        val sessions = api(transport).agentSessions("owner", "repo", ownerAccount = " Owner ")
        assertEq(mapOf("ownerAccount" to "owner"), transport.requests.single().bodyJson())
        assertEq(42, sessions.single().id)
        assertEq("Claude Code", sessions.single().providerLabel)
        assertEq(7, sessions.single().prNumber)
        assertEq("3 files · ↑2 ↓1", sessions.single().diffLabel)
    }

    fun testAgentTranscriptPostsOwnerAccountAndParsesTheDetail() {
        val transport = FakeTransport { request ->
            assertEq("/api/repo/owner/repo/agents/42/transcript", request.path())
            jsonResponse(mapOf("ok" to true, "status" to "done", "transcript" to "Running tests\nflutter test passed"))
        }
        val detail = api(transport).agentTranscript("owner", "repo", 42, ownerAccount = "owner")
        assertEq(mapOf("ownerAccount" to "owner"), transport.requests.single().bodyJson())
        assertEq("done", detail.status)
        expect(detail.transcript.contains("flutter test passed"))
        expect(!detail.sealedForOtherDevice)
    }

    fun testActionListAndLogPostOwnerAccountAndParsePayloads() {
        val transport = routeByPath { path, _, _ ->
            when (path) {
                "/api/repo/owner/repo/actions/list" -> jsonResponse(
                    mapOf(
                        "ok" to true,
                        "workflows" to listOf(mapOf("path" to ".forkmesh/test.yml", "name" to "Test suite", "on" to listOf("push"), "stepCount" to 2)),
                        "runs" to listOf(mapOf("id" to 7, "workflowName" to "Test suite", "status" to "running", "ref" to "refs/heads/main")),
                    ),
                )
                "/api/repo/owner/repo/actions/7/log" -> jsonResponse(mapOf("ok" to true, "id" to 7, "status" to "success", "log" to "flutter test passed"))
                else -> null
            }
        }
        val api = api(transport)
        val actions = api.repoActions("owner", "repo", ownerAccount = "owner", ts = "1000", sig = "signed-list")
        val log = api.actionLog("owner", "repo", 7, ownerAccount = "owner", ts = "1001", sig = "signed-log")
        assertEq("POST", transport.requests[0].method)
        assertEq(mapOf("ownerAccount" to "owner", "ts" to "1000", "sig" to "signed-list"), transport.requests[0].bodyJson())
        assertEq("Test suite", actions.workflows.single().name)
        assertEq("Running", actions.runs.single().statusLabel)
        assertEq("/api/repo/owner/repo/actions/7/log", transport.requests[1].path())
        assertEq(mapOf("ownerAccount" to "owner", "ts" to "1001", "sig" to "signed-log"), transport.requests[1].bodyJson())
        assertEq("Success", log.statusLabel)
        assertEq("flutter test passed", log.log)
        api.repoActions("owner", "repo")
        assertEq(emptyMap<String, Any?>(), transport.requests[2].bodyJson())
    }

    fun testDesktopCommandPostsTheSignedRequestPayloadAndParsesQueueStatus() {
        val transport = FakeTransport { jsonResponse(mapOf("ok" to true, "queued" to 1)) }
        val result = api(transport).desktopCommand(
            "owner", "repo",
            ownerAccount = "owner", command = "action.rerun", target = "run:7", ts = "1000", sig = "signed-command",
            payload = mapOf("runId" to 7, "workflowPath" to ".forkmesh/test.yml"),
        )
        expect(result.ok)
        assertEq(1, result.queued)
        val request = transport.requests.single()
        assertEq("POST", request.method)
        assertEq("/api/repo/owner/repo/desktop-commands", request.path())
        assertEq(
            mapOf(
                "ownerAccount" to "owner", "command" to "action.rerun", "target" to "run:7", "ts" to "1000", "sig" to "signed-command",
                "payload" to mapOf("runId" to 7L, "workflowPath" to ".forkmesh/test.yml"),
            ),
            request.bodyJson(),
        )
        expect(request.bodyText().startsWith("{\"ownerAccount\":\"owner\",\"command\":"), request.bodyText())
    }

    fun testUrlsDeriveFromTheRelayUrl() {
        val local = api(FakeTransport { error("unused") }, url = "ws://127.0.0.1:8787/api/repo/mainnode/forkmesh/rooms/world-general/ws")
        assertEq("http://127.0.0.1:8787/api/repo/mainnode/forkmesh/raw?path=README.md", local.rawUri("mainnode", "forkmesh", "README.md"))
        assertEq("http://127.0.0.1:8787/api/repo/o/r/raw?path=docs%2Fa+b.md&ref=main", local.rawUri("o", "r", "docs/a b.md", ref = "main"))
        assertEq("http://127.0.0.1:8787/mainnode/forkmesh", local.cloneUri("mainnode", "forkmesh"))
        assertEq("http://127.0.0.1:8787", local.gitOrigin)
        assertEq("ws://127.0.0.1:8787/api/nodes/events?ticket=t+k%2F1", local.accountEventsUri("t k/1"))

        val prod = api(FakeTransport { error("unused") }, url = "wss://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws")
        assertEq("https://app.forkmesh.com/mainnode/forkmesh", prod.cloneUri("mainnode", "forkmesh"))
        assertEq("https://app.forkmesh.com", prod.gitOrigin)
        assertEq("wss://app.forkmesh.com/api/nodes/events?ticket=abc", prod.accountEventsUri("abc"))

        assertEq("https://x.test", api(FakeTransport { error("unused") }, url = "https://X.test:443/ws").gitOrigin)
        assertEq("http://[::1]:9000", api(FakeTransport { error("unused") }, url = "ws://[::1]:9000/ws").gitOrigin)
        assertEq("ws://h.test:80/api/nodes/events?ticket=a", api(FakeTransport { error("unused") }, url = "ws://h.test:80/ws").accountEventsUri("a"))
        assertEq("http://h.test/x/y", api(FakeTransport { error("unused") }, url = "ws://h.test:80/ws").cloneUri("x", "y"))
    }

    fun testAccountEventTicketOutcomesStayDistinguishable() {
        val transport = FakeTransport { jsonResponse(mapOf("ticket" to "abc")) }
        val signedOut = api(transport)
        assertEq(EventTicket(EventTicketStatus.SIGNED_OUT), signedOut.accountEventTicket())
        assertEq(0, transport.requests.size)

        val api = api(transport, token = "tok")
        assertEq(EventTicket(EventTicketStatus.OK, "abc"), api.accountEventTicket())
        assertEq("/api/accounts/event-ticket", transport.requests.single().path())
        assertEq("Bearer tok", transport.requests.single().headerValue("Authorization"))
        assertEq("application/json", transport.requests.single().headerValue("accept"))

        val cases = listOf(
            textResponse("", 401) to EventTicketStatus.SIGNED_OUT,
            textResponse("", 403) to EventTicketStatus.SIGNED_OUT,
            textResponse("", 502) to EventTicketStatus.UNAVAILABLE,
            jsonResponse(mapOf("authenticated" to false, "ticket" to "x")) to EventTicketStatus.SIGNED_OUT,
            jsonResponse(mapOf("ticket" to "")) to EventTicketStatus.SIGNED_OUT,
            jsonResponse(listOf(1)) to EventTicketStatus.UNAVAILABLE,
            textResponse("not json") to EventTicketStatus.UNAVAILABLE,
        )
        for ((response, status) in cases) {
            transport.handler = { response }
            assertEq(status, api.accountEventTicket().status, response.text())
        }
        transport.handler = { throw IOException("offline") }
        assertEq(EventTicketStatus.UNAVAILABLE, api.accountEventTicket().status)
    }

    fun testPushRegistrationTestAndUnregister() {
        val transport = routeByPath { path, _, request ->
            when {
                path == "/api/accounts/register-device" && request.method == "POST" -> jsonResponse(mapOf("ok" to true, "pushConfigured" to false))
                path == "/api/accounts/register-device" && request.method == "DELETE" -> jsonResponse(mapOf("ok" to true))
                path == "/api/accounts/push-test" -> jsonResponse(
                    mapOf("ok" to true, "configured" to true, "devices" to 1, "delivered" to 1, "results" to listOf(mapOf("environment" to "production", "delivered" to true, "status" to 200))),
                )
                else -> null
            }
        }
        val api = api(transport, token = "tok")
        assertEq(PushRegistration(ok = false), api.registerPushDevice("", "production"))
        assertEq(0, transport.requests.size)
        assertEq(PushRegistration(ok = true, statusCode = 200, relayConfigured = false), api.registerPushDevice("a".repeat(64), "development"))
        assertEq(mapOf("deviceToken" to "a".repeat(64), "environment" to "development"), transport.requests.last().bodyJson())
        val test = api.sendTestPush()
        assertEq(1, test.delivered)
        assertEq("Sent to 1 of 1 device. A ForkMesh banner should arrive within a few seconds.", test.summary)
        expect(api.unregisterPushDevice("a".repeat(64)))
        assertEq("DELETE", transport.requests.last().method)
        assertEq(mapOf("deviceToken" to "a".repeat(64)), transport.requests.last().bodyJson())
        expect(!api.unregisterPushDevice(""))

        transport.handler = { throw IOException("offline") }
        assertEq(PushRegistration(ok = false), api.registerPushDevice("t", "production"))
        assertEq(0, api.sendTestPush().statusCode)
        expect(!api.unregisterPushDevice("t"))
    }

    fun testPublishAndImportReturnTheStatusInsteadOfThrowing() {
        val transport = FakeTransport { request ->
            if (request.path() == "/api/repositories") jsonResponse(mapOf("mirror" to true), 202) else jsonResponse(mapOf("error" to "stale_update"), 409)
        }
        val api = api(transport, token = "tok")
        val published = api.publishRepository(mapOf("owner" to "alice", "catalogSig" to "sig"))
        expect(published.ok)
        assertEq(202, published.statusCode)
        assertEq(true, published.body["mirror"])
        val imported = api.createRepositoryImport(sourceUrl = "https://github.com/a/b", targetOwner = "alice")
        expect(!imported.ok)
        assertEq("stale_update", imported.error)
        assertEq(
            mapOf("sourceUrl" to "https://github.com/a/b", "targetOwner" to "alice", "mode" to "import", "sessionToken" to "tok"),
            transport.requests.last().bodyJson(),
        )
        api.createRepositoryImport(sourceUrl = "u", targetOwner = "o", mode = "mirror", providerToken = "ghp")
        assertEq("ghp", transport.requests.last().bodyJson()["providerToken"])
    }

    fun testASessionShaped401OnAPublishBouncesButBadSignatureDoesNot() {
        var code = "bad_signature"
        val transport = FakeTransport { jsonResponse(mapOf("error" to code), 401) }
        val api = api(transport, token = "tok")
        val codes = ArrayList<String>()
        var unauthorized = 0
        api.onSessionInvalid = { codes.add(it) }
        api.onUnauthorized = { unauthorized++ }
        assertEq(401, api.publishRepository(emptyMap()).statusCode)
        assertEq(0, unauthorized)
        code = "invalid_session"
        api.publishRepository(emptyMap())
        assertEq(listOf("invalid_session"), codes)
        assertEq(1, unauthorized)
    }

    fun testAccountAvatarIsReadOncePerSession() {
        val transport = FakeTransport { request ->
            when (request.path()) {
                "/api/accounts/alice" -> jsonResponse(mapOf("exists" to true, "kind" to "user", "avatarPng" to "iVBOR"))
                "/api/accounts/acme" -> jsonResponse(mapOf("exists" to true, "kind" to "org", "avatarPng" to "x"))
                else -> textResponse("", 404)
            }
        }
        val api = api(transport)
        assertEq("iVBOR", api.accountAvatarPng(" Alice "))
        assertEq("iVBOR", api.accountAvatarPng("alice"))
        assertEq("", api.accountAvatarPng("acme"))
        assertEq("", api.accountAvatarPng("mainnode-a"))
        assertEq("", api.accountAvatarPng("mainnode-a"))
        assertEq("", api.accountAvatarPng(" "))
        assertEq(3, transport.requests.size)
    }

    fun testKnownAccountAvatarIsReadWithoutARequest() {
        val transport = FakeTransport { jsonResponse(mapOf("exists" to true, "kind" to "user", "avatarPng" to "iVBOR")) }
        val api = api(transport)
        assertEq("", api.knownAccountAvatarPng("alice"))
        assertEq(0, transport.requests.size)
        api.accountAvatarPng("alice")
        assertEq("iVBOR", api.knownAccountAvatarPng(" Alice "))
        assertEq("", api.knownAccountAvatarPng(" "))
        assertEq(1, transport.requests.size)
    }

    fun testAccountUserStatusIsCachedAndMissesArePermanent() {
        var now = 1_000L
        val transport = FakeTransport { request ->
            when (request.path()) {
                "/api/accounts/alice" -> jsonResponse(
                    mapOf("exists" to true, "kind" to "user", "userStatus" to mapOf("away" to true, "emoji" to "\uD83C\uDF34", "text" to "Beach")),
                )
                "/api/accounts/acme" -> jsonResponse(mapOf("exists" to true, "kind" to "org", "userStatus" to mapOf("away" to true)))
                else -> textResponse("", 404)
            }
        }
        val api = ApiService(TestSettings(serverUrl = "ws://test.local/ws"), transport) { now }
        val alice = api.accountUserStatus(" Alice ")
        expect(alice.away)
        assertEq("Beach", alice.text)
        assertEq(alice, api.accountUserStatus("alice"))
        assertEq(UserStatus(), api.accountUserStatus("acme"))
        assertEq(UserStatus(), api.accountUserStatus("mainnode-a"))
        assertEq(UserStatus(), api.accountUserStatus(""))
        assertEq(3, transport.requests.size)
        now += ApiService.ACCOUNT_STATUS_TTL_MS
        api.accountUserStatus("alice")
        api.accountUserStatus("acme")
        api.accountUserStatus("mainnode-a")
        assertEq(4, transport.requests.size)
    }

    fun testFederatedRepliesAreReadFromTheProjectionOnly() {
        val transport = FakeTransport {
            jsonResponse(
                mapOf(
                    "comments" to listOf(
                        mapOf("remoteId" to "https://mastodon.test/1", "author" to "@a@mastodon.test", "body" to "hi"),
                        mapOf("author" to "nobody"),
                    ),
                ),
            )
        }
        val api = api(transport)
        assertEq(0, api.federatedReplies("o", "r", kind = "commit", number = 1).size)
        assertEq(0, api.federatedReplies("o", "r", kind = "issue", number = 0).size)
        assertEq(0, transport.requests.size)
        val replies = api.federatedReplies("o", "r", kind = "issue", number = 4)
        assertEq(1, replies.size)
        expect(!replies.single().nativeEvent)
        assertEq(mapOf("kind" to "issue", "number" to "4"), transport.requests.single().query())
    }

    fun testMirrorSetCarriesTheSyncStatus() {
        val transport = FakeTransport {
            jsonResponse(
                mapOf(
                    "generatedAt" to 1800000000000L,
                    "summary" to mapOf("freshestSync" to 1799999990000L),
                    "mirrors" to listOf(mapOf("owner" to "backup", "repo" to "proj", "status" to "online", "integrity" to "ok", "cloneAvailable" to true, "lastSync" to 1799999990000L)),
                ),
            )
        }
        val set = api(transport).mirrorSet("alice", "proj")
        assertEq("backup/proj", set.mirrors.single().label)
        assertEq("verified", set.syncStatus!!["state"])
        assertEq(listOf("backup/proj"), api(transport).mirrors("alice", "proj").map { it.label })
    }

    fun testListDevicesAcceptsAWrappedOrBareList() {
        val transport = FakeTransport { jsonResponse(listOf(mapOf("id" to "d1", "kind" to "mobile_node", "enabled" to 0))) }
        val devices = api(transport, token = "tok").listDevices()
        assertEq("d1", devices.single().id)
        expect(!devices.single().enabled)
        transport.handler = { jsonResponse(mapOf("devices" to "nope")) }
        assertEq(0, api(transport).listDevices().size)
    }

    fun testCommitsAndCommitDetail() {
        val transport = FakeTransport { request ->
            when (request.path()) {
                "/api/repo/o/r/history" -> jsonResponse(mapOf("commits" to listOf(mapOf("hash" to "abc"), "junk")))
                "/api/repo/o/r/commit" -> jsonResponse(mapOf("files" to listOf(mapOf("path" to "a.txt", "adds" to 1)), "diff" to "d"))
                "/api/repo/o/r/search" -> jsonResponse(mapOf("code" to listOf(mapOf("path" to "a.txt", "line" to 3, "text" to "x"))))
                "/api/repo/o/r/branches" -> jsonResponse(mapOf("branches" to listOf(mapOf("name" to "main", "default" to true), mapOf("name" to ""))))
                else -> textResponse("", 404)
            }
        }
        val api = api(transport)
        assertEq(listOf(mapOf<String, Any?>("hash" to "abc")), api.commits("o", "r"))
        api.commits("o", "r")
        assertEq(1, transport.requests.size)
        val detail = api.commitDetail("o", "r", "abc", fallbackCommit = mapOf("hash" to "abc"))
        assertEq(mapOf("path" to "abc"), transport.requests.last().query())
        assertEq("a.txt", detail.files.single().path)
        assertEq("abc", detail.commit["hash"])
        assertEq(3, api.searchRepo("o", "r", "needle x", ref = "dev").code.single().line)
        assertEq(mapOf("q" to "needle x", "ref" to "dev"), transport.requests.last().query())
        assertEq(listOf("main"), api.branches("o", "r").map { it.name })
    }

    private fun mirrorCatalog(): Any = listOf(
        mapOf(
            "owner" to "mirror35",
            "name" to "forkmesh",
            "logicalOwners" to listOf(mapOf("kind" to "organization", "owner" to "forkmesh")),
        ),
        mapOf("owner" to "bob", "name" to "notes"),
    )

    fun testMirrorServedReadsUseThePublicOwnerAfterTheCatalogLoads() {
        val transport = FakeTransport { request ->
            when (request.path()) {
                "/api/repositories" -> jsonResponse(mirrorCatalog())
                "/api/repo/forkmesh/forkmesh/tree" -> jsonResponse(mapOf("entries" to emptyList<Any>()))
                "/api/repo/forkmesh/forkmesh/history" -> jsonResponse(listOf(mapOf("hash" to "h1")))
                "/api/repo/bob/notes/branches" -> jsonResponse(listOf(mapOf("name" to "main")))
                else -> textResponse("", 404)
            }
        }
        val api = api(transport)
        assertEq("mirror35", api.readOwner("mirror35", "forkmesh"))
        val repos = api.repositories()
        assertEq("forkmesh", repos.first().publicOwner)
        assertEq("forkmesh", api.readOwner("mirror35", "forkmesh"))
        assertEq("forkmesh", api.readOwner("MIRROR35", "ForkMesh"))
        assertEq("bob", api.readOwner("bob", "notes"))
        api.tree("mirror35", "forkmesh")
        assertEq("/api/repo/forkmesh/forkmesh/tree", transport.requests.last().path())
        assertEq(listOf(mapOf<String, Any?>("hash" to "h1")), api.commits("mirror35", "forkmesh"))
        assertEq("/api/repo/forkmesh/forkmesh/history", transport.requests.last().path())
        api.branches("bob", "notes")
        assertEq("/api/repo/bob/notes/branches", transport.requests.last().path())
        expect(api.rawUri("mirror35", "forkmesh", "README.md").contains("/api/repo/forkmesh/forkmesh/raw"))
    }

    fun testAnUnroutablePublicOwnerFallsBackToTheServingNode() {
        val transport = FakeTransport { request ->
            when (request.path()) {
                "/api/repositories" -> jsonResponse(mirrorCatalog())
                "/api/repo/forkmesh/forkmesh/branches" -> textResponse("", 503)
                "/api/repo/mirror35/forkmesh/branches" -> jsonResponse(listOf(mapOf("name" to "main")))
                else -> textResponse("", 404)
            }
        }
        val api = api(transport)
        api.repositories()
        assertEq(listOf("main"), api.branches("mirror35", "forkmesh").map { it.name })
        val paths = transport.requests.map { it.path() }.filter { it.endsWith("/branches") }
        assertEq("/api/repo/mirror35/forkmesh/branches", paths.last())
        expect("/api/repo/forkmesh/forkmesh/branches" in paths)
    }

    fun testAProvenPublicRouteIsNotRetriedAgainstTheServingNode() {
        var publicOk = true
        val transport = FakeTransport { request ->
            when (request.path()) {
                "/api/repositories" -> jsonResponse(mirrorCatalog())
                "/api/repo/forkmesh/forkmesh/search" ->
                    if (publicOk) jsonResponse(mapOf("code" to emptyList<Any>())) else textResponse("", 404)
                else -> jsonResponse(mapOf("code" to emptyList<Any>()))
            }
        }
        val api = api(transport)
        api.repositories()
        api.searchRepo("mirror35", "forkmesh", "x")
        publicOk = false
        var failed = false
        try {
            api.searchRepo("mirror35", "forkmesh", "y")
        } catch (_: Exception) {
            failed = true
        }
        expect(failed)
        expect(transport.requests.none { it.path() == "/api/repo/mirror35/forkmesh/search" })
    }
}
