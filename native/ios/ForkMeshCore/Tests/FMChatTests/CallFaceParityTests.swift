import Foundation
import XCTest
@testable import FMChat

enum CallFaceParity {
    static let tolerance = 1e-6

    static let fixture: [String: Any] = {
        var url = URL(fileURLWithPath: #filePath)
        url.deleteLastPathComponent()
        url.appendPathComponent("Fixtures/call-face-parity.json")
        guard let data = try? Data(contentsOf: url),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return [:] }
        return object
    }()

    static var worst: [(String, Double)] = []
    static var hashes: [(String, Int, Int)] = []

    static func at(_ value: Any?, _ path: Any...) -> Any? {
        var current = value
        for key in path {
            if let name = key as? String {
                current = (current as? [String: Any])?[name]
            } else if let index = key as? Int, let items = current as? [Any], index < items.count {
                current = items[index]
            } else {
                return nil
            }
        }
        if current is NSNull { return nil }
        return current
    }

    static func list(_ value: Any?) -> [Any] { value as? [Any] ?? [] }
    static func num(_ value: Any?) -> Double { (value as? NSNumber)?.doubleValue ?? .nan }
    static func int(_ value: Any?) -> Int { (value as? NSNumber)?.intValue ?? -1 }
    static func nums(_ value: Any?) -> [Double] { list(value).map(num) }
    static func bool(_ value: Any?) -> Bool { value as? Bool ?? false }
    static func isNull(_ value: Any?) -> Bool { value == nil || value is NSNull }

    static func record(_ section: String, _ error: Double) {
        if let index = worst.firstIndex(where: { $0.0 == section }) {
            worst[index].1 = max(worst[index].1, error)
        } else {
            worst.append((section, error))
        }
    }

    static func close(_ section: String, _ expected: Any?, _ actual: Double, _ label: String, file: StaticString = #filePath, line: UInt = #line) {
        let e = num(expected)
        if e.isNaN || !actual.isFinite {
            XCTAssertTrue(e.isNaN && !actual.isFinite, "\(section) \(label): expected \(e) but was \(actual)", file: file, line: line)
            return
        }
        let error = abs(actual - e) / max(1, abs(e))
        record(section, error)
        XCTAssertLessThanOrEqual(error, tolerance, "\(section) \(label): expected \(e) but was \(actual)", file: file, line: line)
    }

    static func numbers(_ section: String, _ expected: Any?, _ actual: [Double], _ label: String, file: StaticString = #filePath, line: UInt = #line) {
        let e = list(expected)
        XCTAssertEqual(e.count, actual.count, "\(label) length", file: file, line: line)
        for (i, value) in e.enumerated() where i < actual.count { close(section, value, actual[i], "\(label)[\(i)]", file: file, line: line) }
    }

    static func point(_ section: String, _ expected: Any?, _ actual: CallFacePoint, _ label: String) {
        numbers(section, expected, actual.values, label)
    }

    static func points(_ section: String, _ expected: Any?, _ actual: [CallFacePoint], _ label: String) {
        let e = list(expected)
        XCTAssertEqual(e.count, actual.count, "\(label) count")
        for (i, value) in e.enumerated() where i < actual.count { point(section, value, actual[i], "\(label)[\(i)]") }
    }

    static func frame(_ section: String, _ expected: Any?, _ f: CallFaceFrame, _ label: String) {
        point(section, at(expected, "left"), f.left, "\(label).left")
        point(section, at(expected, "right"), f.right, "\(label).right")
        point(section, at(expected, "mid"), f.mid, "\(label).mid")
        close(section, at(expected, "span"), f.span, "\(label).span")
        point(section, at(expected, "ux"), f.ux, "\(label).ux")
        point(section, at(expected, "uy"), f.uy, "\(label).uy")
        close(section, at(expected, "roll"), f.roll, "\(label).roll")
    }

    static func pose(_ section: String, _ expected: Any?, _ p: CallFacePose, _ label: String) {
        close(section, at(expected, "scale"), p.scale, "\(label).scale")
        for (i, row) in list(at(expected, "rows")).enumerated() { numbers(section, row, p.rows[i], "\(label).rows[\(i)]") }
        numbers(section, at(expected, "t"), p.t, "\(label).t")
        close(section, at(expected, "roll"), p.roll, "\(label).roll")
        close(section, at(expected, "yaw"), p.yaw, "\(label).yaw")
        close(section, at(expected, "pitch"), p.pitch, "\(label).pitch")
        XCTAssertEqual(bool(at(expected, "depth")), p.depth, "\(label).depth")
    }

    static func box(_ section: String, _ expected: Any?, _ b: CallFaceBox, _ label: String) {
        point(section, at(expected, "center"), b.center, "\(label).center")
        point(section, at(expected, "ux"), b.ux, "\(label).ux")
        point(section, at(expected, "uy"), b.uy, "\(label).uy")
        point(section, at(expected, "half"), b.half, "\(label).half")
    }

    static func axis(_ section: String, _ expected: Any?, _ f: CallFaceAxisFrame, _ label: String) {
        point(section, at(expected, "center"), f.center, "\(label).center")
        point(section, at(expected, "x"), f.x, "\(label).x")
        point(section, at(expected, "y"), f.y, "\(label).y")
    }

    static func outfit(_ section: String, _ expected: Any?, _ f: CallOutfitFrame?, _ label: String) {
        guard let expected else { return XCTAssertNil(f, "\(label) should be nil") }
        guard let f else { return XCTFail("\(label) missing") }
        point(section, at(expected, "center"), f.center, "\(label).center")
        point(section, at(expected, "x"), f.x, "\(label).x")
        point(section, at(expected, "y"), f.y, "\(label).y")
        close(section, at(expected, "unit"), f.unit, "\(label).unit")
        close(section, at(expected, "roll"), f.roll, "\(label).roll")
    }

    static func fnv(_ values: [Float]) -> UInt32 {
        var h: UInt32 = 0x811c9dc5
        for v in values { h = (h ^ v.bitPattern) &* 0x01000193 }
        return h
    }

    static func fnv(_ values: [UInt8]) -> UInt32 {
        var h: UInt32 = 0x811c9dc5
        for v in values { h = (h ^ UInt32(v)) &* 0x01000193 }
        return h
    }

    static func countHash(_ section: String, _ same: Bool) {
        if let index = hashes.firstIndex(where: { $0.0 == section }) {
            hashes[index].1 += same ? 1 : 0
            hashes[index].2 += 1
        } else {
            hashes.append((section, same ? 1 : 0, 1))
        }
    }

