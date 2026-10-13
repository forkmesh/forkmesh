import FMApi
import Foundation
import XCTest
@testable import FMAgents

final class PromptTextFilesTests: XCTestCase {
    func testReadsUtf8AndNormalisesLineEndings() throws {
        let file = try PromptTextFiles.read(name: " notes.md ", bytes: Data("\u{FEFF}one\r\ntwo\r\n".utf8))
        XCTAssertEqual(file.name, "notes.md")
        XCTAssertEqual(file.text, "one\ntwo\n")
        XCTAssertEqual(PromptTextFiles.sizeLabel(file), "8 characters")
    }

    func testRefusesEmptyBinaryAndNulFiles() {
        XCTAssertThrowsError(try PromptTextFiles.read(name: "a.txt", bytes: Data())) { error in
            XCTAssertTrue("\(error)".contains("a.txt is empty"))
        }
        XCTAssertThrowsError(try PromptTextFiles.read(name: "", bytes: Data([0xFF, 0xD8, 0xFF, 0xE0]))) { error in
            XCTAssertTrue("\(error)".hasPrefix("That file is not a text file."))
        }
        XCTAssertThrowsError(try PromptTextFiles.read(name: "u16.txt", bytes: Data([0x68, 0x00, 0x69, 0x00])))
        let big = Data(repeating: 0x61, count: PromptTextFiles.maxBytes + 1)
        XCTAssertThrowsError(try PromptTextFiles.read(name: "big.log", bytes: big)) { error in
            XCTAssertEqual("\(error)", "big.log is larger than 32 KB, more than a prompt can carry.")
        }
    }

    func testFoldWrapsEveryFileAfterThePrompt() {
        let files = [PromptTextFile(id: "1", name: "a.log", text: "boom"), PromptTextFile(id: "2", name: "b.txt", text: "ok")]
        XCTAssertEqual(PromptTextFiles.fold("  fix it ", []), "fix it")
        XCTAssertEqual(
            PromptTextFiles.fold(" fix it ", files),
            "fix it\n\n\(PromptTextFiles.header)\n\n===== BEGIN FILE a.log =====\nboom\n===== END FILE a.log =====\n\n"
                + "===== BEGIN FILE b.txt =====\nok\n===== END FILE b.txt =====")
        XCTAssertTrue(PromptTextFiles.fold("", files).hasPrefix(PromptTextFiles.header))
    }

    func testBudgetCountsTheFoldedPrompt() {
        let file = PromptTextFile(id: "1", name: "a.txt", text: String(repeating: "x", count: 90))
        let used = PromptTextFiles.cost("hi", [file])
        XCTAssertEqual(used, PromptTextFiles.fold("hi", [file]).utf16.count)
        XCTAssertNil(PromptTextFiles.problem(adding: file, to: [], prompt: "hi", budget: used))
        XCTAssertNotNil(PromptTextFiles.problem(adding: file, to: [], prompt: "hi", budget: used - 1))
        XCTAssertNil(PromptTextFiles.overBudget("hi", [file], budget: used))
        XCTAssertNotNil(PromptTextFiles.overBudget("hi!", [file], budget: used))
        XCTAssertNil(PromptTextFiles.overBudget(String(repeating: "y", count: 9000), [], budget: 10))
        let full = Array(repeating: PromptTextFile(name: "f", text: "t"), count: PromptTextFiles.maxCount)
        XCTAssertEqual(
            PromptTextFiles.problem(adding: file, to: full, prompt: "", budget: 100_000),
            "A prompt can carry 5 files. Remove one to add another.")
    }
}
