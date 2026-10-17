import FMBase
import XCTest

@testable import FMPackages

final class MarketingWorkspaceTests: XCTestCase {
    let utc = TimeZone(identifier: "UTC")!

    func overviewJSON() -> JSONObject {
        [
            "organization": "acme", "role": "admin", "canManageSettings": true, "assistantAvailable": true,
            "content": [["id": .string(hexA), "channel": "x", "status": "draft", "title": "Launch", "body": "We shipped"]],
            "ads": [[
                "id": .string(hexB), "channel": "google-ads", "status": "active", "name": "Search", "currency": "USD",
                "budgetCents": 10000, "spendCents": 2550, "pacing": ["state": "over"], "platformUrl": "https://ads.google.com",
                "linkChecks": [["level": "warning", "message": "utm_source is missing"]],
            ]],
            "channelCatalog": [
                ["id": "facebook", "name": "Facebook", "category": "Social networks"],
                ["id": "x", "name": "X", "category": "Social networks"],
                ["id": "blog", "name": "Blog", "category": "Owned media"],
                ["id": "github", "name": "GitHub", "category": "Developer communities"],
            ],
            "channelPlaybooks": ["Social networks": ["goal": "Grow", "kpis": ["Followers"], "tips": ["Post often"]]],
            "refreshableChannels": ["x", "blog", "github"],
            "settings": [
                "channels": [
                    ["id": "x", "enabled": true, "displayName": "X (Acme)", "profileUrl": "https://x.com/acme", "hasAccessToken": true,
                     "feed": ["lastError": "x_access_token_required"]],
                    ["id": "blog", "enabled": false, "displayName": "Blog", "profileUrl": ""],
                ],
                "attribution": ["landingUrl": "https://acme.com", "sources": ["x", "reddit"]],
                "targets": ["signupRate": 4.5],
                "analytics": ["project": "", "host": "acme.com", "goals": [
                    ["label": "Signup", "event": "signup", "property": "", "operator": "exact", "value": ""],
                    ["label": "Pro", "event": "checkout", "property": "plan", "operator": "icontains", "value": "pro"],
                ]],
                "x": ["connected": true, "username": "acme", "lastPostUrl": "https://x.com/acme/status/1"],
            ],
            "summary": [
                "published": 2, "queued": 1, "reach": 900, "followers": 450, "engagements": 30,
                "health": ["good": 1, "stale": 1],
                "channels": [
                    ["channel": "x", "followers": 450, "health": ["state": "stale"]],
                    ["channel": "blog", "health": ["state": "stale"]],
                ],
            ],
            "campaigns": [[
                "id": .string(hexA), "name": "Launch week", "status": "active", "objective": "launch", "startsAt": 1_790_000_000_000,
                "tasks": [["title": "Announce", "done": true, "owner": "sam"], ["title": "Recap", "done": false, "owner": ""]],
                "currency": "USD", "budgetCents": 125_050, "channels": ["x", "blog"], "targetVisits": .null,
                "stats": ["content": 1, "ads": 2, "tasksDone": 1, "tasksTotal": 2, "daysLeft": 3,
                          "taggedLink": "https://acme.com/?utm_campaign=launch_week"],
            ]],
            "campaignTemplates": [["key": "contest", "label": "From the site", "name": "Contest", "status": "active"]],
            "experiments": [[
                "id": .string(hexB), "title": "Shorter signup", "status": "running", "stage": "activation", "score": 120,
                "reviewDue": true, "impact": 5, "confidence": 4, "ease": 6,
            ]],
            "experimentsSummary": ["running": 1, "runningLimit": 3],
            "funnel": [["id": .string(hexA), "weekStart": 1_789_430_400_000, "visits": 1000, "signups": 40]],
            "funnelReport": ["weekStart": 1_789_430_400_000, "bottleneck": "signupRate", "bottleneckReason": "Signup rate is low.",
                             "rates": [["key": "signupRate", "label": "Signup rate", "value": 4.0, "change": -0.5, "target": 4.5]]],
            "keywords": [["id": .string(hexA), "term": "git mirror", "position": 8.2, "positionChange": 1.3, "impressions": 340,
                          "clicks": 12, "intent": "commercial", "priority": 2]],
            "redditThreads": [
                ["id": .string(hexA), "status": "to_review", "subreddit": "git", "title": "", "score": 12, "comments": 4],
                ["id": .string(hexB), "status": "replied", "subreddit": "devops", "title": "Mirrors?"],
            ],
            "redditDesk": [
                "subreddits": [["name": "git", "url": "https://www.reddit.com/r/git/new/", "rulesRead": true,
                                "rules": [["title": "No spam"]]]],
                "queries": [["query": "git mirror", "url": "https://www.reddit.com/search/?q=git+mirror&sort=new"]],
                "accounts": [["name": "forkmesh", "url": "https://www.reddit.com/user/forkmesh/submitted/"]],
                "summary": ["open": 1],
            ],
            "posts": [["id": .string(hexA), "channel": "x", "url": "https://x.com/acme/status/2", "views": 1200, "likes": 30]],
            "adsSummary": ["attention": 1, "currencies": [["currency": "USD", "ads": 1, "active": 1, "spendCents": 2550,
                                                             "budgetCents": 10000, "impressions": 1000, "clicks": 25, "ctr": 2.5]]],
            "adPlatforms": [["channel": "google-ads", "name": "Google Ads", "url": "https://ads.google.com"]],
            "suggestions": [
                ["level": "warning", "code": "campaign_ended", "title": "Launch week has ended.", "campaign": .string(hexA),
                 "action": ["type": "campaign-edit", "id": .string(hexA)]],
                ["level": "action", "code": "campaign_nothing_scheduled", "title": "Nothing scheduled",
                 "action": ["type": "content", "campaign": "Launch week", "channel": "x"]],
            ],
        ]
    }