    static func floats(_ section: String, _ expected: Any?, _ actual: [Float], _ stride: Int, _ label: String) {
        XCTAssertEqual(int(at(expected, "length")), actual.count, "\(label) length")
        var sum = 0.0
        for v in actual { sum += Double(v) }
        close(section, at(expected, "sum"), sum, "\(label).sum")
        for (k, value) in nums(at(expected, "samples")).enumerated() where k * stride < actual.count {
            close(section, value, Double(actual[k * stride]), "\(label)[\(k * stride)]")
        }
        countHash(section, UInt32(int(at(expected, "hash"))) == fnv(actual))
    }

    static func strided<T>(_ items: [T], _ stride: Int) -> [T] {
        Swift.stride(from: 0, to: items.count, by: stride).map { items[$0] }
    }

    static func regions(_ value: Any?) -> CallFaceRegions? {
        guard let map = value as? [String: Any] else { return nil }
        return map.mapValues { list($0).map(nums) }
    }

    static func pointList(_ value: Any?) -> [CallFacePoint] {
        list(value).map { CallFacePoint(nums($0)) }
    }

    static func head(_ section: String, _ expected: Any?, _ head: CallFaceLiveHead, _ entry: CallFaceHead, _ points: [CallFacePoint], _ stride: Int, _ label: String) {
        let depths = CallFaceGeometry.vertexDepths(entry.model, head.pose)
        let warp = CallFaceWarp.warpVertices(mesh: entry.mesh, layout: entry.layout, live: head.points, photoSize: 512, depths: depths)
        pose(section, at(expected, "pose"), head.pose, "\(label).pose")
        numbers(section, at(expected, "jawTrust"), head.jawTrust, "\(label).jawTrust")
        self.points(section, at(expected, "points"), strided(head.points, stride), "\(label).points")
        numbers(section, at(expected, "depths"), strided(depths, stride), "\(label).depths")
        floats("\(section).warp", at(expected, "warp"), warp, 17, "\(label).warp")
        axis(section, at(expected, "zone"), CallFaceWarp.zoneFrame(points, head.pose), "\(label).zone")
        let hair = CallFaceWarp.hairRegion(head.pose, width: 640, height: 480)
        point(section, at(expected, "hair", "center"), hair.center, "\(label).hair.center")
        point(section, at(expected, "hair", "size"), hair.size, "\(label).hair.size")
    }

    static func mesh(_ expected: Any?, _ mesh: CallFaceHeadMesh, _ label: String) {
        XCTAssertEqual(int(at(expected, "count")), mesh.count, "\(label).count")
        XCTAssertEqual(int(at(expected, "extraStart")), mesh.extraStart, "\(label).extraStart")
        XCTAssertEqual(int(at(expected, "mouthStart")), mesh.mouthStart, "\(label).mouthStart")
        XCTAssertEqual(list(at(expected, "triangles")).map { list($0).map(int) }, mesh.triangles.map(\.v), "\(label).triangles")
        XCTAssertEqual(list(at(expected, "holes")).map(nums), mesh.triangles.map(\.hole), "\(label).holes")
        XCTAssertEqual(list(at(expected, "mouth")).map(bool), mesh.triangles.map(\.mouth), "\(label).mouth")
        XCTAssertEqual(list(at(expected, "cull")).map(bool), mesh.triangles.map(\.cull), "\(label).cull")
    }

    static func jsRound(_ v: Double) -> Double {
        let f = v.rounded(.down)
        return v - f >= 0.5 ? f + 1 : f
    }

    static func fine(_ v: Double) -> Double { jsRound(v * 1000) / 1000 }

    static func xorshift(_ seed: UInt32) -> () -> Double {
        var s = seed
        return {
            s ^= s << 13
            s ^= s >> 17
            s ^= s << 5
            return Double(s) / 4294967296
        }
    }

    struct Pose {
        var yaw = 0.0
        var pitch = 0.0
        var roll = 0.0
        var scale = 100.0
        var cx = 320.0
        var cy = 240.0
        var open = 0.0
        var close = [0.0, 0.0]
        var smile = 0.0
    }

    struct Flags {
        var depth = false
        var medianLine = false
        var reverseJaw = false
    }

    static func synthetic(_ pose: Pose) -> [CallFacePoint] {
        var t = CallFaces.template.map { [$0.x, $0.y, $0.z ?? .nan] }
        for (i, w) in [(55, 0.45), (56, 0.8), (57, 1.0), (58, 0.8), (59, 0.45), (65, 0.8), (66, 1.0), (67, 0.8)] { t[i][1] += pose.open * 0.35 * w }
        for (i, w) in [(5, 0.3), (6, 0.6), (7, 0.85), (8, 1.0), (9, 0.85), (10, 0.6), (11, 0.3)] { t[i][1] += pose.open * 0.25 * w }
        t[48][0] -= pose.smile * 0.08
        t[48][1] -= pose.smile * 0.06
        t[54][0] += pose.smile * 0.08
        t[54][1] -= pose.smile * 0.06
        for (u, l, side) in [(37, 41, 0), (38, 40, 0), (43, 47, 1), (44, 46, 1)] {
            let a = pose.close[side] * 0.85
            t[u][0] += (t[l][0] - t[u][0]) * a
            t[u][1] += (t[l][1] - t[u][1]) * a
        }
        let cyw = cos(pose.yaw), syw = sin(pose.yaw)
        let cp = cos(pose.pitch), sp = sin(pose.pitch)
        let cr = cos(pose.roll), sr = sin(pose.roll)
        return t.map { v in
            let x = v[0], y = v[1], z = v[2]
            let x1 = x * cyw + z * syw
            let z1 = -x * syw + z * cyw
            let y2 = y * cp - z1 * sp
            let z2 = y * sp + z1 * cp
            return CallFacePoint(pose.cx + pose.scale * (x1 * cr - y2 * sr), pose.cy + pose.scale * (x1 * sr + y2 * cr), pose.scale * z2)
        }
    }

    static func loop(_ points: [CallFacePoint], _ count: Int, _ rotate: Int) -> [CallFacePoint] {
        let out = Array(CallFaceGeometry.resamplePolyline(points + [points[0]], count + 1).prefix(count))
        return Array(out[rotate...] + out[..<rotate])
    }

    static func visionRegions(_ p: [CallFacePoint], _ flags: Flags = Flags()) -> CallFaceRegions {
        func cut(_ q: CallFacePoint) -> CallFacePoint { flags.depth ? q : q.flat }
        func pick(_ idx: [Int]) -> [CallFacePoint] { idx.map { cut(p[$0]) } }
        func shift(_ q: CallFacePoint, _ dy: Double) -> CallFacePoint { CallFacePoint(q.x, q.y + dy, q.z) }
        func band(_ idx: [Int]) -> [CallFacePoint] { loop(pick(idx).map { shift($0, -3) } + pick(idx).map { shift($0, 3) }.reversed(), 6, 2) }
        let jaw = CallFaceGeometry.resamplePolyline(pick(Array(0..<17)), 11)
        var out: [String: [CallFacePoint]] = [
            "faceContour": flags.reverseJaw ? jaw : jaw.reversed(),
            "leftEyebrow": band([22, 23, 24, 25, 26]),
            "rightEyebrow": band([17, 18, 19, 20, 21]),
            "leftEye": loop(pick(Array(42..<48)), 8, 3),
            "rightEye": loop(pick(Array(36..<42)), 8, 5),
            "nose": pick(Array(31..<36)),
            "outerLips": loop(pick(Array(48..<60)), 14, 4),
            "innerLips": loop(pick(Array(60..<68)), 6, 1),
        ]
        out[flags.medianLine ? "medianLine" : "noseCrest"] = CallFaceGeometry.resamplePolyline(pick([27, 28, 29, 30]), 6)
        return out.mapValues { region in region.map { $0.values.map(fine) } }
    }

