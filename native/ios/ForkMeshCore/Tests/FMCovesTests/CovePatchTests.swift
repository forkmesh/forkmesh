import FMBase
import XCTest

@testable import FMCoves

final class CovePatchTests: XCTestCase {
    func testSha1AndGitBlobIds() {
        XCTAssertEqual(Hex.encode(CoveSha1.hash(Array("abc".utf8))), "a9993e364706816aba3e25717850c26c9cd0d89d")
        XCTAssertEqual(
            Hex.encode(CoveSha1.hash(Array("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq".utf8))),
            "84983e441c3bd26ebaae4aa1f95129e5e54670f1")
        XCTAssertEqual(CoveSha1.gitBlobId([]), "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391")
        XCTAssertEqual(CoveSha1.gitBlobId(Array("hello\n".utf8)), "ce013625030ba8dba906f756967f9e9ca394464a")
    }

    func testNewFilePatch() {
        let patch = CovePatch.replaceFile(path: ".forkmesh/coves/x.cove", before: nil, after: Array("{\n}\n".utf8))
        XCTAssertEqual(patch, """
            diff --git a/.forkmesh/coves/x.cove b/.forkmesh/coves/x.cove
            new file mode 100644
            index 0000000000000000000000000000000000000000..\(CoveSha1.gitBlobId(Array("{\n}\n".utf8)))
            --- /dev/null
            +++ b/.forkmesh/coves/x.cove
            @@ -0,0 +1,2 @@
            +{
            +}

            """)
    }

    func testReplacementPatchHandlesMissingNewline() {
        let before = Array("a\nb".utf8)
        let after = Array("c\n".utf8)
        let patch = CovePatch.replaceFile(path: "f", before: before, after: after)
        XCTAssertTrue(patch.contains("index \(CoveSha1.gitBlobId(before))..\(CoveSha1.gitBlobId(after))\n"))
        XCTAssertTrue(patch.contains("@@ -1,2 +1 @@\n-a\n-b\n\\ No newline at end of file\n+c\n"))
        XCTAssertEqual(CovePatch.replaceFile(path: "f", before: after, after: after), "")
    }

    func testHeadLabel() {
        let now = Date(timeIntervalSince1970: 1_758_700_000)
        XCTAssertEqual(CovePatch.head(account: "Alice", slug: "0123456789abcdef", now: now), "alice:mobile-cove/0123456789ab-0924-074640")
        XCTAssertEqual(CovePatch.head(account: "", slug: "ab", now: now), "mobile-cove/ab-0924-074640")
    }
}
