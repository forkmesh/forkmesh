// The mirror import sheet: the URL pre-check and
// what the sheet posts and says.

import FMAccount
import FMApi
import FMBase
import XCTest
@testable import FMCode

@MainActor
final class ReposMirrorImportTests: XCTestCase {
    // MARK: validateSourceUrl

    func testAcceptsEveryCanonicalProviderHost() {
        for url in [
            "https://github.com/owner/repository",
            "https://gitlab.com/owner/repository",
            "https://codeberg.org/owner/repository",
            // A subgroup path still names an owner and a repository.
            "https://gitlab.com/group/subgroup/repository",
        ] {
            XCTAssertNil(MirrorImportModel.validateSourceUrl(url), url)
        }
    }

    func testToleratesAMissingSchemeWwwAndSurroundingSpace() {
        XCTAssertNil(MirrorImportModel.validateSourceUrl(" github.com/owner/repository "))
        XCTAssertNil(MirrorImportModel.validateSourceUrl("https://www.github.com/owner/repository"))
        XCTAssertNil(MirrorImportModel.validateSourceUrl("HTTPS://GitHub.com:443/owner/repository?tab=readme#top"))
    }

    func testRejectsAnEmptyURL() {
        XCTAssertEqual(MirrorImportModel.validateSourceUrl("   "), "Paste a repository URL.")
    }

    func testRejectsANonHttpsSchemeAndEmbeddedCredentials() {
        XCTAssertTrue(MirrorImportModel.validateSourceUrl("http://github.com/owner/repo")!.contains("https://"))
        XCTAssertTrue(MirrorImportModel.validateSourceUrl("https://user:pass@github.com/owner/repo")!.contains("https://"))
        XCTAssertEqual(MirrorImportModel.validateSourceUrl("https://github.com:abc/o/r"), "Use an https:// repository URL.")
    }

    func testRejectsANonCanonicalProviderHost() {
        XCTAssertTrue(
            MirrorImportModel.validateSourceUrl("https://bitbucket.org/owner/repo")!.contains("github.com, gitlab.com, codeberg.org"))
        // Look-alike hosts must not slip through the www. trim.
        XCTAssertTrue(
            MirrorImportModel.validateSourceUrl("https://github.com.evil.test/owner/repo")!
                .contains("github.com, gitlab.com, codeberg.org"))
    }

    func testRejectsAnOwnerOnlyURL() {
        XCTAssertEqual(MirrorImportModel.validateSourceUrl("https://codeberg.org/m33"), "Include the owner and the repository name.")
        XCTAssertEqual(MirrorImportModel.validateSourceUrl("https://codeberg.org/m33/"), "Include the owner and the repository name.")
    }

    func testAdvertisesExactlyTheThreeDocumentedProviders() {
        XCTAssertEqual(MirrorImportModel.providers, ["github.com", "gitlab.com", "codeberg.org"])
    }

    // MARK: The sheet

    private func model(_ client: FakeRepoClient, session: AuthSession? = repoTestSession()) -> MirrorImportModel {
        MirrorImportModel(client: client, session: { session })
    }

    func testPostsTheDocumentedFieldsForAGithubURL() async {
        let client = FakeRepoClient()
        let sheet = model(client)
        sheet.url = "  https://github.com/owner/repository  "
        let notice = await sheet.submit()
        XCTAssertEqual(client.imports.count, 1)
        let post = client.imports[0]
        XCTAssertEqual(post.sourceUrl, "https://github.com/owner/repository")
        XCTAssertEqual(post.targetOwner, "alice")
        XCTAssertEqual(post.mode, "import")
        // No token typed: none sent (ApiService drops an empty one).
        XCTAssertEqual(post.providerToken, "")
        // Success closes the sheet with the "not a mirror yet" copy.
        XCTAssertTrue(notice?.contains("external entry") ?? false)
        XCTAssertFalse(sheet.busy)
    }

    func testPostsGitlabAndCodebergURLsUnchanged() async {
        for url in ["https://gitlab.com/owner/repository", "https://codeberg.org/owner/repository"] {
            let client = FakeRepoClient()
            let sheet = model(client)
            sheet.url = url
            _ = await sheet.submit()
            XCTAssertEqual(client.imports.first?.sourceUrl, url)
        }
    }