    func testTheChannelNeedingAttentionSkipsDisabledChannelsAndRanksAttentionFirst() {
        var overview = MarketingOverview(json: overviewJSON())
        XCTAssertEqual(MarketingChannelAttention.next([overview])?.channel.id, "x")
        XCTAssertEqual(MarketingChannelAttention.next([overview])?.caption, "X (Acme): stale numbers")
        var github = MarketingChannelStats(id: "github", reach: 0, followers: 0, impressions: 0, engagements: 0, clicks: 0)
        github.health = "setup"
        overview.channels.append(github)
        XCTAssertEqual(MarketingChannelAttention.next([overview])?.channel.id, "x")
        var other = MarketingOverview(json: overviewJSON())
        other.organization = "beta"
        other.channels[0].health = "attention"
        let next = MarketingChannelAttention.next([overview, other])
        XCTAssertEqual(next?.organization, "beta")
        XCTAssertEqual(next?.title, "Needs attention")
        overview.channels[0].health = "good"
        XCTAssertEqual(MarketingChannelAttention.next([overview])?.channel.id, "github")
        overview.channels.removeLast()
        XCTAssertNil(MarketingChannelAttention.next([overview]))
    }

    func testOverviewParsesTheWholeWorkspace() {
        let overview = MarketingOverview(json: overviewJSON())
        XCTAssertTrue(overview.canManageSettings)
        XCTAssertTrue(overview.assistantAvailable)
        XCTAssertEqual(overview.channelName("x"), "X (Acme)")
        XCTAssertEqual(overview.channelName("facebook"), "Facebook")
        XCTAssertEqual(overview.addableChannels.map(\.id), ["facebook", "github"])
        XCTAssertEqual(overview.staleChannels, ["x"])
        XCTAssertEqual(overview.setting("x")?.lastError, "x_access_token_required")
        XCTAssertTrue(overview.setting("x")?.takesToken ?? false)
        XCTAssertEqual(overview.playbook("x")?.tips, ["Post often"])
        XCTAssertNil(overview.playbook("blog"))
        XCTAssertEqual(overview.campaigns.first?.facts(overview), ["Launch", "1/2 tasks", "1 post", "2 ads", "3 days left"])
        XCTAssertEqual(overview.campaigns.first?.link, "https://acme.com/?utm_campaign=launch_week")
        XCTAssertEqual(overview.campaignNames, ["Launch week"])
        XCTAssertEqual(overview.experiments.first?.facts(overview), ["Activation", "ICE 120", "Review due"])
        XCTAssertTrue(overview.experiments.first?.needsAttention ?? false)
        XCTAssertEqual(overview.experimentsRunning, 1)
        XCTAssertEqual(overview.funnelReport.rates.first?.text, "4% · target 4.5% · -0.5 pts")
        XCTAssertTrue(overview.funnelReport.rates.first?.belowTarget ?? false)
        XCTAssertEqual(overview.funnel.first?.facts(overview), ["1000 visits", "40 signups"])
        XCTAssertEqual(overview.keywords.first?.facts(overview),
                       ["Position 8.2 (+1.3)", "340 impressions", "12 clicks", "Commercial", "Priority 2"])
        XCTAssertEqual(overview.redditThreads.first?.title, "r/git thread")
        XCTAssertEqual(overview.openRedditThreads.map(\.id), [hexA])
        XCTAssertEqual(overview.redditSubreddits.first?.rules, ["No spam"])
        XCTAssertEqual(overview.redditQueriesText, "git mirror")
        XCTAssertEqual(overview.posts.first?.facts(overview), ["X (Acme)", "1200 views", "30 likes"])
        XCTAssertEqual(overview.posts(channel: "x").count, 1)
        XCTAssertEqual(overview.adTotals.first?.facts.first, "25.50 USD spent of 100.00 USD")
        XCTAssertEqual(overview.adsNeedingAttention, 1)
        XCTAssertTrue(overview.ads.first?.needsAttention ?? false)
        XCTAssertEqual(overview.ads.first?.consoleUrl, "https://ads.google.com")
        XCTAssertEqual(overview.ads.first?.linkChecks, ["utm_source is missing"])
        XCTAssertEqual(overview.x.username, "acme")
        XCTAssertEqual(overview.healthCounts["stale"], 1)
        XCTAssertEqual(overview.content(query: "ship", status: nil, channel: "x").count, 1)
        XCTAssertEqual(overview.content(query: "", status: "published", channel: nil).count, 0)
        XCTAssertEqual(overview.ads(status: "active", query: "sea").count, 1)
    }

