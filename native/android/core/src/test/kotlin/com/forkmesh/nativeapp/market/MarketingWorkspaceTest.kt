package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.time.ZoneOffset

class MarketingWorkspaceTest {
    private val hexA = "a".repeat(32)
    private val hexB = "b".repeat(32)
    private val utc = ZoneOffset.UTC

    private fun overviewJson(): Map<String, Any?> = mapOf(
        "organization" to "acme", "role" to "admin", "canManageSettings" to true, "assistantAvailable" to true,
        "content" to listOf(mapOf("id" to hexA, "channel" to "x", "status" to "draft", "title" to "Launch", "body" to "We shipped")),
        "ads" to listOf(
            mapOf(
                "id" to hexB, "channel" to "google-ads", "status" to "active", "name" to "Search", "currency" to "USD",
                "budgetCents" to 10000L, "spendCents" to 2550L, "pacing" to mapOf("state" to "over"), "platformUrl" to "https://ads.google.com",
                "linkChecks" to listOf(mapOf("level" to "warning", "message" to "utm_source is missing")),
            ),
        ),
        "channelCatalog" to listOf(
            mapOf("id" to "facebook", "name" to "Facebook", "category" to "Social networks"),
            mapOf("id" to "x", "name" to "X", "category" to "Social networks"),
            mapOf("id" to "blog", "name" to "Blog", "category" to "Owned media"),
            mapOf("id" to "github", "name" to "GitHub", "category" to "Developer communities"),
        ),
        "channelPlaybooks" to mapOf("Social networks" to mapOf("goal" to "Grow", "kpis" to listOf("Followers"), "tips" to listOf("Post often"))),
        "refreshableChannels" to listOf("x", "blog", "github"),
        "settings" to mapOf(
            "channels" to listOf(
                mapOf(
                    "id" to "x", "enabled" to true, "displayName" to "X (Acme)", "profileUrl" to "https://x.com/acme", "hasAccessToken" to true,
                    "feed" to mapOf("lastError" to "x_access_token_required"),
                ),
                mapOf("id" to "blog", "enabled" to false, "displayName" to "Blog", "profileUrl" to ""),
            ),
            "attribution" to mapOf("landingUrl" to "https://acme.com", "sources" to listOf("x", "reddit")),
            "targets" to mapOf("signupRate" to 4.5),
            "analytics" to mapOf(
                "project" to "", "host" to "acme.com",
                "goals" to listOf(
                    mapOf("label" to "Signup", "event" to "signup", "property" to "", "operator" to "exact", "value" to ""),
                    mapOf("label" to "Pro", "event" to "checkout", "property" to "plan", "operator" to "icontains", "value" to "pro"),
                ),
            ),
            "x" to mapOf("connected" to true, "username" to "acme"),
        ),
        "summary" to mapOf(
            "published" to 2L, "queued" to 1L, "reach" to 900L, "followers" to 450L, "engagements" to 30L,
            "health" to mapOf("good" to 1L, "stale" to 1L),
            "channels" to listOf(
                mapOf("channel" to "x", "followers" to 450L, "health" to mapOf("state" to "stale")),
                mapOf("channel" to "blog", "health" to mapOf("state" to "stale")),
            ),
        ),
        "campaigns" to listOf(
            mapOf(
                "id" to hexA, "name" to "Launch week", "status" to "active", "objective" to "launch", "startsAt" to 1_790_000_000_000L,
                "tasks" to listOf(mapOf("title" to "Announce", "done" to true, "owner" to "sam"), mapOf("title" to "Recap", "done" to false, "owner" to "")),
                "currency" to "USD", "budgetCents" to 125_050L, "channels" to listOf("x", "blog"), "targetVisits" to null,
                "stats" to mapOf(
                    "content" to 1L, "ads" to 2L, "tasksDone" to 1L, "tasksTotal" to 2L, "daysLeft" to 3L,
                    "taggedLink" to "https://acme.com/?utm_campaign=launch_week",
                ),
            ),
        ),
        "campaignTemplates" to listOf(mapOf("key" to "contest", "label" to "From the site", "name" to "Contest", "status" to "active")),
        "experiments" to listOf(
            mapOf(
                "id" to hexB, "title" to "Shorter signup", "status" to "running", "stage" to "activation", "score" to 120L, "reviewDue" to true,
                "impact" to 5L, "confidence" to 4L, "ease" to 6L,
            ),
        ),
        "experimentsSummary" to mapOf("running" to 1L, "runningLimit" to 3L),
        "funnel" to listOf(mapOf("id" to hexA, "weekStart" to 1_789_430_400_000L, "visits" to 1000L, "signups" to 40L)),
        "funnelReport" to mapOf(
            "weekStart" to 1_789_430_400_000L, "bottleneck" to "signupRate", "bottleneckReason" to "Signup rate is low.",
            "rates" to listOf(mapOf("key" to "signupRate", "label" to "Signup rate", "value" to 4.0, "change" to -0.5, "target" to 4.5)),
        ),
        "keywords" to listOf(
            mapOf(
                "id" to hexA, "term" to "git mirror", "position" to 8.2, "positionChange" to 1.3, "impressions" to 340L, "clicks" to 12L,
                "intent" to "commercial", "priority" to 2L,
            ),
        ),
        "redditThreads" to listOf(
            mapOf("id" to hexA, "status" to "to_review", "subreddit" to "git", "title" to "", "score" to 12L, "comments" to 4L),
            mapOf("id" to hexB, "status" to "replied", "subreddit" to "devops", "title" to "Mirrors?"),
        ),
        "redditDesk" to mapOf(
            "subreddits" to listOf(
                mapOf("name" to "git", "url" to "https://www.reddit.com/r/git/new/", "rulesRead" to true, "rules" to listOf(mapOf("title" to "No spam"))),
            ),
            "queries" to listOf(mapOf("query" to "git mirror", "url" to "https://www.reddit.com/search/?q=git+mirror&sort=new")),
            "accounts" to listOf(mapOf("name" to "forkmesh", "url" to "https://www.reddit.com/user/forkmesh/submitted/")),
            "summary" to mapOf("open" to 1L),
        ),
        "posts" to listOf(mapOf("id" to hexA, "channel" to "x", "url" to "https://x.com/acme/status/2", "views" to 1200L, "likes" to 30L)),
        "adsSummary" to mapOf(
            "attention" to 1L,
            "currencies" to listOf(
                mapOf(
                    "currency" to "USD", "ads" to 1L, "active" to 1L, "spendCents" to 2550L, "budgetCents" to 10000L, "impressions" to 1000L,
                    "clicks" to 25L, "ctr" to 2.5,
                ),
            ),
        ),
        "adPlatforms" to listOf(mapOf("channel" to "google-ads", "name" to "Google Ads", "url" to "https://ads.google.com")),
        "suggestions" to listOf(
            mapOf(
                "level" to "warning", "code" to "campaign_ended", "title" to "Launch week has ended.", "campaign" to hexA,
                "action" to mapOf("type" to "campaign-edit", "id" to hexA),
            ),
            mapOf(
                "level" to "action", "code" to "campaign_nothing_scheduled", "title" to "Nothing scheduled",
                "action" to mapOf("type" to "content", "campaign" to "Launch week", "channel" to "x"),
            ),
        ),
    )

