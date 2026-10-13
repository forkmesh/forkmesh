import FMApi
import FMBase
import FMChat
import FMControl
import Photos
import SwiftUI
import UIKit

enum AgentAttachmentsSupport {
    @MainActor
    static func loader(hub explicitHub: DesktopControlHub? = nil) -> AgentAttachmentLoader {
        let api: ApiService? = Services.get(ApiService.self)
        let control: LocalControlService? = Services.get(LocalControlService.self)
        let signedOut: @Sendable (AgentAttachmentRef) async throws -> Data = { @Sendable _ in
            throw AgentAttachmentError("Sign in to see this photo.")
        }
        let noDesktop: @Sendable (AgentAttachmentRef) async throws -> Data = { @Sendable _ in
            throw AgentAttachmentError("Connect to the desktop on this network to open this attachment.")
        }
        let relay: @Sendable (AgentAttachmentRef) async throws -> Data = api?.agentAttachmentRelayFetch() ?? signedOut
        if let hub: DesktopControlHub = explicitHub ?? Services.get(DesktopControlHub.self) {
            return hub.attachmentLoader(store: .shared, relay: relay)
        }
        let desktop: @Sendable (AgentAttachmentRef) async throws -> Data = control?.attachmentFetch() ?? noDesktop
        return AgentAttachmentLoader(store: .shared, relay: relay, desktop: desktop)
    }
}

private final class AgentThumbnailCache: @unchecked Sendable {
    static let shared = AgentThumbnailCache()
    private let cache: NSCache<NSString, UIImage> = {
        let cache = NSCache<NSString, UIImage>()
        cache.totalCostLimit = 24 * 1024 * 1024
        return cache
    }()

    func image(_ key: String) -> UIImage? { cache.object(forKey: key as NSString) }

    func put(_ key: String, _ image: UIImage) {
        let cost = Int(image.size.width * image.size.height * image.scale * image.scale * 4)
        cache.setObject(image, forKey: key as NSString, cost: cost)
    }
}

struct AgentAttachmentStrip: View {
    let refs: [AgentAttachmentRef]
    let loader: AgentAttachmentLoader
    var trailing: Bool = false
    @State private var viewing: AgentAttachmentViewerItem?

    private var pictures: [AgentAttachmentRef] { refs.filter(\.showsThumbnail) }
    private var files: [AgentAttachmentRef] { refs.filter { !$0.showsThumbnail } }

    var body: some View {
        VStack(alignment: trailing ? .trailing : .leading, spacing: FmSpace.x2) {
            if !pictures.isEmpty {
                ScrollView(.horizontal, showsIndicators: false) {
                    LazyHStack(spacing: FmSpace.x2) {
                        ForEach(pictures) { (ref: AgentAttachmentRef) in
                            Button {
                                viewing = AgentAttachmentViewerItem(refs: pictures, start: ref.id)
                            } label: {
                                AgentAttachmentThumbnail(ref: ref, loader: loader)
                            }
                            .buttonStyle(.plain)
                            .accessibilityLabel(Text("Photo \(ref.name). Opens full screen."))
                        }
                    }
                }
                .frame(height: AgentAttachmentThumbnail.side)
                .defaultScrollAnchor(trailing ? .trailing : .leading)
            }
            ForEach(files) { (ref: AgentAttachmentRef) in
                AgentAttachmentFileRow(ref: ref, loader: loader)
            }
        }
        .fullScreenCover(item: $viewing) { (item: AgentAttachmentViewerItem) in
            AgentAttachmentViewer(refs: item.refs, start: item.start, loader: loader)
        }
    }
}

struct AgentAttachmentViewerItem: Identifiable {
    let id = UUID()
    let refs: [AgentAttachmentRef]
    let start: String
}

struct AgentAttachmentThumbnail: View {
    static let side: CGFloat = 72
    @Environment(\.fmPalette) private var fm
    let ref: AgentAttachmentRef
    let loader: AgentAttachmentLoader
    var size: CGFloat = AgentAttachmentThumbnail.side
    @State private var image: UIImage?
    @State private var failed: Bool = false

    private var radius: CGFloat { min(FmRadius.lg, size / 4) }

    var body: some View {
        ZStack {
            fm.bgOverlay
            if let image = image {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
            } else if failed {
                Image(systemName: "photo.badge.exclamationmark")
                    .foregroundStyle(fm.textTertiary)
            } else {
                ProgressView()
                    .controlSize(size < 40 ? .mini : .small)
            }
        }
        .frame(width: size, height: size)
        .clipShape(RoundedRectangle(cornerRadius: radius, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: radius, style: .continuous)
                .stroke(fm.border, lineWidth: 0.5)
        )
        .task(id: ref.cacheKey) { await load() }
    }

