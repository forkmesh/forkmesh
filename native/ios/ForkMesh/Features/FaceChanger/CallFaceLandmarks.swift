import CoreGraphics
import CoreVideo
import FMBase
import Foundation
import ImageIO
import Vision

struct CallFaceAnalysis {
    let landmarks: Bool
    var regions: [String: [[Double]]]?
    var mask: JSONValue?

    init(landmarks: Bool) {
        self.landmarks = landmarks
    }
}

enum CallFaceLandmarks {
    private static let queue = DispatchQueue(label: "com.forkmesh.call-face-landmarks", qos: .userInteractive)
    static let maskWidth = 256

    static func detect(dataURL: String, landmarks: Bool = true, mask: Bool = false, accurate: Bool = false) async -> JSONValue {
        await withCheckedContinuation { (continuation: CheckedContinuation<JSONValue, Never>) in
            queue.async {
                continuation.resume(returning: run(dataURL: dataURL, landmarks: landmarks, mask: mask, accurate: accurate))
            }
        }
    }

    private static func run(dataURL: String, landmarks wantLandmarks: Bool, mask wantMask: Bool, accurate: Bool) -> JSONValue {
        guard let comma = dataURL.firstIndex(of: ","),
            let data = Data(base64Encoded: String(dataURL[dataURL.index(after: comma)...])),
            let source = CGImageSourceCreateWithData(data as CFData, nil),
            let image = CGImageSourceCreateImageAtIndex(source, 0, nil),
            let analysis = analyze(image: image, landmarks: wantLandmarks, mask: wantMask, accurate: accurate)
        else { return .null }
        return reply(analysis)
    }

    static func analyze(image: CGImage, landmarks wantLandmarks: Bool, mask wantMask: Bool, accurate: Bool) -> CallFaceAnalysis? {
        let faceRequest = VNDetectFaceLandmarksRequest()
        faceRequest.constellation = .constellation76Points
        let personRequest = VNGeneratePersonSegmentationRequest()
        personRequest.qualityLevel = accurate ? .accurate : .balanced
        personRequest.outputPixelFormat = kCVPixelFormatType_OneComponent8
        var requests: [VNRequest] = []
        if wantLandmarks { requests.append(faceRequest) }
        if wantMask { requests.append(personRequest) }
        guard !requests.isEmpty else { return nil }
        let size = CGSize(width: image.width, height: image.height)
        let handler = VNImageRequestHandler(cgImage: image, orientation: .up, options: [:])
        do {
            try handler.perform(requests)
        } catch {
            return nil
        }
        var analysis = CallFaceAnalysis(landmarks: wantLandmarks)
        if wantLandmarks { analysis.regions = regions(faceRequest, size: size) }
        if wantMask, let buffer = personRequest.results?.first?.pixelBuffer {
            analysis.mask = maskValue(buffer)
        }
        return analysis
    }

    static func reply(_ analysis: CallFaceAnalysis) -> JSONValue {
        var reply = JSONObject()
        reply["regions"] = analysis.landmarks ? json(analysis.regions) : .null
        if let mask = analysis.mask { reply["mask"] = mask }
        return .object(reply)
    }

    static func json(_ regions: [String: [[Double]]]?) -> JSONValue {
        guard let regions else { return .null }
        var out = JSONObject()
        for (name, points) in regions {
            out[name] = .array(points.map { point in .array(point.map { .double($0) }) })
        }
        return .object(out)
    }

    private static func regions(_ request: VNDetectFaceLandmarksRequest, size: CGSize) -> [String: [[Double]]]? {
        regions(request.results ?? [], size: size)
    }

    static func regions(_ faces: [VNFaceObservation], size: CGSize) -> [String: [[Double]]]? {
        guard let face = faces.max(by: { area($0.boundingBox) < area($1.boundingBox) }),
            let landmarks = face.landmarks
        else { return nil }
        let named: [(String, VNFaceLandmarkRegion2D?)] = [
            ("faceContour", landmarks.faceContour),
            ("leftEyebrow", landmarks.leftEyebrow),
            ("rightEyebrow", landmarks.rightEyebrow),
            ("leftEye", landmarks.leftEye),
            ("rightEye", landmarks.rightEye),
            ("nose", landmarks.nose),
            ("noseCrest", landmarks.noseCrest),
            ("medianLine", landmarks.medianLine),
            ("outerLips", landmarks.outerLips),
            ("innerLips", landmarks.innerLips),
            ("leftPupil", landmarks.leftPupil),
            ("rightPupil", landmarks.rightPupil),
        ]
        var regions: [String: [[Double]]] = [:]
        for (name, region) in named {
            guard let region, region.pointCount > 0 else { continue }
            regions[name] = region.pointsInImage(imageSize: size).map { point in
                [Double(point.x), Double(size.height - point.y)]
            }
        }
        return regions
    }

    static func maskValue(_ buffer: CVPixelBuffer) -> JSONValue {
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(buffer) else { return .null }
        let sourceWidth = CVPixelBufferGetWidth(buffer)
        let sourceHeight = CVPixelBufferGetHeight(buffer)
        let stride = CVPixelBufferGetBytesPerRow(buffer)
        let bytes = base.assumingMemoryBound(to: UInt8.self)
        let width = min(maskWidth, sourceWidth)
        let height = max(1, Int((Double(sourceHeight) * Double(width) / Double(max(1, sourceWidth))).rounded()))
        return .object([
            "width": .int(Int64(width)),
            "height": .int(Int64(height)),
            "data": .string(downscale(bytes, stride: stride, from: (sourceWidth, sourceHeight), to: (width, height)).base64EncodedString()),
        ])
    }

    static func downscale(
        _ bytes: UnsafePointer<UInt8>, stride: Int, from source: (Int, Int), to target: (Int, Int)
    ) -> Data {
        var out = Data(count: target.0 * target.1)
        out.withUnsafeMutableBytes { (raw: UnsafeMutableRawBufferPointer) in
            guard let pixels = raw.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
            for y in 0..<target.1 {
                let y0 = y * source.1 / target.1
                let y1 = max(y0 + 1, (y + 1) * source.1 / target.1)
                for x in 0..<target.0 {
                    let x0 = x * source.0 / target.0
                    let x1 = max(x0 + 1, (x + 1) * source.0 / target.0)
                    var sum = 0
                    for row in y0..<y1 {
                        for column in x0..<x1 { sum += Int(bytes[row * stride + column]) }
                    }
                    pixels[y * target.0 + x] = UInt8(sum / ((y1 - y0) * (x1 - x0)))
                }
            }
        }
        return out
    }

    private static func area(_ rect: CGRect) -> CGFloat {
        rect.width * rect.height
    }
}