    private fun overview() = MarketingOverview.fromJson(overviewJson())

    fun testOverviewParsesTheWholeWorkspace() {
        val overview = overview()
        expect(overview.canManageSettings && overview.assistantAvailable)
        assertEq("X (Acme)", overview.channelName("x"))
        assertEq("Facebook", overview.channelName("facebook"))
        assertEq(listOf("facebook", "github"), overview.addableChannels.map { it.id })
        assertEq(listOf("x"), overview.staleChannels)
        assertEq("x_access_token_required", overview.setting("x")?.lastError)
        expect(overview.setting("x")?.takesToken == true)
        assertEq(listOf("Post often"), overview.playbook("x")?.third)
        assertEq(null, overview.playbook("blog"))
        assertEq(listOf("Launch", "1/2 tasks", "1 post", "2 ads", "3 days left"), overview.campaigns.first().facts(overview))
        assertEq("https://acme.com/?utm_campaign=launch_week", overview.campaigns.first().link)
        assertEq(listOf("Launch week"), overview.campaignNames)
        assertEq(listOf("Activation", "ICE 120", "Review due"), overview.experiments.first().facts(overview))
        assertEq("4% · target 4.5% · -0.5 pts", overview.funnelReport.rates.first().text)
        expect(overview.funnelReport.rates.first().belowTarget)
        assertEq(listOf("1000 visits", "40 signups"), overview.funnel.first().facts(overview))
        assertEq(listOf("Position 8.2 (+1.3)", "340 impressions", "12 clicks", "Commercial", "Priority 2"), overview.keywords.first().facts(overview))
        assertEq("r/git thread", overview.redditThreads.first().title)
        assertEq(listOf(hexA), overview.openRedditThreads.map { it.id })
        assertEq(listOf("No spam"), overview.redditSubreddits.first().rules)
        assertEq("git mirror", overview.redditQueriesText)
        assertEq(listOf("X (Acme)", "1200 views", "30 likes"), overview.posts.first().facts(overview))
        assertEq("25.50 USD spent of 100.00 USD", overview.adTotals.first().facts.first())
        assertEq(1, overview.adsNeedingAttention)
        expect(overview.ads.first().needsAttention)
        assertEq("https://ads.google.com", overview.ads.first().consoleUrl)
        assertEq(listOf("utm_source is missing"), overview.ads.first().linkChecks)
        assertEq("acme", overview.x.username)
        assertEq(1, overview.healthCounts["stale"])
        assertEq(1, overview.content("ship", null, "x").size)
        assertEq(1, overview.ads("active", "sea").size)
        assertEq(listOf(MarketingDate("Starts", 1_790_000_000_000L)), overview.campaigns.first().dates)
    }

    fun testContentDraftRoundTripsAndStampsPublishing() {
        val overview = overview()
        var draft = MarketingDraft.of(MarketingKind.CONTENT, overview, zone = utc)
        assertEq("x", draft["channel"])
        assertEq("draft", draft["status"])
        assertEq("Title is required.", assertFails<MarketingFormException> { draft.body(overview, 5, utc) }.message)
        draft = draft.with("title", "Launch post").with("scheduledAt", "2026-10-08 09:30").with("tags", "launch, x, launch")
            .with("status", "published").with("url", "http://insecure")
        assertFails<MarketingFormException> { draft.body(overview, 5, utc) }
        val body = draft.with("url", "").body(overview, 5, utc)
        assertEq(1_791_451_800_000L, body["scheduledAt"])
        assertEq(listOf("launch", "x"), body["tags"])
        assertEq(5L, body["publishedAt"])
        val existing = MarketingDraft.of(
            MarketingKind.CONTENT, overview,
            mapOf("title" to "Old", "scheduledAt" to 1_791_451_800_000L, "publishedAt" to 9L, "status" to "published"), hexA, utc,
        )
        assertEq("2026-10-08 09:30", existing["scheduledAt"])
        assertEq(null, existing.body(overview, 5, utc)["publishedAt"])
        assertFails<MarketingFormException> { existing.with("scheduledAt", "next week").body(overview, 5, utc) }
    }

    fun testCampaignDraftKeepsTaskOwnersAndMoney() {
        val overview = overview()
        val campaign = overview.campaigns[0]
        var draft = MarketingDraft.of(MarketingKind.CAMPAIGN, overview, campaign.json, campaign.id, utc)
        assertEq("[x] Announce\nRecap", draft["tasks"])
        assertEq("1250.50", draft["budgetCents"])
        assertEq("x, blog", draft["channels"])
        assertEq("", draft["targetVisits"])
        assertEq("2026-09-21", draft["startsAt"])
        draft = draft.with("tasks", "[x] Announce\n[ ] Recap\nNew one").with("budgetCents", "99,5").with("currency", "")
        val body = draft.body(overview, 0, utc)
        assertEq(listOf("sam", "", ""), body.maps("tasks").map { it.str("owner") })
        assertEq(listOf(true, false, false), body.maps("tasks").map { it.bool("done") })
        assertEq(9950L, body["budgetCents"])
        expect(!body.containsKey("currency"))
        expect(body.containsKey("targetVisits") && body["targetVisits"] == null)
        assertFails<MarketingFormException> { draft.with("budgetCents", "12.345").body(overview, 0, utc) }
        assertEq(listOf(true, true), MarketingForms.taskToggleBody(campaign, 1, true).maps("tasks").map { it.bool("done") })
        assertEq("Ship", MarketingForms.addTaskBody(campaign, "Ship").maps("tasks").last().str("title"))
        val template = MarketingForms.templateDraft(overview.campaignTemplates[0], overview, utc)
        expect(template.isNew)
        assertEq("Contest", template["name"])
        expect(!template.body(overview, 0, utc).containsKey("key"))
    }

