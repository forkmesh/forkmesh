import FMBase
import Foundation
import XCTest

@testable import FMVault

final class VaultTransferTests: XCTestCase {
    func testParsesQuotedFieldsCRLFAndByteOrderMark() {
        let text = "\u{FEFF}a,b,c\r\n\"x, y\",\"say \"\"hi\"\"\",\"two\nlines\"\r\n\r\nlast,,\n"
        XCTAssertEqual(VaultTransfer.parseCSV(text), [["a", "b", "c"], ["x, y", "say \"hi\"", "two\nlines"], ["last", "", ""]])
    }

    func testReadsAppleExportAndKeepsTheOneTimeCodeInNotes() throws {
        let text = """
            Title,URL,Username,Password,Notes,OTPAuth
            GitHub,https://github.com/,octo,pw-one,work,otpauth://totp/GitHub:octo?secret=ABC
            Bank (bank.example),https://bank.example/,me,pw-two,,
            """
        let read = try VaultTransfer.readCSV(text)
        XCTAssertEqual(read.source, .applePasswords)
        XCTAssertEqual(read.logins.count, 2)
        let fields = read.logins[0].fields
        XCTAssertEqual(fields.str("title"), "GitHub")
        XCTAssertEqual(fields.str("username"), "octo")
        XCTAssertEqual(fields.str("password"), "pw-one")
        XCTAssertEqual(fields.str("notes"), "work\n\notpauth://totp/GitHub:octo?secret=ABC")
        XCTAssertEqual(read.logins[1].host, "bank.example")
    }

    func testDetectsTheCommonExportFormats() {
        XCTAssertEqual(VaultTransfer.source(forHeader: ["title", "url", "username", "password", "notes"]), .duckDuckGo)
        XCTAssertEqual(VaultTransfer.source(forHeader: ["name", "url", "username", "password", "note"]), .chrome)
        XCTAssertEqual(
            VaultTransfer.source(forHeader: ["url", "username", "password", "httpRealm", "formActionOrigin", "guid"]), .firefox)
        XCTAssertEqual(
            VaultTransfer.source(forHeader: ["folder", "favorite", "type", "name", "notes", "fields", "reprompt", "login_uri", "login_username", "login_password", "login_totp"]),
            .bitwarden)
        XCTAssertEqual(VaultTransfer.source(forHeader: ["Password", "Site"]), .other)
    }

    func testReadsBitwardenColumnsAndSkipsSecureNotes() throws {
        let text = """
            folder,favorite,type,name,notes,fields,reprompt,login_uri,login_username,login_password,login_totp
            ,,login,Mail,,,0,https://mail.example,me@mail.example,secret-1,
            ,,note,Shopping list,eggs,,0,,,,
            """
        let read = try VaultTransfer.readCSV(text)
        XCTAssertEqual(read.source, .bitwarden)
        XCTAssertEqual(read.logins.map(\.title), ["Mail"])
        XCTAssertEqual(read.logins.first?.url, "https://mail.example")
        XCTAssertEqual(read.skippedRows, 1)
    }

