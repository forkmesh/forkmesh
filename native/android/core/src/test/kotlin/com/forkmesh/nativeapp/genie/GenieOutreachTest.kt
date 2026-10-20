package com.forkmesh.nativeapp.genie

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.market.CrmOrganizationData
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.security.MessageDigest

class GenieOutreachTest {
    private val ada = "a".repeat(32)
    private val bob = "b".repeat(32)
    private val carol = "c".repeat(32)
    private val dan = "d".repeat(32)
    private val eve = "e".repeat(32)
    private val fin = "f".repeat(32)

    private fun organization(flow: String = "Investors", edit: (MutableList<MutableMap<String, Any?>>) -> Unit = {}): GenieCrmOrganization {
        val contacts = mutableListOf<MutableMap<String, Any?>>(
            mutableMapOf("id" to ada, "name" to "Ada Lovelace", "email" to "ada@example.com", "flow" to "customers", "stage" to "lead"),
            mutableMapOf(
                "id" to bob, "name" to "Bob Builder", "email" to "Bob@Example.com", "company" to "Builders", "title" to "CTO",
                "github" to "https://github.com/bobb", "flow" to "customers", "stage" to "lead", "fields" to mapOf("photo" to "https://img.test/bob.jpg"),
            ),
            mutableMapOf("id" to carol, "name" to "Carol", "mastodon" to "@carol@social.test", "flow" to "customers", "stage" to "lead"),
            mutableMapOf("id" to dan, "name" to "Dan", "email" to "dan@example.com", "flow" to "customers", "stage" to "Not interested"),
            mutableMapOf("id" to eve, "name" to "Eve", "email" to "eve@example.com", "flow" to "customers", "stage" to "customer"),
            mutableMapOf("id" to fin, "name" to "Fin", "phone" to "123", "flow" to "customers", "stage" to "lead"),
        )
        edit(contacts)
        val json = mapOf(
            "organization" to "acme",
            "contacts" to contacts,
            "emails" to listOf(
                mapOf("id" to "e1", "contactId" to ada, "subject" to "Intro", "status" to "delivered", "sentAt" to 1_790_000_000_000L),
                mapOf("id" to "e2", "contactId" to bob, "subject" to "Hello", "status" to "external", "direction" to "inbound"),
                mapOf("id" to "e3", "contactId" to carol, "subject" to "Not yet", "status" to "draft"),
            ),
            "settings" to mapOf(
                "fromName" to "Sam from ForkMesh",
                "zoho" to mapOf("region" to "eu", "connected" to true),
                "flows" to listOf(mapOf("id" to "customers", "name" to flow, "stages" to listOf("lead", "qualified", "customer"))),
            ),
            "memberEmails" to listOf("sam@acme.test", "bob@example.com", "not-an-address"),
        )
        return GenieCrmOrganization.fromJson(json)
    }

    private fun pick(id: String, flow: String = "Investors"): GenieOutreachPick {
        val data = organization(flow)
        val contact = data.data.contacts.first { it.id == id }
        return GenieOutreachPick("acme", contact, flow, GenieOutreach.channel(contact)!!, "Sam")
    }

    fun testOnlyOpenReachableContactsNeverEmailedAreCandidates() {
        val data = organization()
        assertEq(listOf(bob, carol), GenieOutreach.candidates(data).map { it.id })
        assertEq(listOf(carol), GenieOutreach.candidates(data, setOf(bob)).map { it.id })
        val contacted = organization { it[1]["lastContactedAt"] = 1 }
        assertEq(listOf(carol), GenieOutreach.candidates(contacted).map { it.id })
        assertEq(GenieOutreachChannel.Email, GenieOutreach.channel(data.data.contacts[1]))
        assertEq(GenieOutreachChannel.Mastodon, GenieOutreach.channel(data.data.contacts[2]))
        assertEq(null, GenieOutreach.channel(data.data.contacts[5]))
    }

    fun testPickUsesTheCrmSenderAndTheRandomDraw() {
        val data = organization()
        val first = GenieOutreach.pick(listOf(data), sender = "ada") { 0.0 }
        assertEq(bob, first?.contact?.id)
        assertEq("Sam from ForkMesh", first?.sender)
        assertEq("Investors", first?.flowName)
        assertEq("Email Bob Builder?", first?.caption)
        assertEq("eu", first?.zohoRegion)
        val last = GenieOutreach.pick(listOf(data)) { 0.99 }
        assertEq(carol, last?.contact?.id)
        assertEq("Toot to Carol?", last?.caption)
        assertEq(null, GenieOutreach.pick(listOf(data), setOf(bob, carol)))
    }

