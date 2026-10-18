import Foundation
import XCTest
@testable import FMPackages

final class SecureDriveFormatsTests: XCTestCase {
    func testSheets() {
        var sheet = SecureDriveSheet.fromCsv(sdBytes("Item,Cost\r\nPaper,4\r\nInk,\"1,5\"\r\nTotal,=SUM(B2:B3)\r\n"))
        XCTAssertEqual(sheet.rows, 4)
        XCTAssertEqual(sheet.columns, 2)
        XCTAssertEqual(sheet.raw(2, 1), "1,5")
        XCTAssertEqual(sheet.display(3, 1), "4", "SUM skips text")
        sheet.setRaw(2, 1, "1.5")
        XCTAssertEqual(sheet.display(3, 1), "5.5")
        sheet.setRaw(0, 3, "=B2*2+B3^2")
        XCTAssertEqual(sheet.display(0, 3), "10.25")
        sheet.setRaw(1, 3, "=AVERAGE(B2:B3)")
        XCTAssertEqual(sheet.display(1, 3), "2.75")
        sheet.setRaw(2, 3, "=B2/0")
        XCTAssertEqual(sheet.display(2, 3), "#DIV/0!")
        sheet.setRaw(3, 3, "=D5")
        sheet.setRaw(4, 3, "=D4+1")
        XCTAssertEqual(sheet.display(3, 3), "#CYCLE!")
        sheet.setRaw(5, 0, "=IF(B2>3,\"big\",\"small\")")
        XCTAssertEqual(sheet.display(5, 0), "big")
        sheet.setRaw(5, 1, "=ROUND(10/3,2)")
        XCTAssertEqual(sheet.display(5, 1), "3.33")
        sheet.setRaw(5, 2, "=NOPE(1)")
        XCTAssertEqual(sheet.display(5, 2), "#NAME?")
        sheet.setRaw(6, 0, "line one\nline \"two\"")
        sheet.setRaw(7, 0, "=MIN(B2:B3)+MAX(B2:B3)+COUNT(A1:B3)+ABS(-2)")
        XCTAssertEqual(sheet.display(7, 0), "9.5")
        sheet.setRaw(7, 1, "=1/3")
        XCTAssertEqual(sheet.display(7, 1), "0.333333333333")
        sheet.setRaw(7, 2, "=\"a\"=\"A\"")
        XCTAssertEqual(sheet.display(7, 2), "1")
        sheet.setRaw(7, 3, "=1+")
        XCTAssertEqual(sheet.display(7, 3), "#ERROR!")
        sheet.setRaw(8, 0, "=A1+1")
        XCTAssertEqual(sheet.display(8, 0), "#VALUE!")
        sheet.setRaw(8, 1, "=.5e1 + 1.")
        XCTAssertEqual(sheet.display(8, 1), "6")
        let again = SecureDriveSheet.fromCsv(sheet.toCsv())
        XCTAssertEqual(again.raw(6, 0), sheet.raw(6, 0))
        XCTAssertEqual(again.raw(3, 1), sheet.raw(3, 1))
        XCTAssertEqual(SecureDriveSheet.parseCell("AB12")?.row, 11)
        XCTAssertEqual(SecureDriveSheet.parseCell("AB12")?.column, 27)
        XCTAssertNil(SecureDriveSheet.parseCell("ABC1"))
        XCTAssertEqual(SecureDriveSheet.columnName(27), "AB")
        XCTAssertEqual(SecureDriveSheet.columnName(0), "A")
        XCTAssertEqual(SecureDriveSheet().toCsv(), [])
        XCTAssertEqual(SecureDriveSheet.fromCsv(sdBytes("\u{FEFF}a, b ")).toCsv(), sdBytes("a,\" b \"\r\n"))
    }

    func testDrawings() {
        var drawing = SecureDriveDrawing(width: 800, height: 600, background: "#fafafa")
        drawing.strokes = [
            SecureDriveStroke(color: "#0969da", width: 5, points: [SecureDrivePoint(10, 20), SecureDrivePoint(30.5, 40), SecureDrivePoint(50, 60)]),
            SecureDriveStroke(color: "#cf222e", width: 2, points: [SecureDrivePoint(5, 5)]),
        ]
        let parsed = SecureDriveDrawing.fromSvg(drawing.toSvg())
        XCTAssertNotNil(parsed)
        XCTAssertEqual(parsed?.width, 800)
        XCTAssertEqual(parsed?.background, "#fafafa")
        XCTAssertEqual(parsed?.strokes.count, 2)
        XCTAssertEqual(parsed?.strokes.first?.points.count, 3)
        XCTAssertEqual(parsed?.strokes.first?.color, "#0969da")
        XCTAssertEqual(parsed?.strokes.last?.points.count, 1)
        XCTAssertEqual(parsed?.strokes.first?.points[1], SecureDrivePoint(30.5, 40))
        XCTAssertNil(SecureDriveDrawing.fromSvg(sdBytes("<svg xmlns=\"http://www.w3.org/2000/svg\"><path d=\"M0 0 L1 1\"/></svg>")))
        XCTAssertNil(SecureDriveDrawing.fromSvg(sdBytes("not xml")))
        XCTAssertEqual(SecureDriveDrawing.shape(.box, from: SecureDrivePoint(0, 0), to: SecureDrivePoint(10, 5)).count, 5)
        XCTAssertEqual(SecureDriveDrawing.shape(.oval, from: SecureDrivePoint(0, 0), to: SecureDrivePoint(10, 5)).count, 73)
        var erasing = parsed!
        XCTAssertTrue(erasing.erase(at: SecureDrivePoint(6, 6), width: 1))
        XCTAssertEqual(erasing.strokes.count, 1)
        XCTAssertTrue(SecureDriveDrawing.validColor("#A0b1C2"))
        XCTAssertFalse(SecureDriveDrawing.validColor("#A0b1C"))
        XCTAssertFalse(SecureDriveDrawing.validColor("red"))
    }

    func testDesktopDrawingParses() {
        let desktop = """
        <?xml version="1.0" encoding="UTF-8"?>
        <svg xmlns="http://www.w3.org/2000/svg" width="1600" height="1000" viewBox="0 0 1600 1000" data-forkmesh-drawing="1">
            <rect width="1600" height="1000" fill="#ffffff"/>
            <path d="M100.0 100.0 L500.0 100.0 L500.0 400.0" fill="none" stroke="#0969da" stroke-width="6.0" stroke-linecap="round" stroke-linejoin="round"/>
        </svg>
        """
        let drawing = SecureDriveDrawing.fromSvg(sdBytes(desktop))
        XCTAssertEqual(drawing?.strokes.first?.points.count, 3)
        XCTAssertEqual(drawing?.strokes.first?.width, 6)
    }
}
