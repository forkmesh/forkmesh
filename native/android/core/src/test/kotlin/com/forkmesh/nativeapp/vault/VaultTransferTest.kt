package com.forkmesh.nativeapp.vault

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.util.Base64

class VaultTransferTest {
    private fun item(id: String, fields: Map<String, Any?>, owner: String = "alice", error: String = "") =
        VaultItem(id, owner, owner == "alice", if (owner == "alice") "owner" else "read", 1, 0, owner, fields = fields, error = error)

    private fun code(block: () -> Unit): String = assertFails<VaultException>(block).code

    fun testParsesQuotedFieldsCrlfAndByteOrderMark() {
        val text = "﻿a,b,c\r\n\"x, y\",\"say \"\"hi\"\"\",\"two\nlines\"\r\n\r\nlast,,\n"
        assertEq(listOf(listOf("a", "b", "c"), listOf("x, y", "say \"hi\"", "two\nlines"), listOf("last", "", "")), VaultTransfer.parseCsv(text))
    }

    fun testReadsAppleExportAndKeepsTheOneTimeCodeInNotes() {
        val text = """
            Title,URL,Username,Password,Notes,OTPAuth
            GitHub,https://github.com/,octo,pw-one,work,otpauth://totp/GitHub:octo?secret=ABC
            Bank (bank.example),https://bank.example/,me,pw-two,,
        """.trimIndent()
        val read = VaultTransfer.readCsv(text)
        assertEq(VaultTransferSource.APPLE_PASSWORDS, read.source)
        assertEq(2, read.logins.size)
        val fields = read.logins[0].fields
        assertEq("GitHub", fields["title"])
        assertEq("octo", fields["username"])
        assertEq("pw-one", fields["password"])
        assertEq("work\n\notpauth://totp/GitHub:octo?secret=ABC", fields["notes"])
        assertEq("bank.example", read.logins[1].host)
    }

    fun testDetectsTheCommonExportFormats() {
        assertEq(VaultTransferSource.DUCK_DUCK_GO, VaultTransfer.source(listOf("title", "url", "username", "password", "notes")))
        assertEq(VaultTransferSource.CHROME, VaultTransfer.source(listOf("name", "url", "username", "password", "note")))
        assertEq(VaultTransferSource.FIREFOX, VaultTransfer.source(listOf("url", "username", "password", "httpRealm", "formActionOrigin", "guid")))
        assertEq(
            VaultTransferSource.BITWARDEN,
            VaultTransfer.source(listOf("folder", "favorite", "type", "name", "notes", "fields", "reprompt", "login_uri", "login_username", "login_password", "login_totp")),
        )
        assertEq(VaultTransferSource.ONE_PASSWORD, VaultTransfer.source(listOf("Title", "Url", "Username", "Password", "OTPAuth", "Favorite", "Archived", "Tags", "Notes")))
        assertEq(VaultTransferSource.APPLE_PASSWORDS, VaultTransfer.source(listOf("Title", "URL", "Username", "Password", "Notes", "OTPAuth")))
        assertEq(VaultTransferSource.OTHER, VaultTransfer.source(listOf("Password", "Site")))
    }

    fun testReadsBitwardenColumnsAndSkipsSecureNotes() {
        val text = """
            folder,favorite,type,name,notes,fields,reprompt,login_uri,login_username,login_password,login_totp
            ,,login,Mail,,,0,https://mail.example,me@mail.example,secret-1,
            ,,note,Shopping list,eggs,,0,,,,
        """.trimIndent()
        val read = VaultTransfer.readCsv(text)
        assertEq(VaultTransferSource.BITWARDEN, read.source)
        assertEq(listOf("Mail"), read.logins.map { it.title })
        assertEq("https://mail.example", read.logins.first().url)
        assertEq(1, read.skippedRows)
    }

    fun testReadsLatin1Files() {
        val bytes = "title,password\nCafé,pw\n".toByteArray(Charsets.ISO_8859_1)
        assertEq("Café", VaultTransfer.readCsv(bytes).logins.single().title)
    }