    static func paint(_ ellipses: [[Double]], _ width: Int, _ height: Int, _ scale: Double, _ originX: Double, _ originY: Double, _ salt: Int) -> CallFaceImage {
        var data = [UInt8](repeating: 0, count: width * height * 4)
        for py in 0..<height {
            let wy = (Double(py) + 0.5) / scale + originY
            for px in 0..<width {
                let wx = (Double(px) + 0.5) / scale + originX
                let mix = (Int32(truncatingIfNeeded: px) &* 73856093) ^ (Int32(truncatingIfNeeded: py) &* 19349663) ^ (Int32(truncatingIfNeeded: salt) &* 83492791)
                let noise = Int(mix & 7)
                var color = [196, 150, 126]
                for e in ellipses {
                    let dx = wx - e[0]
                    let dy = wy - e[1]
                    if (dx * dx) / (e[2] * e[2]) + (dy * dy) / (e[3] * e[3]) <= 1 { color = [Int(e[4]), Int(e[5]), Int(e[6])] }
                }
                let at = (py * width + px) * 4
                data[at] = UInt8(color[0] + noise)
                data[at + 1] = UInt8(color[1] + noise)
                data[at + 2] = UInt8(color[2] + noise)
                data[at + 3] = 255
            }
        }
        return CallFaceImage(width: width, height: height, rgba: data)
    }

    static func anchorsOf(_ points: [CallFacePoint], _ yaw: Double?) -> CallFaceAnchors {
        CallFaceAnchors(
            eyeL: CallFaceGeometry.centroid(points, CallFaceGeometry.leftEye),
            eyeR: CallFaceGeometry.centroid(points, CallFaceGeometry.rightEye),
            mouth: CallFaceGeometry.centroid(points, [48, 54, 51, 57]),
            mouthWidth: CallFaceJS.hypot(points[54].x - points[48].x, points[54].y - points[48].y),
            open: CallFaceGeometry.clampValue(CallFaceGeometry.mouthGap(points) * 3, 0, 1),
            yaw: yaw
        )
    }

    static func anchors(_ section: String, _ expected: Any?, _ a: CallFaceAnchors, _ label: String) {
        point(section, at(expected, "eyeL"), a.eyeL, "\(label).eyeL")
        point(section, at(expected, "eyeR"), a.eyeR, "\(label).eyeR")
        point(section, at(expected, "mouth"), a.mouth ?? CallFacePoint(.nan, .nan), "\(label).mouth")
        close(section, at(expected, "open"), a.open ?? .nan, "\(label).open")
        close(section, at(expected, "yaw"), a.yaw ?? .nan, "\(label).yaw")
    }

    static func summary() -> String {
        "call face parity: worst relative error "
            + worst.map { "\($0.0)=\(String(format: "%.2e", $0.1))" }.joined(separator: ", ")
            + "; bit-identical hashes " + hashes.map { "\($0.0) \($0.1)/\($0.2)" }.joined(separator: ", ")
    }
}

final class CallFaceParityTests: XCTestCase {
    private typealias P = CallFaceParity
    private static let maya = CallFaces.head(CallFaces.byId("maya")!)
    private var maya: CallFaceHead { Self.maya }

    private static let staticPoses: [(String, P.Pose, P.Flags)] = [
        ("neutral", P.Pose(), P.Flags()),
        ("turn-left", P.Pose(yaw: -0.45), P.Flags()),
        ("turn-right", P.Pose(yaw: 0.45), P.Flags()),
        ("tilted", P.Pose(roll: 0.3), P.Flags()),
        ("nodding", P.Pose(pitch: 0.25), P.Flags()),
        ("mouth-open", P.Pose(open: 1), P.Flags()),
        ("eyes-closed", P.Pose(close: [1, 1]), P.Flags()),
        ("wink-smile", P.Pose(close: [1, 0], smile: 1), P.Flags()),
        ("depth-turned", P.Pose(yaw: 0.3, pitch: -0.1), P.Flags(depth: true)),
        ("depth-open", P.Pose(roll: -0.2, open: 0.6), P.Flags(depth: true)),
        ("median-line", P.Pose(yaw: 0.1), P.Flags(medianLine: true, reverseJaw: true)),
        ("far-small", P.Pose(yaw: 0.2, scale: 40, cx: 100, cy: 90), P.Flags()),
    ]

    override class func tearDown() {
        print(P.summary())
        super.tearDown()
    }

    func testTheCataloguesMatchTheWeb() {
        let faces = P.list(P.fixture["faces"])
        XCTAssertEqual(faces.count, CallFaces.all.count)
        for (i, face) in faces.enumerated() where i < CallFaces.all.count {
            let native = CallFaces.all[i]
            XCTAssertEqual(P.at(face, "id") as? String, native.id)
            XCTAssertEqual(P.at(face, "label") as? String, native.label)
            P.numbers("catalogue", P.at(face, "points"), native.points, "\(native.id).points")
            P.numbers("catalogue", P.at(face, "head"), native.head, "\(native.id).head")
        }
        let choices = P.list(P.at(P.fixture, "misc", "choices"))
        XCTAssertEqual(choices.map { "\(P.at($0, "id") ?? "")|\(P.at($0, "label") ?? "")|\(P.bool(P.at($0, "custom")))" },
                       CallFaces.choices().map { "\($0.id)|\($0.label)|\($0.custom)" })
        let outfits = P.list(P.at(P.fixture, "outfits", "catalogue"))
        XCTAssertEqual(outfits.map { "\(P.at($0, "id") ?? "")|\(P.at($0, "label") ?? "")" }, CallOutfits.all.map { "\($0.id)|\($0.label)" })
        let constants = P.at(P.fixture, "outfits", "constants")
        XCTAssertEqual(P.num(P.at(constants, "size")), CallOutfits.size)
        XCTAssertEqual(P.num(P.at(constants, "unit")), CallOutfits.unit)
        P.point("catalogue", P.at(constants, "neck"), CallOutfits.neck, "neck")
        XCTAssertEqual(P.num(P.at(constants, "drop")), CallOutfits.drop)
        XCTAssertEqual(P.num(P.at(constants, "chinSpans")), CallOutfits.chinSpans)
        XCTAssertEqual(P.num(P.at(constants, "roll")), CallOutfits.rollShare)
        XCTAssertEqual(P.int(P.at(constants, "tile")), CallOutfits.tile)
        XCTAssertNil(CallOutfits.byId("nobody"))
        XCTAssertNil(CallFaces.byId("nobody"))
        XCTAssertTrue(CallFaces.isCustom("custom"))
        XCTAssertFalse(CallFaces.isCustom("maya"))
    }