    func testContentDraftRoundTripsAndStampsPublishing() throws {
        let overview = MarketingOverview(json: overviewJSON())
        var draft = MarketingDraft(kind: .content, overview: overview, zone: utc)
        XCTAssertEqual(draft["channel"], "x")
        XCTAssertEqual(draft["status"], "draft")
        XCTAssertThrowsError(try draft.body(overview: overview, now: 5, zone: utc)) { error in
            XCTAssertEqual((error as? MarketingFormError)?.message, "Title is required.")
        }
        draft["title"] = "Launch post"
        draft["scheduledAt"] = "2026-10-08 09:30"
        draft["tags"] = "launch, x, launch"
        draft["status"] = "published"
        draft["url"] = "http://insecure"
        XCTAssertThrowsError(try draft.body(overview: overview, now: 5, zone: utc))
        draft["url"] = ""
        let body = try draft.body(overview: overview, now: 5, zone: utc)
        XCTAssertEqual(body.int64("scheduledAt"), 1_791_451_800_000)
        XCTAssertEqual(body.strings("tags"), ["launch", "x"])
        XCTAssertEqual(body.int64("publishedAt"), 5)

        let existing = MarketingDraft(kind: .content, id: hexA, overview: overview,
                                      json: ["title": "Old", "scheduledAt": 1_791_451_800_000, "publishedAt": 9, "status": "published"],
                                      zone: utc)
        XCTAssertEqual(existing["scheduledAt"], "2026-10-08 09:30")
        XCTAssertNil(try existing.body(overview: overview, now: 5, zone: utc)["publishedAt"])
        XCTAssertThrowsError(try {
            var bad = existing
            bad["scheduledAt"] = "next week"
            _ = try bad.body(overview: overview, now: 5, zone: utc)
        }())
    }

    func testCampaignDraftKeepsTaskOwnersAndMoney() throws {
        let overview = MarketingOverview(json: overviewJSON())
        let campaign = overview.campaigns[0]
        var draft = MarketingDraft(kind: .campaign, id: campaign.id, overview: overview, json: campaign.json, zone: utc)
        XCTAssertEqual(draft["tasks"], "[x] Announce\nRecap")
        XCTAssertEqual(draft["budgetCents"], "1250.50")
        XCTAssertEqual(draft["channels"], "x, blog")
        XCTAssertEqual(draft["targetVisits"], "")
        XCTAssertEqual(draft["startsAt"], "2026-09-21")
        draft["tasks"] = "[x] Announce\n[ ] Recap\nNew one"
        draft["budgetCents"] = "99,5"
        draft["currency"] = ""
        let body = try draft.body(overview: overview, now: 0, zone: utc)
        XCTAssertEqual(body.maps("tasks").map { $0.str("owner") }, ["sam", "", ""])
        XCTAssertEqual(body.maps("tasks").map { $0.bool("done") }, [true, false, false])
        XCTAssertEqual(body.int64("budgetCents"), 9950)
        XCTAssertNil(body["currency"])
        XCTAssertEqual(body["targetVisits"], .null)
        draft["budgetCents"] = "12.345"
        XCTAssertThrowsError(try draft.body(overview: overview, now: 0, zone: utc))

        XCTAssertEqual(MarketingForms.taskToggleBody(campaign, index: 1, done: true).maps("tasks").map { $0.bool("done") }, [true, true])
        XCTAssertEqual(MarketingForms.addTaskBody(campaign, title: "Ship").maps("tasks").last?.str("title"), "Ship")
        let template = MarketingForms.templateDraft(overview.campaignTemplates[0], overview: overview, zone: utc)
        XCTAssertTrue(template.isNew)
        XCTAssertEqual(template["name"], "Contest")
        XCTAssertNil(try template.body(overview: overview, now: 0, zone: utc)["key"])
    }

    func testSettingsDraftsBuildTheServerShapes() throws {
        let overview = MarketingOverview(json: overviewJSON())
        var channel = MarketingForms.channelDraft(overview, channel: "x", zone: utc)
        XCTAssertEqual(channel.fields.map(\.key), ["enabled", "displayName", "profileUrl", "accessToken", "clearAccessToken"])
        XCTAssertEqual(channel["enabled"], "true")
        channel["profileUrl"] = "https://x.com/acme_hq"
        var body = try channel.body(overview: overview, now: 0, zone: utc)
        let channels = body.maps("channels")
        XCTAssertEqual(channels.map { $0.str("id") }, ["x", "blog"])
        XCTAssertEqual(channels[0].str("profileUrl"), "https://x.com/acme_hq")
        XCTAssertNil(channels[0]["accessToken"])
        XCTAssertEqual(channels[1]["enabled"], false)
        channel["accessToken"] = "secret"
        body = try channel.body(overview: overview, now: 0, zone: utc)
        XCTAssertEqual(body.maps("channels")[0].str("accessToken"), "secret")
        XCTAssertEqual(MarketingForms.channelDraft(overview, channel: "blog", zone: utc).fields.count, 3)
        XCTAssertEqual(MarketingForms.channelsBody(overview, adding: ["github"]).maps("channels").map { $0.str("id") },
                       ["x", "blog", "github"])

        let goals = MarketingForms.settingsDraft(.goals, overview: overview, zone: utc)
        XCTAssertEqual(goals["goals"], "Signup | signup\nPro | checkout | plan | icontains | pro")
        let analytics = try goals.body(overview: overview, now: 0, zone: utc).obj("analytics")
        XCTAssertEqual(analytics.maps("goals").count, 2)
        XCTAssertEqual(analytics.maps("goals")[1].str("operator"), "icontains")
        XCTAssertNil(analytics.maps("goals")[0]["property"])

        var targets = MarketingForms.settingsDraft(.targets, overview: overview, zone: utc)
        XCTAssertEqual(targets["signupRate"], "4.5")
        targets["activationRate"] = "30"
        XCTAssertEqual(try targets.body(overview: overview, now: 0, zone: utc).obj("targets"),
                       ["signupRate": 4.5, "activationRate": 30.0])
        targets["activationRate"] = "300"
        XCTAssertThrowsError(try targets.body(overview: overview, now: 0, zone: utc))

        let watch = MarketingForms.settingsDraft(.redditWatch, overview: overview, zone: utc)
        XCTAssertEqual(try watch.body(overview: overview, now: 0, zone: utc).obj("reddit"),
                       ["subreddits": ["git"], "queries": ["git mirror"], "accounts": ["forkmesh"]])
        let attribution = MarketingForms.settingsDraft(.attribution, overview: overview, zone: utc)
        XCTAssertEqual(attribution["sources"], "x, reddit")
        XCTAssertEqual(try attribution.body(overview: overview, now: 0, zone: utc).obj("attribution").strings("sources"), ["x", "reddit"])
    }

