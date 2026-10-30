import Foundation

public struct CallFaceModel: Equatable, Sendable {
    public let id: String
    public let label: String
    public let points: [Double]
    public let head: [Double]

    public init(id: String, label: String, points: [Double], head: [Double]) {
        self.id = id
        self.label = label
        self.points = points
        self.head = head
    }
}

public struct CallFaceHead: Equatable, Sendable {
    public let layout: [CallFacePoint]
    public let model: [CallFacePoint]
    public let mesh: CallFaceHeadMesh
}

public struct CallFaceChoice: Equatable, Sendable {
    public let id: String
    public let label: String
    public let custom: Bool
}

public struct CallFaceLocated: Equatable, Sendable {
    public let points: [CallFacePoint]?
    public let pupils: [CallFacePoint]?
}

public enum CallFaces {
    public static let storageKey = "forkmesh.chat.callFace"
    public static let photoBase = "/assets/call-faces/"
    public static let photoSize = 512.0
    public static let frameRate = 24.0
    public static let maxSide = 960.0
    public static let holdMs = 500.0
    public static let fadeIn = 0.3
    public static let fadeOut = 0.2

    public static let all: [CallFaceModel] = [
        CallFaceModel(
            id: "maya",
            label: "Maya",
            points: [
                154.1, 292.1, 153.5, 321.6, 154.1, 352.3, 156.4, 380.1, 162.8, 406.7,
                173.2, 429.8, 191.7, 449.5, 221.3, 464.6, 256.0, 471.5, 293.1, 465.7,
                326.6, 451.3, 350.4, 431.0, 364.8, 406.7, 372.9, 380.1, 377.0, 352.3,
                379.9, 321.6, 380.5, 292.1, 172.1, 257.9, 183.6, 247.5, 198.7, 244.0,
                215.5, 245.2, 230.5, 248.6, 283.2, 249.2, 298.3, 244.0, 316.8, 242.3,
                337.1, 247.5, 350.4, 257.9, 257.7, 282.8, 258.3, 303.1, 259.5, 323.3,
                257.7, 341.8, 234.0, 352.8, 244.4, 358.6, 257.7, 360.4, 271.1, 358.6,
                283.2, 352.8, 176.7, 284.5, 190.6, 278.2, 210.3, 278.2, 227.6, 288.0,
                212.0, 291.5, 191.7, 290.3, 284.4, 288.0, 300.0, 276.4, 318.5, 276.4,
                337.1, 282.8, 320.3, 290.3, 300.0, 291.5, 200.4, 387.6, 218.9, 378.9,
                239.2, 376.6, 256.0, 378.3, 274.5, 376.6, 298.3, 378.9, 323.2, 385.8,
                306.4, 407.8, 283.2, 422.9, 256.0, 426.4, 230.5, 422.9, 212.0, 407.8,
                205.6, 389.3, 228.8, 385.8, 256.0, 386.4, 284.4, 385.8, 318.5, 387.6,
                284.4, 415.4, 256.0, 417.1, 228.8, 415.4,
            ],
            head: [
                255.8, 67.7, 297.9, 61.9, 337.0, 78.1, 372.9, 99.7, 405.2, 127.5,
                433.4, 161.1, 460.6, 199.9, 485.7, 246.7, 502.9, 303.1, 503.3, 365.9,
                484.9, 429.4, 450.0, 490.2, 386.0, 512.0, 321.1, 512.0, 255.8, 512.0,
                193.6, 512.0, 136.4, 494.6, 88.3, 456.6, 60.5, 402.4, 47.3, 347.1,
                48.5, 294.0, 62.0, 246.7, 77.6, 206.0, 99.8, 171.6, 124.5, 142.0,
                152.3, 116.9, 182.5, 94.5, 217.2, 77.9,
            ]
        ),
        CallFaceModel(
            id: "theo",
            label: "Theo",
            points: [
                131.5, 292.1, 131.5, 321.6, 133.8, 352.3, 137.3, 380.1, 143.1, 406.7,
                153.5, 429.8, 173.2, 447.8, 209.1, 458.8, 255.4, 462.3, 301.7, 458.8,
                331.8, 447.8, 343.4, 429.8, 355.6, 406.7, 363.1, 380.1, 365.4, 352.3,
                367.2, 321.6, 367.7, 292.1, 158.2, 263.7, 178.4, 255.0, 198.7, 251.5,
                218.9, 251.5, 237.5, 256.2, 281.5, 255.0, 300.0, 251.5, 320.3, 251.5,
                339.9, 255.0, 353.8, 263.7, 257.7, 282.8, 258.3, 304.2, 259.5, 325.1,
                260.1, 345.3, 232.3, 354.6, 245.0, 358.6, 259.5, 360.4, 274.5, 358.6,
                287.8, 354.6, 177.8, 283.9, 191.7, 277.0, 211.4, 278.2, 229.9, 287.4,
                212.0, 289.7, 191.7, 289.2, 285.5, 287.4, 300.0, 278.2, 319.1, 278.2,
                337.1, 283.9, 320.3, 289.2, 300.0, 289.7, 205.6, 390.5, 221.3, 387.0,
                240.9, 383.5, 256.0, 384.7, 271.1, 383.5, 290.7, 385.8, 308.1, 389.9,
                294.2, 397.4, 280.9, 404.4, 256.0, 407.8, 231.1, 404.4, 218.9, 397.4,
                209.1, 391.1, 229.9, 393.4, 256.0, 395.1, 282.6, 393.4, 304.6, 390.5,
                282.6, 393.9, 256.0, 395.7, 229.9, 393.9,
            ],
            head: [
                256.4, 43.0, 300.6, 52.9, 339.7, 73.6, 370.5, 103.5, 393.1, 137.6,
                406.9, 174.1, 416.7, 210.0, 422.2, 246.6, 424.9, 285.0, 424.0, 327.3,
                420.7, 377.6, 401.2, 428.1, 365.6, 473.5, 318.3, 512.0, 256.4, 512.0,
                193.9, 512.0, 133.8, 501.0, 94.9, 449.1, 66.4, 398.1, 53.7, 344.2,
                54.4, 292.7, 56.4, 246.6, 63.3, 202.5, 77.6, 160.5, 99.1, 121.1,
                129.3, 87.2, 167.3, 61.7, 210.5, 45.3,
            ]
        ),
        CallFaceModel(
            id: "kofi",
            label: "Kofi",
            points: [
                122.8, 292.1, 122.8, 321.6, 124.6, 352.3, 128.6, 380.1, 136.2, 406.7,
                148.3, 429.8, 166.8, 447.8, 205.6, 460.5, 256.0, 464.6, 304.6, 460.5,
                339.9, 447.8, 352.7, 429.8, 361.4, 406.7, 366.6, 380.1, 368.9, 352.3,
                369.5, 321.6, 370.6, 292.1, 163.4, 257.9, 180.2, 250.4, 198.7, 245.7,
                217.2, 246.9, 235.7, 250.9, 281.5, 250.9, 300.0, 245.7, 320.3, 245.7,
                339.9, 250.9, 357.3, 259.6, 257.7, 282.8, 258.3, 303.1, 259.5, 323.3,
                258.3, 343.6, 235.7, 355.2, 246.2, 359.8, 259.5, 361.5, 274.5, 358.6,
                286.1, 353.4, 176.7, 282.2, 191.7, 275.3, 210.3, 276.4, 230.5, 287.4,
                212.0, 288.6, 191.7, 288.6, 284.4, 287.4, 300.0, 276.4, 318.5, 275.3,
                337.1, 282.2, 320.3, 288.6, 300.0, 289.7, 203.9, 390.5, 225.9, 384.1,
                240.9, 381.2, 256.0, 382.9, 272.8, 381.2, 291.3, 384.1, 309.8, 389.3,
                296.5, 399.2, 281.5, 407.8, 256.0, 410.7, 232.3, 407.8, 215.5, 400.9,
                206.8, 391.6, 229.9, 393.9, 256.0, 395.7, 282.6, 393.9, 306.4, 389.9,
                282.6, 395.1, 256.0, 396.8, 229.9, 395.1,
            ],
            head: [
                256.1, 48.2, 298.6, 59.0, 336.5, 78.4, 367.1, 106.2, 388.6, 139.8,
                403.7, 174.3, 411.9, 209.9, 416.3, 245.4, 419.1, 282.6, 428.9, 328.6,
                426.1, 381.0, 406.9, 434.5, 376.0, 494.5, 319.2, 512.0, 256.1, 512.0,
                191.0, 512.0, 125.8, 512.0, 86.9, 457.6, 57.5, 403.8, 45.8, 346.7,
                48.4, 292.8, 48.9, 245.4, 56.0, 199.8, 72.1, 156.8, 93.1, 115.5,
                126.2, 82.5, 167.1, 60.5, 211.0, 47.7,
            ]
        ),
        CallFaceModel(
            id: "ingrid",
            label: "Ingrid",
            points: [
                150.6, 292.1, 151.8, 321.6, 154.1, 352.3, 159.3, 380.1, 168.6, 404.9,
                181.9, 425.2, 200.4, 441.4, 225.9, 453.0, 256.6, 458.2, 289.0, 453.0,
                313.9, 441.4, 333.6, 425.2, 348.1, 404.9, 358.5, 380.1, 364.8, 352.3,
                367.2, 321.6, 367.7, 292.1, 165.1, 263.7, 180.2, 257.9, 196.9, 255.0,
                215.5, 255.0, 232.3, 257.9, 283.2, 257.9, 300.0, 255.0, 318.5, 255.0,
                337.1, 257.9, 350.4, 263.7, 257.7, 282.8, 258.3, 303.1, 259.5, 323.3,
                259.5, 344.2, 234.0, 355.2, 246.2, 359.8, 259.5, 361.5, 274.5, 359.8,
                286.1, 355.2, 174.9, 283.9, 190.6, 276.4, 210.3, 277.0, 228.8, 286.3,
                210.3, 291.5, 191.7, 291.5, 284.4, 286.3, 301.7, 276.4, 320.3, 276.4,
                337.1, 282.8, 320.3, 291.5, 301.7, 292.1, 205.6, 390.5, 222.4, 385.8,
                240.9, 381.2, 256.0, 382.9, 272.8, 381.2, 293.1, 384.7, 308.1, 388.2,
                298.3, 399.7, 281.5, 410.7, 256.0, 415.4, 232.3, 410.7, 217.2, 401.5,
                209.1, 390.5, 228.8, 393.4, 256.0, 395.1, 283.2, 393.4, 304.6, 389.3,
                283.2, 393.9, 256.0, 395.7, 228.8, 393.9,
            ],
            head: [
                256.0, 48.1, 300.5, 51.1, 342.0, 67.4, 378.1, 92.8, 406.4, 126.0,
                427.9, 163.1, 443.3, 203.1, 452.2, 245.9, 461.1, 292.7, 461.6, 344.9,
                446.9, 398.1, 417.3, 448.2, 373.9, 490.8, 317.1, 512.0, 256.0, 512.0,
                194.0, 512.0, 145.2, 476.0, 106.4, 433.5, 82.6, 384.2, 76.7, 332.2,
                76.5, 286.9, 78.8, 245.9, 89.1, 207.8, 103.4, 172.4, 119.4, 136.9,
                142.9, 104.1, 174.4, 76.5, 212.9, 57.2,
            ]
        ),
        CallFaceModel(
            id: "ren",
            label: "Ren",
            points: [
                150.6, 292.1, 148.3, 321.6, 148.3, 352.3, 150.6, 380.1, 156.4, 406.7,
                166.8, 429.8, 185.4, 451.3, 213.2, 469.8, 250.8, 486.0, 289.0, 475.0,
                313.9, 458.2, 330.7, 436.8, 341.7, 413.6, 349.2, 386.4, 352.7, 358.6,
                354.4, 329.1, 355.6, 296.7, 166.8, 259.6, 178.4, 251.5, 196.9, 247.5,
                217.2, 247.5, 234.0, 252.7, 283.2, 250.9, 298.3, 245.7, 316.8, 245.2,
                333.6, 247.5, 341.7, 254.4, 259.5, 282.8, 261.2, 303.1, 262.9, 323.3,
                265.8, 345.3, 240.9, 355.2, 250.8, 359.8, 264.7, 362.1, 278.0, 359.8,
                289.6, 353.4, 173.2, 286.3, 188.8, 278.2, 209.1, 278.2, 228.8, 288.0,
                209.1, 292.1, 190.6, 292.1, 284.4, 286.3, 298.3, 276.4, 316.8, 275.3,
                333.6, 281.1, 318.5, 289.7, 300.0, 291.5, 198.7, 400.9, 215.5, 390.5,
                235.7, 387.6, 256.0, 388.2, 274.5, 386.4, 293.1, 389.3, 302.9, 392.2,
                296.5, 419.4, 279.7, 436.2, 256.0, 441.4, 232.3, 436.2, 212.0, 422.9,
                203.9, 401.5, 225.9, 398.6, 256.0, 399.2, 283.2, 397.4, 300.0, 393.9,
                281.5, 422.9, 256.0, 426.4, 228.8, 422.9,
            ],
            head: [
                254.3, 33.2, 298.7, 52.0, 334.5, 79.9, 359.6, 114.4, 375.4, 149.9,
                387.2, 182.5, 397.2, 213.9, 406.2, 246.5, 418.8, 284.1, 431.5, 331.9,
                429.4, 386.2, 408.9, 440.4, 375.1, 497.5, 316.4, 512.0, 254.3, 512.0,
                189.2, 512.0, 124.5, 512.0, 60.0, 490.1, 24.1, 430.1, 3.5, 367.3,
                0.2, 304.5, 9.3, 246.5, 25.4, 194.3, 46.2, 146.3, 71.7, 100.9,
                109.3, 64.7, 154.8, 40.1, 204.4, 28.0,
            ]
        ),
    ]