    func testTemplateAndFaceModelsMatch() {
        P.points("models", P.fixture["template"], CallFaces.template, "template")
        for entry in P.list(P.fixture["faceModels"]) {
            let id = P.at(entry, "id") as? String ?? ""
            guard let face = CallFaces.byId(id) else { return XCTFail(id) }
            let head = CallFaces.head(face)
            P.points("models", P.at(entry, "canonical"), CallFaces.facePoints(face), "\(id).canonical")
            P.points("models", P.at(entry, "layout"), head.layout, "\(id).layout")
            P.points("models", P.at(entry, "model"), head.model, "\(id).model")
            P.mesh(P.at(entry, "mesh"), head.mesh, id)
            let eyes = CallFaceWarp.photoEyes(head.layout, photoSize: 512)
            for (k, eye) in P.list(P.at(entry, "eyes")).enumerated() { P.numbers("models", eye, eyes[k], "\(id).eyes[\(k)]") }
        }
    }

    func testDelaunayMatches() {
        let points = P.pointList(P.at(P.fixture, "delaunay", "points"))
        let expected = P.list(P.at(P.fixture, "delaunay", "triangles")).map { P.list($0).map(P.int) }
        XCTAssertFalse(expected.isEmpty)
        XCTAssertEqual(expected, CallFaceGeometry.delaunay(points))
    }

    func testMiscMatches() {
        let misc = P.fixture["misc"]
        let constants = P.at(misc, "constants")
        XCTAssertEqual(P.int(P.at(constants, "landmarkCount")), CallFaceGeometry.landmarkCount)
        XCTAssertEqual(P.int(P.at(constants, "extraCount")), CallFaceGeometry.extraCount)
        XCTAssertEqual(P.int(P.at(constants, "cheekCount")), CallFaceGeometry.cheekCount)
        XCTAssertEqual(P.int(P.at(constants, "mouthExtraCount")), CallFaceGeometry.mouthExtraCount)
        XCTAssertEqual(P.num(P.at(constants, "foreheadLift")), CallFaceGeometry.foreheadLift)
        XCTAssertEqual(P.int(P.at(constants, "vertexFloats")), CallFaceWarp.vertexFloats)
        for row in P.list(P.at(misc, "outputSize")) {
            let args = P.nums(P.at(row, 0))
            let size = args.count > 2 ? CallFaces.outputSize(width: args[0], height: args[1], limit: args[2]) : CallFaces.outputSize(width: args[0], height: args[1])
            XCTAssertEqual(P.nums(P.at(row, 1)), [Double(size.0), Double(size.1)], "outputSize \(args)")
        }
        XCTAssertEqual(P.list(P.at(misc, "custom")).map(P.bool), [true, false])
        let pupilRegions: CallFaceRegions = ["leftPupil": [[300, 200]], "rightPupil": [[200, 201]]]
        P.points("misc", P.at(misc, "pupils", 0), CallFaces.pupils(pupilRegions, scale: 2) ?? [], "pupils")
        XCTAssertNil(CallFaces.pupils(["leftPupil": [[300, 200]]]))
        let host = CallFaces.located(["faceContour": [[0, 0]]], scale: 1)
        XCTAssertNil(host.points)
        XCTAssertNil(host.pupils)
        for row in P.list(P.at(misc, "vertexRegion")) {
            let v = P.int(P.at(row, 0))
            P.close("misc", P.at(row, 1), CallFaceWarp.vertexRegion(v), "vertexRegion \(v)")
            P.close("misc", P.at(row, 2), CallFaceWarp.vertexRegion(v, extraStart: 150), "vertexRegion \(v)/150")
        }
        for row in P.list(P.at(misc, "coverRect")) {
            let a = P.nums(P.at(row, 0))
            P.numbers("misc", P.at(row, 1), CallFaceWarp.coverRect(imageWidth: a[0], imageHeight: a[1], width: a[2], height: a[3]), "coverRect \(a)")
        }
        let looks: [[CallFacePoint?]?] = [nil, [CallFacePoint(0.1, -0.2), CallFacePoint(0.3, 0.4)], [CallFacePoint(0.1, .nan), nil]]
        for (i, look) in looks.enumerated() { P.numbers("misc", P.at(misc, "lookUniform", i), CallFaceWarp.lookUniform(look), "lookUniform \(i)") }
        let box = CallFaceBox(center: CallFacePoint(10, 20), ux: CallFacePoint(0.6, 0.8), uy: CallFacePoint(-0.8, 0.6), half: CallFacePoint(4, 5))
        P.axis("misc", P.at(misc, "axisFrame"), CallFaceWarp.axisFrame(box), "axisFrame")
        P.close("misc", P.at(misc, "luma"), CallFaceExpression.lumaOf(200, 100, 50), "luma")
        P.close("misc", P.at(misc, "redness"), CallFaceExpression.rednessOf(200, 100, 50), "redness")
        P.numbers("misc", P.at(misc, "pairClosure"), CallFaceExpression.pairClosure([0.9, 0.2]), "pairClosure")
        P.points("misc", P.at(misc, "closeNone"), CallFaceExpression.closeLids([CallFacePoint(1, 2)], [0, 0.005]), "closeNone")
        XCTAssertNil(CallFaceGeometry.landmarksFromRegions(["faceContour": [[0, 0]]]))
        var neutral = P.regions(P.at(P.fixture, "statics", 0, "regions")) ?? [:]
        neutral["noseCrest"] = [[1, 2]]
        XCTAssertNil(CallFaceGeometry.landmarksFromRegions(neutral))
        P.points("misc", P.at(misc, "resample", 0), CallFaceGeometry.resamplePolyline([CallFacePoint(1, 2, 3)], 3), "resample0")
        let line = [CallFacePoint(0, 0), CallFacePoint(3, 4), CallFacePoint(3, 4), CallFacePoint(6, 8)]
        P.points("misc", P.at(misc, "resample", 1), CallFaceGeometry.resamplePolyline(line, 5), "resample1")
        XCTAssertEqual(P.int(P.at(misc, "ringCount")), CallFaceGeometry.ringCount(maya.model.count))
    }

