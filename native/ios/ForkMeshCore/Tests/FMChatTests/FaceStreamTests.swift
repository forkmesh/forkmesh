import Foundation
import XCTest
@testable import FMChat

enum FaceStreamFixture {
    static let data: [String: Any] = {
        var url = URL(fileURLWithPath: #filePath)
        url.deleteLastPathComponent()
        url.appendPathComponent("Fixtures/face-stream-parity.json")
        guard let data = try? Data(contentsOf: url),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return [:] }
        return object
    }()

    static func list(_ v: Any?) -> [Any] { v as? [Any] ?? [] }
    static func obj(_ v: Any?) -> [String: Any] { v as? [String: Any] ?? [:] }
    static func num(_ v: Any?) -> Double { (v as? NSNumber)?.doubleValue ?? .nan }
    static func int(_ v: Any?) -> Int { (v as? NSNumber)?.intValue ?? 0 }
    static func nums(_ v: Any?) -> [Double] { list(v).map(num) }
    static func points(_ v: Any?) -> [CallFacePoint] {
        let flat = nums(v)
        return stride(from: 0, to: flat.count - 1, by: 2).map { CallFacePoint(flat[$0], flat[$0 + 1]) }
    }
    static func bytes(_ hex: Any?) -> [UInt8] {
        let text = Array((hex as? String ?? "").utf8)
        func nibble(_ c: UInt8) -> UInt8 { c >= 97 ? c - 87 : c - 48 }
        return stride(from: 0, to: text.count - 1, by: 2).map { nibble(text[$0]) << 4 | nibble(text[$0 + 1]) }
    }
    static func hex(_ bytes: [UInt8]) -> String { bytes.map { String(format: "%02x", $0) }.joined() }
}

final class FaceStreamTests: XCTestCase {
    typealias F = FaceStreamFixture

    func close(_ a: Double, _ b: Double, _ label: String, worst: inout Double) {
        let e = abs(a - b) / max(1, abs(b))
        worst = max(worst, e)
        XCTAssertLessThanOrEqual(e, 1e-9, label)
    }

    func testFaceStreamFixtureIsLoaded() {
        XCTAssertEqual(F.data["format"] as? String, "face-stream-parity")
        XCTAssertEqual(F.list(F.data["template"]).count, 68)
        for (i, row) in F.list(F.data["template"]).enumerated() { XCTAssertEqual(F.nums(row), FaceStream.template[i]) }
    }

    func testFaceStreamPacketsMatchTheGoReference() throws {
        var worst = 0.0
        var packets = 0
        for stream in F.list(F.data["streams"]).map(F.obj) {
            let c = F.obj(stream["config"])
            let encoder = try FaceStreamEncoder(FaceStreamConfig(width: F.int(c["width"]), height: F.int(c["height"]), fps: F.int(c["fps"]),
                precision: F.int(c["precision"]), keyInterval: F.int(c["keyInterval"])))
            let decoder = FaceStreamDecoder()
            let requests = Set(F.list(stream["requestKeyAt"]).map(F.int))
            let drops = Set(F.list(stream["drop"]).map(F.int))
            let from = F.int(stream["decodeFrom"])
            var decoded = F.list(stream["decoded"]).map(F.obj).makeIterator()
            let expected = F.list(stream["packets"])
            for (i, raw) in F.list(stream["inputs"]).map(F.obj).enumerated() {
                if requests.contains(i) { encoder.requestKey() }
                let pose = F.nums(raw["pose"])
                let input = FaceStreamInput(time: UInt32(F.int(raw["time"])), points: F.points(raw["points"]),
                    pose: FaceStreamPose(rotation: Array(pose[0..<4]), x: pose[4], y: pose[5], scale: pose[6]),
                    expression: FaceStreamExpression(values: F.nums(raw["expression"])))
                let packet = try encoder.encode(input)
                packets += 1
                XCTAssertEqual(F.hex(packet), expected[i] as? String, "\(stream["name"] ?? "") packet \(i)")
                if i < from || drops.contains(i) { continue }
                guard let want = decoded.next() else { return XCTFail("decoded list too short") }
                do {
                    let got = try decoder.decode(F.bytes(expected[i]))
                    let frame = F.obj(want["frame"])
                    XCTAssertNil(want["error"], "frame \(i) should fail")
                    XCTAssertEqual(got.key, frame["key"] as? Bool)
                    XCTAssertEqual(got.stale, frame["stale"] as? Bool)
                    XCTAssertEqual(got.seq, F.int(frame["seq"]))
                    XCTAssertEqual(got.keyId, F.int(frame["keyId"]))
                    XCTAssertEqual(Int(got.time), F.int(frame["time"]))
                    XCTAssertEqual(got.precision, F.int(frame["precision"]))
                    for (k, v) in (got.pose.rotation + [got.pose.x, got.pose.y, got.pose.scale]).enumerated() { close(v, F.nums(frame["pose"])[k], "pose \(i)", worst: &worst) }
                    for (k, v) in got.expression.values.enumerated() { close(v, F.nums(frame["expression"])[k], "expression \(i)", worst: &worst) }
                    let flat = F.nums(frame["points"])
                    for (k, p) in got.points.enumerated() {
                        close(p.x, flat[k * 2], "x \(i).\(k)", worst: &worst)
                        close(p.y, flat[k * 2 + 1], "y \(i).\(k)", worst: &worst)
                    }
                } catch let error as FaceStreamError {
                    XCTAssertEqual(error.code, want["error"] as? String, "frame \(i)")
                }
            }
        }
        print("FaceStream parity: \(packets) packets byte-identical, worst decoded difference \(worst)")
    }

