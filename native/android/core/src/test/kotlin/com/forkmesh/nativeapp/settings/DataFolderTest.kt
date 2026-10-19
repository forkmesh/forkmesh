package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.account.projects.ProjectScope
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.nio.file.Files

class DataFolderTest {
    private fun scratch(): File = Files.createTempDirectory("data-folder").toFile()

    private fun write(base: File, path: String, text: String): File =
        File(base, path).also { it.parentFile.mkdirs(); it.writeText(text) }

    fun testProjectFoldersMatchTheDesktopLayout() {
        assertEq("forkmesh", DataFolder.projectFolder(ProjectScope.legacy))
        assertEq("work.example.com_8443", DataFolder.projectFolder(ProjectScope("work.example.com:8443", "p/work.example.com:8443/")))
        assertEq(listOf("data", "config", "cache", "state", "logs", "ssh"), DataFolder.ROOTS)
        assertEq("config/ForkMesh/settings.json", DataFolder.SETTINGS_PATH)
    }

    fun testTheAlertStaysUntilTheFolderAndIdentityAreSaved() {
        val prefs = MemoryPrefStore()
        assertEq(DataFolderNeed.ChooseFolder, DataFolder.need(prefs, "forkmesh", "K", reachable = false))
        prefs.putString(DataFolder.LOCATION_KEY, "content://tree")
        assertEq(DataFolderNeed.ReconnectFolder, DataFolder.need(prefs, "forkmesh", "K", reachable = false))
        assertEq(DataFolderNeed.SaveIdentity, DataFolder.need(prefs, "forkmesh", "K", reachable = true))
        prefs.putString(DataFolder.kitKey("forkmesh"), "K")
        assertEq(null, DataFolder.need(prefs, "forkmesh", "K", reachable = true))
        assertEq(DataFolderNeed.SaveIdentity, DataFolder.need(prefs, "forkmesh", "ROTATED", reachable = true))
    }

    fun testSettingsKeepTheirTypesAndSkipDeviceKeys() {
        val raw = mapOf<String, Any?>(
            "theme/mode" to "dark", "notifications/enabled" to true, "chat/scale" to 4609434218613702656L,
            "chat/step" to 3, "chat/zoom" to 1.5f, "tags" to setOf("b", "a"),
            DataFolder.LOCATION_KEY to "content://tree", "install/installed" to true,
        )
        val text = Json.stringify(DataFolder.exportSettings(raw))
        val restored = DataFolder.decodeSettings(Json.parseObject(text))
        assertEq(
            mapOf<String, Any>(
                "chat/scale" to 4609434218613702656L, "chat/step" to 3, "chat/zoom" to 1.5f,
                "notifications/enabled" to true, "tags" to setOf("a", "b"), "theme/mode" to "dark",
            ),
            restored,
        )
        assertEq(emptyMap<String, Any>(), DataFolder.decodeSettings(mapOf("type" to "forkmesh.mobile-settings")))
    }

    fun testBackupCopiesChangedFilesAndRestoreBringsThemBack() {
        val base = scratch()
        val documents = File(base, "app/flutter")
        val support = File(base, "app/files")
        write(documents, "notes/a.txt", "hello")
        write(support, "chat-history/relay.json", "chat")
        write(support, "LocalModels/model.bin", "big")
        write(support, "projects/work/x.txt", "other project")
        val root = File(base, "picked/${DataFolder.FOLDER_NAME}")
        val store = DataFolderStore(FileDataFolderIo(root))
        expect(store.prepare("android", 1L))
        expect(store.hasData)
        expect(store.prepareProject(DataFolderProject("forkmesh", "", "", "ForkMesh", "K")))
        for (name in DataFolder.ROOTS) expect(File(root, "forkmesh/$name").isDirectory, "$name exists")
        val trees = listOf(
            DataFolderTree("documents", documents, setOf("projects")),
            DataFolderTree("support", support, setOf("projects")),
        )
        val first = store.backup("forkmesh", trees)
        assertEq(2, first.copied)
        expect(first.failed.isEmpty())
        assertEq("hello", File(root, "forkmesh/data/documents/notes/a.txt").readText())
        assertEq("chat", File(root, "forkmesh/data/support/chat-history/relay.json").readText())
        expect(!File(root, "forkmesh/data/support/LocalModels").exists())
        expect(!File(root, "forkmesh/data/support/projects").exists())

        assertEq(0, store.backup("forkmesh", trees).copied)
        val changed = write(documents, "notes/a.txt", "hello again")
        changed.setLastModified(changed.lastModified() + 60_000)
        assertEq(1, store.backup("forkmesh", trees).copied)
        assertEq("hello again", File(root, "forkmesh/data/documents/notes/a.txt").readText())

        val fresh = File(base, "reinstalled/flutter")
        val freshSupport = File(base, "reinstalled/files")
        write(fresh, "notes/a.txt", "made before restoring")
        val restored = DataFolderStore(FileDataFolderIo(root)).restore(
            "forkmesh", listOf(DataFolderTree("documents", fresh), DataFolderTree("support", freshSupport)),
        )
        assertEq(1, restored.copied)
        assertEq(1, restored.unchanged)
        assertEq("made before restoring", File(fresh, "notes/a.txt").readText())
        assertEq("chat", File(freshSupport, "chat-history/relay.json").readText())
    }