    fun testSettingsDraftsBuildTheServerShapes() {
        val overview = overview()
        var channel = MarketingForms.channelDraft(overview, "x", utc)
        assertEq(listOf("enabled", "displayName", "profileUrl", "accessToken", "clearAccessToken"), channel.fields.map { it.key })
        assertEq("true", channel["enabled"])
        channel = channel.with("profileUrl", "https://x.com/acme_hq")
        var channels = channel.body(overview, 0, utc).maps("channels")
        assertEq(listOf("x", "blog"), channels.map { it.str("id") })
        assertEq("https://x.com/acme_hq", channels[0].str("profileUrl"))
        expect(!channels[0].containsKey("accessToken"))
        assertEq(false, channels[1]["enabled"])
        channels = channel.with("accessToken", "secret").body(overview, 0, utc).maps("channels")
        assertEq("secret", channels[0].str("accessToken"))
        assertEq(3, MarketingForms.channelDraft(overview, "blog", utc).fields.size)
        assertEq(listOf("x", "blog", "github"), MarketingForms.channelsBody(overview, adding = listOf("github")).maps("channels").map { it.str("id") })

        val goals = MarketingForms.settingsDraft(MarketingKind.GOALS, overview, utc)
        assertEq("Signup | signup\nPro | checkout | plan | icontains | pro", goals["goals"])
        val analytics = goals.body(overview, 0, utc).obj("analytics")
        assertEq(2, analytics.maps("goals").size)
        assertEq("icontains", analytics.maps("goals")[1].str("operator"))
        expect(!analytics.maps("goals")[0].containsKey("property"))

        val targets = MarketingForms.settingsDraft(MarketingKind.TARGETS, overview, utc)
        assertEq("4.5", targets["signupRate"])
        assertEq(mapOf("signupRate" to 4.5, "activationRate" to 30.0), targets.with("activationRate", "30").body(overview, 0, utc).obj("targets"))
        assertFails<MarketingFormException> { targets.with("activationRate", "300").body(overview, 0, utc) }

        val watch = MarketingForms.settingsDraft(MarketingKind.REDDIT_WATCH, overview, utc)
        assertEq(
            mapOf("subreddits" to listOf("git"), "queries" to listOf("git mirror"), "accounts" to listOf("forkmesh")),
            watch.body(overview, 0, utc).obj("reddit"),
        )
        val attribution = MarketingForms.settingsDraft(MarketingKind.ATTRIBUTION, overview, utc)
        assertEq("x, reddit", attribution["sources"])
        assertEq(listOf("x", "reddit"), attribution.body(overview, 0, utc).obj("attribution").strings("sources"))
    }

    fun testRedditAccountsRoundTripThroughTheWatchForm() {
        val overview = overview()
        assertEq(listOf("forkmesh"), overview.redditAccounts.map { it.name })
        assertEq("https://www.reddit.com/user/forkmesh/submitted/", overview.redditAccounts.first().url)
        assertEq("forkmesh", overview.redditAccountsText)
        val watch = MarketingForms.settingsDraft(MarketingKind.REDDIT_WATCH, overview, utc)
        val field = watch.fields.first { it.key == "accounts" }
        assertEq("Accounts", field.label)
        assertEq("Up to 10, without u/", field.hint)
        assertEq("forkmesh", watch["accounts"])
        val edited = watch.with("accounts", "forkmesh\n  u/helper \n\nhttps://www.reddit.com/user/other/").body(overview, 0, utc).obj("reddit")
        assertEq(listOf("forkmesh", "u/helper", "https://www.reddit.com/user/other/"), edited.strings("accounts"))
        assertEq(listOf("git"), edited.strings("subreddits"))
        assertEq(emptyList<String>(), watch.with("accounts", "").body(overview, 0, utc).obj("reddit").strings("accounts"))
        val empty = MarketingOverview.fromJson(overviewJson() + ("redditDesk" to mapOf("accounts" to listOf(mapOf("name" to "", "url" to "")))))
        assertEq(emptyList<String>(), empty.redditAccounts.map { it.name })
        assertEq("", MarketingForms.settingsDraft(MarketingKind.REDDIT_WATCH, empty, utc)["accounts"])
        assertEq("That is not a Reddit username.", PackagesException("invalid_reddit_account").message)
        assertEq("Up to 10 Reddit accounts.", PackagesException("too_many_reddit_accounts").message)
    }