    public static let template: [CallFacePoint] = CallFaceGeometry.buildTemplate(all.map(facePoints))

    static let builtInHeads: [String: CallFaceHead] = Dictionary(uniqueKeysWithValues: all.map { ($0.id, buildHead($0)) })

    public static func facePoints(_ face: CallFaceModel) -> [CallFacePoint] {
        CallFaceGeometry.canonicalShape(CallFaceGeometry.pointsFromFlat(face.points))
    }

    public static func head(_ face: CallFaceModel) -> CallFaceHead {
        if let cached = builtInHeads[face.id], all.contains(face) { return cached }
        return buildHead(face)
    }

    static func buildHead(_ face: CallFaceModel) -> CallFaceHead {
        let points = facePoints(face)
        let ring = CallFaceGeometry.pointsFromFlat(face.head)
        let model = CallFaceGeometry.headModel(points, ring)
        return CallFaceHead(
            layout: CallFaceGeometry.headLayout(points, ring),
            model: model,
            mesh: CallFaceGeometry.buildHeadMesh(model, template)
        )
    }

    public static func byId(_ id: String) -> CallFaceModel? {
        all.first { $0.id == id }
    }

    public static func isCustom(_ id: String) -> Bool {
        id == CallFaceCustom.id
    }

    public static func choices() -> [CallFaceChoice] {
        all.map { CallFaceChoice(id: $0.id, label: $0.label, custom: false) } + [CallFaceChoice(id: CallFaceCustom.id, label: CallFaceCustom.label, custom: true)]
    }

