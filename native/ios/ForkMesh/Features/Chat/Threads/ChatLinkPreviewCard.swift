import FMBase
import FMChat
import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

struct ChatLinkPreviews: View {
    let text: String
    let autoLoad: Bool
    @State private var service: ChatLinkPreviewService? = Services.get(ChatLinkPreviewService.self)

    var body: some View {
        if let service, service.enabled {
            let urls: [URL] = ChatLinkPreviewParsing.candidates(ChatLinkPreviewParsing.links(in: text))
            if !urls.isEmpty {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    ForEach(urls, id: \.absoluteString) { (url: URL) in
                        ChatLinkPreviewCard(url: url, service: service, autoLoad: autoLoad)
                    }
                }
            }
        }
    }
}

struct ChatLinkPreviewCard: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL
    let url: URL
    let service: ChatLinkPreviewService
    let autoLoad: Bool

    var body: some View {
        content
            .task(id: url.absoluteString) { if autoLoad { await service.load(url) } }
    }

    @ViewBuilder private var content: some View {
        switch service.state(for: url) {
        case .ready(let preview):
            card(preview)
                .padding(.top, FmSpace.x2)
                .transition(.opacity)
        case .idle where !autoLoad:
            loadButton
                .padding(.top, FmSpace.x2)
        case .loading where !autoLoad:
            Text("Loading preview\u{2026}")
                .font(.system(size: 14))
                .foregroundStyle(fm.textSecondary)
                .padding(.top, FmSpace.x2)
        default:
            Color.clear.frame(height: 0)
        }
    }

    private var loadButton: some View {
        let shape: RoundedRectangle = RoundedRectangle(cornerRadius: 14, style: .continuous)
        return Button(action: { Task { await service.load(url) } }) {
            HStack(spacing: 7) {
                ChatLinkSiteBadge(site: ChatLinkSites.site(for: url))
                Text("Show preview of \(ChatLinkSites.bareHost(url.host ?? ""))")
                    .font(.system(size: 14))
                    .foregroundStyle(fm.accent)
                    .lineLimit(1)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .frame(maxWidth: 520, alignment: .leading)
            .background(fm.bgRaised.opacity(fm.dark ? 0.55 : 0.85))
            .clipShape(shape)
            .overlay(shape.stroke(fm.border, lineWidth: 1))
        }
        .buttonStyle(.plain)
        .accessibilityHint("Asks the site for a preview. The site sees this phone's address.")
        .accessibilityIdentifier("chat-link-preview-load")
    }

    private func card(_ preview: ChatLinkPreview) -> some View {
        let shape: RoundedRectangle = RoundedRectangle(cornerRadius: 14, style: .continuous)
        return VStack(alignment: .leading, spacing: 0) {
            if preview.kind != .image || preview.imageUrl.isEmpty || !preview.title.isEmpty {
                VStack(alignment: .leading, spacing: 5) {
                    if !preview.title.isEmpty {
                        Text(preview.title)
                            .font(.system(size: 16, weight: .semibold))
                            .foregroundStyle(fm.textPrimary)
                            .lineLimit(3)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    if !preview.author.isEmpty {
                        Text(preview.author)
                            .font(.system(size: 15))
                            .foregroundStyle(fm.textSecondary)
                            .lineLimit(1)
                    }
                    if !preview.detail.isEmpty && (preview.author.isEmpty || preview.imageUrl.isEmpty) && !preview.isVideo {
                        Text(preview.detail)
                            .font(.system(size: 14))
                            .foregroundStyle(fm.textSecondary)
                            .lineLimit(3)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    HStack(spacing: 7) {
                        ChatLinkSiteBadge(site: preview.site)
                        Text(preview.siteName)
                            .font(.system(size: 14))
                            .foregroundStyle(fm.textSecondary)
                            .lineLimit(1)
                    }
                    .padding(.top, 2)
                }
                .padding(.horizontal, 14)
                .padding(.vertical, 12)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            if !preview.imageUrl.isEmpty {
                ChatLinkPreviewImage(source: preview.imageUrl, video: preview.isVideo, service: service)
            }
        }
        .frame(maxWidth: 520, alignment: .leading)
        .background(fm.bgRaised.opacity(fm.dark ? 0.55 : 0.85))
        .clipShape(shape)
        .overlay(shape.stroke(fm.border, lineWidth: 1))
        .contentShape(shape)
        .onTapGesture { openURL(url) }
        .contextMenu {
            Button(action: { openURL(url) }) { Label("Open link", systemImage: "safari") }
            Button(action: {
                #if canImport(UIKit)
                UIPasteboard.general.string = url.absoluteString
                #endif
            }) { Label("Copy link", systemImage: "doc.on.doc") }
            ShareLink(item: url) { Label("Share link", systemImage: "square.and.arrow.up") }
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel("\(preview.siteName): \(preview.title)")
        .accessibilityHint("Opens the link")
        .accessibilityAddTraits(.isLink)
    }
}

struct ChatLinkSiteBadge: View {
    let site: ChatLinkSite?

    var body: some View {
        if let site {
            Image(systemName: site.symbol)
                .font(.system(size: 9, weight: .bold))
                .foregroundStyle(Color.white)
                .frame(width: 24, height: 17)
                .background(RoundedRectangle(cornerRadius: 5, style: .continuous).fill(Color(fmARGB: 0xFF00_0000 | site.color)))
                .overlay(RoundedRectangle(cornerRadius: 5, style: .continuous).stroke(Color.white.opacity(0.18), lineWidth: 0.5))
                .accessibilityHidden(true)
        } else {
            Image(systemName: "globe")
                .font(.system(size: 14))
                .foregroundStyle(Color.secondary)
                .frame(width: 24, height: 17)
                .accessibilityHidden(true)
        }
    }
}

@MainActor
enum ChatLinkImageCache {
    static let images: NSCache<NSString, UIImage> = {
        let cache: NSCache<NSString, UIImage> = NSCache<NSString, UIImage>()
        cache.countLimit = 120
        return cache
    }()
}

struct ChatLinkPreviewImage: View {
    @Environment(\.fmPalette) private var fm
    let source: String
    let video: Bool
    let service: ChatLinkPreviewService
    @State private var image: UIImage?
    @State private var failed: Bool = false

    var body: some View {
        Group {
            if failed {
                Color.clear.frame(height: 0)
            } else if let image {
                if video {
                    Color.clear
                        .aspectRatio(16.0 / 9.0, contentMode: .fit)
                        .overlay {
                            Image(uiImage: image).resizable().scaledToFill()
                        }
                        .clipped()
                        .overlay {
                            Image(systemName: "play.fill")
                                .font(.system(size: 22, weight: .bold))
                                .foregroundStyle(Color.white)
                                .frame(width: 58, height: 58)
                                .background(Circle().fill(Color.black.opacity(0.55)))
                        }
                } else {
                    Image(uiImage: image)
                        .resizable()
                        .scaledToFit()
                        .frame(maxWidth: .infinity, maxHeight: 300)
                        .background(fm.bgOverlay)
                }
            } else {
                Rectangle()
                    .fill(fm.bgOverlay)
                    .aspectRatio(video ? 16.0 / 9.0 : 1.91, contentMode: .fit)
            }
        }
        .task(id: source) { await load() }
    }

    private func load() async {
        if let cached = ChatLinkImageCache.images.object(forKey: source as NSString) {
            image = cached
            return
        }
        guard let data = await service.imageData(source) else {
            failed = true
            return
        }
        let decoded: UIImage? = await Task.detached(priority: .userInitiated) { () -> UIImage? in
            guard let full = UIImage(data: data) else { return nil }
            return full.preparingThumbnail(of: ChatLinkPreviewImage.fit(full.size, longest: 1_200)) ?? full
        }.value
        guard let decoded else {
            failed = true
            return
        }
        ChatLinkImageCache.images.setObject(decoded, forKey: source as NSString)
        withAnimation(.easeOut(duration: 0.2)) { image = decoded }
    }

    nonisolated static func fit(_ size: CGSize, longest: CGFloat) -> CGSize {
        let side: CGFloat = max(size.width, size.height)
        guard side > longest, side > 0 else { return size }
        let scale: CGFloat = longest / side
        return CGSize(width: (size.width * scale).rounded(), height: (size.height * scale).rounded())
    }
}
