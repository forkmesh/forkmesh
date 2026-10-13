import FMApi
import FMSurfaces
import ImageIO
import SwiftUI
import UIKit
import UniformTypeIdentifiers

final class ShareViewController: UIViewController {
    private let model = ShareModel()

    override func viewDidLoad() {
        super.viewDidLoad()
        model.finish = { [weak self] (route: Route?) in
            self?.done(open: route)
        }
        let host = UIHostingController(rootView: ShareComposeView(model: model))
        addChild(host)
        host.view.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(host.view)
        NSLayoutConstraint.activate([
            host.view.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            host.view.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            host.view.topAnchor.constraint(equalTo: view.topAnchor),
            host.view.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])
        host.didMove(toParent: self)
        let items = (extensionContext?.inputItems as? [NSExtensionItem]) ?? []
        Task { await model.load(items) }
    }

    private func done(open route: Route?) {
        guard let route, let url = SurfaceLink.url(for: route), let context = extensionContext else {
            extensionContext?.completeRequest(returningItems: nil, completionHandler: nil)
            return
        }
        if openContainingApp(url) {
            context.completeRequest(returningItems: nil, completionHandler: nil)
            return
        }
        context.open(url) { [weak self] (opened: Bool) in
            DispatchQueue.main.async {
                if opened {
                    self?.extensionContext?.completeRequest(returningItems: nil, completionHandler: nil)
                } else {
                    self?.model.waitingForApp = true
                }
            }
        }
    }

    private func openContainingApp(_ url: URL) -> Bool {
        typealias OpenURL = @convention(c) (
            AnyObject, Selector, NSURL, NSDictionary, (@convention(block) (Bool) -> Void)?
        ) -> Void
        let selector = NSSelectorFromString("openURL:options:completionHandler:")
        var responder: UIResponder? = self
        while let current = responder {
            if let application = current as? UIApplication, application.responds(to: selector) {
                let open = unsafeBitCast(application.method(for: selector), to: OpenURL.self)
                open(application, selector, url as NSURL, NSDictionary(), nil)
                return true
            }
            responder = current.next
        }
        return false
    }
}

@MainActor
final class ShareModel: ObservableObject {
    @Published var text: String = ""
    @Published var urls: [String] = []
    @Published var images: [Data] = []
    @Published var loading = true
    @Published var error: String?
    @Published var saved = false
    @Published var waitingForApp = false
    var finish: ((Route?) -> Void)?

    static let maxImageBytes = ShareHandoffStore.maxImageBytes
    nonisolated static let maxPixels = 2_048

    func load(_ items: [NSExtensionItem]) async {
        var texts: [String] = []
        var links: [String] = []
        var pictures: [Data] = []
        for item in items {
            for provider in item.attachments ?? [] {
                if provider.hasItemConformingToTypeIdentifier(UTType.image.identifier) {
                    if pictures.count < ShareHandoffStore.maxImages,
                       let data = await ShareModel.imageData(provider), data.count <= ShareModel.maxImageBytes,
                       let small = ShareModel.shrink(data), small.count <= ShareDrop.maxImageBytes {
                        pictures.append(small)
                    }
                } else if provider.hasItemConformingToTypeIdentifier(UTType.url.identifier) {
                    if let url = await ShareModel.url(provider) {
                        if url.isFileURL {
                            continue
                        }
                        links.append(url.absoluteString)
                    }
                } else if provider.hasItemConformingToTypeIdentifier(UTType.plainText.identifier) {
                    if let value = await ShareModel.text(provider) {
                        texts.append(value)
                    }
                }
            }
        }
        text = texts.joined(separator: "\n\n")
        urls = ShareHandoffStore.cleanURLs(links)
        images = pictures
        loading = false
        if !images.isEmpty {
            send()
        }
    }

    var isEmpty: Bool {
        text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && urls.isEmpty && images.isEmpty
    }

    func send() {
        do {
            let handoff = try ShareKeychainDrop().save(text: text, urls: urls, images: images)
            saved = true
            finish?(handoff.route)
        } catch let failure as ShareHandoffError {
            error = failure.message
        } catch {
            self.error = "Could not save what you shared."
        }
    }

    func cancel() {
        finish?(nil)
    }

    func close() {
        finish?(nil)
    }