    fun testRedditThreadActivityLine() {
        val now = 10L * 86_400_000L
        fun thread(vararg pairs: Pair<String, Any?>) = MarketingRecord(MarketingKind.REDDIT_THREAD, mapOf("id" to hexA, "subreddit" to "git", *pairs))
        assertEq("not read yet", thread().redditActivity(now))
        assertEq(
            "4 comments · 12 points · 1 view · read 2h ago",
            thread("comments" to 4L, "score" to 12L, "views" to 1L, "checkedAt" to now - 2 * 3_600_000L).redditActivity(now),
        )
        assertEq(
            "1 comment · 3 points · 250 views · your comment: 40 views, 5 points · read 3d ago as u/forkmesh",
            thread(
                "comments" to 1L, "score" to 3L, "views" to 250L, "monitorCommentUrl" to "https://www.reddit.com/r/git/comments/abc/x/def/",
                "commentViews" to 40L, "commentScore" to 5L, "readAs" to "forkmesh", "checkedAt" to now - 3 * 86_400_000L,
            ).redditActivity(now),
        )
        val commentOnly = thread(
            "comments" to 2L, "score" to null, "views" to null, "monitorCommentUrl" to "https://www.reddit.com/r/git/comments/abc/x/def/",
            "commentViews" to 7L, "commentScore" to null, "commentAuthor" to "forkmesh", "author" to "someone", "checkedAt" to now - 60_000L,
        ).redditActivity(now)
        expect("7 views" in commentOnly, commentOnly)
        assertEq("2 comments · your comment: 7 views · read 1m ago", commentOnly)
        assertEq(
            "8 comments · 2 points · views show only to u/forkmesh · read 5m ago as u/helper",
            thread(
                "comments" to 8L, "score" to 2L, "views" to null, "monitorCommentUrl" to "https://www.reddit.com/r/git/comments/abc/x/def/",
                "commentAuthor" to "forkmesh", "author" to "someone", "readAs" to "helper", "checkedAt" to now - 5 * 60_000L,
            ).redditActivity(now),
        )
        assertEq(
            "views show only to u/someone · read 1h ago",
            thread("author" to "someone", "commentAuthor" to "", "checkedAt" to now - 3_600_000L).redditActivity(now),
        )
        assertEq("12 points · not read yet", thread("score" to 12L, "author" to "someone").redditActivity(now))
        assertEq(
            "read 1h ago",
            thread("commentViews" to 9L, "checkedAt" to now - 3_600_000L).redditActivity(now),
        )
        assertEq(listOf("r/git", "To Review"), overview().redditThreads.first().facts(overview()))
        assertEq("", overview().posts.first().redditActivity(now))
    }

    fun testRecordDraftsShapeTheirBodies() {
        val overview = overview()
        val keyword = MarketingDraft.of(MarketingKind.KEYWORD, overview, overview.keywords[0].json, hexA, utc)
        assertEq("2", keyword["priority"])
        assertEq("", keyword["snapshot.position"])
        expect(!keyword.body(overview, 7, utc).containsKey("snapshot"))
        val snapshot = keyword.with("snapshot.position", "6,5").body(overview, 7, utc).obj("snapshot")
        assertEq(6.5, snapshot["position"])
        expect(snapshot.containsKey("impressions") && snapshot["impressions"] == null)
        assertEq(7L, snapshot["at"])

        val post = MarketingDraft.of(MarketingKind.POST, overview, zone = utc).with("url", "https://x.com/acme/status/3").with("views", "1,200")
        val postBody = post.body(overview, 0, utc)
        assertEq(1200L, postBody["views"])
        expect(!postBody.containsKey("likes") && !postBody.containsKey("channel") && !postBody.containsKey("postedAt"))

        val metric = MarketingForms.metricDraft(overview, "x", utc)
        assertEq("450", metric["followers"])
        val metricBody = metric.body(overview, 0, utc)
        assertEq("x", metricBody["channel"])
        assertEq(0L, metricBody["rating"])
        assertEq("stars", MarketingForms.metricDraft(overview, "github", utc).fields.first().key)

        val thread = MarketingDraft.of(MarketingKind.REDDIT_THREAD, overview, zone = utc)
        assertEq("to_review", thread["status"])
        val threadBody = thread.with("url", "https://www.reddit.com/r/git/comments/abc/x/").with("rulesChecked", "true").body(overview, 0, utc)
        assertEq(true, threadBody["rulesChecked"])
        assertEq(false, threadBody["disclosed"])

        val ad = MarketingDraft.of(MarketingKind.AD, overview, zone = utc)
        assertEq("google-ads", ad["channel"])
        assertEq("lifetime", ad["budgetType"])
        assertEq("EUR", ad.with("name", "Retarget").with("currency", "eur").body(overview, 0, utc)["currency"])
        assertEq("25.50", MarketingDraft.of(MarketingKind.AD_RESULTS, overview, overview.ads[0].json, hexB, utc)["spendCents"])

        val xBody = MarketingForms.xPostDraft(overview, overview.content[0]).body(overview, 0, utc)
        assertEq("We shipped", xBody["text"])
        assertEq(hexA, xBody["contentId"])

        assertEq(hexA, MarketingForms.suggestionDraft(overview.suggestions[0], overview, utc)?.id)
        assertEq("Launch week", MarketingForms.suggestionDraft(overview.suggestions[1], overview, utc)?.get("campaign"))
        val idea = MarketingIdea("content", "Teaser", "", "x", "Soon", 2)
        val ideaDraft = MarketingForms.ideaDraft(idea, overview, overview.campaigns[0], 0, utc)
        assertEq("1970-01-03", ideaDraft?.get("scheduledAt"))
        assertEq("Soon", ideaDraft?.get("body"))
        assertEq(null, MarketingForms.ideaDraft(MarketingIdea("task", "t", "", "", "", 0), overview, null, 0, utc))
    }

    fun testHelpers() {
        assertEq(1200L, MarketingMoney.cents("12"))
        assertEq(123_450L, MarketingMoney.cents("$1,234.5"))
        assertEq(99L, MarketingMoney.cents("0,99"))
        assertEq(null, MarketingMoney.cents("-3"))
        assertEq(null, MarketingMoney.cents("abc"))
        assertEq(null, MarketingDates.parse("2026-02-30", utc))
        assertEq(1_791_417_600_000L, MarketingDates.parse("2026-10-08", utc))
        assertEq("2026-10-08", MarketingDates.text(1_791_417_600_000L, true, utc))
        assertEq("To Review", MarketingText.title("to_review"))
        assertEq("Video Views", MarketingText.title("video-views"))
        assertEq("12.3K", MarketingText.count(12_345))
        assertEq("-4", MarketingText.signed(-4))
        assertEq(listOf("a", "b", "c"), MarketingForms.splitList("a; b,\nc, a"))
    }

