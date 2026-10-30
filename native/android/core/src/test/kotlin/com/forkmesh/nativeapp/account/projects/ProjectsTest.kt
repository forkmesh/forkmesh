package com.forkmesh.nativeapp.account.projects

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.AppEnv
import com.forkmesh.nativeapp.api.AuthController
import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ProjectsTest {
    private val prod = ProjectDefaults.PROD_HOST
    private val next = "relay.example.com"

    private fun withSecond(directory: ProjectDirectory): ProjectDirectory {
        directory.add(Project.tryParse(next, label = "Home Relay")!!)
        return directory
    }

    fun testNormalizeOriginAcceptsWhatPeopleType() {
        assertEq("staging.forkmesh.com", Project.normalizeOrigin("staging.forkmesh.com"))
        assertEq("relay.example.com", Project.normalizeOrigin("https://Relay.Example.com:443/some/path"))
        assertEq("relay.example.com:8443", Project.normalizeOrigin("wss://relay.example.com:8443/api/repo/x/ws"))
        assertEq("192.168.1.20:8787", Project.normalizeOrigin("192.168.1.20:8787"))
        assertEq("[::1]:8787", Project.normalizeOrigin("http://[::1]:8787"))
        assertEq(null, Project.normalizeOrigin(""))
        assertEq(null, Project.normalizeOrigin("not a host"))
        assertEq(null, Project.normalizeOrigin("https://-bad-.com"))
    }

    fun testWorldAndHomepageDerivation() {
        val custom = Project.tryParse("app.example.org", now = 1)!!
        assertEq("p/app.example.org/", custom.namespace)
        assertEq("wss://app.example.org${AppEnv.RELAY_PATH}", custom.serverUrl)
        assertEq("https://world.example.org/", custom.worldUri)
        assertEq("https://example.org/", custom.homepageUri)
        assertEq("https://app.example.org", custom.httpOrigin)
        val loopback = Project.tryParse("localhost:8787", now = 1)!!
        assertEq("ws://localhost:8787${AppEnv.RELAY_PATH}", loopback.serverUrl)
        assertEq("http://localhost:8787", loopback.httpOrigin)
        assertEq("localhost:8787 · on this network", loopback.subtitle)
        assertEq("world.example.com", Project.deriveWorldHost("example.com"))
        assertEq("", Project.deriveWorldHost("10.0.0.1"))
        assertEq("https://example.org/terms", LegalLinks.termsFor(custom.homepageUri))
        assertEq("https://forkmesh.com/privacy", LegalLinks.privacyFor(ProjectDefaults.production(0).homepageUri))
        assertEq("https://example.org/#membership", LegalLinks.membershipFor(custom.homepageUri))
        assertEq("https://example.org/design", LegalLinks.designFor(custom.homepageUri))
        assertEq("https://forkmesh.com/design", LegalLinks.designFor(ProjectDefaults.production(0).homepageUri))
        assertEq("https://forkmesh.com/desktop", LegalLinks.desktopFor(ProjectDefaults.production(0).homepageUri))
        assertEq("https://example.org/desktop", LegalLinks.desktopFor(custom.homepageUri))
    }

    fun testFeatureSetsRoundTripTheDartJson() {
        val declared = RelayFeatureSet.declared(setOf(RelayFeature.accounts, RelayFeature.repositories, RelayFeature.repoRead, RelayFeature.chat))
        assertEq(declared, RelayFeatureSet.fromJson(declared.toJson()))
        expect(!declared.has(RelayFeature.agents))
        val learned = RelayFeatureSet.undeclared().markMissing(RelayFeature.ads)
        assertEq(listOf("!ads"), learned.toJson())
        assertEq(learned, RelayFeatureSet.fromJson(listOf("!ads")))
        expect(learned.has(RelayFeature.chat) && !learned.has(RelayFeature.ads))
        assertEq(declared, declared.markMissing(RelayFeature.chat))
        assertEq(RelayFeature.ads, RelayFeature.learnableFromPath("/api/ads/serve?placement=mobile"))
        assertEq(null, RelayFeature.learnableFromPath("/api/repo/a/b/issues"))
        val noCore = RelayFeatureSet.fromVersionBody(mapOf("features" to listOf("chat")))
        assertEq(setOf(RelayFeature.accounts, RelayFeature.repositories, RelayFeature.repoRead), noCore.missingCore)
    }

    fun testProjectJsonRoundTripsTheStoredFormat() {
        val storedRow = Json.parseObject(
            "{\"id\":\"app.forkmesh.com\",\"label\":\"ForkMesh\",\"serverUrl\":\"wss://app.forkmesh.com/x\",\"addedAt\":5," +
                "\"lastUsedAt\":6,\"verifiedAt\":7,\"verifiedRev\":\"abc\",\"builtIn\":true,\"features\":[\"!ads\"]," +
                "\"worldUrl\":\"https://world.forkmesh.com/\",\"homepageUrl\":\"https://forkmesh.com/\",\"namespace\":\"\"}",
        )
        val project = Project.fromJson(storedRow)
        assertEq(storedRow, Json.parseObject(Json.stringify(project.toJson())))
        expect(project.scope.isLegacy)
        val preProjects = Project.fromJson(mapOf("id" to "relay.example.com"))
        assertEq("", preProjects.namespace)
        assertEq("relay.example.com", preProjects.label)
    }

    fun testAProjectNeedsOnlyANameAndKeepsItsOwnNamespace() {
        val prefs = MemoryPrefStore()
        val directory = ProjectDirectory.load(prefs)
        val project = directory.addNamed("  Side Quest ", logo = "side.png")!!
        expect(!project.hasInstance)
        assertEq("Side Quest", project.label)
        expect(project.id.startsWith("local-side_quest-"))
        assertEq(ProjectScope.namespaceFor(project.id), project.namespace)
        assertEq("", project.serverUrl)
        assertEq("", project.httpOrigin)
        assertEq(Project.NO_INSTANCE_LABEL, project.subtitle)
        assertEq("side.png", project.logo)
        assertEq(null, directory.addNamed("   "))
        expect(directory.addNamed("Side Quest")!!.id != project.id)
        val restored = ProjectDirectory.load(prefs).byId(project.id)!!
        expect(!restored.hasInstance)
        assertEq("side.png", restored.logo)
        assertEq(false, restored.toJson()["hasInstance"])
        assertEq("", restored.serverUrl)
        assertEq("Side Quest is not connected to an instance yet.", loginSubtitle(directory.projects, restored))
    }

    fun testANoInstanceProjectSavedWithAServerUrlIsNotLinkedToIt() {
        val old = Project.fromJson(mapOf("id" to "local-tea-1", "label" to "Tea", "serverUrl" to AppEnv.PROD_SERVER_URL, "hasInstance" to false))
        expect(!old.hasInstance)
        assertEq("", old.serverUrl)
        assertEq("", old.httpOrigin)
    }

    fun testAttachingAnInstanceLinksTheSameProject() {
        val prefs = MemoryPrefStore()
        val directory = ProjectDirectory.load(prefs)
        val project = directory.addNamed("Tea")!!
        val instance = Project.tryParse("relay.example.com")!!.copy(verifiedRev = "abc")
        assertEq(null, directory.attach(project.id, Project.named("Other")!!))
        assertEq(null, directory.attach(prod, instance))
        val attached = directory.attach(project.id, instance)!!
        assertEq(project.id, attached.id)
        assertEq(project.namespace, attached.namespace)
        expect(attached.hasInstance)
        assertEq(instance.serverUrl, attached.serverUrl)
        assertEq("https://relay.example.com", attached.httpOrigin)
        assertEq("abc", attached.verifiedRev)
        assertEq(null, directory.attach(project.id, instance))
        val restored = ProjectDirectory.load(prefs).byId(project.id)!!
        expect(restored.hasInstance)
        assertEq(instance.serverUrl, restored.serverUrl)
    }

    fun testAnInstanceBelongsToOnlyOneProject() {
        val directory = ProjectDirectory.load(MemoryPrefStore())
        val instance = Project.tryParse("relay.example.com")!!
        val tea = directory.addNamed("Tea")!!
        val coffee = directory.addNamed("Coffee")!!
        expect(directory.attach(tea.id, instance) != null)
        assertEq(tea.id, directory.ownerOf(instance)?.id)
        assertEq(null, directory.attach(coffee.id, instance))
        assertEq(false, directory.byId(coffee.id)!!.hasInstance)
        assertEq(null, directory.attach(coffee.id, Project.tryParse("https://relay.example.com/ws")!!))
        assertEq(null, directory.attach(coffee.id, Project.tryParse(prod)!!))
        assertEq(null, directory.attach(coffee.id, Project.tryParse("forkmesh.com")!!))

        val count = directory.projects.size
        assertEq(tea.id, directory.add(instance).id)
        assertEq(tea.id, directory.useUrl("wss://relay.example.com/ws")?.id)
        assertEq(tea.id, directory.activeId)
        assertEq(count, directory.projects.size)

        val probe = ProjectProbe(transport = HttpTransport { _ -> error("no request expected") })
        val duplicate = probe.check("https://relay.example.com/", directory.projects)
        assertEq(ProbeStatus.duplicate, duplicate.status)
        assertEq(tea.id, duplicate.knownAs?.id)
        assertEq("That instance already belongs to the project Tea. An instance can only be in one project.", duplicate.message)
        assertEq("This instance is already one of your projects.", probe.check(prod, directory.projects).message)
        expect(directory.attach(coffee.id, Project.tryParse("other.example.com")!!) != null)
    }

    fun testALogoCanBeSetAndClearedOnAnyProject() {
        val directory = ProjectDirectory.load(MemoryPrefStore())
        directory.setLogo(prod, "fm.png")
        assertEq("fm.png", directory.byId(prod)!!.logo)
        assertEq("fm.png", directory.byId(prod)!!.toJson()["logo"])
        directory.setLogo(prod, "")
        assertEq(null, directory.byId(prod)!!.toJson()["logo"])
        val old = Project.fromJson(mapOf("id" to "relay.example.com"))
        expect(old.hasInstance)
        assertEq("", old.logo)
    }

    fun testFreshInstallDefaultsToForkMeshAndNoLongerOffersNext() {
        val prefs = MemoryPrefStore()
        val directory = ProjectDirectory.load(prefs, clock = { 10 })
        assertEq(listOf(prod), directory.projects.map { it.id })
        assertEq(prod, directory.activeId)
        expect(directory.projects.all { it.builtIn })
        assertEq("", directory.byId(prod)!!.namespace)
        assertEq(prod, prefs.getString(ProjectDirectory.ACTIVE_KEY))
        assertEq("p/$next/", withSecond(directory).byId(next)!!.namespace)
    }

    fun testLegacyInstallAndStoredActiveLandOnProduction() {
        assertEq(prod, ProjectDirectory.load(MemoryPrefStore(), legacyInstall = true).activeId)
        val stored = MemoryPrefStore(mapOf(ProjectDirectory.ACTIVE_KEY to prod))
        assertEq(prod, ProjectDirectory.load(stored).activeId)
    }

    fun testStoredListIsKeptAndCustomProjectsFollowBuiltIns() {
        val custom = Project.tryParse("relay.example.com", label = "Home", now = 3)!!
        val list = Json.stringify(listOf(custom.toJson(), ProjectDefaults.production(1).copy(label = "Work").toJson()))
        val prefs = MemoryPrefStore(mapOf(ProjectDirectory.LIST_KEY to list, ProjectDirectory.ACTIVE_KEY to "relay.example.com"))
        val directory = ProjectDirectory.load(prefs)
        assertEq(listOf(prod, "relay.example.com"), directory.projects.map { it.id })
        assertEq("Work", directory.byId(prod)!!.label)
        assertEq("relay.example.com", directory.activeId)
    }

    fun testLegacyServerUrlOverrideBecomesAProjectOnce() {
        val prefs = MemoryPrefStore(mapOf(AccountSettings.SERVER_URL_KEY to "wss://staging.example.com${AppEnv.RELAY_PATH}"))
        val directory = ProjectDirectory.load(prefs, legacyInstall = true)
        assertEq("staging.example.com", directory.activeId)
        assertEq("", directory.active.namespace)
        directory.setActive(prod)
        expect(directory.remove("staging.example.com"))
        val again = ProjectDirectory.load(prefs, legacyInstall = true)
        expect(!again.contains("staging.example.com"))
    }

    fun testSetActiveRemoveAndFallback() {
        var now = 100L
        val directory = ProjectDirectory.load(MemoryPrefStore(), clock = { now })
        val custom = directory.add(Project.tryParse("relay.example.com", now = now)!!)
        expect(!directory.remove(prod))
        now = 200
        directory.setActive(custom.id)
        expect(!directory.remove(custom.id))
        assertEq(200L, directory.active.lastUsedAt)
        assertEq(prod, directory.fallbackFor(custom.id).id)
        directory.markFeatureMissing(custom.id, RelayFeature.ads)
        expect(!directory.byId(custom.id)!!.features.has(RelayFeature.ads))
        directory.update(custom.id, label = "  ", worldUrl = "world.example.com")
        assertEq(custom.id, directory.byId(custom.id)!!.label)
        assertEq("https://world.example.com/", directory.byId(custom.id)!!.worldUrl)
    }

    private fun response(status: Int, body: String, headers: Map<String?, List<String>> = emptyMap()) =
        HttpClient.Response(status, headers, body.toByteArray())

    fun testProbeAdmitsOnlyForkMeshRelaysWithTheCore() {
        var reply = response(200, "{\"ok\":true,\"rev\":\"r1\",\"worker\":\"w\",\"version\":\"1.2.4\"}")
        val requested = ArrayList<String>()
        val probe = ProjectProbe(HttpTransport { request -> requested.add(request.url); reply }, clock = { 9 })
        val ok = probe.check("relay.example.com")
        assertEq(ProbeStatus.ok, ok.status)
        assertEq(listOf("https://relay.example.com/api/version", "https://relay.example.com/api/instance"), requested)
        assertEq("r1", ok.project!!.verifiedRev)
        assertEq("Connected to ForkMesh 1.2.4.", ok.message)
        assertEq(ProbeStatus.notHttps, probe.check("http://relay.example.com").status)
        assertEq(ProbeStatus.invalidUrl, probe.check("!!").status)
        assertEq(ProbeStatus.duplicate, probe.check("app.forkmesh.com", listOf(ProjectDefaults.production(0))).status)
        reply = response(200, "{\"ok\":true}")
        assertEq(ProbeStatus.notForkMesh, probe.check("relay.example.com").status)
        reply = response(503, "", mapOf("x-forkmesh-worker" to listOf("w")))
        assertEq(ProbeStatus.serverError, probe.check("relay.example.com").status)
        reply = response(200, "{\"ok\":true,\"rev\":\"r\",\"worker\":\"w\",\"features\":[\"chat\"]}")
        val missing = probe.check("relay.example.com")
        assertEq(ProbeStatus.missingCore, missing.status)
        expect(missing.message.contains("Accounts"))
        val failing = ProjectProbe(HttpTransport { throw java.net.SocketTimeoutException() })
        assertEq(ProbeStatus.timeout, failing.check("relay.example.com").status)
        val refused = ProjectProbe(HttpTransport { throw java.net.ConnectException() })
        assertEq(ProbeStatus.unreachable, refused.check("relay.example.com").status)
    }

    fun testProbeNamesTheInstanceAndFindsItsLogo() {
        val version = response(200, "{\"ok\":true,\"rev\":\"r1\",\"worker\":\"w\",\"version\":\"1.2.4\"}")
        var instance = response(200, "{\"ok\":true,\"name\":\"Acme Code\",\"logo\":\"/instance-logo.png\"}")
        val probe = ProjectProbe(HttpTransport { request -> if (request.url.endsWith("/api/instance")) instance else version })
        val named = probe.check("relay.example.com")
        assertEq("Connected to Acme Code 1.2.4.", named.message)
        assertEq("https://relay.example.com/instance-logo.png", named.logoUrl)
        assertEq("Acme Code", named.project!!.label)
        assertEq("Work", probe.check("relay.example.com", label = "Work").project!!.label)
        instance = response(200, "{\"ok\":true,\"name\":\"Acme Code\",\"logo\":\"//evil.example.com/x.png\"}")
        assertEq("", probe.check("relay.example.com").logoUrl)
        instance = response(404, "")
        val older = probe.check("relay.example.com")
        assertEq(ProbeStatus.ok, older.status)
        assertEq("Connected to ForkMesh 1.2.4.", older.message)
        assertEq("", older.logoUrl)
        assertEq(older.project!!.id, older.project!!.label)
        expect(!older.branded)
    }

    fun testProbeReadsTheLogoFromTheManifestOfAnOlderBrandedInstance() {
        val version = response(200, "{\"ok\":true,\"rev\":\"r1\",\"worker\":\"w\"}")
        var instance = response(200, "{\"ok\":true,\"name\":\"Acme Code\",\"branded\":true}")
        var manifest = response(200, "{\"icons\":[{\"src\":\"/instance-logo.svg\"},{\"src\":\"/instance-logo.png\"}]}")
        val requested = ArrayList<String>()
        val probe = ProjectProbe(HttpTransport { request ->
            requested.add(request.url)
            when {
                request.url.endsWith("/api/instance") -> instance
                request.url.endsWith("/favicon/site.webmanifest") -> manifest
                else -> version
            }
        })
        val older = probe.check("relay.example.com")
        expect(older.branded)
        assertEq("https://relay.example.com/instance-logo.png", older.logoUrl)
        expect(requested.last() == "https://relay.example.com/favicon/site.webmanifest")
        manifest = response(200, "{\"icons\":[{\"src\":\"/favicon/android-chrome-192x192.png\"}]}")
        assertEq("", probe.check("relay.example.com").logoUrl)
        manifest = response(404, "")
        assertEq("", probe.check("relay.example.com").logoUrl)
        instance = response(200, "{\"ok\":true,\"name\":\"Acme Code\",\"branded\":true,\"logo\":\"\"}")
        requested.clear()
        val noLogo = probe.check("relay.example.com")
        assertEq("", noLogo.logoUrl)
        expect(noLogo.branded)
        expect(requested.none { it.endsWith("/favicon/site.webmanifest") })
    }

    private class RecordingDriver(override val directory: ProjectDirectory) : ProjectSwitchDriver {
        val steps = ArrayList<String>()
        var access = true
        var connected: Boolean? = true
        var failAdopt = false

        override val hasAppAccess: Boolean get() = access

        override fun isConnected(): Boolean? = connected

        override fun leave() {
            steps.add("leave")
        }

        override fun clear() {
            steps.add("clear")
        }

        override fun adopt(projectId: String) {
            steps.add("adopt:$projectId")
            if (failAdopt) throw IllegalStateException("keystore busy")
            directory.setActive(projectId)
        }

        override fun join() {
            steps.add("join")
        }

        override fun forget(projectId: String) {
            steps.add("forget:$projectId")
        }
    }

    fun testSwitchRunsTheStepsInOrder() {
        val directory = withSecond(ProjectDirectory.load(MemoryPrefStore())).apply { setActive(next) }
        val driver = RecordingDriver(directory)
        val phases = ArrayList<SwitchPhase>()
        val controller = ProjectSwitchController(driver, sleep = {})
        controller.addListener { phases.add(controller.phase) }
        val result = controller.switchTo(prod)
        expect(result.ok && !result.needsSignIn)
        assertEq(listOf("leave", "clear", "adopt:$prod", "join"), driver.steps)
        assertEq(listOf(SwitchPhase.leaving, SwitchPhase.clearing, SwitchPhase.adopting, SwitchPhase.joining, SwitchPhase.done), phases)
        assertEq(prod, directory.activeId)
        assertEq("Switched to ForkMesh", controller.statusLine)
        assertEq(true, controller.awaitConnection(prod))
        driver.steps.clear()
        expect(controller.switchTo(prod).ok)
        assertEq(emptyList<String>(), driver.steps)
    }

    fun testSwitchReportsOfflineSignInAndFailure() {
        val directory = withSecond(ProjectDirectory.load(MemoryPrefStore())).apply { setActive(next) }
        val driver = RecordingDriver(directory).apply { connected = false; access = false }
        val controller = ProjectSwitchController(driver, sleep = {})
        val result = controller.switchTo(prod)
        expect(result.ok && result.needsSignIn)
        assertEq(false, controller.awaitConnection(prod))
        driver.failAdopt = true
        val failed = controller.switchTo(next)
        expect(!failed.ok)
        assertEq("Could not finish switching to Home Relay. keystore busy.", failed.error)
        assertEq(SwitchPhase.failed, controller.phase)
        assertEq("That project is no longer in your list.", controller.switchTo("gone.example").error)
    }

    fun testSwitchDoesNotWaitForTheRelayToConnect() {
        val directory = withSecond(ProjectDirectory.load(MemoryPrefStore())).apply { setActive(next) }
        val driver = RecordingDriver(directory).apply { connected = null }
        var now = 0L
        val slept = ArrayList<Long>()
        val controller = ProjectSwitchController(driver, joinTimeoutMs = 1_000, sleep = { slept.add(it); now += it }, clock = { now })
        val result = controller.switchTo(prod)
        expect(result.ok)
        assertEq(SwitchPhase.done, controller.phase)
        assertEq(emptyList<Long>(), slept)
        assertEq(0L, now)
        assertEq(false, controller.awaitConnection(prod))
        expect(now >= 1_000)
        assertEq(null, controller.awaitConnection(next))
        driver.connected = true
        assertEq(true, controller.awaitConnection(prod))
    }

    fun testAwaitConnectionStopsWhenAnotherSwitchStarts() {
        val directory = withSecond(ProjectDirectory.load(MemoryPrefStore())).apply { setActive(next) }
        val driver = RecordingDriver(directory).apply { connected = null }
        lateinit var controller: ProjectSwitchController
        var switched = false
        controller = ProjectSwitchController(driver, sleep = {
            if (!switched) {
                switched = true
                controller.switchTo(next)
            }
        })
        expect(controller.switchTo(prod).ok)
        assertEq(null, controller.awaitConnection(prod))
        assertEq(next, directory.activeId)
    }

    fun testRemovingTheActiveProjectSwitchesAwayFirstAndForgetsIt() {
        val directory = ProjectDirectory.load(MemoryPrefStore())
        val custom = directory.add(Project.tryParse("relay.example.com")!!)
        directory.setActive(prod)
        directory.setActive(custom.id)
        val driver = RecordingDriver(directory)
        val controller = ProjectSwitchController(driver, sleep = {})
        expect(!controller.removeProject(prod).ok)
        val result = controller.removeProject(custom.id)
        expect(result.ok)
        assertEq(prod, directory.activeId)
        expect(!directory.contains(custom.id))
        assertEq("forget:${custom.id}", driver.steps.last())
    }

    fun testSessionsAreKeptPerProjectAndTheLegacySlotMoves() {
        val secrets = MemorySecretStore(mapOf(AuthController.SESSION_KEY to "{\"nodeName\":\"alice\",\"sessionToken\":\"t\"}"))
        val prefs = MemoryPrefStore(mapOf(AuthController.GUEST_MODE_KEY to true))
        AuthController.migrateUnscopedSession(secrets, prefs, prod)
        assertEq(null, secrets.read(AuthController.SESSION_KEY))
        assertEq("alice", AuthController.readSession(secrets, prod)?.nodeName)
        assertEq(null, AuthController.readSession(secrets, next))
        assertEq(true, prefs.getBool("auth/guestMode/$prod"))
        expect(AuthController.isSignedInOn(secrets, prefs, prod))
        expect(!AuthController.isSignedInOn(secrets, prefs, next))
        AuthController.forgetProject(secrets, prefs, prod)
        assertEq(null, AuthController.readSession(secrets, prod))
    }

    fun testProjectSettingsAreIsolated() {
        val prefs = MemoryPrefStore()
        val secrets = MemorySecretStore()
        val directory = withSecond(ProjectDirectory.load(prefs))
        fun settingsFor(id: String): AccountSettings {
            val scope = directory.byId(id)!!.scope
            return AccountSettings.forProject(prefs, scope.prefs(prefs), scope.secrets(secrets), { directory.byId(id)!!.serverUrl }, {})
        }
        val production = settingsFor(prod)
        val nextSettings = settingsFor(next)
        production.setDisplayName("Prod Alice")
        nextSettings.setDisplayName("Next Alice")
        nextSettings.setGithubToken("gh-next")
        assertEq("Prod Alice", prefs.getString("profile/name"))
        assertEq("Next Alice", prefs.getString("p/$next/profile/name"))
        assertEq("gh-next", secrets.read("p/$next/import/githubToken"))
        assertEq(null, secrets.read("import/githubToken"))
        assertEq("", settingsFor(prod).githubToken)
        assertEq(AppEnv.PROD_SERVER_URL, production.serverUrl)
        assertEq("wss://$next${AppEnv.RELAY_PATH}", nextSettings.serverUrl)
        expect(!nextSettings.hasServerUrlOverride)
    }

    fun testCrossProjectCatalogReadsEachSignedInProject() {
        val directory = withSecond(ProjectDirectory.load(MemoryPrefStore()))
        val seen = ArrayList<String>()
        val catalog = CrossProjectCatalog(
            HttpTransport { request ->
                synchronized(seen) { seen.add(request.url + " " + request.headers["Authorization"]) }
                if (request.url.contains(next)) {
                    HttpClient.Response(401, emptyMap(), ByteArray(0))
                } else {
                    HttpClient.Response(200, emptyMap(), "[{\"owner\":\"a\",\"name\":\"r\"},{\"owner\":\"\",\"name\":\"x\"}]".toByteArray())
                }
            },
        )
        val tokens = mapOf(prod to "tp", next to "tn")
        val updates = ArrayList<List<ProjectCatalogEntry>>()
        val entries = catalog.refresh(directory, { tokens[it] }) { updates.add(it) }
        assertEq(1, entries.first { it.project.id == prod }.repositories.size)
        assertEq("Your session on Home Relay has expired.", entries.first { it.project.id == next }.error)
        expect(entries.none { it.loading })
        expect(updates.first().all { it.loading })
        expect(seen.contains("https://$prod/api/repositories Bearer tp"))
    }

    fun testCrossProjectCatalogSkipsProjectsWithoutAnInstance() {
        val directory = ProjectDirectory.load(MemoryPrefStore())
        val local = directory.addNamed("Tea")!!
        val seen = ArrayList<String>()
        val catalog = CrossProjectCatalog(
            HttpTransport { request ->
                synchronized(seen) { seen.add(request.url) }
                HttpClient.Response(200, emptyMap(), "[{\"owner\":\"a\",\"name\":\"r\"}]".toByteArray())
            },
        )
        val entries = catalog.refresh(directory, { "token" }) {}
        expect(entries.none { it.project.id == local.id })
        expect(seen.none { !it.startsWith("https://$prod/") })
    }
}