    func testSyntheticRegionsMatchTheGenerator() {
        let statics = P.list(P.fixture["statics"])
        XCTAssertEqual(statics.count, Self.staticPoses.count)
        for (i, (name, pose, flags)) in Self.staticPoses.enumerated() where i < statics.count {
            XCTAssertEqual(name, P.at(statics[i], "name") as? String)
            let regions = P.visionRegions(P.synthetic(pose), flags)
            let stored = P.regions(P.at(statics[i], "regions")) ?? [:]
            XCTAssertEqual(Set(stored.keys), Set(regions.keys), "\(name) keys")
            for (key, region) in stored {
                let ours = regions[key] ?? []
                XCTAssertEqual(region.count, ours.count, "\(name).\(key)")
                for (k, q) in region.enumerated() where k < ours.count { P.numbers("generator", q, ours[k], "\(name).\(key)[\(k)]") }
            }
        }
    }

    func testStaticCasesMatch() {
        let template = CallFaces.template
        for item in P.list(P.fixture["statics"]) {
            let name = P.at(item, "name") as? String ?? ""
            let s = "statics"
            guard let points = CallFaceGeometry.landmarksFromRegions(P.regions(P.at(item, "regions"))) else { return XCTFail(name) }
            P.points(s, P.at(item, "points"), points, "\(name).points")
            P.frame(s, P.at(item, "frame"), CallFaceGeometry.faceFrame(points), "\(name).frame")
            P.points(s, P.at(item, "normalized"), CallFaceGeometry.normalizeShape(points), "\(name).normalized")
            P.points(s, P.at(item, "canonical"), CallFaceGeometry.canonicalShape(points), "\(name).canonical")
            P.close(s, P.at(item, "mouthGap"), CallFaceGeometry.mouthGap(points), "\(name).mouthGap")
            P.close(s, P.at(item, "noseTurn"), CallFaceGeometry.noseTurn(points), "\(name).noseTurn")
            XCTAssertEqual(P.bool(P.at(item, "hasDepth")), CallFaceGeometry.hasDepth(points), "\(name).hasDepth")
            P.pose(s, P.at(item, "pose"), CallFaceGeometry.solvePose(maya.model, points), "\(name).pose")
            let head = CallFaceGeometry.liveHead(maya.model, points)
            P.head(s, P.at(item, "head"), head, maya, points, 2, "\(name).head")
            P.box(s, P.at(item, "eyeBoxes", 0), CallFaceExpression.eyeBox(points, 0), "\(name).eyeBox0")
            P.box(s, P.at(item, "eyeBoxes", 1), CallFaceExpression.eyeBox(points, 1), "\(name).eyeBox1")
            P.close(s, P.at(item, "eyeOpenness", 0), CallFaceExpression.eyeOpenness(points, 0), "\(name).open0")
            P.close(s, P.at(item, "eyeOpenness", 1), CallFaceExpression.eyeOpenness(points, 1), "\(name).open1")
            P.box(s, P.at(item, "tongueWindow"), CallFaceExpression.tongueWindow(points), "\(name).tongue")
            P.points(s, P.at(item, "closed"), Array(CallFaceExpression.closeLids(points, [0.8, 0.3])[36..<48]), "\(name).closed")
            let outfit = CallOutfits.frame(points)
            P.outfit(s, P.at(item, "outfit"), outfit, "\(name).outfit")
            if let outfit { P.point(s, P.at(item, "outfitUv"), CallOutfits.uv(outfit, CallFacePoint(320, 400)), "\(name).outfitUv") }
            P.points(s, P.at(item, "fit"), CallFaceGeometry.fitTemplate(template, P.anchorsOf(points, head.pose.yaw)), "\(name).fit")
            let plain = CallFaceAnchors(eyeL: CallFaceGeometry.centroid(points, CallFaceGeometry.leftEye), eyeR: CallFaceGeometry.centroid(points, CallFaceGeometry.rightEye))
            P.points(s, P.at(item, "fitPlain"), CallFaceGeometry.fitTemplate(template, plain), "\(name).fitPlain")
        }
    }

    func testEveryFaceFollowsTheHead() {
        guard let neutral = CallFaceGeometry.landmarksFromRegions(P.visionRegions(P.synthetic(P.Pose(yaw: 0.2)))),
              let depth = CallFaceGeometry.landmarksFromRegions(P.visionRegions(P.synthetic(P.Pose(yaw: -0.25, pitch: 0.1)), P.Flags(depth: true)))
        else { return XCTFail("landmarks") }
        for entry in P.list(P.fixture["everyFace"]) {
            let id = P.at(entry, "id") as? String ?? ""
            guard let face = CallFaces.byId(id) else { return XCTFail(id) }
            let head = CallFaces.head(face)
            P.head("everyFace", P.at(entry, "flat"), CallFaceGeometry.liveHead(head.model, neutral), head, neutral, 4, "\(id).flat")
            P.head("everyFace", P.at(entry, "depth"), CallFaceGeometry.liveHead(head.model, depth), head, depth, 4, "\(id).depth")
        }
    }