    func testRecordDraftsShapeTheirBodies() throws {
        let overview = MarketingOverview(json: overviewJSON())
        var keyword = MarketingDraft(kind: .keyword, id: hexA, overview: overview, json: overview.keywords[0].json, zone: utc)
        XCTAssertEqual(keyword["priority"], "2")
        XCTAssertEqual(keyword["snapshot.position"], "")
        XCTAssertNil(try keyword.body(overview: overview, now: 7, zone: utc)["snapshot"])
        keyword["snapshot.position"] = "6,5"
        let snapshot = try keyword.body(overview: overview, now: 7, zone: utc).obj("snapshot")
        XCTAssertEqual(snapshot["position"], .double(6.5))
        XCTAssertEqual(snapshot["impressions"], .null)
        XCTAssertEqual(snapshot.int64("at"), 7)

        var post = MarketingDraft(kind: .post, overview: overview, zone: utc)
        post["url"] = "https://x.com/acme/status/3"
        post["views"] = "1,200"
        let postBody = try post.body(overview: overview, now: 0, zone: utc)
        XCTAssertEqual(postBody.int64("views"), 1200)
        XCTAssertNil(postBody["likes"])
        XCTAssertNil(postBody["channel"])
        XCTAssertNil(postBody["postedAt"])

        let metric = MarketingForms.metricDraft(overview, channel: "x", zone: utc)
        XCTAssertEqual(metric["followers"], "450")
        let metricBody = try metric.body(overview: overview, now: 0, zone: utc)
        XCTAssertEqual(metricBody.str("channel"), "x")
        XCTAssertEqual(metricBody["rating"], .int(0))
        XCTAssertEqual(MarketingForms.metricDraft(overview, channel: "github", zone: utc).fields.first?.key, "stars")

        var thread = MarketingDraft(kind: .redditThread, overview: overview, zone: utc)
        XCTAssertEqual(thread["status"], "to_review")
        thread["url"] = "https://www.reddit.com/r/git/comments/abc/x/"
        thread["rulesChecked"] = "true"
        let threadBody = try thread.body(overview: overview, now: 0, zone: utc)
        XCTAssertEqual(threadBody["rulesChecked"], true)
        XCTAssertEqual(threadBody["disclosed"], false)

        var ad = MarketingDraft(kind: .ad, overview: overview, zone: utc)
        XCTAssertEqual(ad["channel"], "google-ads")
        XCTAssertEqual(ad["budgetType"], "lifetime")
        ad["name"] = "Retarget"
        ad["currency"] = "eur"
        XCTAssertEqual(try ad.body(overview: overview, now: 0, zone: utc).str("currency"), "EUR")
        let results = MarketingDraft(kind: .adResults, id: hexB, overview: overview, json: overview.ads[0].json, zone: utc)
        XCTAssertEqual(results["spendCents"], "25.50")

        let x = MarketingForms.xPostDraft(overview, content: overview.content[0])
        let xBody = try x.body(overview: overview, now: 0, zone: utc)
        XCTAssertEqual(xBody.str("text"), "We shipped")
        XCTAssertEqual(xBody.str("contentId"), hexA)

        let fromSuggestion = MarketingForms.suggestionDraft(overview.suggestions[0], overview: overview, zone: utc)
        XCTAssertEqual(fromSuggestion?.id, hexA)
        let content = MarketingForms.suggestionDraft(overview.suggestions[1], overview: overview, zone: utc)
        XCTAssertEqual(content?["campaign"], "Launch week")
        let idea = MarketingIdea(json: ["kind": "content", "title": "Teaser", "copy": "Soon", "channel": "x", "inDays": 2])
        let ideaDraft = MarketingForms.ideaDraft(idea, overview: overview, campaign: overview.campaigns[0], now: 0, zone: utc)
        XCTAssertEqual(ideaDraft?["scheduledAt"], "1970-01-03")
        XCTAssertEqual(ideaDraft?["body"], "Soon")
        XCTAssertNil(MarketingForms.ideaDraft(MarketingIdea(json: ["kind": "task", "title": "t"]), overview: overview, campaign: nil, now: 0))
    }

    func testHelpers() {
        XCTAssertEqual(MarketingMoney.cents("12"), 1200)
        XCTAssertEqual(MarketingMoney.cents("$1,234.5"), 123_450)
        XCTAssertEqual(MarketingMoney.cents("0,99"), 99)
        XCTAssertNil(MarketingMoney.cents("-3"))
        XCTAssertNil(MarketingMoney.cents("abc"))
        XCTAssertEqual(MarketingDates.parse("2026-02-30", zone: utc), nil)
        XCTAssertEqual(MarketingDates.parse("2026-10-08", zone: utc), 1_791_417_600_000)
        XCTAssertEqual(MarketingDates.text(1_791_417_600_000, time: true, zone: utc), "2026-10-08")
        XCTAssertEqual(MarketingText.title("to_review"), "To Review")
        XCTAssertEqual(MarketingText.title("video-views"), "Video Views")
        XCTAssertEqual(MarketingText.count(12_345), "12.3K")
        XCTAssertEqual(MarketingText.signed(-4), "-4")
        XCTAssertEqual(MarketingFormsTestSupport.split("a; b,\nc, a"), ["a", "b", "c"])
    }

