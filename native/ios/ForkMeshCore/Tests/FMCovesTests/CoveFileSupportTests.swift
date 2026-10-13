import XCTest

@testable import FMCoves

final class CoveFileSupportTests: XCTestCase {
    func testTenTemplatesWithUniqueIdsAndPaths() {
        let templates = CoveFiles.templates()
        XCTAssertEqual(templates.count, 10)
        XCTAssertEqual(Set(templates.map(\.id)).count, 10)
        XCTAssertEqual(Set(templates.map(\.defaultPath)).count, 10)
        for template in templates {
            XCTAssertFalse(template.title.isEmpty || template.description.isEmpty || template.mime.isEmpty)
            XCTAssertEqual(CoveFiles.editorKind(path: template.defaultPath, mime: template.mime), template.editor)
            switch template.editor {
            case .spreadsheet: XCTAssertFalse(CoveFiles.parseCsv(template.body).isEmpty)
            case .pipeline:
                guard case .success(let pipeline) = CovePipeline.parse(template.body) else { return XCTFail("pipeline") }
                XCTAssertEqual(pipeline.stages.count, 5)
            case .text: break
            }
        }
        XCTAssertEqual(CoveFiles.template("budget")?.defaultPath, "budget.csv")
        XCTAssertNil(CoveFiles.template("nope"))
    }

    func testCsvMatchesDesktop() {
        let parsed = CoveFiles.parseCsv("Name,Notes,Amount\r\n\"Acme, Inc.\",\"Line one\nLine two\",\"12\"\"34\"\r\n")
        XCTAssertEqual(parsed, [["Name", "Notes", "Amount"], ["Acme, Inc.", "Line one\nLine two", "12\"34"]])
        XCTAssertEqual(CoveFiles.parseCsv(CoveFiles.serializeCsv(parsed)), parsed)
        let serialized = CoveFiles.serializeCsv([["a,b", "c\"d", "e"], ["", "x\ny"]])
        XCTAssertEqual(Array(serialized.utf8), Array(Data(hex: "22612c62222c2263222264222c650d0a2c22780a79220d0a")))
        XCTAssertEqual(CoveFiles.parseCsv(""), [[""]])
        XCTAssertEqual(CoveFiles.parseCsv("A,,\r\n,,\r\n"), [["A", "", ""], ["", "", ""]])
        XCTAssertEqual(CoveFiles.parseCsv("a\nb"), [["a"], ["b"]])
    }

    func testShareUrlsMatchDesktop() {
        XCTAssertEqual(CoveFiles.shareUrl(owner: "al ice", repository: "my repo", coveId: "id-1"), "forkmesh://cove/al%20ice/my%20repo/id-1")
        XCTAssertEqual(CoveFiles.shareUrl(owner: "", repository: "r", coveId: "c"), "")
        let parsed = CoveFiles.parseShareUrl("forkmesh://cove/alice/repo/abc")
        XCTAssertEqual(parsed?.owner, "alice")
        XCTAssertEqual(parsed?.repository, "repo")
        XCTAssertEqual(parsed?.coveId, "abc")
        XCTAssertNil(CoveFiles.parseShareUrl("forkmesh://note/alice/repo/abc"))
        XCTAssertNil(CoveFiles.parseShareUrl("forkmesh://cove/alice/abc"))
        XCTAssertEqual(CoveFiles.parseRouteId("a/b/c")?.coveId, "c")
        XCTAssertNil(CoveFiles.parseRouteId("a/b"))
    }

    func testUniqueNames() {
        XCTAssertEqual(CoveFiles.uniqueName("notes.md", existing: ["notes.md", "notes-2.md"]), "notes-3.md")
        XCTAssertEqual(CoveFiles.uniqueName("a/leads.pipeline.json", existing: ["a/leads.pipeline.json"]), "a/leads-2.pipeline.json")
        XCTAssertEqual(CoveFiles.uniqueName("x.md", existing: []), "x.md")
    }

    func testPipelineEditing() throws {
        guard case .success(var pipeline) = CovePipeline.parse(CoveFiles.defaultPipelineJson()) else { return XCTFail("parse") }
        let now = Date(timeIntervalSince1970: 1_700_000_000)
        let lead = pipeline.addLead(actor: "alice", now: now, id: "lead-1")
        XCTAssertEqual(lead.stage, "new")
        pipeline.move(leadId: "lead-1", to: "won", actor: "alice", now: now)
        pipeline.move(leadId: "lead-1", to: "missing", actor: "alice", now: now)
        var edited = pipeline.leads[0]
        edited.value = 1500
        edited.company = "Acme"
        pipeline.update(edited, now: now)
        XCTAssertEqual(pipeline.totals().first { $0.stage.id == "won" }?.value, 1500)
        guard case .success(let reparsed) = CovePipeline.parse(pipeline.body) else { return XCTFail("reparse") }
        XCTAssertEqual(reparsed.leads.first?.stage, "won")
        XCTAssertEqual(reparsed.leads.first?.company, "Acme")
        XCTAssertEqual(reparsed.history.count, 2)
        XCTAssertEqual(reparsed.history.last?["action"]?.stringValue, "moved")
        XCTAssertEqual(reparsed.history.last?["at"]?.stringValue, "2023-11-14T22:13:20.000Z")
        var deleting = reparsed
        deleting.delete(leadId: "lead-1", actor: "bob")
        XCTAssertTrue(deleting.leads.isEmpty)
        XCTAssertEqual(deleting.history.count, 3)
    }

    func testPipelineValidation() {
        XCTAssertEqual(parseError("[]"), .notObject)
        XCTAssertEqual(parseError("{"), .invalidJson)
        XCTAssertEqual(parseError("{\"stages\":[]}"), .noStages)
        XCTAssertEqual(parseError("{\"stages\":[{\"id\":\"a\",\"name\":\"A\"},{\"id\":\"a\",\"name\":\"B\"}]}"), .badStage)
        XCTAssertEqual(parseError("{\"stages\":[{\"id\":\"a\",\"name\":\"A\"}],\"leads\":{}}"), .badArrays)
    }

    private func parseError(_ body: String) -> CovePipeline.ParseError? {
        if case .failure(let error) = CovePipeline.parse(body) { return error }
        return nil
    }
}

private extension Data {
    init(hex: String) {
        var bytes: [UInt8] = []
        var index = hex.startIndex
        while index < hex.endIndex {
            let next = hex.index(index, offsetBy: 2)
            bytes.append(UInt8(hex[index..<next], radix: 16) ?? 0)
            index = next
        }
        self.init(bytes)
    }
}