    fun testReadsTheCsvInsideSafarisExportArchive() {
        val decoder = Base64.getDecoder()
        val deflated = decoder.decode(
            "UEsDBBQAAAAIAAQLQl05nPsGBgAAAAQAAAAYAAAAX19NQUNPU1gvLl9QYXNzd29yZHMuY3N2yyrNywYAUEsDBBQAAAAIAAQLQl0eIBjWWAAAAF0AAAAXAAAAUGFzc3dvcmRzL1Bhc3N3b3Jkcy5jc3Z7v3t/SGZJTqpOaJCPTmhxalFeYm6qTkBicXF5flGKjl9+SWqxjn9IgGNpSQYvV3FmSapecn6ugkZuqqZORklJQbGVvj5MVF8HqLWgXLcqs0DXUEeHlwsAUEsBAhQDFAAAAAgABAtCXTmc+wYGAAAABAAAABgAAAAAAAAAAAAAAIABAAAAAF9fTUFDT1NYLy5fUGFzc3dvcmRzLmNzdlBLAQIUAxQAAAAIAAQLQl0eIBjWWAAAAF0AAAAXAAAAAAAAAAAAAACAATwAAABQYXNzd29yZHMvUGFzc3dvcmRzLmNzdlBLBQYAAAAAAgACAIsAAADJAAAAAAA=",
        )
        expect(VaultZip.isZip(deflated))
        val read = VaultTransfer.readCsv(deflated)
        assertEq(VaultTransferSource.APPLE_PASSWORDS, read.source)
        assertEq(listOf(VaultTransferLogin(title = "site.com (me)", url = "https://site.com/", username = "me", password = "pw-zip-1")), read.logins)

        val stored = decoder.decode(
            "UEsDBBQAAAAAAAQLQl0AqAZpQwAAAEMAAAAFAAAAcC5DU1Z0aXRsZSx1cmwsdXNlcm5hbWUscGFzc3dvcmQsbm90ZXMKQSxodHRwczovL2EuZXhhbXBsZSx1LHB3LXN0b3JlZCwKUEsBAhQDFAAAAAAABAtCXQCoBmlDAAAAQwAAAAUAAAAAAAAAAAAAAIABAAAAAHAuQ1NWUEsFBgAAAAABAAEAMwAAAGYAAAAAAA==",
        )
        assertEq(listOf("pw-stored"), VaultTransfer.readCsv(stored).logins.map { it.password })

        val noCsv = decoder.decode(
            "UEsDBBQAAAAAAAQLQl2DFtyMAQAAAAEAAAAKAAAAcmVhZG1lLnR4dHhQSwECFAMUAAAAAAAEC0JdgxbcjAEAAAABAAAACgAAAAAAAAAAAAAAgAEAAAAAcmVhZG1lLnR4dFBLBQYAAAAAAQABADgAAAApAAAAAAA=",
        )
        assertEq("zip_no_csv", code { VaultTransfer.readCsv(noCsv) })
        assertEq("csv_too_large", code { VaultZip.firstCsv(deflated, 10) })
        val damaged = deflated.copyOf()
        damaged[120] = (damaged[120].toInt() xor 0xff).toByte()
        assertEq("zip_unreadable", code { VaultTransfer.readCsv(damaged) })
    }