    func testKeywordImport() {
        let rows = MarketingKeywordImport.rows("""
        Top queries,Clicks,Impressions,CTR,Position
        git mirror,12,340,3.5%,8.2
        "forkmesh, cli",1,"1,200",0.1%,"3,5"
        """)
        XCTAssertEqual(rows.map { $0.str("term") }, ["git mirror", "forkmesh, cli"])
        XCTAssertEqual(rows[1]["impressions"], .int(1200))
        XCTAssertEqual(rows[1]["position"], .double(3.5))
        XCTAssertEqual(MarketingKeywordImport.top(rows, limit: 1).first?.str("term"), "git mirror")
        XCTAssertEqual(MarketingKeywordImport.rows("one\ntwo").map { $0.str("term") }, ["one", "two"])
        XCTAssertEqual(MarketingKeywordImport.rows(" \n"), [])
    }

    @MainActor func testClientRoutesEveryWrite() async throws {
        let transport = FakePackagesTransport()
        let client = PackagesClient(transport: transport)
        let overview = MarketingOverview(json: overviewJSON())
        transport.on("POST", "/api/marketing/acme/campaigns", ["campaign": ["id": .string(hexB)]])
        var campaign = MarketingDraft(kind: .campaign, overview: overview, zone: utc)
        campaign["name"] = "Fall"
        let saved = try await client.saveMarketing(campaign, organization: "acme", overview: overview, now: 0, zone: utc)
        XCTAssertEqual(saved.obj("campaign").str("id"), hexB)
        XCTAssertEqual(transport.last("POST", "/api/marketing/acme/campaigns")?.body?.str("name"), "Fall")

        transport.on("PATCH", "/api/marketing/acme/experiments/" + hexB, ["experiment": ["id": .string(hexB)]])
        _ = try await client.setMarketingStatus(.experiment, id: hexB, status: "done", organization: "acme")
        XCTAssertEqual(transport.last("PATCH", "/api/marketing/acme/experiments/" + hexB)?.body?.str("status"), "done")
        do {
            _ = try await client.setMarketingStatus(.experiment, id: hexB, status: "bogus", organization: "acme")
            XCTFail("expected a refusal")
        } catch {}

        transport.on("PATCH", "/api/marketing/acme/ads/" + hexB, ["ad": ["id": .string(hexB)]])
        let results = MarketingDraft(kind: .adResults, id: hexB, overview: overview, json: overview.ads[0].json, zone: utc)
        try await client.saveMarketing(results, organization: "acme", overview: overview, now: 0)
        XCTAssertEqual(transport.last("PATCH", "/api/marketing/acme/ads/" + hexB)?.body?.int64("spendCents"), 2550)

        transport.on("POST", "/api/marketing/acme/funnel", ["funnel": []])
        var week = MarketingDraft(kind: .funnelWeek, id: hexA, overview: overview, zone: utc)
        week["weekStart"] = "2026-10-05"
        try await client.saveMarketing(week, organization: "acme", overview: overview, now: 0, zone: utc)
        XCTAssertEqual(transport.last("POST", "/api/marketing/acme/funnel")?.body?.int64("weekStart"), 1_791_158_400_000)

        transport.on("PUT", "/api/marketing/acme/settings", ["settings": [:]])
        try await client.addMarketingChannels(["github"], organization: "acme", overview: overview)
        XCTAssertEqual(transport.last("PUT", "/api/marketing/acme/settings")?.body?.maps("channels").count, 3)

        transport.on("DELETE", "/api/marketing/acme/reddit/" + hexA, ["deleted": true])
        try await client.deleteMarketing(.redditThread, id: hexA, organization: "acme")
        XCTAssertNotNil(transport.last("DELETE", "/api/marketing/acme/reddit/" + hexA))
        do {
            try await client.deleteMarketing(.metric, id: hexA, organization: "acme")
            XCTFail("metrics cannot be deleted")
        } catch {}

        var refreshed = overviewJSON()
        refreshed["refresh"] = ["items": 3, "imported": 1, "metrics": ["followers": 460]]
        transport.on("POST", "/api/marketing/acme/refresh", refreshed)
        let (after, message) = try await client.refreshMarketingChannel("x", organization: "acme")
        XCTAssertEqual(message, "3 posts read · 1 new · 460 followers")
        XCTAssertEqual(after.organization, "acme")

        transport.on("POST", "/api/marketing/acme/assistant", [
            "summary": "Ship more", "suggestions": [["kind": "content", "title": "Teaser", "channel": "x"]],
        ])
        let advice = try await client.marketingAdvice(organization: "acme", campaignId: hexA, question: "What next?")
        XCTAssertEqual(advice.suggestions.first?.actionTitle, "Add to plan")
        XCTAssertEqual(transport.last("POST", "/api/marketing/acme/assistant")?.body?.str("campaignId"), hexA)

        transport.on("GET", "/api/marketing/acme/performance", ["performance": [
            "status": "ok", "totals": ["sessions": 120, "conversions": 4], "previous": ["sessions": 100],
            "campaigns": [["campaign": "launch", "source": "x", "medium": "social", "sessions": 50, "conversions": 2]],
            "channels": [["label": "Social", "sessions": 60, "bounceRate": 0.5]],
            "insights": [["level": "win", "title": "Launch converts"]],
        ]])
        let performance = try await client.marketingPerformance(organization: "acme", days: 7, now: 7 * 86_400_000)
        XCTAssertTrue(performance.ok)
        XCTAssertEqual(performance.sessionsChange, 20)
        XCTAssertEqual(performance.campaigns.first?.label, "launch (x / social)")
        XCTAssertEqual(performance.channels.first?.facts, "60 visits · 50% bounce")
        XCTAssertEqual(transport.last("GET", "/api/marketing/acme/performance")?.query.first?.1, "0")

        transport.on("GET", "/api/marketing/acme/report", ["generatedAt": 1, "markdown": "# Weekly"])
        let report = try await client.marketingReport(organization: "acme")
        XCTAssertEqual(report.markdown, "# Weekly")

        transport.on("POST", "/api/marketing/acme/links", ["url": "https://acme.com/?utm_source=x",
                                                            "checks": [["message": "utm_medium is missing"]]])
        var link = MarketingDraft(kind: .link, overview: overview, zone: utc)
        link["source"] = "x"
        let built = try await client.buildMarketingLink(link, organization: "acme", overview: overview, now: 0)
        XCTAssertEqual(built.checks, ["utm_medium is missing"])

        transport.on("POST", "/api/marketing/acme/keywords", ["created": 2, "updated": 0, "skipped": 1])
        let imported = try await client.importMarketingKeywords(
            MarketingKeywordImport.rows("a\nb\nc"), organization: "acme", source: "Search Console", observedAt: 9)
        XCTAssertEqual(imported.created, 2)
        XCTAssertEqual(transport.last("POST", "/api/marketing/acme/keywords")?.body?.maps("rows").count, 3)

        transport.on("DELETE", "/api/marketing/acme/x-connection", ["x": ["connected": false]])
        try await client.disconnectX(organization: "acme")
        XCTAssertNotNil(transport.last("DELETE", "/api/marketing/acme/x-connection"))

        var invalid = MarketingDraft(kind: .content, overview: overview, zone: utc)
        invalid["title"] = ""
        do {
            try await client.saveMarketing(invalid, organization: "acme", overview: overview, now: 0)
            XCTFail("expected a form error")
        } catch {
            XCTAssertEqual(PackagesError.describe(error), "Title is required.")
        }
        XCTAssertEqual(PackagesError("reddit_thread_exists").message, "That thread is already on the desk.")
    }
}