    func testFaceStreamReferencesMatchTheGoReference() throws {
        for case let entry as [String: Any] in F.list(F.data["references"]) {
            let input = F.obj(entry["input"])
            let crop = F.list(input["crop"]).map(F.int)
            let maskText = input["mask"] as? String ?? ""
            let reference = FaceStreamReference(
                id: F.int(input["id"]), format: F.int(input["format"]), time: UInt32(F.int(input["time"])), cropX: crop[0], cropY: crop[1], cropW: crop[2], cropH: crop[3],
                width: F.int(input["width"]), height: F.int(input["height"]), points: F.points(input["points"]), maskW: F.int(input["maskW"]), maskH: F.int(input["maskH"]),
                mask: maskText.isEmpty ? nil : maskText.utf8.map { $0 == 49 ? 1 : 0 }, image: F.bytes(input["image"]))
            XCTAssertEqual(F.hex(try reference.encoded()), entry["packet"] as? String)
            let got = try FaceStreamReference.decode(F.bytes(entry["packet"]))
            let want = F.obj(entry["decoded"])
            XCTAssertEqual(got.mask.map { $0.map { String($0) }.joined() } ?? "", want["mask"] as? String)
            XCTAssertEqual(got.points.flatMap { [$0.x, $0.y] }, F.nums(want["points"]))
            XCTAssertEqual(F.hex(got.image), want["image"] as? String)
            XCTAssertThrowsError(try FaceStreamDecoder().decode(F.bytes(entry["packet"])))
        }
    }

    func testFaceStreamRandomBytesNeverCrash() {
        var state: UInt32 = 0x2390
        func next() -> UInt8 {
            state ^= state << 13
            state ^= state >> 17
            state ^= state << 5
            return UInt8(truncatingIfNeeded: state)
        }
        let key = F.bytes(F.list(F.obj(F.list(F.data["streams"]).first)["packets"]).first)
        for round in 0..<400 {
            let decoder = FaceStreamDecoder()
            _ = try? decoder.decode(key)
            var packet = (0..<(2 + round % 90)).map { _ in next() }
            if round % 3 == 0 { packet[0] = packet[0] & 0x4F | 0x10 }
            if let frame = try? decoder.decode(packet) {
                XCTAssertTrue(frame.points.allSatisfy { $0.x.isFinite && $0.y.isFinite })
            }
            _ = try? FaceStreamReference.decode(packet + (0..<(round * 3)).map { _ in next() })
            var atlas = packet + (0..<round).map { _ in next() }
            atlas[0] = atlas[0] & 0x0F | 0xD0
            if let decoded = try? FaceStreamAtlas.decode(atlas) {
                XCTAssertTrue(decoded.tiles.allSatisfy { $0.mouth <= 1 && $0.smile <= 1 && $0.points.count == 20 })
            }
        }
    }

