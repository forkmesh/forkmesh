package com.forkmesh.nativeapp.ci

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class FakeProviderMirrorClient : ProviderMirrorClient {
    val calls = ArrayList<Pair<String, Map<String, Any?>>>()
    val answers = HashMap<String, Map<String, Any?>>()
    var failure: Exception? = null

    override fun providerMirror(repo: RepoRef, action: String, fields: Map<String, Any?>): Map<String, Any?> {
        calls.add(action to fields)
        failure?.let { throw it }
        return answers[action] ?: emptyMap()
    }
}

class ProviderMirrorTest {
    private val catalog = mapOf(
        "providers" to listOf(
            mapOf("key" to "github", "label" to "GitHub", "web" to "https://github.com", "tokenUrl" to "t", "tokenHelp" to "h"),
            mapOf("key" to "gitlab", "label" to "GitLab", "web" to "https://gitlab.com", "tokenUrl" to "t", "tokenHelp" to "h"),
        ),
        "resources" to listOf(
            mapOf("key" to "code", "label" to "Code", "labels" to emptyMap<String, Any?>(), "icon" to "git-branch", "hint" to "h",
                "directions" to listOf("off", "push"), "providers" to listOf("github", "gitlab"), "default" to "push"),
            mapOf("key" to "pulls", "label" to "Pull requests", "labels" to mapOf("gitlab" to "Merge requests"), "icon" to "p",
                "hint" to "h", "directions" to listOf("off", "push", "pull", "both"), "providers" to listOf("github", "gitlab"), "default" to "off"),
            mapOf("key" to "discussions", "label" to "Discussions", "labels" to emptyMap<String, Any?>(), "icon" to "d", "hint" to "h",
                "directions" to listOf("off", "push", "pull", "both"), "providers" to listOf("github"), "default" to "off"),
        ),
    )

    private val mirror = mapOf(
        "provider" to "github", "providerLabel" to "GitHub", "remote" to "octo/tool", "remoteUrl" to "https://github.com/octo/tool",
        "tokenHint" to "…1234", "active" to true, "prune" to false,
        "resources" to mapOf(
            "code" to mapOf("direction" to "push", "mode" to "auto", "comments" to true),
            "pulls" to mapOf("direction" to "pull", "mode" to "manual", "comments" to false),
        ),
        "status" to mapOf(
            "login" to "octo", "canPush" to true,
            "codePush" to mapOf("at" to 1000L, "ok" to true, "refs" to 3L),
            "lastSync" to mapOf("at" to 2000L, "ok" to true, "imported" to 2L, "comments" to 1L),
        ),
        "history" to listOf(mapOf("at" to 2000L, "kind" to "sync", "ok" to true, "message" to "Imported 2 item(s)")),
    )

    fun testRemoteParsingAndDraftValidation() {
        assertEq("octo/tool", ProviderMirrorDraft.cleanRemote(" https://github.com/octo/tool.git "))
        assertEq("octo/tool", ProviderMirrorDraft.cleanRemote("gitlab.com/octo/tool"))
        assertEq(null, ProviderMirrorDraft.cleanRemote("octo/sub/tool"))
        assertEq(null, ProviderMirrorDraft.cleanRemote("octo/.hidden"))
        assertEq(null, ProviderMirrorDraft.cleanRemote("octo/to ol"))
        val draft = ProviderMirrorDraft(remote = "octo/tool")
        assertEq("Paste an access token.", draft.validationError(hasStoredToken = false))
        assertEq(null, draft.validationError(hasStoredToken = true))
        assertEq("That token does not look right.", draft.copy(token = "has space").validationError(true))
        assertEq("GitLab → ForkMesh", ProviderMirrorText.direction("pull", "GitLab"))
    }

    fun testErrorsKeepTheServerMessage() {
        val error = assertFails<ProviderMirrorError> {
            ProviderMirrorRequests.decode(ApiJsonResult(400, mapOf("error" to "verification_failed", "message" to "The token was refused.")))
        }
        assertEq("The token was refused.", error.message)
        assertEq("Choose GitHub or GitLab.", ProviderMirrorError(400, "invalid_provider").message)
    }

    fun testLoadSaveAndManualImport() {
        val client = FakeProviderMirrorClient()
        client.answers["get"] = mapOf("ok" to true, "catalog" to catalog, "codeHosted" to true)
        client.answers["save"] = mapOf("ok" to true, "catalog" to catalog, "codeHosted" to true, "mirror" to mirror, "created" to true)
        client.answers["candidates"] = mapOf("ok" to true, "more" to false, "items" to listOf(
            mapOf("key" to "12", "number" to 12L, "title" to "Speed up", "author" to "erin", "state" to "open", "url" to "u"),
            mapOf("key" to "11", "number" to 11L, "title" to "Typo", "author" to "dan", "state" to "closed", "merged" to true, "imported" to true),
        ))
        client.answers["import"] = mapOf("ok" to true, "catalog" to catalog, "mirror" to mirror, "imported" to 1L)
        val model = ProviderMirrorModel(client, RepoRef("alice", "tool"))
        model.load()
        expect(model.loaded)
        assertEq(null, model.mirror)
        assertEq(listOf("code", "pulls", "discussions"), model.resources.map { it.key })
        assertEq("Merge requests", model.resources[1].title("gitlab"))
        expect(!model.resources[2].supported("gitlab"))

        val pulls = model.resources[1]
        val draft = ProviderMirrorDraft(remote = "https://github.com/octo/tool", token = "github_pat_1234")
            .with(pulls, ProviderMirrorSetting(direction = "pull", mode = "manual", comments = false))
        expect(model.save(draft, "Tool"))
        assertEq("Created octo/tool and connected.", model.notice)
        val sent = client.calls.last { it.first == "save" }.second
        assertEq("octo/tool", sent["remote"])
        assertEq("github_pat_1234", sent["token"])
        @Suppress("UNCHECKED_CAST")
        val resources = sent["resources"] as Map<String, Map<String, Any?>>
        assertEq("push", resources["code"]?.get("direction"))
        assertEq("manual", resources["pulls"]?.get("mode"))
        assertEq(false, resources["pulls"]?.get("comments"))

        assertEq("octo", model.mirror?.login)
        assertEq(3, model.mirror?.codePushRefs)
        assertEq(listOf("pulls"), model.importKinds.map { it.key })

        model.openCandidates("pulls")
        assertEq(listOf("12", "11"), model.candidates.map { it.key })
        assertEq("merged", model.candidates[1].stateLabel)
        model.toggle(model.candidates[1])
        expect(model.selected.isEmpty())
        model.toggle(model.candidates[0])
        assertEq(1, model.importSelected())
        val imported = client.calls.last { it.first == "import" }.second
        assertEq(listOf("12"), imported["keys"])
        assertEq("pulls", imported["kind"])

        client.failure = ProviderMirrorError(400, "verification_failed", "The token was refused.")
        model.verify()
        assertEq("The token was refused.", model.error)
    }

    fun testStatusLines() {
        val state = ProviderMirrorState.fromJson(mirror)!!
        assertEq(true to "Pushed 3 branches and tags 1 min ago.", ProviderMirrorText.codeLine(state, true, 61_000))
        assertEq(null to "Waiting for this repository to be hosted on ForkMesh.", ProviderMirrorText.codeLine(state, false, 61_000))
        assertEq(true to "Checked just now: 2 new, 1 comment.", ProviderMirrorText.syncLine(state, 2_500))
    }
}