extension MarketingWorkspaceTests {
    @MainActor func testRedditAccountsRoundTripThroughTheWatchSettings() async throws {
        let overview = MarketingOverview(json: overviewJSON())
        XCTAssertEqual(overview.redditAccounts, [MarketingNamedLink(id: "forkmesh", name: "forkmesh",
                                                                    url: "https://www.reddit.com/user/forkmesh/submitted/")])
        XCTAssertTrue(MarketingOverview(json: [:]).redditAccounts.isEmpty)

        var watch = MarketingForms.settingsDraft(.redditWatch, overview: overview, zone: utc)
        let field = try XCTUnwrap(watch.fields.first { $0.key == "accounts" })
        XCTAssertEqual(field.label, "Accounts")
        XCTAssertEqual(field.hint, "Up to 10, without u/")
        XCTAssertEqual(watch["accounts"], "forkmesh")
        watch["accounts"] = "forkmesh\n u/acme_team \n\nhttps://www.reddit.com/user/helper/"

        let transport = FakePackagesTransport()
        let client = PackagesClient(transport: transport)
        transport.on("PUT", "/api/marketing/acme/settings", ["settings": [:]])
        try await client.saveMarketing(watch, organization: "acme", overview: overview, now: 0, zone: utc)
        XCTAssertEqual(transport.last("PUT", "/api/marketing/acme/settings")?.body?.obj("reddit"), [
            "subreddits": ["git"], "queries": ["git mirror"],
            "accounts": ["forkmesh", "u/acme_team", "https://www.reddit.com/user/helper/"],
        ])

        watch["accounts"] = (1...11).map { "user\($0)" }.joined(separator: "\n")
        XCTAssertThrowsError(try watch.body(overview: overview, now: 0, zone: utc))
        XCTAssertEqual(PackagesError("invalid_reddit_account").message, "That is not a Reddit username.")
        XCTAssertEqual(PackagesError("too_many_reddit_accounts").message, "Up to 10 Reddit accounts.")
    }

    func testRedditThreadActivityLine() {
        let now: Int64 = 1_800_000_000_000
        func thread(_ json: JSONObject) -> MarketingRecord {
            var base: JSONObject = ["id": .string(hexA), "status": "replied", "subreddit": "git"]
            base.merge(json)
            return MarketingRecord(kind: .redditThread, json: base)
        }
        XCTAssertEqual(thread([:]).redditActivity(now: now), "not read yet")
        XCTAssertEqual(thread(["comments": 4, "score": 1, "views": 1200, "readAs": "forkmesh", "checkedAt": .int(now - 7_200_000)])
            .redditActivity(now: now), "4 comments · 1 point · 1200 views · read 2h ago as u/forkmesh")
        XCTAssertEqual(thread(["comments": 3, "score": 5, "views": .null, "author": "op_user", "checkedAt": .int(now - 120_000)])
            .redditActivity(now: now), "3 comments · 5 points · views show only to u/op_user · read 2m ago")

        let onlyComment = thread([
            "comments": 2, "score": .null, "views": .null, "monitorCommentUrl": "https://www.reddit.com/r/git/comments/abc/x/def/",
            "commentViews": 7, "commentScore": .null, "commentAuthor": "forkmesh", "author": "op_user",
            "checkedAt": .int(now - 3 * 86_400_000),
        ]).redditActivity(now: now)
        XCTAssertTrue(onlyComment.contains("7 views"))
        XCTAssertEqual(onlyComment, "2 comments · your comment: 7 views · read 3d ago")

        let hiddenComment = thread([
            "score": 9, "monitorCommentUrl": "https://www.reddit.com/r/git/comments/abc/x/def/", "commentScore": 3,
            "commentAuthor": "forkmesh", "author": "op_user", "checkedAt": .int(now - 120_000),
        ]).redditActivity(now: now)
        XCTAssertEqual(hiddenComment, "9 points · your comment: 3 points · views show only to u/forkmesh · read 2m ago")

        let unwatched = thread(["commentViews": 7, "commentScore": 2, "checkedAt": .int(now - 120_000)]).redditActivity(now: now)
        XCTAssertEqual(unwatched, "read 2m ago")
        XCTAssertEqual(MarketingRecord(kind: .post, json: [:]).redditActivity(now: now), "")
    }