    private static func load(_ provider: NSItemProvider, _ type: UTType) async -> NSSecureCoding? {
        await withCheckedContinuation { (continuation: CheckedContinuation<NSSecureCoding?, Never>) in
            provider.loadItem(forTypeIdentifier: type.identifier, options: nil) { item, _ in
                continuation.resume(returning: item)
            }
        }
    }

    private static func imageData(_ provider: NSItemProvider) async -> Data? {
        await withCheckedContinuation { (continuation: CheckedContinuation<Data?, Never>) in
            provider.loadDataRepresentation(forTypeIdentifier: UTType.image.identifier) { data, _ in
                continuation.resume(returning: data)
            }
        }
    }

    nonisolated static func shrink(_ data: Data) -> Data? {
        let sourceOptions: CFDictionary = [kCGImageSourceShouldCache: false] as CFDictionary
        guard let source = CGImageSourceCreateWithData(data as CFData, sourceOptions),
              CGImageSourceGetCount(source) > 0
        else { return nil }
        let thumbnailOptions: CFDictionary = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceThumbnailMaxPixelSize: maxPixels,
        ] as CFDictionary
        guard let image = CGImageSourceCreateThumbnailAtIndex(source, 0, thumbnailOptions) else { return nil }
        let opaque: Bool = [.none, .noneSkipFirst, .noneSkipLast].contains(image.alphaInfo)
        let type: UTType = opaque ? .jpeg : .png
        let out = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(out as CFMutableData, type.identifier as CFString, 1, nil)
        else { return nil }
        let encodeOptions: CFDictionary = [kCGImageDestinationLossyCompressionQuality: 0.9] as CFDictionary
        CGImageDestinationAddImage(destination, image, opaque ? encodeOptions : nil)
        return CGImageDestinationFinalize(destination) ? out as Data : nil
    }

    private static func url(_ provider: NSItemProvider) async -> URL? {
        let item = await load(provider, .url)
        if let url = item as? URL { return url }
        if let text = item as? String { return URL(string: text) }
        if let data = item as? Data { return URL(dataRepresentation: data, relativeTo: nil) }
        return nil
    }

    private static func text(_ provider: NSItemProvider) async -> String? {
        let item = await load(provider, .plainText)
        if let text = item as? String { return text }
        if let data = item as? Data { return String(data: data, encoding: .utf8) }
        return nil
    }
}

struct ShareComposeView: View {
    @ObservedObject var model: ShareModel

    var body: some View {
        NavigationStack {
            Form {
                if model.loading {
                    Section {
                        ProgressView()
                    }
                } else if model.waitingForApp {
                    Section {
                        Label("Saved for ForkMesh", systemImage: "checkmark.circle.fill")
                            .foregroundStyle(.green)
                        Text("Open ForkMesh and the composer will have it ready.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                } else {
                    Section("Instructions for the agent") {
                        TextField("What should the agent do?", text: $model.text, axis: .vertical)
                            .lineLimit(3...10)
                    }
                    if !model.urls.isEmpty {
                        Section("Links") {
                            ForEach(model.urls, id: \.self) { link in
                                Text(link)
                                    .font(.footnote)
                                    .lineLimit(2)
                            }
                        }
                    }
                    if !model.images.isEmpty {
                        Section("Images") {
                            ScrollView(.horizontal, showsIndicators: false) {
                                HStack(spacing: 8) {
                                    ForEach(Array(model.images.enumerated()), id: \.offset) { item in
                                        SharedImageThumb(data: item.element)
                                    }
                                }
                            }
                        }
                    }
                    if let error = model.error {
                        Section {
                            Label(error, systemImage: "exclamationmark.triangle")
                                .foregroundStyle(.red)
                        }
                    }
                    Section {
                        Text("ForkMesh opens the composer with this attached. Pick the repository and model there.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .navigationTitle("Send to agent")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { model.cancel() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    if model.waitingForApp {
                        Button("Done") { model.close() }
                    } else {
                        Button("Send") { model.send() }
                            .disabled(model.loading || model.isEmpty || model.saved)
                    }
                }
            }
        }
    }
}

struct SharedImageThumb: View {
    let data: Data

    var body: some View {
        Group {
            if let image = UIImage(data: data) {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
            } else {
                Image(systemName: "photo")
                    .foregroundStyle(.secondary)
            }
        }
        .frame(width: 64, height: 64)
        .clipShape(RoundedRectangle(cornerRadius: 8))
    }
}