    func testReadsTheCSVInsideSafarisExportArchive() throws {
        let deflated = try XCTUnwrap(Data(base64Encoded: "UEsDBBQAAAAIAAQLQl05nPsGBgAAAAQAAAAYAAAAX19NQUNPU1gvLl9QYXNzd29yZHMuY3N2yyrNywYAUEsDBBQAAAAIAAQLQl0eIBjWWAAAAF0AAAAXAAAAUGFzc3dvcmRzL1Bhc3N3b3Jkcy5jc3Z7v3t/SGZJTqpOaJCPTmhxalFeYm6qTkBicXF5flGKjl9+SWqxjn9IgGNpSQYvV3FmSapecn6ugkZuqqZORklJQbGVvj5MVF8HqLWgXLcqs0DXUEeHlwsAUEsBAhQDFAAAAAgABAtCXTmc+wYGAAAABAAAABgAAAAAAAAAAAAAAIABAAAAAF9fTUFDT1NYLy5fUGFzc3dvcmRzLmNzdlBLAQIUAxQAAAAIAAQLQl0eIBjWWAAAAF0AAAAXAAAAAAAAAAAAAACAATwAAABQYXNzd29yZHMvUGFzc3dvcmRzLmNzdlBLBQYAAAAAAgACAIsAAADJAAAAAAA="))
        XCTAssertTrue(VaultZip.isZip(deflated))
        let read = try VaultTransfer.readCSV(deflated)
        XCTAssertEqual(read.source, .applePasswords)
        XCTAssertEqual(read.logins, [VaultTransferLogin(title: "site.com (me)", url: "https://site.com/", username: "me", password: "pw-zip-1")])

        let stored = try XCTUnwrap(Data(base64Encoded: "UEsDBBQAAAAAAAQLQl0AqAZpQwAAAEMAAAAFAAAAcC5DU1Z0aXRsZSx1cmwsdXNlcm5hbWUscGFzc3dvcmQsbm90ZXMKQSxodHRwczovL2EuZXhhbXBsZSx1LHB3LXN0b3JlZCwKUEsBAhQDFAAAAAAABAtCXQCoBmlDAAAAQwAAAAUAAAAAAAAAAAAAAIABAAAAAHAuQ1NWUEsFBgAAAAABAAEAMwAAAGYAAAAAAA=="))
        XCTAssertEqual(try VaultTransfer.readCSV(stored).logins.map(\.password), ["pw-stored"])

        let noCSV = try XCTUnwrap(Data(base64Encoded: "UEsDBBQAAAAAAAQLQl2DFtyMAQAAAAEAAAAKAAAAcmVhZG1lLnR4dHhQSwECFAMUAAAAAAAEC0JdgxbcjAEAAAABAAAACgAAAAAAAAAAAAAAgAEAAAAAcmVhZG1lLnR4dFBLBQYAAAAAAQABADgAAAApAAAAAAA="))
        XCTAssertThrowsError(try VaultTransfer.readCSV(noCSV)) { XCTAssertEqual(($0 as? VaultError)?.code, "zip_no_csv") }
        XCTAssertThrowsError(try VaultZip.firstCSV(in: deflated, maximumBytes: 10)) { XCTAssertEqual(($0 as? VaultError)?.code, "csv_too_large") }
        var damaged = [UInt8](deflated)
        damaged[120] ^= 0xff
        XCTAssertThrowsError(try VaultTransfer.readCSV(Data(damaged))) { XCTAssertEqual(($0 as? VaultError)?.code, "zip_unreadable") }
    }

    func testOneTimeCodeLinksRoundTrip() throws {
        XCTAssertEqual(VaultBase32.encode(Array("foobar".utf8)), "MZXW6YTBOI")
        XCTAssertEqual(VaultBase32.decode("mzxw6ytboi======").map { String(decoding: $0, as: UTF8.self) }, "foobar")
        XCTAssertNil(VaultBase32.decode("not base32!"))
        let parsed = try XCTUnwrap(VaultOTPAuth(uri: "otpauth://totp/ACME%20Co:jane@acme.test?secret=JBSWY3DPEHPK3PXP&algorithm=sha256&digits=8&period=60"))
        XCTAssertEqual(parsed.issuer, "ACME Co")
        XCTAssertEqual(parsed.account, "jane@acme.test")
        XCTAssertEqual(parsed.algorithm, .sha256)
        XCTAssertEqual(parsed.digits, 8)
        XCTAssertEqual(parsed.period, 60)
        XCTAssertEqual(VaultBase32.encode(parsed.secret), "JBSWY3DPEHPK3PXP")
        XCTAssertEqual(VaultOTPAuth(uri: parsed.uri), parsed)
        let plain = try XCTUnwrap(VaultOTPAuth(uri: "otpauth://totp/octo?secret=JBSWY3DPEHPK3PXP"))
        XCTAssertEqual(plain.issuer, "")
        XCTAssertEqual(plain.account, "octo")
        XCTAssertEqual(plain.algorithm, .sha1)
        XCTAssertEqual(plain.digits, 6)
        XCTAssertNil(VaultOTPAuth(uri: "otpauth://hotp/x?secret=JBSWY3DPEHPK3PXP&counter=1"))
        XCTAssertNil(VaultOTPAuth(uri: "https://example.com"))
    }

    func testTitleFallsBackToHostThenUsername() {
        XCTAssertEqual(VaultTransferLogin(url: "example.org", password: "p").fields.str("title"), "example.org")
        XCTAssertEqual(VaultTransferLogin(username: "me", password: "p").fields.str("title"), "me")
        XCTAssertEqual(VaultTransferLogin(password: "p").fields.str("title"), VaultTransfer.untitled)
    }