    @MainActor func testModelSavesReloadsAndReportsErrors() async throws {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/marketing", ["organizations": [["name": "acme", "installed": true]]])
        transport.on("GET", "/api/marketing/acme", overviewJSON())
        let client = PackagesClient(transport: transport)
        let model = MarketingModel(client: client, zone: utc, clock: { 1_000 })
        await model.refresh()
        let overview = try XCTUnwrap(model.overview)
        XCTAssertEqual(model.organization, "acme")

        transport.on("POST", "/api/marketing/acme/content", status: 201, ["content": ["id": .string(hexB)]])
        var draft = try XCTUnwrap(model.newDraft(.content))
        draft["title"] = "Teaser"
        let error = await model.save(draft)
        XCTAssertNil(error)
        XCTAssertEqual(model.notice, "Content added.")
        XCTAssertEqual(transport.paths.suffix(2), ["POST /api/marketing/acme/content", "GET /api/marketing/acme"])

        transport.on("POST", "/api/marketing/acme/reddit", status: 400, ["error": "invalid_reddit_thread_url"])
        var thread = try XCTUnwrap(model.newDraft(.redditThread))
        thread["url"] = "https://example.com/x"
        let refused = await model.save(thread)
        XCTAssertEqual(refused, "Paste a Reddit thread link (https://www.reddit.com/r/…/comments/…).")

        transport.on("PATCH", "/api/marketing/acme/content/" + hexA, ["content": ["id": .string(hexA)]])
        await model.setStatus(.content, id: hexA, status: "published")
        XCTAssertEqual(transport.last("PATCH", "/api/marketing/acme/content/" + hexA)?.body?.int64("publishedAt"), 1_000)
        XCTAssertEqual(model.notice, "Moved to Published.")

        transport.on("PATCH", "/api/marketing/acme/campaigns/" + hexA, ["campaign": ["id": .string(hexA)]])
        await model.toggleTask(overview.campaigns[0], index: 1, done: true)
        XCTAssertEqual(transport.last("PATCH", "/api/marketing/acme/campaigns/" + hexA)?.body?.maps("tasks").map { $0.bool("done") },
                       [true, true])

        transport.on("POST", "/api/marketing/acme/assistant", ["campaignId": .string(hexA), "summary": "Go",
                                                                "suggestions": [["kind": "task", "title": "Post recap"]]])
        await model.ask(campaignId: hexA)
        let idea = try XCTUnwrap(model.advice?.suggestions.first)
        let applied = await model.apply(idea)
        XCTAssertNil(applied)
        XCTAssertEqual(transport.last("PATCH", "/api/marketing/acme/campaigns/" + hexA)?.body?.maps("tasks").last?.str("title"), "Post recap")

        transport.on("POST", "/api/marketing/acme/refresh", status: 502, ["error": "x_access_token_required", "channel": "x"])
        await model.refreshStale()
        XCTAssertEqual(model.errorText, "X (Acme) could not be refreshed: Add an X API bearer token to this channel.")

        let missing = await model.importKeywords(" ", source: "", limit: 10, observedAt: 0)
        XCTAssertEqual(missing, "No keywords found in that text.")
        let deleted = await model.delete(.post, id: hexA)
        XCTAssertFalse(deleted)
    }
}

extension MarketingWorkspaceTests {
    static let png = "data:image/png;base64,iVBORw0KGgo="

    func readyJSON(post: Bool, image: Bool = true) -> JSONObject {
        var ready: JSONObject = [
            "channel": "x", "needsPost": .bool(!post),
            "history": [["id": .string(hexB), "text": "Old", "status": "posted", "closedAt": 5, "closedBy": "sam",
                         "url": "https://x.com/acme/status/9"]],
        ]
        if post {
            var item: JSONObject = [
                "id": .string(hexA), "text": "Mirrors in one click https://acme.com", "length": 44, "hasImage": true,
                "imageAlt": "A chart", "why": "Launch week", "model": "claude-opus-5-5", "author": "herald",
                "createdAt": 1_000, "intentUrl": "https://x.com/intent/post?text=Mirrors",
            ]
            if image { item["imageDataUrl"] = .string(Self.png) }
            ready["post"] = .object(item)
        }
        return ready
    }