    func testTheSequenceMatchesTheStagePipeline() {
        let sequence = P.fixture["sequence"]
        let scale = P.num(P.at(sequence, "scale"))
        let frames = P.list(P.at(sequence, "frames"))
        let outputs = P.list(P.at(sequence, "outputs"))
        XCTAssertEqual(frames.count, 60)
        let probe = CallFaceExpressionProbe()
        let pointSmoother = CallFacePointSmoother(count: 68, minCutoff: 1.5, beta: 1.5)
        let anchorSmoother = CallFaceAnchorSmoother()
        let outfitSmoother = CallOutfitSmoother()
        let template = CallFaces.template
        var live: [CallFacePoint]?
        var expression: CallFaceExpressionReading?
        let s = "sequence"
        for (k, frame) in frames.enumerated() {
            let out = outputs[k]
            let time = P.num(P.at(frame, "time"))
            let found = CallFaces.located(P.regions(P.at(frame, "regions")), scale: scale)
            if let stored = P.at(out, "found") { P.point(s, stored, found.points?[30] ?? CallFacePoint(.nan, .nan), "\(k).found") } else { XCTAssertNil(found.points, "frame \(k) found") }
            if let stored = P.at(out, "pupils") { P.points(s, stored, found.pupils ?? [], "\(k).pupils") } else { XCTAssertNil(found.pupils, "frame \(k) pupils") }
            let points = found.points.map { pointSmoother.smooth($0, time: time, scale: CallFaceGeometry.faceFrame($0).span) }
            if let points {
                P.points(s, P.at(out, "points"), points, "\(k).points")
                live = points
                let crop = CallFaceExpression.probeCrop(points)
                let stored = P.at(out, "crop")
                XCTAssertEqual(P.int(P.at(stored, "width")), crop.width, "\(k) crop width")
                XCTAssertEqual(P.int(P.at(stored, "height")), crop.height, "\(k) crop height")
                P.close(s, P.at(stored, "scale"), crop.scale, "\(k) crop scale")
                P.numbers(s, P.at(stored, "origin"), [crop.minX, crop.minY], "\(k) crop origin")
                let paintOrigin = P.nums(P.at(stored, "paintOrigin"))
                let ellipses = P.list(P.at(frame, "ellipses")).map(P.nums)
                let image = P.paint(ellipses, P.int(P.at(stored, "width")), P.int(P.at(stored, "height")), P.num(P.at(stored, "scale")), paintOrigin[0], paintOrigin[1], k)
                expression = probe.measure(image: image, crop: crop, points: points, time: time, pupils: found.pupils, lids: true) ?? expression
            } else {
                XCTAssertNil(P.at(out, "points"), "frame \(k) points")
            }
            if let e = P.at(out, "expression") {
                guard let reading = expression else { return XCTFail("frame \(k) expression missing") }
                P.numbers(s, P.at(e, "close"), reading.close, "\(k).close")
                P.points(s, P.at(e, "look"), reading.look, "\(k).look")
                P.close(s, P.at(e, "tongue"), reading.tongue, "\(k).tongue")
                P.close(s, P.at(e, "skinRed"), reading.skinRed, "\(k).skinRed")
                P.close(s, P.at(e, "skinLuma"), reading.skinLuma, "\(k).skinLuma")
            } else {
                XCTAssertNil(expression, "frame \(k) expression")
            }
            let drawn = live.map { held in expression.map { CallFaceExpression.closeLids(held, $0.close) } ?? held }
            if let drawn {
                P.points(s, P.at(out, "drawn"), Array(drawn[36..<48]), "\(k).drawn")
                let head = CallFaceGeometry.liveHead(maya.model, drawn)
                let depths = CallFaceGeometry.vertexDepths(maya.model, head.pose)
                P.pose(s, P.at(out, "pose"), head.pose, "\(k).pose")
                P.points(s, P.at(out, "head"), P.strided(head.points, 5), "\(k).head")
                P.numbers(s, P.at(out, "depths"), P.strided(depths, 7), "\(k).depths")
                let warp = CallFaceWarp.warpVertices(mesh: maya.mesh, layout: maya.layout, live: head.points, photoSize: 512, depths: depths)
                P.floats("\(s).warp", P.at(out, "warp"), warp, 211, "\(k).warp")
                P.box(s, P.at(out, "tongueBox"), CallFaceExpression.tongueWindow(drawn), "\(k).tongueBox")
            }
            P.outfit(s, P.at(out, "outfit"), outfitSmoother.smooth(CallOutfits.frame(live)), "\(k).outfit")
            if let points {
                let smooth = anchorSmoother.smooth(P.anchorsOf(points, k % 9 == 4 ? nil : CallFaceGeometry.noseTurn(points)), time: time)
                P.anchors(s, P.at(out, "anchors"), smooth, "\(k).anchors")
                P.points(s, P.at(out, "fit"), CallFaceGeometry.fitTemplate(template, smooth), "\(k).fit")
            }
        }
    }

    func testFiltersMatch() {
        let filters = P.fixture["filters"]
        let inputs = P.list(P.at(filters, "oneEuro", "inputs")).map(P.nums)
        let outputs = P.list(P.at(filters, "oneEuro", "outputs"))
        let resetAt = P.int(P.at(filters, "oneEuro", "resetAt"))
        XCTAssertEqual(inputs.count, 40)
        let plain = CallFaceOneEuroFilter()
        let custom = CallFaceOneEuroFilter(minCutoff: 0.6, beta: 0.3, derivativeCutoff: 2)
        for (k, input) in inputs.enumerated() {
            if k == resetAt {
                plain.reset()
                custom.reset()
            }
            let values = [plain.filter(input[0], time: input[1], scale: input[2]), custom.filter(input[0], time: input[1], scale: input[2])]
            P.numbers("filters", outputs[k], values, "oneEuro \(k)")
        }
        let blink = CallFaceBlinkTracker()
        let blinkOutputs = P.list(P.at(filters, "blink", "outputs"))
        for (k, input) in P.list(P.at(filters, "blink", "inputs")).enumerated() {
            let edges = P.list(P.at(input, "edges")).map { P.isNull($0) ? Double.nan : P.num($0) }
            let openness = P.at(input, "openness").map(P.nums)
            let result = blink.update(CallFaceBlinkInput(edges: edges, openness: openness, elapsed: P.num(P.at(input, "elapsed")), time: P.num(P.at(input, "time")), moving: P.bool(P.at(input, "moving"))))
            P.numbers("filters", blinkOutputs[k], result, "blink \(k)")
        }
    }

    func testDetectionDecodingMatches() {
        let d = P.fixture["detection"]
        let next = P.xorshift(UInt32(P.int(P.at(d, "seed"))))
        let anchors = CallFaceMesh.blazeAnchors()
        XCTAssertEqual(P.int(P.at(d, "anchorCount")), anchors.count)
        var scores = [Float](repeating: 0, count: anchors.count)
        var boxes = [Float](repeating: 0, count: anchors.count * 16)
        let keys: [(Double, Double)] = [(0.43, 0.4), (0.57, 0.4), (0.5, 0.5), (0.5, 0.58), (0.36, 0.45), (0.64, 0.45)]
        for (i, anchor) in anchors.enumerated() {
            let ax = anchor.x
            let ay = anchor.y
            let near = abs(ax - 0.5) < 0.07 && abs(ay - 0.47) < 0.07
            scores[i] = Float(near ? 1 + 3 * next() : -9 + 6 * next())
            let r = i * 16
            boxes[r] = Float((0.5 - ax) * 128 + (next() - 0.5) * 3)
            boxes[r + 1] = Float((0.47 - ay) * 128 + (next() - 0.5) * 3)
            boxes[r + 2] = Float(56 + 8 * next())
            boxes[r + 3] = Float(60 + 8 * next())
            for (k, key) in keys.enumerated() {
                boxes[r + 4 + k * 2] = Float((key.0 - ax) * 128 + (next() - 0.5) * 2)
                boxes[r + 5 + k * 2] = Float((key.1 - ay) * 128 + (next() - 0.5) * 2)
            }
        }
        P.points("detection", P.at(d, "anchors"), P.strided(anchors, 37), "anchors")
        P.numbers("detection", P.at(d, "scores"), P.strided(scores.map(Double.init), 41), "scores")
        P.numbers("detection", P.at(d, "boxes"), P.strided(boxes.map(Double.init), 113), "boxes")
        guard let decoded = CallFaceMesh.decodeDetections(scores: scores, boxes: boxes, anchors: anchors) else { return XCTFail("no detection") }
        let e = P.at(d, "decoded")
        for (key, value) in [("score", decoded.score), ("cx", decoded.cx), ("cy", decoded.cy), ("w", decoded.w), ("h", decoded.h)] {
            P.close("detection", P.at(e, key), value, key)
        }
        P.points("detection", P.at(e, "keypoints"), decoded.keypoints, "keypoints")
        XCTAssertNil(CallFaceMesh.decodeDetections(scores: [Float](repeating: -5, count: anchors.count), boxes: boxes, anchors: anchors))
        let letterbox = CallFaceLetterbox(scale: 640, x: 0, y: (640 - 480) / 2 / 640)
        XCTAssertEqual(letterbox, CallFaceLetterbox(width: 640, height: 480))
        let roi = CallFaceMesh.roiFromDetection(decoded, letterbox)
        let r = P.at(d, "roi")
        P.numbers("detection", ["cx", "cy", "size", "angle"].map { P.at(r, $0) ?? NSNull() }, [roi.cx, roi.cy, roi.size, roi.angle], "roi")
    }