    fun testBestPutsTheBiggestDealFirstThenANextStepThenEmailThenTheOldest() {
        assertEq(bob, GenieOutreach.best(listOf(organization()))?.contact?.id)
        assertEq(carol, GenieOutreach.best(listOf(organization { it[2]["value"] = 5000 }))?.contact?.id)
        assertEq(bob, GenieOutreach.best(listOf(organization { it[2]["value"] = 5000; it[1]["value"] = 5000 }))?.contact?.id)
        val stepped = organization { it[2]["value"] = 5000; it[1]["value"] = 5000; it[2]["nextStep"] = "Send the demo" }
        assertEq(carol, GenieOutreach.best(listOf(stepped))?.contact?.id)
        val older = organization {
            it[1]["mastodon"] = "@bob@social.test"
            it[1]["email"] = ""
            it[1]["createdAt"] = 200
            it[2]["createdAt"] = 100
        }
        assertEq(carol, GenieOutreach.best(listOf(older))?.contact?.id)
        assertEq(bob, GenieOutreach.best(listOf(older), setOf(carol))?.contact?.id)
        assertEq(bob, GenieOutreach.best(listOf(organization()), only = GenieOutreachChannel.Email)?.contact?.id)
        assertEq(carol, GenieOutreach.best(listOf(organization()), only = GenieOutreachChannel.Mastodon)?.contact?.id)
        assertEq(null, GenieOutreach.best(listOf(organization()), setOf(bob, carol)))
    }

    fun testSuggestedEmailsCcTheOtherOrganizationMembers() {
        val found = GenieOutreach.best(listOf(organization()))
        assertEq(listOf("sam@acme.test"), found?.cc)
        val toot = GenieOutreach.best(listOf(organization()), only = GenieOutreachChannel.Mastodon)
        assertEq(emptyList<String>(), toot?.cc)
    }

    fun testMastodonHandles() {
        assertEq("@carol@social.test", GenieMastodonHandle.of("@carol@Social.test")?.mention)
        assertEq("carol", GenieMastodonHandle.of("carol@social.test")?.user)
        assertEq("@carol@social.test", GenieMastodonHandle.of("https://social.test/@carol")?.mention)
        assertEq("https://social.test/api/v1/accounts/lookup?acct=carol", GenieMastodonHandle.of("@carol@social.test")?.lookupUrl)
        assertEq(null, GenieMastodonHandle.of("@carol"))
        assertEq(null, GenieMastodonHandle.of("https://social.test/about"))
        assertEq(null, GenieMastodonHandle.of("carol smith@social.test"))
    }

    fun testImagesComeFromAPhotoGithubAndGravatar() {
        val hash = MessageDigest.getInstance("SHA-256").digest("bob@example.com".toByteArray()).joinToString("") { "%02x".format(it) }
        assertEq(
            listOf("https://img.test/bob.jpg", "https://github.com/bobb.png?size=160", "https://gravatar.com/avatar/$hash?s=160&d=404"),
            GenieOutreach.images(pick(bob).contact),
        )
        assertEq(emptyList<String>(), GenieOutreach.images(pick(carol).contact))
    }

    fun testThePromptSaysWhoTheyAreAndHowToPitch() {
        val email = GenieOutreach.messages(pick(bob))
        assertEq(2, email.size)
        expect(email[0].content.contains("Write as Sam."))
        expect(email[1].content.contains("Name: Bob Builder"))
        expect(email[1].content.contains("Company: Builders"))
        expect(email[1].content.contains("Subject:"))
        val toot = GenieOutreach.messages(pick(carol, flow = "Press"))
        expect(toot[1].content.contains("@carol@social.test"))
        expect(toot[1].content.contains("journalist"))
    }

    fun testParsingAnEmailAndAToot() {
        val email = GenieOutreach.parse(
            "```\nSubject: **Mirrors for Builders**\n\nHi Bob, ForkMesh keeps your repositories safe across hosts you choose.\n\nSam\n```",
            pick(bob), writer = "Claude",
        )
        assertEq("Mirrors for Builders", email?.subject)
        assertEq("Bob@Example.com", email?.to)
        expect(email?.byModel == true)
        assertEq(null, GenieOutreach.parse("Subject: Hi\n\nToo short", pick(bob)))
        assertEq(null, GenieOutreach.parse("Subject: Hi [Name]\n\nHello [Name], a long enough body for the check to pass.", pick(bob)))
        val toot = GenieOutreach.parse("\"Hello! ForkMesh mirrors your code across many hosts. #git\"", pick(carol))
        expect(toot?.body?.startsWith("@carol@social.test Hello!") == true)
    }

