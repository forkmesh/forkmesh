import Foundation
import XCTest

@testable import FMChat

final class CallFaceDepthTests: XCTestCase {
    private func field(width: Int, height: Int, orientation: CallDepthOrientation, metres: (Int, Int) -> Float) -> CallDepthField {
        var values: [Float] = []
        for y in 0..<height {
            for x in 0..<width { values.append(metres(x, y)) }
        }
        return CallDepthField(width: width, height: height, values: values, orientation: orientation)!
    }

    func testOrientationMapsUprightCornersToSensorCorners() {
        XCTAssertEqual(CallDepthOrientation.up.sensor(u: 0.25, v: 0.75).x, 0.25)
        let right = CallDepthOrientation.right.sensor(u: 0, v: 0)
        XCTAssertEqual(right.x, 0)
        XCTAssertEqual(right.y, 1)
        let mirrored = CallDepthOrientation.leftMirrored.sensor(u: 0.2, v: 0.6)
        XCTAssertEqual(mirrored.x, 0.6, accuracy: 1e-9)
        XCTAssertEqual(mirrored.y, 0.2, accuracy: 1e-9)
        let left = CallDepthOrientation.left.sensor(u: 0.2, v: 0.6)
        XCTAssertEqual(left.x, 0.4, accuracy: 1e-9)
        XCTAssertEqual(left.y, 0.2, accuracy: 1e-9)
        XCTAssertTrue(CallDepthOrientation.right.uprightSize(width: 640, height: 480) == (480, 640))
        XCTAssertTrue(CallDepthOrientation.down.uprightSize(width: 640, height: 480) == (640, 480))
    }

    func testRightOrientationReadsTheRotatedSensorColumn() {
        let depth = field(width: 8, height: 6, orientation: .right) { x, _ in Float(0.3 + Double(x) * 0.05) }
        let top = depth.metres(u: 0.5, v: 0.05, radius: 0)
        let bottom = depth.metres(u: 0.5, v: 0.95, radius: 0)
        XCTAssertEqual(top ?? 0, 0.3, accuracy: 1e-6)
        XCTAssertEqual(bottom ?? 0, 0.65, accuracy: 1e-6)
    }

    func testSamplingSkipsHolesAndTakesTheNeighbourhoodMedian() {
        let depth = field(width: 5, height: 5, orientation: .up) { x, y in
            if x == 2 && y == 2 { return 0 }
            if x == 1 && y == 1 { return .nan }
            return x == 3 ? 0.62 : 0.5
        }
        XCTAssertEqual(depth.metres(u: 0.5, v: 0.5) ?? 0, 0.5, accuracy: 1e-6)
        XCTAssertNil(depth.metres(u: 1.2, v: 0.5))
        let empty = field(width: 3, height: 3, orientation: .up) { _, _ in 0 }
        XCTAssertNil(empty.metres(u: 0.5, v: 0.5))
    }

    func testFocalLengthScalesFromCalibrationOrFieldOfView() {
        XCTAssertEqual(CallFaceDepth.focalPixels(fx: 2880, referenceWidth: 4032, sensorWidth: 640) ?? 0, 457.142857, accuracy: 1e-4)
        XCTAssertEqual(CallFaceDepth.focalPixels(fieldOfViewDegrees: 90, sensorWidth: 640) ?? 0, 320, accuracy: 1e-6)
        XCTAssertNil(CallFaceDepth.focalPixels(fx: 0, referenceWidth: 4032, sensorWidth: 640))
        XCTAssertNil(CallFaceDepth.focalPixels(fieldOfViewDegrees: 0, sensorWidth: 640))
    }

    func testDepthConvertsToNearerIsLargerPixels() {
        XCTAssertEqual(CallFaceDepth.z(depth: 0.45, faceDepth: 0.5, focal: 500), 50, accuracy: 1e-9)
        XCTAssertEqual(CallFaceDepth.z(depth: 0.55, faceDepth: 0.5, focal: 500), -50, accuracy: 1e-9)
        XCTAssertEqual(CallFaceDepth.z(depth: 0.5, faceDepth: 0.5, focal: 500), 0)
    }

    func testReliefGivesEveryPointDepthAndFillsOutliersFromNeighbours() {
        let depth = field(width: 10, height: 10, orientation: .up) { x, _ in
            if (4...6).contains(x) { return 0.45 }
            if x == 9 { return 2.0 }
            return 0.5
        }
        let regions: [String: [[Double]]] = [
            "nose": [[55, 50]],
            "faceContour": [[15, 50], [35, 50], [95, 50]],
            "leftEye": [[25, 30], [35, 30]],
        ]
        let relief = CallFaceDepth.relief(regions, field: depth, width: 100, height: 100, focal: 500)
        XCTAssertNotNil(relief)
        let reliefRegions = relief ?? [:]
        XCTAssertEqual(Set(reliefRegions.keys), Set(regions.keys))
        XCTAssertTrue(reliefRegions.values.allSatisfy { $0.allSatisfy { $0.count == 3 } })
        XCTAssertEqual(reliefRegions["nose"]?[0][2] ?? 0, 50, accuracy: 1e-6)
        XCTAssertEqual(reliefRegions["faceContour"]?[2][2] ?? -1, 0, accuracy: 1e-6)
        XCTAssertEqual(reliefRegions["faceContour"]?[2][0], 95)
        XCTAssertEqual(reliefRegions["leftEye"]?[1][1], 30)
    }

    func testReliefRefusesWhenMostPointsLackDepth() {
        let depth = field(width: 10, height: 10, orientation: .up) { x, _ in x < 2 ? 0.5 : 0 }
        let regions: [String: [[Double]]] = ["faceContour": [[5, 50], [35, 50], [55, 50], [75, 50]]]
        XCTAssertNil(CallFaceDepth.relief(regions, field: depth, width: 100, height: 100, focal: 500))
        XCTAssertNil(CallFaceDepth.relief(regions, field: depth, width: 100, height: 100, focal: 0))
    }

    func testFillPrefersTheNearestSampledNeighbour() {
        XCTAssertEqual(CallFaceDepth.fill([nil, 0.4, nil, nil, 0.6], fallback: 1), [0.4, 0.4, 0.4, 0.6, 0.6])
        XCTAssertEqual(CallFaceDepth.fill([nil, nil], fallback: 0.7), [0.7, 0.7])
    }

    func testMedianAveragesTheMiddlePair() {
        XCTAssertEqual(CallFaceDepth.median([0.3, 0.1, 0.2]), 0.2)
        XCTAssertEqual(CallFaceDepth.median([0.4, 0.1, 0.2, 0.3]) ?? 0, 0.25, accuracy: 1e-9)
        XCTAssertNil(CallFaceDepth.median([]))
    }
}