    func testSendsATypedProviderTokenForThatOneRequestOnly() async {
        let client = FakeRepoClient()
        let sheet = model(client)
        sheet.url = "https://github.com/owner/private"
        sheet.token = "  ghp_secret  "
        _ = await sheet.submit()
        XCTAssertEqual(client.imports.first?.providerToken, "ghp_secret")
        XCTAssertEqual(MirrorImportModel.tokenHelper, "Used for this request only; never stored.")
    }

    func testRejectsANonCanonicalURLBeforeAnyRoundTrip() async {
        let client = FakeRepoClient()
        let sheet = model(client)
        sheet.url = "https://bitbucket.org/owner/repo"
        let notice = await sheet.submit()
        XCTAssertNil(notice)
        XCTAssertTrue(client.imports.isEmpty)
        XCTAssertTrue(sheet.problem?.hasPrefix("Only github.com, gitlab.com, codeberg.org") ?? false)
        // Editing clears the message.
        sheet.url = "https://github.com/owner/repo"
        sheet.urlChanged()
        XCTAssertNil(sheet.problem)
    }

    func testRefusesToImportWhileSignedOut() async {
        let client = FakeRepoClient()
        let sheet = model(client, session: nil)
        sheet.url = "https://github.com/owner/repository"
        let notice = await sheet.submit()
        XCTAssertNil(notice)
        XCTAssertTrue(client.imports.isEmpty)
        XCTAssertEqual(sheet.status, "Sign in to import a repository.")
    }

    private func refusal(_ status: Int, _ body: JSONObject) async -> String {
        let client = FakeRepoClient()
        client.importResult = { ApiJsonResult(status, body) }
        let sheet = model(client)
        sheet.url = "https://github.com/owner/repository"
        let notice = await sheet.submit()
        XCTAssertNil(notice)
        XCTAssertEqual(client.imports.count, 1)
        return sheet.status
    }

    func testExplainsWorkerRefusalsReadably() async {
        let forbidden = await refusal(403, ["error": "target_owner_forbidden"])
        XCTAssertEqual(forbidden, "You cannot import into that owner. Pick an account you administer.")
        let limited = await refusal(429, ["error": "rate_limited"])
        XCTAssertEqual(limited, "The provider is rate-limiting; try again later.")
        let unknown = await refusal(418, ["error": "teapot"])
        XCTAssertEqual(unknown, "Import failed (teapot).")
        let bodyless = await refusal(500, [:])
        XCTAssertEqual(bodyless, "Import failed (HTTP 500).")
        let path = await refusal(400, ["error": "invalid_repository_path"])
        XCTAssertEqual(path, "That does not look like a canonical repository URL.")
        let session = await refusal(401, ["error": "invalid_session"])
        XCTAssertEqual(session, "Your session expired. Sign in again.")
    }

    func testSurfacesATransportFailureWithoutARawStack() async {
        let client = FakeRepoClient()
        client.importResult = { throw ReposTestError(text: "SocketException: Connection failed") }
        let sheet = model(client)
        sheet.url = "https://github.com/owner/repository"
        _ = await sheet.submit()
        XCTAssertEqual(sheet.status, "Import failed: SocketException: Connection failed")
        XCTAssertFalse(sheet.status.contains("#0"))
        XCTAssertFalse(sheet.busy)
    }

    func testPrefersTheWorkerNoticeAndReportsARefresh() async {
        let refreshed = FakeRepoClient()
        refreshed.importResult = { ApiJsonResult(200, ["ok": true, "created": false]) }
        let first = model(refreshed)
        first.url = "https://github.com/owner/repository"
        let refreshedNotice = await first.submit()
        XCTAssertEqual(refreshedNotice, "Refreshed the existing external listing.")

        let spoken = FakeRepoClient()
        spoken.importResult = { ApiJsonResult(200, ["ok": true, "created": true, "notice": "Imported 12 issues."]) }
        let second = model(spoken)
        second.url = "https://github.com/owner/repository"
        let spokenNotice = await second.submit()
        XCTAssertEqual(spokenNotice, "Imported 12 issues.")
    }

    func testSaysTheEntryIsMetadataOnly() {
        XCTAssertTrue(MirrorImportModel.metadataNote.contains("Imports metadata only"))
        XCTAssertTrue(MirrorImportModel.metadataNote.contains("does not own or control the source repository"))
        XCTAssertEqual(MirrorImportModel.title, "Mirror from GitHub/GitLab/Codeberg")
    }
}