    func testFaceMeshRegionsMatch() {
        let m = P.fixture["faceMesh"]
        let mesh = P.pointList(P.at(m, "mesh"))
        XCTAssertEqual(mesh.count, 468)
        let regions = CallFaceMesh.meshRegions(mesh)
        let stored = P.regions(P.at(m, "regions")) ?? [:]
        XCTAssertEqual(Set(stored.keys), Set(regions.keys))
        for (key, region) in stored {
            for (k, q) in region.enumerated() { P.numbers("faceMesh", q, regions[key]?[k] ?? [], "\(key)[\(k)]") }
        }
        P.points("faceMesh", P.at(m, "landmarks"), CallFaceMesh.meshToLandmarks(mesh) ?? [], "landmarks")
        P.points("faceMesh", P.at(m, "flat"), CallFaceMesh.meshToLandmarks(mesh.map(\.flat)) ?? [], "flat")
        let roi = CallFaceMesh.roiFromMesh(mesh)
        let r = P.at(m, "roi")
        P.numbers("faceMesh", ["cx", "cy", "size", "angle"].map { P.at(r, $0) ?? NSNull() }, [roi.cx, roi.cy, roi.size, roi.angle], "roi")
        let points = CallFaceMesh.meshPoints(raw: [96, 96, 19.2, 0, 192, 0], roi: CallFaceRoi(cx: 100, cy: 50, size: 192, angle: 0))
        XCTAssertEqual(points.count, 2)
        XCTAssertEqual(points[0].x, 100, accuracy: 1e-9)
        XCTAssertEqual(points[0].y, 50, accuracy: 1e-9)
        XCTAssertEqual(points[0].z ?? 0, 19.2, accuracy: 1e-5)
        XCTAssertEqual(points[1].x, 4, accuracy: 1e-9)
        XCTAssertEqual(points[1].y, 146, accuracy: 1e-9)
        XCTAssertEqual(CallFaceMesh.sigmoid(0, limit: 60), 0.5)
    }

    func testCustomFaceMatteMatches() {
        let c = P.fixture["custom"]
        let size = P.int(P.at(c, "size"))
        XCTAssertEqual(size, CallFaceCustom.size)
        let raw = P.pointList(P.at(c, "raw"))
        let transform = CallFaceCustom.transform(raw)
        P.close("custom", P.at(c, "transform", "scale"), transform.scale, "transform.scale")
        P.numbers("custom", P.at(c, "transform", "matrix"), transform.matrix, "transform.matrix")
        let placed = raw.map { CallFaceCustom.apply(transform.matrix, $0) }
        P.points("custom", P.at(c, "placed"), placed, "placed")
        let headStored = P.nums(P.at(c, "results", "plain", "head"))
        P.numbers("custom", P.at(c, "flat"), CallFaceCustom.face(placed: placed, head: headStored).points, "flat")
        for row in P.list(P.at(c, "smoothstep")) {
            let v = P.nums(row)
            P.close("custom", v[3], CallFaceCustom.smoothstep(v[0], v[1], v[2]), "smoothstep \(v)")
        }
        let blurIn = P.nums(P.at(c, "blur", "input")).map { Float($0) }
        P.numbers("custom", P.at(c, "blur", "output"), CallFaceCustom.boxBlur(blurIn, size: 8, radius: 2, passes: 2).map(Double.init), "blur")
        var pixels = [UInt8](repeating: 0, count: size * size * 4)
        for y in 0..<size {
            for x in 0..<size {
                let at = (y * size + x) * 4
                let dx = Double(x - 256)
                let dy = Double(y - 300)
                let skin = dx * dx / (150 * 150) + dy * dy / (190 * 190) <= 1
                let hair = !skin && dx * dx / (190 * 190) + (dy + 60) * (dy + 60) / (220 * 220) <= 1
                let noise = Int(((UInt32(x) &* 2654435761) ^ (UInt32(y) &* 40503)) >> 27)
                let base = skin ? [204, 152, 128] : hair ? [52, 38, 30] : [30 + (x >> 3), 90, 140 - (y >> 3)]
                pixels[at] = UInt8(base[0] + noise)
                pixels[at + 1] = UInt8(base[1] + noise)
                pixels[at + 2] = UInt8(base[2] + noise)
                pixels[at + 3] = 255
            }
        }
        var personBytes = [UInt8](repeating: 0, count: size * size)
        for y in 0..<size {
            for x in 0..<size {
                let dx = Double(x - 256)
                let dy = Double(y - 280)
                personBytes[y * size + x] = UInt8(dx * dx / (210 * 210) + dy * dy / (300 * 300) <= 1 ? 250 : (x * 7 + y * 3) & 31)
            }
        }
        let person = CallFaceCustom.personWeights(personBytes)
        P.floats("custom.matte", P.at(c, "personSamples"), person, 997, "person")
        let span = CallFaceGeometry.faceFrame(placed).span
        guard let model = CallFaceCustom.skinModel(pixels: pixels, size: size, points: placed, span: span) else { return XCTFail("skin model") }
        P.numbers("custom", P.at(c, "skinModel", "mean"), model.mean, "skinModel.mean")
        P.numbers("custom", P.at(c, "skinModel", "inverse"), model.inverse, "skinModel.inverse")
        P.floats("custom.matte", P.at(c, "likeness"), CallFaceCustom.skinLikeness(pixels: pixels, size: size, model: model), 997, "likeness")
        let polygons = CallFaceCustom.polygons(placed)
        let face = CallFaceCustom.rasterize(polygons.face, size: size)
        let inner = CallFaceCustom.rasterize(polygons.inner, size: size)
        let neck = CallFaceCustom.rasterize(polygons.neck, size: size)
        for (name, mask) in [("plain", nil), ("person", person)] as [(String, [Float]?)] {
            let e = P.at(c, "results", name)
            XCTAssertEqual(P.list(P.at(e, "polygons")).map(P.int), [polygons.face.count, polygons.inner.count, polygons.neck.count], "\(name) polygons")
            let built = CallFaceCustom.buildMatte(pixels: pixels, size: size, points: placed, person: mask, faceMask: face, innerMask: inner, neckMask: neck)
            P.floats("custom.matte", P.at(e, "matte"), built.matte, 997, "\(name).matte")
            P.floats("custom.matte", P.at(e, "skin"), built.skin, 997, "\(name).skin")
            P.floats("custom.matte", P.at(e, "hair"), built.hair, 997, "\(name).hair")
            P.numbers("custom", P.at(e, "head"), built.head, "\(name).head")
            XCTAssertEqual(CallFaceCustom.matteRGBA(built).count, size * size * 4)
        }
        let built = CallFaces.head(CallFaceModel(id: CallFaceCustom.id, label: CallFaceCustom.label, points: P.nums(P.at(c, "flat")), head: headStored))
        P.points("custom", P.at(c, "faceModel", "layout"), built.layout, "faceModel.layout")
        P.points("custom", P.at(c, "faceModel", "model"), built.model, "faceModel.model")
        P.mesh(P.at(c, "faceModel", "mesh"), built.mesh, "faceModel")
        XCTAssertThrowsError(try CallFaceCustom.validate(nil)) { XCTAssertEqual($0 as? CallFaceCustomError, .faceNotFound) }
        XCTAssertThrowsError(try CallFaceCustom.validate(placed.map { CallFacePoint($0.x * 0.01, $0.y * 0.01) })) { XCTAssertEqual($0 as? CallFaceCustomError, .faceTooSmall) }
    }

