import FMDiff
import Foundation
import XCTest

@testable import FMCode

final class DiffImagesTests: XCTestCase {
    func testKindsByExtension() {
        XCTAssertEqual(DiffImageKind.of("www/Logo.PNG"), .raster)
        XCTAssertEqual(DiffImageKind.of("office/public/logo.svg"), .svg)
        XCTAssertEqual(DiffImageKind.of("a/photo.jpeg"), .raster)
        XCTAssertNil(DiffImageKind.of("src/main.cpp"))
        XCTAssertNil(DiffImageKind.of("a.png/readme"))
        XCTAssertNil(DiffImageKind.of(".png"))
    }

    @MainActor func testImagesGetAPreviewRowInsteadOfTheBinaryNotice() async {
        let png = DiffFile(oldPath: "docs/logo.png", newPath: "docs/logo.png", status: .modified, binary: true)
        let svg = DiffFile(oldPath: "", newPath: "logo.svg", status: .added)
        let code = DiffFile(oldPath: "a.cpp", newPath: "a.cpp", status: .modified)
        var asked: [DiffImageRequest] = []
        let model = DiffReviewModel(
            sections: [DiffReviewSection(title: "Committed", files: [png, svg, code], group: "committed")], scope: "",
            images: { (request: DiffImageRequest) in
                asked.append(request)
                return Data([1, 2, 3])
            })
        XCTAssertTrue(model.rows.contains(.preview(0)))
        XCTAssertTrue(model.rows.contains(.preview(1)))
        XCTAssertFalse(model.rows.contains(.preview(2)))
        XCTAssertFalse(model.rows.contains { if case .notice(0, _, _) = $0 { return true } else { return false } })
        XCTAssertEqual(model.imageRequests(0).map(\.side), [.old, .new])
        XCTAssertEqual(model.imageRequests(1).map(\.side), [.new])
        await model.loadImages(0)
        XCTAssertEqual(asked.map(\.path), ["docs/logo.png", "docs/logo.png"])
        XCTAssertEqual(asked.map(\.group), ["committed", "committed"])
        XCTAssertEqual(model.imageResults[model.imageRequests(0)[1]], .loaded(Data([1, 2, 3])))
        await model.loadImages(0)
        XCTAssertEqual(asked.count, 2)
    }

    @MainActor func testWithoutALoaderBinaryImagesKeepTheNotice() async {
        let png = DiffFile(oldPath: "logo.png", newPath: "logo.png", status: .modified, binary: true)
        let model = DiffReviewModel(files: [png], scope: "")
        XCTAssertFalse(model.rows.contains(.preview(0)))
        XCTAssertTrue(model.rows.contains(.notice(0, 0, "Binary file — contents are not shown.")))
    }

    @MainActor func testAFailedReadShowsItsMessage() async {
        let gone = DiffFile(oldPath: "gone.png", newPath: "gone.png", status: .deleted, binary: true)
        let model = DiffReviewModel(
            files: [gone], scope: "",
            images: { (_: DiffImageRequest) in throw DiffImageError("missing") })
        await model.loadImages(0)
        XCTAssertEqual(model.imageRequests(0).map(\.side), [.old])
        XCTAssertEqual(model.imageResults[model.imageRequests(0)[0]], .failed("missing"))
    }
}