    func testFaceStreamAtlasesMatchTheGoReference() throws {
        let cases = F.list(F.data["atlases"]).map(F.obj)
        XCTAssertEqual(cases.count, 2)
        for entry in cases {
            let input = F.obj(entry["input"])
            let tiles = F.list(input["tiles"]).map(F.obj).map {
                FaceStreamAtlasTile(state: F.int($0["state"]), mouth: F.num($0["mouth"]), smile: F.num($0["smile"]), points: F.points($0["points"]))
            }
            let atlas = FaceStreamAtlas(id: F.int(input["id"]), time: UInt32(F.int(input["time"])), tileW: F.int(input["tileW"]), tileH: F.int(input["tileH"]),
                format: F.int(input["format"]), tiles: tiles, image: F.bytes(input["image"]))
            XCTAssertEqual(F.hex(try atlas.encoded()), entry["packet"] as? String)
            let got = try FaceStreamAtlas.decode(F.bytes(entry["packet"]))
            let want = F.obj(entry["decoded"])
            for (k, tile) in F.list(want["tiles"]).map(F.obj).enumerated() {
                XCTAssertEqual(got.tiles[k].state, F.int(tile["state"]))
                XCTAssertEqual(got.tiles[k].mouth, F.num(tile["mouth"]))
                XCTAssertEqual(got.tiles[k].smile, F.num(tile["smile"]))
                XCTAssertEqual(got.tiles[k].points.flatMap { [$0.x, $0.y] }, F.nums(tile["points"]))
            }
            XCTAssertEqual(F.hex(got.image), want["image"] as? String)
            XCTAssertThrowsError(try FaceStreamDecoder().decode(Array(F.bytes(entry["packet"]).prefix(64)))) { XCTAssertEqual($0 as? FaceStreamError, .notAFrame) }
            let receiver = FaceStreamReceiver()
            guard case .atlas = receiver.receive(F.bytes(entry["packet"])) else { return XCTFail("atlas") }
            XCTAssertEqual(receiver.atlas, got)
        }
    }

    func testFaceStreamMouthAtlasPolicyAndBlend() throws {
        let frontal = FaceStreamSolvedPose(pose: FaceStreamPose(rotation: [1, 0, 0, 0], x: 0, y: 0, scale: 90), yaw: 0, pitch: 0, roll: 0)
        func state(_ mouth: Double, _ width: Double) -> Int? {
            FaceStreamSending.mouthState(pose: frontal, expression: FaceStreamExpression(mouth: mouth), width: width, restWidth: 1)?.state
        }
        XCTAssertEqual(state(0.01, 1), FaceStreamAtlas.closed)
        XCTAssertEqual(state(0.5, 1), FaceStreamAtlas.open)
        XCTAssertEqual(state(0.15, 1.1), FaceStreamAtlas.wide)
        XCTAssertEqual(state(0.15, 0.9), FaceStreamAtlas.round)
        XCTAssertNil(state(0.15, 1))
        let turned = FaceStreamSolvedPose(pose: frontal.pose, yaw: 0.5, pitch: 0, roll: 0)
        XCTAssertNil(FaceStreamSending.mouthState(pose: turned, expression: FaceStreamExpression(mouth: 0.5), width: 1, restWidth: 1))
        let face = FaceStream.project(FaceStreamPose(rotation: [1, 0, 0, 0], x: 640, y: 330, scale: 96))
        let crop = FaceStreamSending.mouthTileCrop(face)
        let tile = FaceStreamSending.mouthTilePoints(face, crop: crop)
        XCTAssertTrue(tile.allSatisfy { $0.x > 0 && $0.x < 64 && $0.y > 0 && $0.y < 48 })
        XCTAssertEqual(((tile[6].x - tile[0].x) * (tile[6].x - tile[0].x) + (tile[6].y - tile[0].y) * (tile[6].y - tile[0].y)).squareRoot(), 40, accuracy: 1)
        let atlas = try FaceStreamAtlas.decode(F.bytes(F.obj(F.list(F.data["atlases"]).first)["packet"]))
        let open = try XCTUnwrap(FaceStreamView.mouthBlend(atlas.tiles, expression: FaceStreamExpression(mouth: 0.6, smile: 0.3), points: nil))
        XCTAssertEqual(open.a, 1)
        XCTAssertGreaterThan(open.opacity, 0.9)
        XCTAssertEqual(FaceStreamView.mouthBlend(atlas.tiles, expression: FaceStreamExpression(mouth: 0, smile: 0.4), points: nil)?.opacity, 0)
        let mesh = FaceStreamView.mouthMesh(face, tileA: atlas.tiles[1], tileB: atlas.tiles[2], tileW: 64, tileH: 48)
        XCTAssertEqual(mesh.count % (3 * FaceStreamView.mouthVertexFloats), 0)
        XCTAssertTrue(mesh.allSatisfy(\.isFinite))
    }

