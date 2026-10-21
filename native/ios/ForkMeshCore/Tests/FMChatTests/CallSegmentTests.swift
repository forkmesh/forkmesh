import Foundation
import XCTest

@testable import FMChat

final class CallSegmentTests: XCTestCase {
    static let here = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    static let repo = here.deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    static let bundledHair = repo.appendingPathComponent("native/ios/ForkMesh/Resources/CallSegmentation/hair-192.fmnn")
    static let browserModels = repo.appendingPathComponent("app/public/assets/call-segmentation")

    static let fixture: [String: Any] = {
        let url = here.appendingPathComponent("Fixtures/call-segment-parity.json")
        guard let data = try? Data(contentsOf: url), let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return [:] }
        return object
    }()

    static func halves(_ text: String) -> [Float] {
        let data = Data(base64Encoded: text) ?? Data()
        let bytes = [UInt8](data)
        return stride(from: 0, to: bytes.count - 1, by: 2).map { CallSegmentHalf.toFloat(UInt16(bytes[$0]) | UInt16(bytes[$0 + 1]) << 8) }
    }

    static func model(_ file: String) throws -> CallSegmentModel {
        try CallSegmentModel.parse(Data(contentsOf: browserModels.appendingPathComponent(file)))
    }

    func testHalfConversionRoundTripsEveryHalf() {
        for bits in 0...0xffff {
            let half = UInt16(bits)
            let value = CallSegmentHalf.toFloat(half)
            if value.isNaN { continue }
            XCTAssertEqual(CallSegmentHalf.fromFloat(value), half, "half \(bits)")
        }
        XCTAssertEqual(CallSegmentHalf.fromFloat(1 + 1.0 / 2048), 0x3c00)
        XCTAssertEqual(CallSegmentHalf.fromFloat(1 + 3.0 / 2048), 0x3c02)
        XCTAssertEqual(CallSegmentHalf.fromFloat(70000), 0x7c00)
        XCTAssertEqual(CallSegmentHalf.fromFloat(1e-9), 0)
    }

    func testBundledHairModelIsTheBrowserModel() throws {
        let bundled = try Data(contentsOf: Self.bundledHair)
        let browser = try Data(contentsOf: Self.browserModels.appendingPathComponent("hair-192.fmnn"))
        XCTAssertEqual(bundled, browser)
        let model = try CallSegmentModel.parse(bundled)
        XCTAssertEqual(model.layers.count, 162)
        XCTAssertEqual(model.inputShape, CallSegmentShape(192, 192, 4))
        XCTAssertEqual(model.outputShape, CallSegmentShape(192, 192, 2))
        XCTAssertEqual(model.outputs, [[162]])
        XCTAssertEqual(model.weights.count, 181_506)
        XCTAssertEqual(Double(model.multiplyAdds) / 1e6, 36.5, accuracy: 0.1)
    }

    func testMeanExpandsIntoRowAndColumnPasses() throws {
        let model = try Self.model("selfie-square.fmnn")
        XCTAssertEqual(model.layers.count, 102)
        let rows = model.layers.filter { $0.op == .meanrow }
        XCTAssertEqual(rows.count, 10)
        let first = try XCTUnwrap(rows.first)
        XCTAssertEqual(model.shapes[first.output]?.w, 1)
        XCTAssertEqual(first.output, 93)
        let next = try XCTUnwrap(model.layers.first { $0.inputs == [first.output] })
        XCTAssertEqual(next.op, .meancol)
        XCTAssertEqual(model.shapes[next.output], CallSegmentShape(1, 1, model.shapes[first.inputs[0]]!.c))
    }

    func testRejectsOtherFiles() {
        XCTAssertThrowsError(try CallSegmentModel.parse(Data([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13])))
    }

    func testPlanKeepsTensorsApartAndAligned() throws {
        let model = try CallSegmentModel.parse(Data(contentsOf: Self.bundledHair))
        let plan = CallSegmentPlan(model)
        var spans: [(Int, Int)] = []
        for (index, offset) in plan.offsets {
            XCTAssertEqual(offset % CallSegmentPlan.alignment, 0)
            spans.append((offset, offset + model.shapes[index]!.count))
        }
        spans.sort { $0.0 < $1.0 }
        for i in 1..<spans.count { XCTAssertLessThanOrEqual(spans[i - 1].1, spans[i].0) }
        XCTAssertLessThanOrEqual(spans.last!.1, plan.arenaCount)
        XCTAssertEqual(plan.steps.count, model.layers.count)
        let first = plan.steps[0]
        XCTAssertEqual(first.fields.count, CallSegmentStep.fieldCount)
        XCTAssertEqual(Array(first.fields[0..<8]), [0, 2, 192, 192, 4, 96, 96, 8])
        XCTAssertEqual(first.fields[28], 96 * 96 * 8)
    }

    func check(_ name: String, file: String, probability: (([Float], Int) -> Float)) throws {
        guard let networks = Self.fixture["networks"] as? [String: Any], let entry = networks[name] as? [String: Any],
            let input = entry["input"] as? [String: Any], let output = entry["output"] as? [String: Any],
            let bytes = (input["bytes"] as? String).flatMap({ Data(base64Encoded: $0) }), let expectedText = output["half"] as? String
        else { return XCTFail("fixture is missing \(name)") }
        let model = try Self.model(file)
        let values = [UInt8](bytes).map { Float($0) / 255 }
        let began = Date()
        let tensors = CallSegmentReference.run(model, input: values)
        let elapsed = Date().timeIntervalSince(began)
        let got = tensors[model.output]!
        let expected = Self.halves(expectedText)
        XCTAssertEqual(got.count, expected.count)
        let channels = model.outputShape.c
        let pixels = got.count / channels
        var worst: Float = 0
        var total: Float = 0
        var off = 0
        for i in 0..<pixels {
            let d = abs(probability(got, i) - probability(expected, i))
            worst = max(worst, d)
            total += d
            if d > 0.02 { off += 1 }
        }
        var logit: Float = 0
        for i in 0..<got.count { logit = max(logit, abs(got[i] - expected[i])) }
        var traceWorst = 0.0
        var traceAt = -1
        for layer in entry["trace"] as? [[String: Any]] ?? [] {
            guard let index = layer["index"] as? Int, let abs = layer["abs"] as? Double, let mine = tensors[index] else { continue }
            let mineAbs = mine.reduce(0.0) { $0 + Double(Swift.abs($1)) }
            let relative = Swift.abs(mineAbs - abs) / max(1, abs)
            if relative > traceWorst {
                traceWorst = relative
                traceAt = index
            }
        }
        print(String(format: "segment parity %@: %d layers, %.1f M MAC, worst probability %.5f, mean %.6f, pixels off by >0.02: %d of %d, worst logit %.4f, worst layer |sum| rel %.2e at %d, %.2f s",
                     name, model.layers.count, Double(model.multiplyAdds) / 1e6, worst, total / Float(pixels), off, pixels, logit, traceWorst, traceAt, elapsed))
        XCTAssertLessThan(worst, 0.05)
        XCTAssertLessThan(total / Float(pixels), 0.001)
        XCTAssertLessThan(traceWorst, 0.005)
    }

    func testReferenceMatchesTheBrowserHairNetwork() throws {
        try check("hair", file: "hair-192.fmnn") { values, i in
            1 / (1 + exp(min(max(values[i * 2] - values[i * 2 + 1], -30), 30)))
        }
    }

    func testReferenceMatchesTheBrowserSelfieNetwork() throws {
        try check("square", file: "selfie-square.fmnn") { values, i in values[i] }
    }
}