    fun testKeywordImport() {
        val rows = MarketingKeywordImport.rows(
            "Top queries,Clicks,Impressions,CTR,Position\ngit mirror,12,340,3.5%,8.2\n\"forkmesh, cli\",1,\"1,200\",0.1%,\"3,5\"",
        )
        assertEq(listOf("git mirror", "forkmesh, cli"), rows.map { it.str("term") })
        assertEq(1200L, rows[1]["impressions"])
        assertEq(3.5, rows[1]["position"])
        assertEq("git mirror", MarketingKeywordImport.top(rows, 1).first().str("term"))
        assertEq(listOf("one", "two"), MarketingKeywordImport.rows("one\ntwo").map { it.str("term") })
        assertEq(emptyList<Map<String, Any?>>(), MarketingKeywordImport.rows(" \n"))
    }

    fun testClientRoutesEveryWrite() {
        val transport = FakeTransport()
        val client = PackagesClient(transport)
        val overview = overview()
        transport.answer("POST", "/api/marketing/acme/campaigns", mapOf("campaign" to mapOf("id" to hexB)))
        val saved = client.saveMarketing(MarketingDraft.of(MarketingKind.CAMPAIGN, overview, zone = utc).with("name", "Fall"), "acme", overview, 0, utc)
        assertEq(hexB, saved.obj("campaign").str("id"))
        assertEq("Fall", transport.last("POST", "/api/marketing/acme/campaigns")?.body?.str("name"))

        client.setMarketingStatus(MarketingKind.EXPERIMENT, hexB, "done", "acme")
        assertEq("done", transport.last("PATCH", "/api/marketing/acme/experiments/$hexB")?.body?.str("status"))
        assertFails<PackagesException> { client.setMarketingStatus(MarketingKind.EXPERIMENT, hexB, "bogus", "acme") }

        client.saveMarketing(MarketingDraft.of(MarketingKind.AD_RESULTS, overview, overview.ads[0].json, hexB, utc), "acme", overview, 0, utc)
        assertEq(2550L, transport.last("PATCH", "/api/marketing/acme/ads/$hexB")?.body?.long("spendCents"))

        client.saveMarketing(MarketingDraft.of(MarketingKind.FUNNEL_WEEK, overview, id = hexA, zone = utc).with("weekStart", "2026-10-05"), "acme", overview, 0, utc)
        assertEq(1_791_158_400_000L, transport.last("POST", "/api/marketing/acme/funnel")?.body?.long("weekStart"))

        client.addMarketingChannels(listOf("github"), "acme", overview)
        assertEq(3, transport.last("PUT", "/api/marketing/acme/settings")?.body?.maps("channels")?.size)

        client.deleteMarketing(MarketingKind.REDDIT_THREAD, hexA, "acme")
        expect(transport.last("DELETE", "/api/marketing/acme/reddit/$hexA") != null)
        assertFails<PackagesException> { client.deleteMarketing(MarketingKind.METRIC, hexA, "acme") }

        transport.answer("POST", "/api/marketing/acme/refresh", overviewJson() + ("refresh" to mapOf("items" to 3L, "imported" to 1L, "metrics" to mapOf("followers" to 460L))))
        val (after, message) = client.refreshMarketingChannel("x", "acme")
        assertEq("3 posts read · 1 new · 460 followers", message)
        assertEq("acme", after.organization)

        transport.answer(
            "POST", "/api/marketing/acme/assistant",
            mapOf("summary" to "Ship more", "suggestions" to listOf(mapOf("kind" to "content", "title" to "Teaser", "channel" to "x"))),
        )
        assertEq("Add to plan", client.marketingAdvice("acme", hexA, "What next?").suggestions.first().actionTitle)
        assertEq(hexA, transport.last("POST", "/api/marketing/acme/assistant")?.body?.str("campaignId"))

        transport.answer(
            "GET", "/api/marketing/acme/performance",
            mapOf(
                "performance" to mapOf(
                    "status" to "ok", "totals" to mapOf("sessions" to 120L, "conversions" to 4L), "previous" to mapOf("sessions" to 100L),
                    "campaigns" to listOf(mapOf("campaign" to "launch", "source" to "x", "medium" to "social", "sessions" to 50L, "conversions" to 2L)),
                    "channels" to listOf(mapOf("label" to "Social", "sessions" to 60L, "bounceRate" to 0.5)),
                    "insights" to listOf(mapOf("level" to "win", "title" to "Launch converts")),
                ),
            ),
        )
        val performance = client.marketingPerformance("acme", 7, now = 7 * 86_400_000L)
        expect(performance.ok)
        assertEq(20L, performance.sessionsChange)
        assertEq("launch (x / social)", performance.campaigns.first().label)
        assertEq("60 visits · 50% bounce", performance.channels.first().facts)
        assertEq("0", transport.last("GET", "/api/marketing/acme/performance")?.query?.first()?.second)

        transport.answer("GET", "/api/marketing/acme/report", mapOf("generatedAt" to 1L, "markdown" to "# Weekly"))
        assertEq("# Weekly", client.marketingReport("acme").markdown)

        transport.answer("POST", "/api/marketing/acme/links", mapOf("url" to "https://acme.com/?utm_source=x", "checks" to listOf(mapOf("message" to "utm_medium is missing"))))
        assertEq(listOf("utm_medium is missing"), client.buildMarketingLink(MarketingDraft.of(MarketingKind.LINK, overview, zone = utc).with("source", "x"), "acme", overview, 0).checks)

        transport.answer("POST", "/api/marketing/acme/keywords", mapOf("created" to 2L, "updated" to 0L, "skipped" to 1L))
        assertEq(Triple(2, 0, 1), client.importMarketingKeywords(MarketingKeywordImport.rows("a\nb\nc"), "acme", "Search Console", 9))
        assertEq(3, transport.last("POST", "/api/marketing/acme/keywords")?.body?.maps("rows")?.size)

        client.disconnectX("acme")
        expect(transport.last("DELETE", "/api/marketing/acme/x-connection") != null)

        val error = assertFails<PackagesException> { client.saveMarketing(MarketingDraft.of(MarketingKind.CONTENT, overview, zone = utc), "acme", overview, 0) }
        assertEq("Title is required.", PackagesException.describe(error))
        assertEq("That thread is already on the desk.", PackagesException("reddit_thread_exists").message)
    }

