import CoreGraphics
import FMBase
import Foundation
import ImageIO
import Vision

enum CallFaceLandmarks {
    private static let queue = DispatchQueue(label: "com.forkmesh.call-face-landmarks", qos: .userInteractive)

    static func detect(dataURL: String) async -> JSONValue {
        await withCheckedContinuation { (continuation: CheckedContinuation<JSONValue, Never>) in
            queue.async { continuation.resume(returning: run(dataURL: dataURL)) }
        }
    }

    private static func run(dataURL: String) -> JSONValue {
        guard let comma = dataURL.firstIndex(of: ","),
            let data = Data(base64Encoded: String(dataURL[dataURL.index(after: comma)...])),
            let source = CGImageSourceCreateWithData(data as CFData, nil),
            let image = CGImageSourceCreateImageAtIndex(source, 0, nil)
        else { return .null }
        let request = VNDetectFaceLandmarksRequest()
        request.constellation = .constellation76Points
        let handler = VNImageRequestHandler(cgImage: image, orientation: .up, options: [:])
        do {
            try handler.perform([request])
        } catch {
            return .null
        }
        let faces = request.results ?? []
        guard let face = faces.max(by: { area($0.boundingBox) < area($1.boundingBox) }),
            let landmarks = face.landmarks
        else { return .null }
        let size = CGSize(width: image.width, height: image.height)
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
        var regions = JSONObject()
        for (name, region) in named {
            guard let region, region.pointCount > 0 else { continue }
            regions[name] = .array(
                region.pointsInImage(imageSize: size).map { point in
                    .array([.double(Double(point.x)), .double(Double(size.height - point.y))])
                })
        }
        return .object(regions)
    }

    private static func area(_ rect: CGRect) -> CGFloat {
        rect.width * rect.height
    }
}