    public static func photoPath(_ face: CallFaceModel) -> String {
        "\(photoBase)\(face.id).jpg"
    }

    public static func mattePath(_ face: CallFaceModel) -> String {
        "\(photoBase)\(face.id)-matte.png"
    }

    public static func pupils(_ regions: CallFaceRegions?, scale: Double = 1) -> [CallFacePoint]? {
        func pick(_ name: String) -> CallFacePoint? {
            guard let first = regions?[name]?.first, first.count >= 2 else { return nil }
            return CallFacePoint(first[0] / scale, first[1] / scale)
        }
        guard let left = pick("leftPupil"), let right = pick("rightPupil") else { return nil }
        return CallFaceJS.sorted([left, right]) { $0.x - $1.x }
    }

    public static func located(_ regions: CallFaceRegions?, scale: Double = 1, landmarks: Bool = true) -> CallFaceLocated {
        let points = landmarks ? CallFaceGeometry.landmarksFromRegions(regions) : nil
        return CallFaceLocated(
            points: points?.map { CallFacePoint($0.values.map { $0 / scale }) },
            pupils: points == nil ? nil : pupils(regions, scale: scale)
        )
    }

    public static func outputSize(width: Double, height: Double, limit: Double = maxSide) -> (Int, Int) {
        let w = CallFaceJS.or(width, 640)
        let h = CallFaceJS.or(height, 480)
        let scale = CallFaceJS.min(1, limit / CallFaceJS.max(w, h))
        return (Int(CallFaceJS.max(2, CallFaceJS.round(w * scale / 2) * 2)), Int(CallFaceJS.max(2, CallFaceJS.round(h * scale / 2) * 2)))
    }
}