    private func load() async {
        let key: String = ref.cacheKey
        if let cached = AgentThumbnailCache.shared.image(key) {
            image = cached
            return
        }
        do {
            let data: Data = try await loader.load(ref)
            let side: CGFloat = Self.side * 3
            let decoded: UIImage? = await Task.detached(priority: .utility) {
                UIImage(data: data)?.preparingThumbnail(of: CGSize(width: side, height: side))
            }.value
            if Task.isCancelled { return }
            if let decoded = decoded {
                AgentThumbnailCache.shared.put(key, decoded)
                image = decoded
            } else {
                failed = true
            }
        } catch {
            if !Task.isCancelled { failed = true }
        }
    }
}

struct AgentAttachmentFileRow: View {
    @Environment(\.fmPalette) private var fm
    let ref: AgentAttachmentRef
    let loader: AgentAttachmentLoader
    @State private var sharing: AgentShareItem?
    @State private var problem: String?
    @State private var busy: Bool = false

    private var icon: String {
        if ref.isImage { return "photo" }
        if ref.isVideo { return "film" }
        if ref.mediaType.hasPrefix("text/") || ref.mediaType == "application/json" { return "doc.text" }
        return "doc"
    }

    var body: some View {
        HStack(spacing: FmSpace.x3) {
            Image(systemName: icon)
                .foregroundStyle(fm.textSecondary)
                .frame(width: 24)
            VStack(alignment: .leading, spacing: 2) {
                Text(ref.name)
                    .fmText(.bodyMedium)
                    .lineLimit(1)
                    .truncationMode(.middle)
                let detail: String = problem ?? ref.rowDetail
                if !detail.isEmpty {
                    Text(detail)
                        .fmText(.bodySmall, color: problem == nil ? fm.textTertiary : fm.danger)
                        .lineLimit(2)
                        .fmMessageTools(problem)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            if ref.canFetch {
                Button {
                    Task { await share() }
                } label: {
                    if busy {
                        ProgressView().controlSize(.small)
                    } else {
                        Image(systemName: "square.and.arrow.up")
                            .foregroundStyle(fm.accent)
                    }
                }
                .buttonStyle(.plain)
                .frame(minWidth: 44, minHeight: 44)
                .disabled(busy)
                .accessibilityLabel(Text("Share \(ref.name)"))
            }
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x2)
        .background(fm.bgRaised)
        .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
        .accessibilityElement(children: .combine)
        .sheet(item: $sharing) { (item: AgentShareItem) in
            AgentActivityView(items: item.items) { item.cleanup() }
        }
    }

    private func share() async {
        busy = true
        defer { busy = false }
        do {
            let data: Data = try await loader.load(ref)
            sharing = try AgentShareItem.file(data, name: ref.name)
            problem = nil
        } catch let error as AgentAttachmentError {
            problem = error.message
        } catch {
            problem = "Could not open \(ref.name)."
        }
    }
}

struct AgentShareItem: Identifiable {
    let id = UUID()
    let items: [Any]
    let cleanup: () -> Void

    static func image(_ image: UIImage) -> AgentShareItem {
        AgentShareItem(items: [image], cleanup: {})
    }

    static func file(_ data: Data, name: String) throws -> AgentShareItem {
        let folder: URL = FileManager.default.temporaryDirectory
            .appendingPathComponent("agent-share-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let safe: String = AgentAttachmentMedia.displayName(name, fallback: "attachment")
        let url: URL = folder.appendingPathComponent(safe)
        try data.write(to: url, options: [.atomic, .completeFileProtection])
        return AgentShareItem(items: [url], cleanup: { try? FileManager.default.removeItem(at: folder) })
    }
}

struct AgentActivityView: UIViewControllerRepresentable {
    let items: [Any]
    var onFinish: (String?, Bool) -> Void = { _, _ in }
    let onDone: () -> Void

    func makeUIViewController(context: Context) -> UIActivityViewController {
        let controller = UIActivityViewController(activityItems: items, applicationActivities: nil)
        controller.completionWithItemsHandler = { (activity: UIActivity.ActivityType?, completed: Bool, _: [Any]?, _: Error?) in
            onFinish(activity?.rawValue, completed)
            onDone()
        }
        return controller
    }

    func updateUIViewController(_ controller: UIActivityViewController, context: Context) {}
}

struct AgentAttachmentViewer: View {
    @Environment(\.dismiss) private var dismiss
    let refs: [AgentAttachmentRef]
    let loader: AgentAttachmentLoader
    @State private var current: String
    @State private var images: [String: UIImage] = [:]
    @State private var previews: [String: AgentAttachmentPayload] = [:]
    @State private var problems: [String: String] = [:]
    @State private var sharing: AgentShareItem?
    @State private var saveStatus: String?
    @State private var saveFailed: Bool = false

    init(refs: [AgentAttachmentRef], start: String, loader: AgentAttachmentLoader) {
        self.refs = refs
        self.loader = loader
        _current = State(initialValue: start)
    }

    private var currentRef: AgentAttachmentRef? { refs.first { $0.id == current } }

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            TabView(selection: $current) {
                ForEach(refs) { (ref: AgentAttachmentRef) in
                    page(ref)
                        .tag(ref.id)
                }
            }
            .tabViewStyle(.page(indexDisplayMode: refs.count > 1 ? .automatic : .never))
            .ignoresSafeArea()
            VStack {
                topBar
                if let saveStatus = saveStatus {
                    Text(saveStatus)
                        .font(.footnote)
                        .foregroundStyle(saveFailed ? Color.red : Color.white.opacity(0.9))
                        .frame(maxWidth: .infinity, alignment: .trailing)
                        .fmMessageTools(saveStatus, color: Color.white.opacity(0.9))
                        .padding(.horizontal, 16)
                        .accessibilityIdentifier("inline-notice")
                }
                if let preview: AgentAttachmentPayload = previews[current] {
                    AgentAttachmentPreviewNote(payload: preview)
                        .padding(.horizontal, 16)
                }
                Spacer()
            }
        }
        .statusBarHidden()
        .onChange(of: current) { (_: String, _: String) in
            saveStatus = nil
        }
        .sheet(item: $sharing) { (item: AgentShareItem) in
            AgentActivityView(items: item.items) { item.cleanup() }
        }
    }

    private var topBar: some View {
        HStack(spacing: 8) {
            barButton("xmark", label: "Close") { dismiss() }
            Spacer()
            Text(currentRef?.name ?? "")
                .font(.footnote.weight(.semibold))
                .foregroundStyle(Color.white.opacity(0.9))
                .lineLimit(1)
                .truncationMode(.middle)
            Spacer()
            barButton("square.and.arrow.up", label: "Share") { shareCurrent() }
                .disabled(images[current] == nil)
            barButton("square.and.arrow.down", label: "Save to Photos") {
                Task { await saveCurrent() }
            }
            .disabled(images[current] == nil)
        }
        .padding(.horizontal, 12)
        .padding(.top, 8)
    }

    private func barButton(_ symbol: String, label: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.body.weight(.semibold))
                .foregroundStyle(Color.white)
                .frame(width: 44, height: 44)
                .background(Circle().fill(Color.white.opacity(0.14)))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(label))
    }

    @ViewBuilder private func page(_ ref: AgentAttachmentRef) -> some View {
        Group {
            if let image = images[ref.id] {
                AgentZoomableImage(image: image)
            } else if let problem = problems[ref.id] {
                VStack(spacing: 12) {
                    Image(systemName: "photo.badge.exclamationmark")
                        .font(.largeTitle)
                    Text(problem)
                        .font(.callout)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 32)
                    FmMessageTools(problem, color: Color.white.opacity(0.8))
                }
                .foregroundStyle(Color.white.opacity(0.8))
            } else {
                ProgressView().tint(Color.white)
            }
        }
        .task(id: ref.cacheKey) { await load(ref) }
    }

    private func load(_ ref: AgentAttachmentRef) async {
        if images[ref.id] != nil { return }
        do {
            let payload: AgentAttachmentPayload = try await loader.loadPayload(ref)
            let data: Data = payload.data
            let decoded: UIImage? = await Task.detached(priority: .userInitiated) {
                UIImage(data: data)?.preparingForDisplay()
            }.value
            if let decoded = decoded {
                if payload.preview {
                    previews[ref.id] = payload.withoutData
                } else {
                    previews.removeValue(forKey: ref.id)
                }
                images[ref.id] = decoded
            } else {
                problems[ref.id] = "This picture could not be shown."
            }
        } catch let error as AgentAttachmentError {
            problems[ref.id] = error.message
        } catch {
            problems[ref.id] = "This picture could not be loaded."
        }
    }

    private func shareCurrent() {
        guard let image = images[current] else { return }
        sharing = AgentShareItem.image(image)
    }

    private func saveCurrent() async {
        guard let image = images[current] else { return }
        let status: PHAuthorizationStatus = await PHPhotoLibrary.requestAuthorization(for: .addOnly)
        guard status == .authorized || status == .limited else {
            show("Allow ForkMesh to add photos in Settings to save this.", failed: true)
            return
        }
        do {
            try await PHPhotoLibrary.shared().performChanges {
                _ = PHAssetChangeRequest.creationRequestForAsset(from: image)
            }
            FmHaptics.commit()
            show("Saved to Photos.")
        } catch {
            show("Could not save the photo.", failed: true)
        }
    }

    private func show(_ message: String, failed: Bool = false) {
        saveFailed = failed
        saveStatus = message
    }
}