    fun testRestoreNeverLeavesTheProjectFolders() {
        val base = scratch()
        val root = File(base, DataFolder.FOLDER_NAME)
        write(root, "forkmesh/${DataFolder.INDEX_PATH}", """{"documents/../../escape.txt":"1:1","documents/ok.txt":"1:1"}""")
        write(root, "forkmesh/data/documents/ok.txt", "ok")
        val local = File(base, "local")
        val report = DataFolderStore(FileDataFolderIo(root)).restore("forkmesh", listOf(DataFolderTree("documents", local)))
        assertEq(1, report.copied)
        expect(!File(base, "escape.txt").exists())
    }

    fun testProjectsAreReadBackFromTheFolder() {
        val root = File(scratch(), DataFolder.FOLDER_NAME)
        val store = DataFolderStore(FileDataFolderIo(root))
        val work = DataFolderProject("work.example.com", "work.example.com", "p/work.example.com/", "Work", "W")
        store.prepareProject(work)
        store.prepareProject(DataFolderProject("forkmesh", "", "", "ForkMesh", "K"))
        write(root, "stray/project.json", "{}")
        assertEq(listOf("forkmesh", "work.example.com"), store.projects().map { it.slug })
        assertEq(work, store.projects().last())
        expect(store.writeSettings(DataFolder.exportSettings(mapOf("theme/mode" to "dark")), "forkmesh"))
        expect(File(root, "forkmesh/config/ForkMesh/settings.json").readText().contains("theme/mode"))
        expect(store.readSettings("work.example.com") == null)
    }

    fun testTheIdentityKitRestoresTheSameKey() {
        val root = File(scratch(), DataFolder.FOLDER_NAME)
        val store = DataFolderStore(FileDataFolderIo(root))
        val identity = Identity.fromSeed(ByteArray(32) { 7 })
        val prefs = MemoryPrefStore()
        assertFails<RecoveryKitException> {
            DataFolderIdentity.saveKit(identity, "short", "short", "forkmesh", store, prefs, 1L)
        }
        DataFolderIdentity.saveKit(identity, "correct horse battery", "correct horse battery", "forkmesh", store, prefs, 1L)
        assertEq(identity.publicKeyB64url, prefs.getString(DataFolder.kitKey("forkmesh")))
        val kit = store.readKit("forkmesh") ?: throw AssertionError("no kit")
        expect(!kit.contains(B64.encodeUrlNoPadding(identity.exportSeed())))
        val secrets = MemorySecretStore()
        assertFails<RecoveryKitException> {
            DataFolderIdentity.restoreSeed(kit, "wrong passphrase!!", "", secrets)
        }
        assertEq(null, secrets.read(Identity.SEED_KEY))
        assertEq(identity.publicKeyB64url, DataFolderIdentity.restoreSeed(kit, "correct horse battery", "p/work/", secrets))
        assertEq(identity.publicKeyB64url, Identity.loadOrCreate(ProjectScope("work", "p/work/").secrets(secrets)).publicKeyB64url)
    }
}
