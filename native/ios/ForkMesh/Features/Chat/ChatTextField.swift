import FMAgents
import FMChat
import SwiftUI
import UIKit

struct ChatTextField: UIViewRepresentable {
    @Binding var text: String
    let placeholder: String
    @Binding var focused: Bool
    var maxLines: Int = 5
    var fontSize: CGFloat? = nil
    var textColor: UIColor = .label
    var placeholderColor: UIColor = .placeholderText
    var tint: UIColor? = nil
    var onPasteImages: (([PastedImage]) -> Void)? = nil

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeUIView(context: Context) -> ComposerUITextView {
        let view: ComposerUITextView = ComposerUITextView()
        view.delegate = context.coordinator
        view.backgroundColor = .clear
        view.font = font
        view.adjustsFontForContentSizeCategory = true
        view.textContainerInset = UIEdgeInsets(top: 6, left: 0, bottom: 6, right: 0)
        view.textContainer.lineFragmentPadding = 0
        view.autocapitalizationType = .sentences
        view.isScrollEnabled = true
        view.alwaysBounceVertical = false
        view.showsVerticalScrollIndicator = false
        view.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        return view
    }

    func updateUIView(_ view: ComposerUITextView, context: Context) {
        context.coordinator.parent = self
        view.onPasteImages = onPasteImages
        if view.text != text {
            view.text = text
            view.invalidateIntrinsicContentSize()
        }
        view.textColor = textColor
        view.tintColor = tint
        view.placeholder = placeholder
        view.placeholderColor = placeholderColor
        view.refreshPlaceholder()
        if focused != view.isFirstResponder {
            let coordinator: Coordinator = context.coordinator
            DispatchQueue.main.async {
                let wanted: Bool = coordinator.parent.focused
                if wanted && !view.isFirstResponder && view.window != nil {
                    view.becomeFirstResponder()
                } else if !wanted && view.isFirstResponder {
                    view.resignFirstResponder()
                }
            }
        }
    }

    func sizeThatFits(_ proposal: ProposedViewSize, uiView: ComposerUITextView, context: Context) -> CGSize? {
        let width: CGFloat = proposal.width ?? 280
        let fitted: CGSize = uiView.sizeThatFits(CGSize(width: width, height: .greatestFiniteMagnitude))
        let line: CGFloat = uiView.font?.lineHeight ?? 20
        let insets: CGFloat = uiView.textContainerInset.top + uiView.textContainerInset.bottom
        let maxHeight: CGFloat = line * CGFloat(max(1, maxLines)) + insets
        return CGSize(width: width, height: min(maxHeight, max(line + insets, fitted.height)))
    }

    private var font: UIFont {
        guard let fontSize else { return UIFont.preferredFont(forTextStyle: .body) }
        return UIFontMetrics(forTextStyle: .body).scaledFont(for: UIFont.systemFont(ofSize: fontSize))
    }

    final class Coordinator: NSObject, UITextViewDelegate {
        var parent: ChatTextField

        init(_ parent: ChatTextField) { self.parent = parent }

        func textViewDidChange(_ textView: UITextView) {
            parent.text = textView.text
            (textView as? ComposerUITextView)?.refreshPlaceholder()
            textView.invalidateIntrinsicContentSize()
        }

        func textViewDidBeginEditing(_ textView: UITextView) {
            if !parent.focused { parent.focused = true }
        }

        func textViewDidEndEditing(_ textView: UITextView) {
            if parent.focused { parent.focused = false }
        }
    }
}

enum ChatPastedImages {
    @MainActor
    static func send(_ images: [PastedImage], hub: ChatHub) async {
        for image in images {
            guard let jpeg = MessagesImages.jpeg(image.data, maxSide: 1_600, maxBytes: ChatConversationLayout.maxMediaBytes) else {
                hub.post("That image could not be prepared.")
                continue
            }
            _ = await hub.sendMedia(fileName: MessagesImages.photoName(), mime: "image/jpeg", data: jpeg)
        }
    }
}