    fun testModelSavesReloadsAndReportsErrors() {
        val transport = FakeTransport()
        transport.answer("GET", "/api/marketing", mapOf("organizations" to listOf(mapOf("name" to "acme", "installed" to true))))
        transport.answer("GET", "/api/marketing/acme", overviewJson())
        val model = MarketingModel(PackagesClient(transport), zone = utc, clock = { 1_000L })
        model.refresh()
        val overview = model.overview!!
        assertEq("acme", model.organization)

        transport.answer("POST", "/api/marketing/acme/content", mapOf("content" to mapOf("id" to hexB)), 201)
        assertEq(null, model.save(model.newDraft(MarketingKind.CONTENT)!!.with("title", "Teaser")))
        assertEq("Content added.", model.notice)
        assertEq(listOf("POST /api/marketing/acme/content", "GET /api/marketing/acme"), transport.sent.takeLast(2).map { "${it.method} ${it.path}" })

        transport.answer("POST", "/api/marketing/acme/reddit", mapOf("error" to "invalid_reddit_thread_url"), 400)
        assertEq(
            "Paste a Reddit thread link (https://www.reddit.com/r/…/comments/…).",
            model.save(model.newDraft(MarketingKind.REDDIT_THREAD)!!.with("url", "https://example.com/x")),
        )

        model.setStatus(MarketingKind.CONTENT, hexA, "published")
        assertEq(1_000L, transport.last("PATCH", "/api/marketing/acme/content/$hexA")?.body?.long("publishedAt"))
        assertEq("Moved to Published.", model.notice)

        model.toggleTask(overview.campaigns[0], 1, true)
        assertEq(listOf(true, true), transport.last("PATCH", "/api/marketing/acme/campaigns/$hexA")?.body?.maps("tasks")?.map { it.bool("done") })

        transport.answer("POST", "/api/marketing/acme/assistant", mapOf("campaignId" to hexA, "summary" to "Go", "suggestions" to listOf(mapOf("kind" to "task", "title" to "Post recap"))))
        model.ask(hexA)
        assertEq(null, model.apply(model.advice!!.suggestions.first()))
        assertEq("Post recap", transport.last("PATCH", "/api/marketing/acme/campaigns/$hexA")?.body?.maps("tasks")?.last()?.str("title"))

        transport.answer("POST", "/api/marketing/acme/refresh", mapOf("error" to "x_access_token_required", "channel" to "x"), 502)
        model.refreshStale()
        assertEq("X (Acme) could not be refreshed: Add an X API bearer token to this channel.", model.errorText)

        assertEq("No keywords found in that text.", model.importKeywords(" ", "", 10, 0))
        transport.answer("DELETE", "/api/marketing/acme/posts/$hexA", mapOf("error" to "post_not_found"), 404)
        assertEq(false, model.delete(MarketingKind.POST, hexA))
        expect(model.working.isEmpty())
    }

    private fun readyJson(id: String?, image: String = ""): Map<String, Any?> = mapOf(
        "channel" to "x", "needsPost" to (id == null),
        "post" to id?.let {
            mapOf(
                "id" to it, "text" to "We shipped ForkMesh 2 https://forkmesh.com", "length" to 46L, "hasImage" to true, "imageDataUrl" to image,
                "imageAlt" to "Release card", "why" to "Launch week momentum", "model" to "claude-opus-5-5", "author" to "herald",
                "createdAt" to 5_000L, "intentUrl" to "https://x.com/intent/post?text=We",
            )
        },
        "history" to listOf(
            mapOf("id" to hexB, "text" to "Old post", "status" to "posted", "closedAt" to 4_000L, "closedBy" to "sam", "url" to "https://x.com/acme/status/1"),
            mapOf("text" to "No id"),
        ),
    )

    fun testReadyPostParsesLoadsAndCloses() {
        val png = "data:image/png;base64," + java.util.Base64.getEncoder().encodeToString(byteArrayOf(1, 2, 3))
        val ready = MarketingReadyPosts.fromJson(readyJson(hexA, png))
        val post = ready.post!!
        expect(!ready.needsPost)
        assertEq("46/280", post.lengthText)
        expect(!post.overLimit)
        assertEq("Written 2h ago by claude-opus-5-5", post.written("2h ago"))
        assertEq("image/png", post.imageMime)
        assertEq("png", post.imageExtension)
        assertEq(listOf<Byte>(1, 2, 3), post.imageBytes()?.toList())
        assertEq(1, ready.history.size)
        assertEq("Posted", ready.last?.statusTitle)
        assertEq("https://x.com/acme/status/1", ready.last?.url)

        val long = MarketingReadyPost.fromJson(mapOf("id" to hexA, "text" to "a".repeat(300), "author" to "herald"))
        expect(long.overLimit)
        assertEq("300/280", long.lengthText)
        assertEq("herald", long.writer)
        assertEq(null, long.imageBytes())
        assertEq("", MarketingReadyPost.fromJson(mapOf("imageDataUrl" to "data:image/gif;base64,AAAA")).imageMime)
        assertEq(null, MarketingReadyPost.fromJson(mapOf("imageDataUrl" to "data:image/png;base64,***")).imageBytes())

        val empty = MarketingReadyPosts.fromJson(readyJson(null))
        expect(empty.needsPost)
        assertEq(null, empty.post)
        expect(MarketingReadyPosts.fromJson(mapOf("post" to mapOf("id" to "short", "text" to "x"))).needsPost)

        assertEq(null, overview().readyX)
        val withReady = MarketingOverview.fromJson(overviewJson() + ("readyPosts" to mapOf("x" to readyJson(hexA))))
        assertEq(hexA, withReady.readyX?.post?.id)
        expect(withReady.readyX?.post?.hasImage == true)

        val transport = FakeTransport()
        val client = PackagesClient(transport)
        transport.answer("GET", "/api/marketing/acme/ready-post", mapOf("ok" to true, "readyPost" to readyJson(hexA, png)))
        assertEq(png, client.readyPost("acme").post?.imageDataUrl)
        assertEq(listOf("channel" to "x"), transport.last("GET", "/api/marketing/acme/ready-post")?.query)
        transport.answer("PATCH", "/api/marketing/acme/ready-post", mapOf("ok" to true, "readyPost" to readyJson(null)))
        expect(client.closeReadyPost("acme", hexA, "skipped").needsPost)
        assertEq(mapOf("channel" to "x", "id" to hexA, "status" to "skipped"), transport.last("PATCH", "/api/marketing/acme/ready-post")?.body)
        client.closeReadyPost("acme", hexA, "posted", url = "https://x.com/acme/status/2")
        assertEq("https://x.com/acme/status/2", transport.last("PATCH", "/api/marketing/acme/ready-post")?.body?.str("url"))
        assertFails<PackagesException> { client.closeReadyPost("acme", hexA, "replaced") }
        assertFails<PackagesException> { client.closeReadyPost("acme", "nope", "posted") }
    }

