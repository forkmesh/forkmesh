// The single ForkMesh Ads slot (`mobile` placement). First-party, human-reviewed, text-only: every ad
// string is plain Text, never markup or a remote image; the slot is labelled
// "Sponsored"; no ad, a failed request, a
// switched-off preference or a "Hide" still in force all render nothing.
// Logic: FMApi's SponsoredAdModel.

import FMAccount
import FMApi
import FMBase
import SwiftUI

@MainActor
struct SponsoredAdCard: View {
    @State var model: SponsoredAdModel?

    /// Applied only when an ad is shown, so a collapsed slot leaves no gap.
    private let margin: EdgeInsets?

    init(margin: EdgeInsets? = nil) {
        self.margin = margin
        if let api = Services.get(ApiService.self) {
            _model = State(initialValue: SponsoredAdModel(settings: Services.get(SettingsService.self), api: api))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        if let model = model {
            SponsoredAdSlot(model: model, margin: margin)
                .task(id: model.allowed) {
                    await model.load()
                }
        }
    }
}

private struct SponsoredAdSlot: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.openURL) var openURL
    let model: SponsoredAdModel
    let margin: EdgeInsets?

    var body: some View {
        if let ad = model.visibleAd {
            FmCard(radius: FmRadius.lg, margin: margin) {
                SponsoredAdContent(
                    ad: ad,
                    onFollow: {
                        if let url = model.follow(ad) {
                            openURL(url)
                        }
                    },
                    onHide: { model.hide() })
            }
            .accessibilityElement(children: .contain)
            .accessibilityLabel(Text("Sponsored message"))
            .accessibilityIdentifier("sponsored-ad-card")
        }
    }
}

private struct SponsoredAdContent: View {
    @Environment(\.fmPalette) var fm
    let ad: SponsoredAd
    let onFollow: () -> Void
    let onHide: () -> Void

    private var host: String {
        return ad.uri?.host ?? ""
    }

    private var caption: some View {
        HStack(spacing: 0) {
            Text("Sponsored").fmText(.labelSmall, color: fm.textTertiary)
            if !ad.sponsor.isEmpty {
                Text(verbatim: "  ·  ").fmText(.labelSmall, color: fm.textTertiary)
                Text(verbatim: ad.sponsor)
                    .fmText(.labelSmall, color: fm.textTertiary)
                    .lineLimit(1)
                    .truncationMode(.tail)
            }
        }
    }

    private var actions: some View {
        HStack(spacing: FmSpace.x1) {
            FmToneButton(
                label: ad.cta, tone: .accent, tooltip: "Opens \(host) in your browser", onPressed: onFollow)
                .accessibilityIdentifier("sponsored-ad-cta")
            FmTextButton("Hide", color: fm.textTertiary, minHeight: 32, onPressed: onHide)
                .accessibilityIdentifier("sponsored-ad-hide")
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            caption
            Text(verbatim: ad.headline)
                .fmText(.titleSmall, weight: FmType.emphasis)
                .lineLimit(2)
                .truncationMode(.tail)
                .padding(.top, FmSpace.x2)
            if !ad.body.isEmpty {
                Text(verbatim: ad.body)
                    .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                    .lineLimit(4)
                    .truncationMode(.tail)
                    .padding(.top, FmSpace.x1)
            }
            // The host is shown because the CTA leaves the app: the user sees
            // where the button goes before tapping it.
            Text(verbatim: host)
                .fmText(.labelSmall, color: fm.textTertiary)
                .lineLimit(1)
                .padding(.top, FmSpace.x2)
            actions
                .padding(.top, FmSpace.x3)
        }
    }
}