struct AgentAttachmentPreviewNote: View {
    let payload: AgentAttachmentPayload

    var body: some View {
        let detail: String = payload.previewDetail
        VStack(spacing: 2) {
            Text(payload.previewNote)
                .font(.footnote.weight(.semibold))
            if !detail.isEmpty {
                Text(detail)
                    .font(.caption)
                    .opacity(0.8)
            }
        }
        .foregroundStyle(Color.white)
        .multilineTextAlignment(.center)
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(Color.black.opacity(0.55)))
        .accessibilityElement(children: .combine)
    }
}

struct AgentZoomableImage: UIViewRepresentable {
    let image: UIImage

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIView(context: Context) -> UIScrollView {
        let scroll = UIScrollView()
        scroll.minimumZoomScale = 1
        scroll.maximumZoomScale = 6
        scroll.bouncesZoom = true
        scroll.showsHorizontalScrollIndicator = false
        scroll.showsVerticalScrollIndicator = false
        scroll.contentInsetAdjustmentBehavior = .never
        scroll.delegate = context.coordinator
        let imageView = UIImageView(image: image)
        imageView.contentMode = .scaleAspectFit
        imageView.isUserInteractionEnabled = true
        imageView.accessibilityIgnoresInvertColors = true
        imageView.translatesAutoresizingMaskIntoConstraints = false
        scroll.addSubview(imageView)
        NSLayoutConstraint.activate([
            imageView.leadingAnchor.constraint(equalTo: scroll.contentLayoutGuide.leadingAnchor),
            imageView.trailingAnchor.constraint(equalTo: scroll.contentLayoutGuide.trailingAnchor),
            imageView.topAnchor.constraint(equalTo: scroll.contentLayoutGuide.topAnchor),
            imageView.bottomAnchor.constraint(equalTo: scroll.contentLayoutGuide.bottomAnchor),
            imageView.widthAnchor.constraint(equalTo: scroll.frameLayoutGuide.widthAnchor),
            imageView.heightAnchor.constraint(equalTo: scroll.frameLayoutGuide.heightAnchor),
        ])
        let doubleTap = UITapGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.doubleTapped(_:)))
        doubleTap.numberOfTapsRequired = 2
        scroll.addGestureRecognizer(doubleTap)
        context.coordinator.imageView = imageView
        context.coordinator.scrollView = scroll
        return scroll
    }

    func updateUIView(_ scroll: UIScrollView, context: Context) {
        if context.coordinator.imageView?.image !== image {
            context.coordinator.imageView?.image = image
            scroll.setZoomScale(1, animated: false)
        }
    }

    final class Coordinator: NSObject, UIScrollViewDelegate {
        weak var imageView: UIImageView?
        weak var scrollView: UIScrollView?

        func viewForZooming(in scrollView: UIScrollView) -> UIView? { imageView }

        @objc func doubleTapped(_ gesture: UITapGestureRecognizer) {
            guard let scroll = scrollView else { return }
            if scroll.zoomScale > 1 {
                scroll.setZoomScale(1, animated: true)
                return
            }
            let point: CGPoint = gesture.location(in: imageView)
            let scale: CGFloat = min(3, scroll.maximumZoomScale)
            let size = CGSize(width: scroll.bounds.width / scale, height: scroll.bounds.height / scale)
            scroll.zoom(to: CGRect(x: point.x - size.width / 2, y: point.y - size.height / 2, width: size.width, height: size.height), animated: true)
        }
    }
}
