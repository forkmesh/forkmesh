import FMBase
import FMWatchWire
import Foundation
import XCTest

@testable import FMAgents

final class AgentWatchDiffTests: XCTestCase {
    func testAgentDiffBecomesTheWatchsFileList() {
        let diff = AgentDiff(json: [
            "id": 7, "branch": "fm/fix", "base": "main", "merged": false,
            "committed": "diff --git a/a.swift b/a.swift\n--- a/a.swift\n+++ b/a.swift\n@@ -1,2 +1,3 @@ struct A\n-old\n+new\n+more\n ctx\n",
            "uncommitted": "diff --git a/b.md b/c.md\nsimilarity index 90%\nrename from b.md\nrename to c.md\n",
            "untracked": ["notes.txt"], "truncated": true,
        ])
        let watch = AgentWatchDiff.make(key: "desktop:o/r:7", diff: diff)
        XCTAssertTrue(watch.ok)
        XCTAssertEqual(watch.key, "desktop:o/r:7")
        XCTAssertEqual(watch.branchLine, "fm/fix \u{2192} main")
        XCTAssertTrue(watch.truncated)
        XCTAssertEqual(watch.files.map(\.path), ["a.swift", "c.md", "notes.txt"])
        XCTAssertEqual(watch.files.map(\.group), [.committed, .uncommitted, .untracked])
        XCTAssertEqual(watch.files.map(\.status), ["M", "R", "A"])
        XCTAssertEqual(watch.files[1].oldPath, "b.md")
        let a = watch.files[0]
        XCTAssertEqual(a.added, 2)
        XCTAssertEqual(a.removed, 1)
        XCTAssertEqual(a.lines.map(\.kind), [.hunk, .removed, .added, .added, .context])
        XCTAssertEqual(a.lines.map(\.text), ["struct A", "old", "new", "more", "ctx"])
        XCTAssertEqual(a.lines.map(\.number), [1, 1, 1, 2, 3])
    }

    func testEmptyDiffHasNoFiles() {
        let watch = AgentWatchDiff.make(key: "k", diff: AgentDiff(json: ["merged": true]))
        XCTAssertTrue(watch.files.isEmpty)
        XCTAssertTrue(watch.merged)
        XCTAssertEqual(watch.emptyNote, "Merged. Nothing left to review.")
    }
}