    func testFaceStreamSenderKeepsPicturesInsideTheBudget() throws {
        let sender = try FaceStreamSender(FaceStreamConfig(width: 1280, height: 720))
        let face = FaceStream.project(FaceStreamPose(rotation: [1, 0, 0, 0], x: 640, y: 330, scale: 96))
        let crop = sender.crop(face)
        let picture = [UInt8](repeating: 7, count: 10000)
        var sent = 0
        var bytes = 0
        for t in stride(from: 0.0, through: 60000, by: 500) {
            let score = sender.score(points: face, reading: nil, light: nil)
            if sender.due(time: t, score: score) {
                bytes += try sender.reference(time: t, crop: crop, points: face, format: FaceStreamReference.jpeg, image: picture, mask: nil, score: score).count
                sent += 1
            }
        }
        XCTAssertGreaterThanOrEqual(sent, 5)
        XCTAssertLessThanOrEqual(Double(bytes), 10500 + 1400 * 60)
        guard let candidate = sender.mouthCandidate(time: 0, points: face, reading: nil) else { return XCTFail("closed tile") }
        XCTAssertEqual(candidate.pick.state, FaceStreamAtlas.closed)
        sender.storeMouthTile(candidate, rgba: [UInt8](repeating: 128, count: 64 * 48 * 4))
        XCTAssertFalse(sender.atlasDue(time: 100))
        XCTAssertEqual(sender.tileCount, 1)
    }

    func testFaceStreamMatteFollowsTheEdgeAndThePlateKeepsTheBody() throws {
        let entry = F.obj(F.list(F.data["references"]).first)
        let reference = try FaceStreamReference.decode(F.bytes(entry["packet"]))
        let w = reference.width, h = reference.height
        let coarse = FaceStreamView.coarseMask(reference)
        var rgba = [UInt8](repeating: 255, count: w * h * 4)
        for i in 0..<(w * h) {
            let person = coarse[i] > 0.5
            rgba[i * 4] = person ? 60 : 220
            rgba[i * 4 + 1] = person ? 40 : 230
            rgba[i * 4 + 2] = person ? 30 : 240
        }
        let alpha = try XCTUnwrap(FaceStreamView.matte(reference, rgba: rgba))
        var agree = 0
        for i in 0..<(w * h) where (alpha[i] > 0.5) == (coarse[i] > 0.5) { agree += 1 }
        XCTAssertGreaterThan(Double(agree) / Double(w * h), 0.97)
        let layers = FaceStreamView.layers(reference, alpha: alpha)
        let frame = CallFaceGeometry.faceFrame(reference.points)
        let eye = Int(frame.mid.y) * w + Int(frame.mid.x)
        XCTAssertGreaterThan(layers.head[eye], 0.9)
        XCTAssertGreaterThan(layers.removal[eye], 0.9)
        let bottom = 5 * w + 5
        XCTAssertLessThan(layers.head[bottom], 0.05)
        XCTAssertLessThan(layers.removal[bottom], 0.05)
        var below = 0
        for y in 0..<h {
            let qy = ((Double(w / 2) + 0.5 - frame.mid.x) * frame.uy.x + (Double(y) + 0.5 - frame.mid.y) * frame.uy.y) / frame.span
            if qy > 1.6 && layers.head[y * w + w / 2] < layers.removal[y * w + w / 2] + 0.2 { below += 1 }
        }
        XCTAssertGreaterThan(below, 0)
        let plate = FaceStreamView.plate(rgba: rgba, width: w, height: h, removal: FaceStreamView.grow(layers.removal, w, h))
        XCTAssertGreaterThan(Int(plate[eye * 4]), 150)
        XCTAssertEqual(plate[bottom * 4], rgba[bottom * 4])
        let unmixed = FaceStreamView.unmix(rgba: rgba, plate: plate, alpha: alpha)
        XCTAssertEqual(unmixed[eye * 4], rgba[eye * 4])
        let padded = FaceStreamView.padFloats(alpha, w, h, 10)
        XCTAssertEqual(padded.count, (w + 20) * (h + 20))
        XCTAssertEqual(padded[0], alpha[0])
    }