    func testRefusesFilesThatAreNotPasswordExports() {
        XCTAssertThrowsError(try VaultTransfer.readCSV("")) { XCTAssertEqual(($0 as? VaultError)?.code, "csv_empty") }
        XCTAssertThrowsError(try VaultTransfer.readCSV("name,address\nA,B\n")) {
            XCTAssertEqual(($0 as? VaultError)?.code, "csv_unknown_columns")
        }
        XCTAssertThrowsError(try VaultTransfer.readCSV("title,password\n,\n")) {
            XCTAssertEqual(($0 as? VaultError)?.code, "csv_no_logins")
        }
    }

    func testWrittenFileReadsBackUnchanged() throws {
        let logins = [
            VaultTransferLogin(title: "Comma, inc", url: "https://a.example", username: " spaced ", password: "q\"uote,=1", notes: "line one\nline two", otpauth: "otpauth://totp/a?secret=B"),
            VaultTransferLogin(title: "Plain", url: "https://b.example", username: "b", password: "-starts-with-dash"),
        ]
        let csv = VaultTransfer.writeCSV(logins)
        XCTAssertTrue(csv.hasPrefix("Title,URL,Username,Password,Notes,OTPAuth\r\n"))
        let read = try VaultTransfer.readCSV(csv)
        XCTAssertEqual(read.source, .applePasswords)
        XCTAssertEqual(read.logins, logins)
    }

    func testExportSplitsTheOneTimeCodeOutOfNotes() {
        let item = VaultItem(
            id: "i", owner: "alice", owned: true, permission: "owner", revision: 1, updatedAt: 0, updatedBy: "alice",
            fields: ["title": .string("GitHub"), "url": .string("https://github.com"), "username": .string("octo"),
                     "password": .string("pw"), "notes": .string("work\n\notpauth://totp/GitHub?secret=ABC")])
        let login = VaultTransferLogin(item: item)
        XCTAssertEqual(login.notes, "work")
        XCTAssertEqual(login.otpauth, "otpauth://totp/GitHub?secret=ABC")
        XCTAssertEqual(login.fields.str("notes"), "work\n\notpauth://totp/GitHub?secret=ABC")
    }

    func testPlanSkipsLoginsAlreadyInTheVaultAndRepeatsInTheFile() {
        let existing = VaultItem(
            id: "i", owner: "alice", owned: true, permission: "owner", revision: 1, updatedAt: 0, updatedBy: "alice",
            fields: ["title": .string("GitHub"), "url": .string("https://github.com/login"), "username": .string("octo"),
                     "password": .string("pw"), "notes": .string("")])
        let unreadable = VaultItem(
            id: "j", owner: "bob", owned: false, permission: "read", revision: 1, updatedAt: 0, updatedBy: "bob",
            error: "item_unreadable")
        let incoming = [
            VaultTransferLogin(title: "GitHub", url: "https://www.github.com/", username: "Octo", password: "pw"),
            VaultTransferLogin(title: "GitHub", url: "https://github.com", username: "octo", password: "new-pw"),
            VaultTransferLogin(title: "Mail", url: "mail.example", username: "me", password: "m"),
            VaultTransferLogin(title: "Mail again", url: "https://mail.example", username: "me", password: "m"),
        ]
        let plan = VaultTransfer.plan(incoming, existing: [existing, unreadable])
        XCTAssertEqual(plan.duplicates, 2)
        XCTAssertEqual(plan.fresh.map(\.password), ["new-pw", "m"])
    }

    func testExportListsReadableLoginsSortedByTitle() {
        func item(_ id: String, _ title: String, password: String = "p", error: String = "") -> VaultItem {
            VaultItem(
                id: id, owner: "a", owned: true, permission: "owner", revision: 1, updatedAt: 0, updatedBy: "a",
                fields: error.isEmpty ? ["title": .string(title), "password": .string(password)] : [:], error: error)
        }
        let logins = VaultTransfer.logins(from: [item("1", "zeta"), item("2", "Alpha"), item("3", "", error: "x"), item("4", "empty", password: "")])
        XCTAssertEqual(logins.map(\.title), ["Alpha", "zeta"])
    }

