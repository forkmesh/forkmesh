import FMCode
import SwiftUI
import UIKit
import WebKit

struct DiffImagePreview: View {
    @Environment(\.fmPalette) private var fm

    let model: DiffReviewModel
    let file: Int

    private static let height: CGFloat = 220

    var body: some View {
        let requests: [DiffImageRequest] = model.imageRequests(file)
        HStack(alignment: .top, spacing: FmSpace.x2) {
            ForEach(requests, id: \.self) { (request: DiffImageRequest) in
                side(request, labelled: requests.count > 1)
            }
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x2)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(fm.bgOverlay)
        .task(id: file) {
            await model.loadImages(file)
        }
    }

    private func side(_ request: DiffImageRequest, labelled: Bool) -> some View {
        let result: DiffImageResult = model.imageResults[request] ?? .loading
        let raster: UIImage? = DiffImagePreview.raster(result, path: request.path)
        return VStack(alignment: .leading, spacing: FmSpace.x1) {
            if labelled {
                Text(request.side.label)
                    .fmText(.labelSmall, color: request.side == .old ? fm.danger : fm.success, weight: FmType.emphasis)
            }
            ZStack {
                DiffImageCheckerboard(dark: fm.dark)
                content(result, raster: raster, path: request.path)
            }
            .frame(maxWidth: .infinity)
            .frame(height: DiffImagePreview.height)
            .clipShape(RoundedRectangle(cornerRadius: FmRadius.md))
            .overlay(RoundedRectangle(cornerRadius: FmRadius.md).stroke(fm.border, lineWidth: 1))
            Text(caption(result, raster: raster))
                .fmText(.bodySmall, color: fm.textTertiary)
                .lineLimit(1)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityElement(children: .combine)
        .accessibilityLabel(Text("\(request.side.label) image \(request.path)"))
    }

    private static func raster(_ result: DiffImageResult, path: String) -> UIImage? {
        guard case .loaded(let data) = result, DiffImageKind.of(path) == .raster else { return nil }
        return UIImage(data: data)
    }

    @ViewBuilder private func content(_ result: DiffImageResult, raster: UIImage?, path: String) -> some View {
        switch result {
        case .loading:
            ProgressView()
        case .failed(let message):
            VStack(spacing: FmSpace.x1) {
                Text(message)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .multilineTextAlignment(.center)
                FmMessageTools("\(message) (\(path))")
            }
            .padding(FmSpace.x2)
        case .loaded(let data):
            if DiffImageKind.of(path) == .svg {
                DiffSvgView(data: data)
            } else if let image = raster {
                Image(uiImage: image)
                    .resizable()
                    .interpolation(image.size.width < 64 ? .none : .high)
                    .scaledToFit()
                    .padding(FmSpace.x2)
            } else {
                VStack(spacing: FmSpace.x1) {
                    Text(DiffImages.unreadable)
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .multilineTextAlignment(.center)
                    FmMessageTools("\(DiffImages.unreadable) (\(path))")
                }
                .padding(FmSpace.x2)
            }
        }
    }

    private func caption(_ result: DiffImageResult, raster: UIImage?) -> String {
        guard case .loaded(let data) = result else { return " " }
        let size: String = ByteCountFormatter.string(fromByteCount: Int64(data.count), countStyle: .file)
        if let image = raster {
            let scale: CGFloat = image.scale
            return "\(Int(image.size.width * scale)) × \(Int(image.size.height * scale)) · \(size)"
        }
        return size
    }
}

private struct DiffImageCheckerboard: View {
    let dark: Bool

    var body: some View {
        Canvas { (context: inout GraphicsContext, size: CGSize) in
            let cell: CGFloat = 8
            let light: Color = dark ? Color(white: 0.16) : Color(white: 0.97)
            let shade: Color = dark ? Color(white: 0.21) : Color(white: 0.9)
            context.fill(Path(CGRect(origin: .zero, size: size)), with: .color(light))
            var path = Path()
            var row = 0
            var y: CGFloat = 0
            while y < size.height {
                var x: CGFloat = row % 2 == 0 ? 0 : cell
                while x < size.width {
                    path.addRect(CGRect(x: x, y: y, width: cell, height: cell))
                    x += cell * 2
                }
                y += cell
                row += 1
            }
            context.fill(path, with: .color(shade))
        }
    }
}

private struct DiffSvgView: UIViewRepresentable {
    let data: Data

    func makeUIView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.websiteDataStore = .nonPersistent()
        config.defaultWebpagePreferences.allowsContentJavaScript = false
        config.preferences.javaScriptCanOpenWindowsAutomatically = false
        let view = WKWebView(frame: .zero, configuration: config)
        view.isOpaque = false
        view.backgroundColor = .clear
        view.scrollView.backgroundColor = .clear
        view.scrollView.isScrollEnabled = false
        view.scrollView.bounces = false
        view.isUserInteractionEnabled = false
        return view
    }

    func updateUIView(_ view: WKWebView, context: Context) {
        guard context.coordinator.shown != data else { return }
        context.coordinator.shown = data
        view.loadHTMLString(DiffSvgView.page(data), baseURL: nil)
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    static func page(_ data: Data) -> String {
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            + "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src data:; style-src 'unsafe-inline'\">"
            + "<style>html,body{margin:0;height:100%;background:transparent}"
            + "body{display:flex;align-items:center;justify-content:center}"
            + "img{max-width:calc(100% - 16px);max-height:calc(100% - 16px);width:100%;height:100%;object-fit:contain}</style>"
            + "</head><body><img alt=\"\" src=\"data:image/svg+xml;base64,\(data.base64EncodedString())\"></body></html>"
    }

    final class Coordinator {
        var shown: Data?
    }
}