    func testFaceStreamAnchorMakesTheReferenceItsOwnIdentity() throws {
        let entry = F.obj(F.list(F.data["references"]).first)
        let reference = try FaceStreamReference.decode(F.bytes(entry["packet"]))
        let matrix = FaceStreamView.photoTransform(reference.points)
        let size = FaceStreamView.photoSize
        let person = [Float](repeating: 0, count: size * size)
        var head = [Float](repeating: 0, count: size * size)
        let placed = reference.points.map { CallFaceCustom.apply(matrix, $0) }
        let mid = CallFaceGeometry.faceFrame(placed).mid
        for y in 0..<size {
            for x in 0..<size where abs(Double(x) - mid.x) < 140 && abs(Double(y) - mid.y - 40) < 200 { head[y * size + x] = 1 }
        }
        let face = FaceStreamView.face(photo: [UInt8](repeating: 128, count: size * size * 4), person: person, head: head, points: reference.points, matrix: matrix)
        XCTAssertEqual(face.aux.count, size * size * 4)
        let pose = FaceStreamSending.pose(reference.points).pose
        let place = FaceStreamPlace(points: reference.points, pose: pose, matrix: matrix, pad: 0, width: reference.width, height: reference.height)
        let anchor = FaceStreamView.anchor(head: face.head, place: place, width: 320, height: 400)
        let sx = 320.0 / Double(reference.width), sy = 400.0 / Double(reference.height)
        let r3 = FaceStream.rotationRows(pose.rotation)[2]
        let live = reference.points.enumerated().map { i, p in CallFacePoint(p.x * sx, p.y * sy, pose.scale * (sx + sy) / 2 * FaceStream.dot3(r3, FaceStream.template[i])) }
        let fitted = CallFaceGeometry.liveHead(face.head.model, live)
        let points = FaceStreamView.warpPoints(fitted.points, drawn: live, mouthStart: face.head.mesh.mouthStart, anchor: anchor)
        var worst = 0.0
        for v in 0..<face.head.mesh.mouthStart where !(60..<68).contains(v) {
            let l = face.head.layout[v]
            let det = matrix[0] * matrix[3] - matrix[1] * matrix[2]
            let px = (matrix[3] * (l.x - matrix[4]) - matrix[2] * (l.y - matrix[5])) / det * sx
            let py = (-matrix[1] * (l.x - matrix[4]) + matrix[0] * (l.y - matrix[5])) / det * sy
            worst = max(worst, abs(points[v].x - px), abs(points[v].y - py))
        }
        XCTAssertLessThan(worst, 0.6)
    }