    fun testModelLoadsTheReadyImageAndClosesThePost() {
        val png = "data:image/png;base64,AQID"
        val transport = FakeTransport()
        transport.answer("GET", "/api/marketing", mapOf("organizations" to listOf(mapOf("name" to "acme", "installed" to true))))
        transport.answer("GET", "/api/marketing/acme", overviewJson() + ("readyPosts" to mapOf("x" to readyJson(hexA))))
        val model = MarketingModel(PackagesClient(transport), zone = utc, clock = { 1_000L })
        model.refresh()
        assertEq(hexA, model.readyX?.post?.id)
        expect(model.readyImageMissing)

        transport.answer("GET", "/api/marketing/acme/ready-post", mapOf("ok" to true, "readyPost" to readyJson(hexA, png)))
        model.loadReadyPost()
        assertEq(png, model.readyX?.post?.imageDataUrl)
        expect(!model.readyImageMissing)
        assertEq("", model.overview?.readyX?.post?.imageDataUrl)

        transport.answer("GET", "/api/marketing/acme", overviewJson() + ("readyPosts" to mapOf("x" to readyJson(hexB))))
        model.reload()
        assertEq(hexB, model.readyX?.post?.id)
        expect(model.readyImageMissing)

        transport.answer("PATCH", "/api/marketing/acme/ready-post", mapOf("error" to "ready_post_changed", "readyPost" to readyJson(hexA)), 409)
        assertEq(false, model.closeReadyPost("posted", "https://x.com/acme/status/7"))
        assertEq("Herald replaced that post. The card shows the current one.", model.notice)
        assertEq(hexB, transport.last("PATCH", "/api/marketing/acme/ready-post")?.body?.str("id"))
        assertEq("https://x.com/acme/status/7", transport.last("PATCH", "/api/marketing/acme/ready-post")?.body?.str("url"))

        transport.answer("PATCH", "/api/marketing/acme/ready-post", mapOf("ok" to true, "readyPost" to readyJson(null)))
        transport.answer("GET", "/api/marketing/acme", overviewJson() + ("readyPosts" to mapOf("x" to readyJson(null))))
        assertEq(true, model.closeReadyPost("skipped", "https://x.com/acme/status/7"))
        assertEq("Skipped. Herald writes the next one.", model.notice)
        expect(transport.last("PATCH", "/api/marketing/acme/ready-post")?.body?.containsKey("url") == false)
        assertEq(listOf("PATCH /api/marketing/acme/ready-post", "GET /api/marketing/acme"), transport.sent.takeLast(2).map { "${it.method} ${it.path}" })
        expect(model.readyX?.needsPost == true)
        assertEq("Old post", model.readyX?.last?.text)
        assertEq(false, model.closeReadyPost("posted"))
        expect(model.working.isEmpty())
    }

    private fun appStoreJson(): Map<String, Any?> {
        val days = (1..44).map { index ->
            mapOf("day" to java.time.LocalDate.of(2026, 9, 1).plusDays(index - 1L).toString(), "downloads" to index.toLong(), "redownloads" to 1L, "updates" to 2L)
        }
        return mapOf(
            "ok" to true, "canPublish" to true,
            "appStore" to mapOf(
                "app" to mapOf("id" to "6803060735", "name" to "ForkMesh", "bundleId" to "com.forkmesh.forkmesh"),
                "lookup" to mapOf(
                    "appId" to "6803060735", "country" to "US", "name" to "ForkMesh", "version" to "2.4", "releasedAt" to 1_790_000_000_000L,
                    "genre" to "Developer Tools", "price" to "Free", "rating" to 4.6, "ratings" to 12L, "url" to "https://apps.apple.com/us/app/id6803060735",
                ),
                "versions" to listOf(
                    mapOf("version" to "2.4", "build" to "88", "status" to mapOf("state" to "READY_FOR_SALE", "label" to "Ready for sale", "tone" to "good"), "createdAt" to 5L),
                    mapOf("build" to "no version"),
                ),
                "reviews" to listOf(
                    mapOf("createdAt" to 7L, "rating" to 4L, "title" to "Handy", "territory" to "USA", "body" to "Nice"),
                    mapOf("rating" to 0L),
                ),
                "publishedAt" to 9_000L, "publishedBy" to "alice", "fresh" to true,
                "firstDay" to "2026-09-01", "lastDay" to "2026-10-14",
                "days" to days.filter { it["day"] != "2026-10-10" } + listOf(mapOf("day" to "bogus", "downloads" to 99L)),
                "total" to mapOf("downloads" to 990L, "redownloads" to 44L, "updates" to 88L),
                "bestDay" to mapOf("day" to "2026-10-14", "downloads" to 44L),
                "windows" to listOf(
                    mapOf(
                        "days" to 7L, "from" to "2026-10-08", "to" to "2026-10-14", "downloads" to 287L, "redownloads" to 7L, "updates" to 14L,
                        "previous" to mapOf("downloads" to 238L, "redownloads" to 7L, "updates" to 14L), "comparable" to true,
                        "territories" to listOf(mapOf("code" to "US", "downloads" to 200L), mapOf("code" to "DE", "downloads" to 87L)), "territoryCount" to 2L,
                        "devices" to listOf(mapOf("name" to "iPhone", "downloads" to 250L), mapOf("name" to "iPad", "downloads" to 37L)),
                    ),
                    mapOf(
                        "days" to 30L, "from" to "2026-09-15", "to" to "2026-10-14", "downloads" to 885L,
                        "previous" to mapOf("downloads" to 105L), "comparable" to false,
                    ),
                    mapOf("days" to 90L, "downloads" to 990L, "previous" to mapOf("downloads" to 0L), "comparable" to true),
                ),
            ),
        )
    }