    @MainActor
    func testReviewDescribesWhatAnImportWillDo() {
        let review = VaultImportReview(source: "DuckDuckGo", logins: [], fresh: 3, duplicates: 1, skippedRows: 2)
        XCTAssertEqual(review.title, "Import from DuckDuckGo?")
        XCTAssertEqual(
            review.message,
            "3 new logins will be encrypted into your vault. 1 already in the vault will be skipped. 2 rows have no username or password and will be left out.")
        XCTAssertEqual(
            VaultTransferModel.reportText(VaultImportReport(added: 2, duplicates: 1, remaining: 1, stoppedBy: "This vault is full."), source: "Apple Passwords"),
            "Imported 2 logins from Apple Passwords. Skipped 1 already in the vault. Stopped with 1 left: This vault is full.")
        XCTAssertEqual(VaultTransferModel.errorText(VaultError("csv_unknown_columns")),
                       "That file has no Password column. Export a CSV file from your password manager and try again.")
    }

    @MainActor
    func testImportAndExportAgainstTheRealVault() async throws {
        let bridge = try VaultBridge.start()
        defer { bridge.stop() }
        let account = AccountBox("alice")
        let client = VaultClient(request: bridge.request(as: { account.name }))
        client.newMemoryKiB = VaultKdf.floorMemoryKiB
        client.newPasses = VaultKdf.floorPasses
        let platform = RecordingPlatform()
        let vault = VaultService(
            request: bridge.request(as: { account.name }), accountProvider: { account.name }, store: InMemorySecureStore(),
            platform: platform, clock: FixedClock(epochMillis: 1_700_000_000_000), client: client)
        defer { vault.dispose() }
        let transfer = VaultTransferModel(vault: vault)

        try await vault.refresh()
        _ = try await vault.create("correct horse battery staple")
        XCTAssertTrue(transfer.available)
        try await vault.save(["title": .string("GitHub"), "url": .string("https://github.com"), "username": .string("octo"),
                              "password": .string("pw-1")])

        let file = """
            title,url,username,password,notes
            GitHub,https://github.com,octo,pw-1,
            Mail,https://mail.example,me@mail.example,pw-2,from ddg
            Shop,shop.example,buyer,pw-3,
            """
        transfer.prepareFile(Data(file.utf8))
        let review = try XCTUnwrap(transfer.review)
        XCTAssertEqual(review.source, "DuckDuckGo")
        XCTAssertEqual(review.fresh, 2)
        XCTAssertEqual(review.duplicates, 1)
        await transfer.confirmImport()
        XCTAssertNil(transfer.review)
        XCTAssertEqual(transfer.message, "Imported 2 logins from DuckDuckGo. Skipped 1 already in the vault.")
        XCTAssertEqual(Set(vault.items.map(\.title)), ["GitHub", "Mail", "Shop"])
        XCTAssertEqual(vault.items.first { $0.title == "Shop" }?.host, "shop.example")

        transfer.prepareFile(Data(file.utf8))
        XCTAssertNil(transfer.review)
        XCTAssertEqual(transfer.message, "Every login from DuckDuckGo is already in your vault.")

        vault.receiveImport(VaultIncomingImport(source: "Apple Passwords", logins: [
            VaultTransferLogin(title: "Forum", url: "https://forum.example", username: "f", password: "pw-4"),
        ]))
        transfer.takeIncoming()
        XCTAssertNil(vault.incomingImport)
        XCTAssertEqual(transfer.review?.title, "Import from Apple Passwords?")
        await transfer.confirmImport()
        XCTAssertEqual(vault.items.count, 4)

        transfer.requestExport()
        XCTAssertTrue(transfer.confirmingExport)
        transfer.confirmExport()
        XCTAssertFalse(transfer.confirmingExport)
        XCTAssertEqual(transfer.exportFile?.count, 4)
        transfer.finishExport(savedAs: "ForkMesh Passwords.csv")
        XCTAssertNil(transfer.exportFile)
        XCTAssertEqual(transfer.message, "4 logins saved to ForkMesh Passwords.csv. Delete the file once the other app has imported it.")
        let csv = try XCTUnwrap(transfer.exportCSV())
        let back = try VaultTransfer.readCSV(csv)
        XCTAssertEqual(back.logins.map(\.title), ["Forum", "GitHub", "Mail", "Shop"])
        XCTAssertEqual(back.logins.first { $0.title == "Mail" }?.notes, "from ddg")

        vault.lock()
        XCTAssertFalse(transfer.available)
        XCTAssertNil(transfer.exportCSV())
        XCTAssertEqual(transfer.message, "The vault is locked.")
        vault.receiveImport(VaultIncomingImport(source: "Apple Passwords", logins: [VaultTransferLogin(password: "x")]))
        transfer.takeIncoming()
        XCTAssertNotNil(vault.incomingImport, "waits for the unlock")
        await vault.signedOut()
        XCTAssertNil(vault.incomingImport)
    }
}