    private fun storedZip(name: String, content: ByteArray): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        java.util.zip.ZipOutputStream(out).use { zip ->
            val entry = java.util.zip.ZipEntry(name)
            entry.method = java.util.zip.ZipEntry.STORED
            entry.size = content.size.toLong()
            entry.compressedSize = content.size.toLong()
            entry.crc = java.util.zip.CRC32().apply { update(content) }.value
            zip.putNextEntry(entry)
            zip.write(content)
            zip.closeEntry()
        }
        return out.toByteArray()
    }

    fun testAnArchiveInsideTheArchiveIsRefusedNotFollowed() {
        val csv = "title,url,username,password,notes\nA,https://a.example,u,pw-inner,\n".toByteArray()
        val once = storedZip("p.csv", csv)
        assertEq(listOf("pw-inner"), VaultTransfer.readCsv(once).logins.map { it.password })
        val twice = storedZip("p.csv", once)
        assertEq("zip_unreadable", code { VaultTransfer.readCsv(twice) })
    }

    fun testTitleFallsBackToHostThenUsername() {
        assertEq("example.org", VaultTransferLogin(url = "example.org", password = "p").fields["title"])
        assertEq("me", VaultTransferLogin(username = "me", password = "p").fields["title"])
        assertEq(VaultTransfer.UNTITLED, VaultTransferLogin(password = "p").fields["title"])
    }

    fun testRefusesFilesThatAreNotPasswordExports() {
        assertEq("csv_empty", code { VaultTransfer.readCsv("") })
        assertEq("csv_unknown_columns", code { VaultTransfer.readCsv("name,address\nA,B\n") })
        assertEq("csv_no_logins", code { VaultTransfer.readCsv("title,password\n,\n") })
        assertEq("csv_too_large", code { VaultTransfer.readCsv(ByteArray(VaultTransfer.MAX_FILE_BYTES + 1)) })
    }

    fun testWrittenFileReadsBackUnchanged() {
        val logins = listOf(
            VaultTransferLogin("Comma, inc", "https://a.example", " spaced ", "q\"uote,=1", "line one\nline two", "otpauth://totp/a?secret=B"),
            VaultTransferLogin("Plain", "https://b.example", "b", "-starts-with-dash"),
        )
        val csv = VaultTransfer.writeCsv(logins)
        expect(csv.startsWith("Title,URL,Username,Password,Notes,OTPAuth\r\n"))
        val read = VaultTransfer.readCsv(csv)
        assertEq(VaultTransferSource.APPLE_PASSWORDS, read.source)
        assertEq(logins, read.logins)
    }

    fun testExportSplitsTheOneTimeCodeOutOfNotes() {
        val login = VaultTransferLogin.of(
            item("i", mapOf("title" to "GitHub", "url" to "https://github.com", "username" to "octo", "password" to "pw", "notes" to "work\n\notpauth://totp/GitHub?secret=ABC")),
        )
        assertEq("work", login.notes)
        assertEq("otpauth://totp/GitHub?secret=ABC", login.otpauth)
        assertEq("work\n\notpauth://totp/GitHub?secret=ABC", login.fields["notes"])
    }

    fun testPlanSkipsLoginsAlreadyInTheVaultAndRepeatsInTheFile() {
        val existing = item("i", mapOf("title" to "GitHub", "url" to "https://github.com/login", "username" to "octo", "password" to "pw", "notes" to ""))
        val unreadable = item("j", emptyMap(), owner = "bob", error = "item_unreadable")
        val incoming = listOf(
            VaultTransferLogin("GitHub", "https://www.github.com/", "Octo", "pw"),
            VaultTransferLogin("GitHub", "https://github.com", "octo", "new-pw"),
            VaultTransferLogin("Mail", "mail.example", "me", "m"),
            VaultTransferLogin("Mail again", "https://mail.example", "me", "m"),
        )
        val plan = VaultTransfer.plan(incoming, listOf(existing, unreadable))
        assertEq(2, plan.duplicates)
        assertEq(listOf("new-pw", "m"), plan.fresh.map { it.password })
    }

    fun testExportListsReadableLoginsSortedByTitle() {
        val items = listOf(
            item("1", mapOf("title" to "zeta", "password" to "p")),
            item("2", mapOf("title" to "Alpha", "password" to "p")),
            item("3", emptyMap(), error = "x"),
            item("4", mapOf("title" to "empty", "password" to "")),
        )
        assertEq(listOf("Alpha", "zeta"), VaultTransfer.logins(items).map { it.title })
    }

    fun testReviewDescribesWhatAnImportWillDo() {
        val review = VaultImportReview("DuckDuckGo", emptyList(), 3, 1, 2)
        assertEq("Import from DuckDuckGo?", review.title)
        assertEq(
            "3 new logins will be encrypted into your vault. 1 already in the vault will be skipped. 2 rows have no username or password and will be left out.",
            review.message,
        )
        assertEq(
            "Imported 2 logins from Apple Passwords. Skipped 1 already in the vault. Stopped with 1 left: This vault is full.",
            VaultTransfer.reportText(VaultImportReport(2, 1, 1, "This vault is full."), "Apple Passwords"),
        )
        assertEq(
            "That file has no Password column. Export a CSV file from your password manager and try again.",
            VaultTransfer.errorText(VaultException("csv_unknown_columns")),
        )
    }

    fun testPrepareReviewsOnlyWhenSomethingIsNew() {
        val existing = listOf(item("i", mapOf("title" to "A", "url" to "https://a.example", "username" to "u", "password" to "p")))
        val same = listOf(VaultTransferLogin("A", "https://a.example", "u", "p"))
        assertEq(VaultImportStart.Message("Every login from Chrome is already in your vault."), VaultTransfer.prepare("Chrome", same, existing))
        assertEq(VaultImportStart.Message("A password manager had no logins to import."), VaultTransfer.prepare("a password manager", emptyList(), existing))
        val fresh = VaultTransfer.prepare("Chrome", same + VaultTransferLogin("B", "https://b.example", "u", "p"), existing)
        assertEq(1, (fresh as VaultImportStart.Review).review.fresh)
        assertEq(VaultImportStart.Message("That file is empty."), VaultTransfer.prepareFile(ByteArray(0), existing))
    }
}