    fun testAppStoreParsesTheReportAndHelpers() {
        val store = MarketingAppStore.fromJson(appStoreJson())
        expect(!store.isEmpty)
        expect(store.canPublish)
        assertEq("ForkMesh", store.name)
        assertEq("com.forkmesh.forkmesh", store.app.bundleId)
        assertEq("4.6★", store.lookup.ratingText)
        assertEq(12L, store.lookup.ratings)
        assertEq("Free", store.lookup.price)
        assertEq(listOf("2.4 (88)"), store.versions.map { it.title })
        assertEq(AppStoreTone.GOOD, store.versions.first().status.tone)
        assertEq(1, store.reviews.size)
        assertEq("★★★★☆", store.reviews.first().starsText)
        assertEq(43, store.days.size)
        assertEq(990L, store.total.downloads)
        assertEq("2026-10-14", store.bestDay?.day)
        assertEq(44L, store.bestDay?.downloads)

        val week = store.window(7)!!
        assertEq(287L, week.counts.downloads)
        assertEq(238L, week.previous.downloads)
        assertEq("+21%", week.changeText)
        assertEq(listOf("US", "DE"), week.territories.map { it.label })
        assertEq(2, week.territoryCount)
        assertEq("iPhone", week.devices.first().label)
        assertEq(null, store.window(30)?.downloadsChange)
        assertEq("", store.window(30)?.changeText)
        assertEq(null, store.window(90)?.downloadsChange)
        assertEq(null, store.window(365))

        assertEq(-50.0, MarketingAppStore.change(5, 10, true))
        assertEq(null, MarketingAppStore.change(5, 10, false))
        assertEq("-50%", MarketingAppStore.changeText(-50.0))
        assertEq("0%", MarketingAppStore.changeText(0.2))
        assertEq(listOf(1L, 1L, 2L, 5L, 10L, 50L, 100L, 2000L), listOf(0L, 1L, 2L, 3L, 7L, 44L, 100L, 1001L).map(MarketingAppStore::axisMax))

        val month = store.chartDays(30)
        assertEq(30, month.size)
        assertEq("2026-09-15", month.first().day)
        assertEq("2026-10-14", month.last().day)
        assertEq(0L, month.first { it.day == "2026-10-10" }.downloads)
        assertEq(44L, month.last().downloads)
        val year = store.chartDays(365)
        assertEq(44, year.size)
        assertEq("2026-09-01", year.first().day)
        assertEq(emptyList<MarketingAppStoreDay>(), store.chartDays(0))

        assertEq("Sent from App Store Connect 2h ago by alice", store.sentText("2h ago"))
        assertEq("Sent from App Store Connect 2h ago", store.copy(publishedBy = "").sentText("2h ago"))
        assertEq("14 Oct", MarketingAppStore.dayLabel("2026-10-14"))
        assertEq("United States", MarketingAppStore.countryName("US"))
        assertEq("USA", MarketingAppStore.countryName("USA"))

        val empty = MarketingAppStore.fromJson(mapOf("ok" to true, "appStore" to mapOf("app" to mapOf<String, Any?>(), "lookup" to mapOf<String, Any?>(), "bestDay" to null, "lastDay" to "")))
        expect(empty.isEmpty)
        expect(empty.lookup.isEmpty)
        assertEq(null, empty.bestDay)
        assertEq(emptyList<MarketingAppStoreDay>(), empty.chartDays(30))
        assertEq("", empty.sentText("now"))
        assertEq("App Store", empty.name)
        expect(MarketingAppStore.EMPTY_TEXT.startsWith("No downloads yet. Run native/ios/tools/asc_store_report.py --publish"))
        expect(MarketingAppStore.fromJson(emptyMap()).isEmpty)
    }

    fun testAppStoreLoadsAndChannelCarriesItsNumbers() {
        val transport = FakeTransport()
        val json = overviewJson().toMutableMap()
        val summary = (json["summary"] as Map<*, *>).entries.associate { it.key.toString() to it.value }.toMutableMap()
        summary["channels"] = (summary["channels"] as List<*>) + mapOf(
            "channel" to "app-store", "downloads" to 885L, "redownloads" to 30L, "updates" to 60L, "ratings" to 12L, "rating" to 4.6,
            "health" to mapOf("state" to "good", "source" to "app-store-connect"),
        )
        json["summary"] = summary
        transport.answer("GET", "/api/marketing", mapOf("organizations" to listOf(mapOf("name" to "acme", "installed" to true))))
        transport.answer("GET", "/api/marketing/acme", json)
        val model = MarketingModel(PackagesClient(transport), zone = utc, clock = { 1_000L })
        model.refresh()
        val channel = model.overview!!.stats("app-store")!!
        expect(channel.isAppStore)
        assertEq(885L, channel.downloads)
        assertEq(30L, channel.redownloads)
        assertEq(60L, channel.updates)
        assertEq(12L, channel.ratings)
        assertEq("app-store-connect", channel.source)
        val metric = MarketingForms.metricDraft(model.overview!!, "app-store", utc)
        assertEq("885", metric["downloads"])
        assertEq("12", metric["ratings"])
        expect(metric.fields.any { it.key == "downloads" })

        assertEq(null, model.appStore)
        transport.answer("GET", "/api/marketing/acme/app-store", appStoreJson())
        assertEq(true, model.loadAppStore())
        assertEq("2026-10-14", model.appStore?.lastDay)
        expect(transport.last("GET", "/api/marketing/acme/app-store") != null)
        transport.answer("GET", "/api/marketing/acme/app-store", mapOf("error" to "forbidden"), 403)
        assertEq(false, model.loadAppStore())
        expect(model.errorText.isNotEmpty())
        assertEq("2026-10-14", model.appStore?.lastDay)
        expect(model.working.isEmpty())
    }
}