    fun testFallbackDraftsNeedNoModel() {
        val email = GenieOutreach.write(pick(bob), emptyList())
        expect(!email.byModel)
        assertEq("ForkMesh for Builders", email.subject)
        expect(email.body.startsWith("Hi Bob,"))
        val toot = GenieOutreach.write(pick(carol), emptyList())
        expect(toot.body.startsWith("@carol@social.test Hi Carol!"))
    }

    fun testTheFirstWriterWithAFactualDraftWins() {
        val liar = GenieOutreachWriter("Liar", 5.0) { "Subject: Growth\n\nHi Bob, we grew 400% last month at https://evil.test and you should join." }
        val slow = GenieOutreachWriter("Slow", 0.1) { Thread.sleep(2000); "Subject: Late\n\nHi Bob, this arrives far too late for anyone to read." }
        val honest = GenieOutreachWriter("Honest", 5.0) { "Subject: Mirrors\n\nHi Bob, ForkMesh keeps your repositories safe across independent hosts.\n\nSam" }
        val draft = GenieOutreach.write(pick(bob), listOf(liar, slow, honest))
        assertEq("Honest", draft.writer)
        assertEq("Mirrors", draft.subject)
        assertEq(listOf("https://evil.test", "400"), GenieOutreach.unsupported("we grew 400% at https://evil.test", "nothing"))
        assertEq(emptyList<String>(), GenieOutreach.unsupported("see https://forkmesh.com", GenieOutreach.PITCH))
    }

    fun testInvestorPipelinesGetADeckAndTheirZohoRegion() {
        val investor = GenieOutreach.best(listOf(organization("Angel investors")))
        expect(investor?.investor == true)
        expect(investor?.needsDeck == true)
        val customer = GenieOutreach.best(listOf(organization("Customers")))
        expect(customer?.investor == false)
        val deck = "https://forkmesh.com/pitch-deck/abcdef123456"
        val messages = GenieOutreach.messages(investor!!, deck)
        expect(messages[0].content.contains("Tagline: Code with agents."))
        expect(messages[1].content.contains(deck))
        val fallback = GenieOutreach.fallback(investor, deck)
        expect(fallback.body.contains(deck))
        assertEq("ForkMesh deck for Builders", fallback.subject)
    }

    fun testAMissingDeckLinkIsPutBackBeforeTheSignOff() {
        val body = GenieOutreach.withDeck("Hi Bob,\n\nA note.\n\nBest,\nSam", "https://d.test/x")
        assertEq("Hi Bob,\n\nA note.\n\nOur deck, made for you: https://d.test/x\n\nBest,\nSam", body)
        assertEq("Hi https://d.test/x", GenieOutreach.withDeck("Hi https://d.test/x", "https://d.test/x"))
    }

    fun testDraftsOpenInZohoMailWithTheTeamOnCc() {
        val draft = GenieOutreachDraft(
            GenieOutreachChannel.Email, "bob@example.com", "Hi there", "Line one\nLine two", byModel = false, zohoRegion = "eu",
            cc = listOf("sam@acme.test", "BOB@example.com"),
        )
        val mailto = draft.mailtoUrl!!
        expect(mailto.startsWith("mailto:bob@example.com?cc=sam%40acme.test&subject=Hi%20there&body=Line%20one%0ALine%20two"))
        val zoho = draft.zohoUrl!!
        expect(zoho.startsWith("https://mail.zoho.eu/zm/comp.do?ct=mailto%3Abob%40example.com%3Fcc%3D"))
        expect(GenieMail.zohoCompose("xx", "a@b.co").startsWith("https://mail.zoho.com/"))
        assertEq("Cc: sam@acme.test, BOB@example.com\nSubject: Hi there\n\nLine one\nLine two", draft.shareText)
    }

    fun testTheLogPacesThePopUpsAndRemembersWhoWasShown() {
        val store = MemoryPrefStore()
        val log = GenieOutreachLog(store)
        val now = 1_790_000_000.0
        expect(!log.due(now, now))
        expect(log.due(now + 180, now))
        log.showed(bob, now + 180)
        expect(!log.due(now + 180 + 7199, now))
        expect(log.due(now + 180 + 7200, now))
        log.drafted(carol)
        val again = GenieOutreachLog(store)
        assertEq(setOf(bob, carol), again.skip(now + 200))
        assertEq(setOf(carol), again.skip(now + 8 * 24 * 3600))
        again.setOff(true)
        expect(!GenieOutreachLog(store).due(now + 99999, now))
    }

    fun testCrmDataWithoutMemberEmailsStillParses() {
        val plain = GenieCrmOrganization(CrmOrganizationData.fromJson(mapOf("organization" to "acme")))
        assertEq(emptyList<String>(), plain.memberEmails)
        assertEq(listOf("sam@acme.test", "bob@example.com"), organization().memberEmails)
    }
}