    private func style(_ paint: CallOutfitPaint, _ fabrics: inout [CallOutfitFabric]) -> String {
        switch paint {
        case let .color(color): return "color \(color.r) \(color.g) \(color.b) \(color.a)"
        case let .fabric(fabric):
            if !fabrics.contains(fabric) { fabrics.append(fabric) }
            return "pattern \(fabrics.firstIndex(of: fabric) ?? -1)"
        case let .gradient(g):
            return "gradient " + g.stops.map { "\($0.offset):\($0.color.r),\($0.color.g),\($0.color.b),\($0.color.a)" }.joined(separator: " ")
        }
    }

    private func expectedStyle(_ value: Any?) -> String {
        if let css = value as? String {
            let color = CallOutfitColor(css: css)
            return "color \(color.r) \(color.g) \(color.b) \(color.a)"
        }
        if let pattern = P.at(value, "pattern") { return "pattern \(P.int(pattern))" }
        return "gradient " + P.list(P.at(value, "stops")).map { stop -> String in
            let color = CallOutfitColor(css: P.at(stop, 1) as? String ?? "")
            return "\(P.num(P.at(stop, 0))):\(color.r),\(color.g),\(color.b),\(color.a)"
        }.joined(separator: " ")
    }

    private func describe(_ command: CallOutfitCommand, _ fabrics: inout [CallOutfitFabric]) -> (String, [Double], String?) {
        switch command {
        case .save: return ("save", [], nil)
        case .restore: return ("restore", [], nil)
        case .beginPath: return ("beginPath", [], nil)
        case .closePath: return ("closePath", [], nil)
        case .fill: return ("fill", [], nil)
        case .stroke: return ("stroke", [], nil)
        case .clip: return ("clip", [], nil)
        case let .moveTo(x, y): return ("moveTo", [x, y], nil)
        case let .lineTo(x, y): return ("lineTo", [x, y], nil)
        case let .quadTo(a, b, c, d): return ("quadTo", [a, b, c, d], nil)
        case let .arc(x, y, r, s, e): return ("arc", [x, y, r, s, e], nil)
        case let .fillRect(x, y, w, h): return ("fillRect", [x, y, w, h], nil)
        case let .translate(x, y): return ("translate", [x, y], nil)
        case let .rotate(a): return ("rotate", [a], nil)
        case let .fillStyle(paint): return ("fillStyle", gradientCoords(paint), style(paint, &fabrics))
        case let .strokeStyle(paint): return ("strokeStyle", gradientCoords(paint), style(paint, &fabrics))
        case let .lineWidth(w): return ("lineWidth", [w], nil)
        case let .lineCap(cap): return ("lineCap", [], cap)
        case let .lineJoin(join): return ("lineJoin", [], join)
        case let .lineDash(dash): return ("lineDash", dash, nil)
        case let .composite(operation): return ("composite", [], operation)
        }
    }

    private func gradientCoords(_ paint: CallOutfitPaint) -> [Double] {
        if case let .gradient(g) = paint { return [g.x0, g.y0, g.x1, g.y1] }
        return []
    }

    func testOutfitDrawingsMatchTheRecordedCanvas() {
        var fabrics: [CallOutfitFabric] = []
        let drawings = P.list(P.at(P.fixture, "outfits", "drawings"))
        XCTAssertEqual(drawings.count, 6)
        for drawing in drawings {
            let id = P.at(drawing, "id") as? String ?? ""
            let ops = P.list(P.at(drawing, "ops"))
            guard let commands = CallOutfitDrawing.commands(id) else { return XCTFail(id) }
            XCTAssertEqual(ops.count, commands.count, "\(id) op count")
            for (i, op) in ops.enumerated() where i < commands.count {
                let e = P.list(op)
                let (name, numbers, text) = describe(commands[i], &fabrics)
                XCTAssertEqual(e.first as? String, name, "\(id) op \(i)")
                switch name {
                case "fillStyle", "strokeStyle":
                    XCTAssertEqual(expectedStyle(P.at(op, 1)), text, "\(id) op \(i) style")
                    if let coords = P.at(op, 1, "gradient") { P.numbers("outfits", coords, numbers, "\(id) op \(i) gradient") }
                case "lineCap", "lineJoin", "composite":
                    XCTAssertEqual(P.at(op, 1) as? String, text, "\(id) op \(i)")
                case "lineDash":
                    P.numbers("outfits", P.at(op, 1), numbers, "\(id) op \(i) dash")
                default:
                    P.numbers("outfits", Array(e.dropFirst()), numbers, "\(id) op \(i) \(name)")
                }
            }
        }
        XCTAssertNil(CallOutfitDrawing.commands(""))
        let tiles = P.list(P.at(P.fixture, "outfits", "tiles"))
        XCTAssertEqual(tiles.count, fabrics.count, "tile count")
        var worstByte = 0
        for (i, tile) in tiles.enumerated() where i < fabrics.count {
            let data = CallOutfitDrawing.fabricTile(fabrics[i])
            for (k, value) in P.nums(P.at(tile, "samples")).enumerated() {
                let diff = abs(Int(data[k * 257]) - Int(value))
                worstByte = max(worstByte, diff)
                XCTAssertLessThanOrEqual(diff, 1, "tile \(i) byte \(k * 257)")
            }
            P.countHash("tiles", UInt32(P.int(P.at(tile, "hash"))) == P.fnv(data))
        }
        P.record("tileBytes", Double(worstByte))
    }
}