    func testFaceStreamPoseRecoversTheTemplateRotation() {
        let q = [0.98, 0.1, -0.15, 0.05]
        let n = q.reduce(0) { $0 + $1 * $1 }.squareRoot()
        let unit = q.map { $0 / n }
        let back = FaceStreamSending.quatFromRows(FaceStream.rotationRows(unit))
        for k in 0..<4 { XCTAssertEqual(back[k], unit[k], accuracy: 1e-12) }
        let pose = FaceStreamPose(rotation: unit, x: 600, y: 300, scale: 90)
        let solved = FaceStreamSending.pose(FaceStream.project(pose).enumerated().map { i, p in
            CallFacePoint(p.x, p.y, 90 * FaceStream.dot3(FaceStream.rotationRows(unit)[2], FaceStream.template[i]))
        })
        XCTAssertEqual(solved.pose.scale, 90, accuracy: 0.5)
        XCTAssertEqual(solved.pose.x, 600, accuracy: 0.5)
        for k in 0..<4 { XCTAssertEqual(solved.pose.rotation[k], unit[k], accuracy: 1e-3) }
    }

    func testFaceStreamReferencePolicyPicksFrontalOpenFaces() {
        let frontal = FaceStreamSolvedPose(pose: FaceStreamPose(rotation: [1, 0, 0, 0], x: 0, y: 0, scale: 90), yaw: 0.02, pitch: 0.01, roll: 0)
        let turned = FaceStreamSolvedPose(pose: frontal.pose, yaw: 0.4, pitch: 0, roll: 0)
        let open = FaceStreamExpression()
        let light = FaceStreamLight(mean: 0.5, contrast: 0.1)
        let good = FaceStreamSending.score(pose: frontal, expression: open, light: light, span: 90)
        XCTAssertGreaterThan(good, 0.8)
        XCTAssertEqual(FaceStreamSending.score(pose: turned, expression: open, light: light, span: 90), 0)
        XCTAssertEqual(FaceStreamSending.score(pose: frontal, expression: FaceStreamExpression(lidLeft: 0.5), light: light, span: 90), 0)
        XCTAssertEqual(FaceStreamSending.score(pose: frontal, expression: FaceStreamExpression(mouth: 0.3), light: light, span: 90), 0)
        XCTAssertEqual(FaceStreamSending.score(pose: frontal, expression: open, light: FaceStreamLight(mean: 0.1, contrast: 0.1), span: 90), 0)
        XCTAssertEqual(FaceStreamSending.score(pose: frontal, expression: open, light: light, span: 20), 0)
        XCTAssertTrue(FaceStreamSending.due(lastTime: nil, lastScore: 0, time: 0, score: good))
        XCTAssertFalse(FaceStreamSending.due(lastTime: 0, lastScore: 0.5, time: 1500, score: 1))
        XCTAssertFalse(FaceStreamSending.due(lastTime: 0, lastScore: 0.5, time: 2500, score: 0.9))
        XCTAssertTrue(FaceStreamSending.due(lastTime: 0, lastScore: 0.5, time: 4500, score: 0.9))
        XCTAssertFalse(FaceStreamSending.due(lastTime: 0, lastScore: 0.85, time: 4500, score: 0.9))
        XCTAssertTrue(FaceStreamSending.due(lastTime: 0, lastScore: 0.95, time: 10000, score: 0.5))
    }

    func testFaceStreamCropFramesTheHeadInsideTheCamera() {
        let points = FaceStream.project(FaceStreamPose(rotation: [1, 0, 0, 0], x: 640, y: 330, scale: 96))
        let crop = FaceStreamSending.crop(points, frameWidth: 1280, frameHeight: 720)
        XCTAssertEqual(Double(crop.w) / Double(crop.h), 0.8, accuracy: 0.01)
        XCTAssertTrue(crop.x >= 0 && crop.y >= 0 && crop.x + crop.w <= 1280 && crop.y + crop.h <= 720)
        let inside = FaceStreamSending.referencePoints(points, crop: crop, width: 256, height: 320)
        XCTAssertTrue(inside.allSatisfy { $0.x > 0 && $0.x < 256 && $0.y > 0 && $0.y < 320 })
        let edge = FaceStream.project(FaceStreamPose(rotation: [1, 0, 0, 0], x: 40, y: 700, scale: 300))
        let clamped = FaceStreamSending.crop(edge, frameWidth: 1280, frameHeight: 720)
        XCTAssertTrue(clamped.x >= 0 && clamped.y >= 0 && clamped.x + clamped.w <= 1280 && clamped.y + clamped.h <= 720)
    }

