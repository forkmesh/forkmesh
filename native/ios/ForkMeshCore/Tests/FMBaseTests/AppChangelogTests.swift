import Foundation
import XCTest
@testable import FMBase

final class AppChangelogTests: XCTestCase {
    private let sample: String = """
    {"schema": 1, "entries": [
      {"version": "1.4.1", "build": "219", "date": "2026-10-13",
       "sections": [{"title": "Calls", "items": ["Blur your background."]}, {"title": "Empty", "items": []}]},
      {"version": "1.4.1", "build": "218", "date": "2026-10-12",
       "sections": [{"title": "Market", "items": ["One-tap install."]}]}
    ]}
    """

    func testDecodesEntriesAndDropsEmptySections() throws {
        let log: AppChangelog = try XCTUnwrap(AppChangelog.decode(Data(sample.utf8)))
        XCTAssertEqual(log.entries.map(\.build), ["219", "218"])
        XCTAssertEqual(log.entries[0].sections, [AppChangelog.Section(title: "Calls", items: ["Blur your background."])])
        XCTAssertEqual(log.entries[0].heading, "Version 1.4.1 (219)")
    }

    func testToleratesMissingFields() throws {
        let log: AppChangelog = try XCTUnwrap(AppChangelog.decode(Data(#"{"entries": [{"build": "7"}]}"#.utf8)))
        XCTAssertEqual(log.entries, [AppChangelog.Entry(version: "", build: "7")])
        XCTAssertNil(AppChangelog.decode(Data("not json".utf8)))
    }

    func testLabelsUseTheBundleVersionAndBuild() {
        XCTAssertEqual(
            AppChangelog.label(infoDictionary: ["CFBundleShortVersionString": "1.4.1", "CFBundleVersion": "218"]),
            "Version 1.4.1 (218)")
        XCTAssertEqual(AppChangelog.label(version: "1.4.1", build: ""), "Version 1.4.1")
        XCTAssertEqual(AppChangelog.label(infoDictionary: nil), "Version 0.0.0")
    }

    func testTheBundledChangelogIsShownAsItIs() {
        let info: [String: Any] = ["CFBundleShortVersionString": "1.4.1", "CFBundleVersion": "219"]
        let matching: AppChangelog = AppChangelog.load(data: Data(sample.utf8), infoDictionary: info)
        XCTAssertEqual(matching.entries.map(\.build), ["219", "218"])
        XCTAssertEqual(matching.entries[0].sections.count, 1)

        let newer: AppChangelog = AppChangelog.load(
            data: Data(sample.utf8), infoDictionary: ["CFBundleShortVersionString": "1.4.2", "CFBundleVersion": "220"])
        XCTAssertEqual(newer.entries.map(\.build), ["219", "218"])

        let placeholder: AppChangelog = AppChangelog.load(data: Data(#"{"schema": 1, "entries": []}"#.utf8), infoDictionary: info)
        XCTAssertEqual(placeholder.entries, [AppChangelog.Entry(version: "1.4.1", build: "219")])
        XCTAssertEqual(AppChangelog.load(data: nil, infoDictionary: info).entries.count, 1)
    }

    func testSharedEntriesUseTheirTitleOrDate() throws {
        let shared: String = """
        {"schema": 1, "entries": [
          {"version": "", "build": "", "date": "2026-10-21", "title": "", "pitch": "",
           "sections": [{"title": "Chat", "items": ["Stick stickers on messages."]}]},
          {"version": "0.7.0", "build": "", "date": "2026-07", "title": "The Living Code City",
           "pitch": "A shared world.", "sections": [{"title": "Highlights", "items": ["Everyone enters the World."]}]}
        ]}
        """
        let log: AppChangelog = AppChangelog.load(
            data: Data(shared.utf8), infoDictionary: ["CFBundleShortVersionString": "1.4.1", "CFBundleVersion": "303"])
        XCTAssertEqual(log.entries.count, 2)
        let locale: Locale = Locale(identifier: "en_US")
        XCTAssertEqual(log.entries[0].formattedDate(locale: locale), "October 21, 2026")
        XCTAssertEqual(log.entries[0].detail(locale: locale), "")
        XCTAssertEqual(log.entries[1].heading, "The Living Code City")
        XCTAssertEqual(log.entries[1].pitch, "A shared world.")
        XCTAssertEqual(log.entries[1].detail(locale: locale), "Version 0.7.0 · July 2026")
    }

    func testDatesReadAsWords() {
        let entry: AppChangelog.Entry = AppChangelog.Entry(version: "1.4.1", build: "219", date: "2026-10-13")
        XCTAssertEqual(entry.formattedDate(locale: Locale(identifier: "en_US")), "October 13, 2026")
        XCTAssertEqual(AppChangelog.Entry(version: "1", build: "2", date: "soon").formattedDate(), "soon")
    }
}
