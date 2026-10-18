import XCTest
@testable import FMDiff

final class LineDiffCountTests: XCTestCase {
    private func count(_ old: String, _ new: String) -> LineChangeCount? {
        LineDiffCount.count(old: Array(old.utf8), new: Array(new.utf8))
    }

    func testCountsAddedAndDeletedLines() {
        XCTAssertEqual(count("a\nb\nc\n", "a\nb\nc\n"), LineChangeCount(added: 0, deleted: 0))
        XCTAssertEqual(count("a\nb\nc\n", "a\nx\nc\n"), LineChangeCount(added: 1, deleted: 1))
        XCTAssertEqual(count("a\nb\nc\n", "a\nb\nc\nd\ne\n"), LineChangeCount(added: 2, deleted: 0))
        XCTAssertEqual(count("a\nb\nc\nd\n", "b\nd\n"), LineChangeCount(added: 0, deleted: 2))
        XCTAssertEqual(count("", "one\ntwo"), LineChangeCount(added: 2, deleted: 0))
        XCTAssertEqual(count("one\ntwo\n", ""), LineChangeCount(added: 0, deleted: 2))
        XCTAssertEqual(count("a\nb", "a\nb\n"), LineChangeCount(added: 1, deleted: 1))
        XCTAssertEqual(count("x\ny\nz\n", "p\nq\n"), LineChangeCount(added: 2, deleted: 3))
    }

    func testBinaryContentHasNoCounts() {
        XCTAssertNil(LineDiffCount.count(old: [0x61, 0x00], new: [0x61]))
        XCTAssertNil(LineDiffCount.count(old: [0x61], new: [0x00]))
        XCTAssertNil(LineDiffCount.added([0x00, 0x0a]))
        XCTAssertEqual(LineDiffCount.added(Array("a\nb".utf8)), LineChangeCount(added: 2, deleted: 0))
        XCTAssertEqual(LineDiffCount.deleted(Array("a\n".utf8)), LineChangeCount(added: 0, deleted: 1))
        XCTAssertFalse(LineDiffCount.isBinary([UInt8](repeating: 0x61, count: 8000) + [0x00]))
    }

    func testLineCountAndSplit() {
        XCTAssertEqual(LineDiffCount.lineCount([]), 0)
        XCTAssertEqual(LineDiffCount.lineCount(Array("a".utf8)), 1)
        XCTAssertEqual(LineDiffCount.lineCount(Array("a\n".utf8)), 1)
        XCTAssertEqual(LineDiffCount.lineCount(Array("a\n\nb".utf8)), 3)
        XCTAssertEqual(LineDiffCount.lines(Array("a\nb".utf8)).map { Array($0) }, [Array("a\n".utf8), Array("b".utf8)])
    }

    func testMatchesTheLongestCommonSubsequence() {
        var generator = SystemRandomNumberGenerator()
        for _ in 0..<300 {
            let a = (0..<Int.random(in: 0...30, using: &generator)).map { _ in "l\(Int.random(in: 0...6, using: &generator))" }
            let b = (0..<Int.random(in: 0...30, using: &generator)).map { _ in "l\(Int.random(in: 0...6, using: &generator))" }
            let common = lcs(a, b)
            let result = count(a.map { $0 + "\n" }.joined(), b.map { $0 + "\n" }.joined())
            XCTAssertEqual(result, LineChangeCount(added: b.count - common, deleted: a.count - common), "\(a) -> \(b)")
        }
    }

    func testLargeRewritesStayFast() {
        let old = (0..<20000).map { "old \($0)\n" }.joined()
        let new = (0..<20000).map { $0 % 3 == 0 ? "old \($0)\n" : "new \($0)\n" }.joined()
        XCTAssertEqual(count(old, new), LineChangeCount(added: 13333, deleted: 13333))
    }

    private func lcs(_ a: [String], _ b: [String]) -> Int {
        if a.isEmpty || b.isEmpty { return 0 }
        var table = [[Int]](repeating: [Int](repeating: 0, count: b.count + 1), count: a.count + 1)
        for i in 1...a.count {
            for j in 1...b.count {
                table[i][j] = a[i - 1] == b[j - 1] ? table[i - 1][j - 1] + 1 : max(table[i - 1][j], table[i][j - 1])
            }
        }
        return table[a.count][b.count]
    }
}