    func testFaceStreamReceiverMapsPointsIntoTheReferenceAndAddsDepth() throws {
        let streams = F.list(F.data["streams"]).map(F.obj)
        let references = F.list(F.data["references"]).map(F.obj)
        let receiver = FaceStreamReceiver()
        guard case .reference = receiver.receive(F.bytes(references[0]["packet"])) else { return XCTFail("reference") }
        XCTAssertNil(receiver.livePoints(width: 320, height: 400))
        guard case .frame = receiver.receive(F.bytes(F.list(streams[0]["packets"])[0])) else { return XCTFail("frame") }
        let live = try XCTUnwrap(receiver.livePoints(width: 320, height: 400))
        XCTAssertEqual(live.count, 68)
        XCTAssertTrue(live.allSatisfy { $0.z != nil })
        XCTAssertGreaterThan(live[30].z ?? 0, live[0].z ?? 0)
        if case .failed(let code) = receiver.receive([0x50]) { XCTAssertEqual(code, "short") } else { XCTFail("short packet") }
        let mouth = FaceStreamView.mouthFrame(live)
        XCTAssertTrue(mouth.center.x.isFinite && mouth.y.y > 0)
    }

    func testFaceStreamLostFramesKeepTheMouthMoving() throws {
        let packets = F.list(F.obj(F.list(F.data["streams"]).first)["packets"])
        let decoder = FaceStreamDecoder()
        _ = try decoder.decode(F.bytes(packets[0]))
        let good = try decoder.decode(F.bytes(packets[1]))
        _ = try decoder.decode(F.bytes(packets[3]))
        var later = try decoder.decode(F.bytes(packets[4]))
        XCTAssertTrue(later.stale)
        later.expression.mouth = good.expression.mouth + 0.4
        let opened = FaceStreamView.conceal(later, goodMouth: good.expression.mouth)
        XCTAssertGreaterThan(opened.points[66].y - later.points[66].y, 5)
        XCTAssertEqual(opened.points[62].y, later.points[62].y)
        XCTAssertEqual(FaceStreamView.conceal(good, goodMouth: 0), good)
        let receiver = FaceStreamReceiver()
        receiver.receive(F.bytes(packets[0]))
        receiver.receive(F.bytes(packets[2]))
        XCTAssertTrue(receiver.needsKey)
        XCTAssertEqual(receiver.frame?.stale, true)
    }

    func testFaceStreamBackdropFillsTheHeadFromTheRoom() throws {
        let entry = F.obj(F.list(F.data["references"]).first)
        let reference = try FaceStreamReference.decode(F.bytes(entry["packet"]))
        let removal = FaceStreamView.removal(reference)
        let w = reference.width, h = reference.height
        XCTAssertEqual(removal.count, w * h)
        let center = CallFaceGeometry.faceFrame(reference.points).mid
        XCTAssertGreaterThan(removal[Int(center.y) * w + Int(center.x)], 0.9)
        XCTAssertEqual(removal[5 * w + 5], 0)
        var rgba = [UInt8](repeating: 0, count: w * h * 4)
        for i in 0..<(w * h) {
            let inHead = removal[i] > 0.5
            rgba[i * 4] = inHead ? 250 : 40
            rgba[i * 4 + 1] = inHead ? 10 : 120
            rgba[i * 4 + 2] = inHead ? 10 : 200
            rgba[i * 4 + 3] = 255
        }
        let filled = FaceStreamView.backdrop(rgba: rgba, width: w, height: h, removal: removal)
        let at = (Int(center.y) * w + Int(center.x)) * 4
        XCTAssertLessThan(abs(Int(filled[at]) - 40), 30)
        XCTAssertLessThan(abs(Int(filled[at + 2]) - 200), 30)
    }
}