    func testReadyPostParsesTheCardAndTheEmptyState() throws {
        var json = overviewJSON()
        json["readyPosts"] = ["x": .object(readyJSON(post: true, image: false))]
        let overview = MarketingOverview(json: json)
        let post = try XCTUnwrap(overview.readyX.post)
        XCTAssertTrue(overview.readyX.available)
        XCTAssertFalse(overview.readyX.needsPost)
        XCTAssertEqual(post.lengthLabel, "44/280")
        XCTAssertFalse(post.tooLong)
        XCTAssertFalse(post.nearLimit)
        XCTAssertEqual(post.writer, "claude-opus-5-5")
        XCTAssertTrue(post.hasImage)
        XCTAssertNil(post.imageData)
        XCTAssertEqual(post.composerURL?.host, "x.com")
        XCTAssertTrue(overview.readyX.needsImage(loaded: nil))
        XCTAssertTrue(overview.canCloseReadyPost)

        let loaded = MarketingReadyState(json: readyJSON(post: true))
        XCTAssertEqual(loaded.post?.imageData, Data([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]))
        XCTAssertFalse(overview.readyX.needsImage(loaded: loaded))
        XCTAssertEqual(overview.readyX.merged(with: loaded).post?.imageDataUrl, Self.png)

        let empty = MarketingReadyState(json: readyJSON(post: false))
        XCTAssertTrue(empty.needsPost)
        XCTAssertNil(empty.post)
        XCTAssertEqual(empty.lastClosed?.statusTitle, "Posted")
        XCTAssertEqual(empty.lastClosed?.link?.absoluteString, "https://x.com/acme/status/9")
        XCTAssertEqual(empty.merged(with: loaded), empty)
        XCTAssertFalse(empty.needsImage(loaded: nil))
        XCTAssertFalse(MarketingOverview(json: overviewJSON()).readyX.available)

        var long = readyJSON(post: true)
        var item = long.obj("post")
        item["length"] = 281
        item["model"] = ""
        item["intentUrl"] = "javascript:alert(1)"
        long["post"] = .object(item)
        let over = try XCTUnwrap(MarketingReadyState(json: long).post)
        XCTAssertTrue(over.tooLong)
        XCTAssertEqual(over.writer, "herald")
        XCTAssertNil(over.composerURL)
        item["length"] = 270
        XCTAssertTrue(MarketingReadyPost(json: item)?.nearLimit ?? false)
        item["id"] = "nope"
        XCTAssertNil(MarketingReadyPost(json: item))

        XCTAssertTrue(MarketingReadyPost.sharePosted(activity: "com.atebits.Tweetie2.ShareExtension", completed: true))
        XCTAssertFalse(MarketingReadyPost.sharePosted(activity: "com.atebits.Tweetie2.ShareExtension", completed: false))
        XCTAssertFalse(MarketingReadyPost.sharePosted(activity: "com.apple.UIKit.activity.CopyToPasteboard", completed: true))
        XCTAssertFalse(MarketingReadyPost.sharePosted(activity: nil, completed: true))

        var auditor = overviewJSON()
        auditor["role"] = "auditor"
        XCTAssertFalse(MarketingOverview(json: auditor).canCloseReadyPost)
    }

    @MainActor func testModelLoadsTheImageAndClosesTheReadyPost() async throws {
        let transport = FakePackagesTransport()
        var json = overviewJSON()
        json["readyPosts"] = ["x": .object(readyJSON(post: true, image: false))]
        var after = overviewJSON()
        after["readyPosts"] = ["x": .object(readyJSON(post: false))]
        transport.on("GET", "/api/marketing", ["organizations": [["name": "acme", "installed": true]]])
        transport.on("GET", "/api/marketing/acme", json)
        transport.on("GET", "/api/marketing/acme/ready-post", ["ok": true, "readyPost": .object(readyJSON(post: true))])
        let model = MarketingModel(client: PackagesClient(transport: transport), zone: utc, clock: { 1_000 })
        await model.refresh()
        XCTAssertNil(model.readyX.post?.imageData)

        await model.loadReadyPost()
        XCTAssertEqual(transport.last("GET", "/api/marketing/acme/ready-post")?.query.first?.1, "x")
        XCTAssertNotNil(model.readyX.post?.imageData)
        let gets = transport.paths.filter { $0 == "GET /api/marketing/acme/ready-post" }.count
        await model.loadReadyPost()
        XCTAssertEqual(transport.paths.filter { $0 == "GET /api/marketing/acme/ready-post" }.count, gets)

        await model.sharedReadyPost(id: hexA, activity: "com.apple.UIKit.activity.CopyToPasteboard", completed: true)
        XCTAssertNil(transport.last("PATCH", "/api/marketing/acme/ready-post"))

        transport.on("PATCH", "/api/marketing/acme/ready-post", ["ok": true, "readyPost": .object(readyJSON(post: false))])
        transport.routes["GET /api/marketing/acme"] = [PackagesReply(status: 200, json: after)]
        await model.sharedReadyPost(id: hexA, activity: "com.atebits.Tweetie2.ShareExtension", completed: true)
        let body = try XCTUnwrap(transport.last("PATCH", "/api/marketing/acme/ready-post")?.body)
        XCTAssertEqual(body.str("id"), hexA)
        XCTAssertEqual(body.str("status"), "posted")
        XCTAssertEqual(body.str("channel"), "x")
        XCTAssertEqual(model.notice, "Marked posted. Herald writes the next one.")
        XCTAssertTrue(model.readyX.needsPost)
        XCTAssertEqual(model.readyX.lastClosed?.status, "posted")

        transport.routes["PATCH /api/marketing/acme/ready-post"] = [
            PackagesReply(status: 409, json: ["error": "ready_post_changed", "readyPost": .object(readyJSON(post: true))]),
        ]
        await model.closeReadyPost(id: hexB, status: "skipped")
        XCTAssertEqual(model.notice, "Herald replaced that post with a newer one.")
        XCTAssertEqual(transport.last("PATCH", "/api/marketing/acme/ready-post")?.body?.str("status"), "skipped")

        do {
            _ = try await PackagesClient(transport: transport).closeMarketingReadyPost(organization: "acme", id: hexA, status: "replaced")
            XCTFail("only posted or skipped")
        } catch {}
    }
}

enum MarketingFormsTestSupport {
    static func split(_ raw: String) -> [String] { MarketingForms.splitList(raw) }
}
