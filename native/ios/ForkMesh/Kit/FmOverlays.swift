// The patterns every screen shares: the inline notice line, the themed
// AlertDialog with the FmDialog layout, and the themed modal bottom sheet.
// Follows native/android's ui/kit/FmOverlays.kt, but with SwiftUI's own
// presentation state (a binding per overlay) in place of a messenger object.

import SwiftUI

struct FmNotice: Identifiable, Equatable {
    let id: UUID
    let message: String
    let isError: Bool
    let actionLabel: String?
    let onAction: (() -> Void)?

    init(_ message: String, isError: Bool = false, actionLabel: String? = nil, onAction: (() -> Void)? = nil) {
        self.id = UUID()
        self.message = message
        self.isError = isError
        self.actionLabel = actionLabel
        self.onAction = onAction
    }

    static func error(_ message: String) -> FmNotice {
        return FmNotice(message, isError: true)
    }

    static func == (lhs: FmNotice, rhs: FmNotice) -> Bool {
        return lhs.id == rhs.id
    }
}

struct FmNoticeLine: View {
    @Environment(\.fmPalette) private var fm

    private let message: String?
    private let isError: Bool
    private let actionLabel: String?
    private let onAction: (() -> Void)?
    private let onDismiss: (() -> Void)?

    init(_ message: String?, isError: Bool = false, actionLabel: String? = nil, onAction: (() -> Void)? = nil,
         onDismiss: (() -> Void)? = nil) {
        self.message = message
        self.isError = isError
        self.actionLabel = actionLabel
        self.onAction = onAction
        self.onDismiss = onDismiss
    }

    init(_ notice: Binding<FmNotice?>) {
        let current: FmNotice? = notice.wrappedValue
        self.message = current?.message
        self.isError = current?.isError ?? false
        self.actionLabel = current?.actionLabel
        self.onAction = current?.onAction.map { (action: @escaping () -> Void) -> () -> Void in
            return {
                action()
                notice.wrappedValue = nil
            }
        }
        self.onDismiss = { notice.wrappedValue = nil }
    }

    var body: some View {
        if let message = message, !message.isEmpty {
            HStack(alignment: .center, spacing: FmSpace.x2) {
                FmIcon(isError ? "error_outline" : "info_outline", size: 16, color: isError ? fm.danger : fm.textTertiary)
                Text(message)
                    .fmText(.bodySmall, color: isError ? fm.danger : fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fixedSize(horizontal: false, vertical: true)
                    .textSelection(.enabled)
                FmMessageTools(message)
                if let actionLabel = actionLabel, let onAction = onAction {
                    FmTextButton(actionLabel, minHeight: 32, onPressed: onAction)
                }
                if let onDismiss = onDismiss {
                    FmIconButton(label: "Dismiss", onPressed: onDismiss) {
                        FmIcon("close", size: 14, color: fm.textTertiary)
                    }
                    .frame(width: 28, height: 28)
                }
            }
            .padding(.vertical, FmSpace.x1)
            .accessibilityElement(children: .contain)
            .accessibilityIdentifier("inline-notice")
        }
    }
}

/// The themed AlertDialog: the overlay layer in dark and
/// the raised one in light, 14pt corners, a real shadow, the heading title,
/// secondary relaxed body text. `content` holds the fields, if any, and
/// `actions` the footer, normally an FmDialogActions.
struct FmDialog<Content: View, Actions: View>: View {
    @Environment(\.fmPalette) private var fm

    private let title: String
    private let message: String?
    private let content: Content
    private let actions: Actions

    init(
        title: String,
        message: String? = nil,
        @ViewBuilder content: () -> Content,
        @ViewBuilder actions: () -> Actions
    ) {
        self.title = title
        self.message = message
        self.content = content()
        self.actions = actions()
    }

    private var copy: some View {
        VStack(alignment: .leading, spacing: FmSpace.x4) {
            Text(title)
                .fmText(.titleMedium)
                .lineLimit(2)
                .truncationMode(.tail)
            if let message = message {
                Text(message)
                    .fmText(.bodyMedium, color: fm.textSecondary, height: FmType.leadingRelaxed)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(message)
            }
            content
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, FmSpace.x5)
        .padding(.top, FmSpace.x5)
        .padding(.bottom, FmSpace.x5)
    }

    var body: some View {
        VStack(spacing: 0) {
            copy
            actions
                .padding(.horizontal, FmSpace.x4)
                .padding(.bottom, FmSpace.x4)
        }
        .frame(maxWidth: 560)
        .background(fm.sheetSurface)
        .clipShape(FmRoundedShape(radius: 14))
        .shadow(color: Color.black.opacity(fm.dark ? 0.6 : 0.2), radius: fm.dark ? 24 : 12, x: 0, y: 8)
        .accessibilityAddTraits(.isModal)
    }
}

/// The confirm dialog with FmDialogActions, or a lone confirm when
/// `hideCancel`.
struct FmConfirmDialog: View {
    private let title: String
    private let message: String?
    private let confirmLabel: String
    private let destructive: Bool
    private let hideCancel: Bool
    private let onCancel: () -> Void
    private let onConfirm: () -> Void

    init(
        title: String,
        message: String? = nil,
        confirmLabel: String = "OK",
        destructive: Bool = false,
        hideCancel: Bool = false,
        onCancel: @escaping () -> Void,
        onConfirm: @escaping () -> Void
    ) {
        self.title = title
        self.message = message
        self.confirmLabel = confirmLabel
        self.destructive = destructive
        self.hideCancel = hideCancel
        self.onCancel = onCancel
        self.onConfirm = onConfirm
    }

    @ViewBuilder private var footer: some View {
        if hideCancel {
            HStack(spacing: 0) {
                Spacer(minLength: 0)
                FmTextButton(confirmLabel, onPressed: onConfirm)
            }
        } else {
            FmDialogActions(
                onCancel: onCancel,
                onConfirm: onConfirm,
                confirmLabel: confirmLabel,
                destructive: destructive
            )
        }
    }

    var body: some View {
        FmDialog(title: title, message: message, content: { EmptyView() }, actions: { footer })
    }
}

/// FmDialog with a single autofocused text field, as the "New file" and
/// branch prompts use it. Confirm is disabled while the trimmed text is empty.
struct FmPromptDialog: View {
    @Binding private var text: String

    private let title: String
    private let message: String?
    private let hint: String
    private let confirmLabel: String
    private let onCancel: () -> Void
    private let onConfirm: () -> Void

    init(
        title: String,
        message: String? = nil,
        text: Binding<String>,
        hint: String = "",
        confirmLabel: String = "Create",
        onCancel: @escaping () -> Void,
        onConfirm: @escaping () -> Void
    ) {
        self.title = title
        self.message = message
        self._text = text
        self.hint = hint
        self.confirmLabel = confirmLabel
        self.onCancel = onCancel
        self.onConfirm = onConfirm
    }

    private var ready: Bool {
        return !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    private func submit() {
        if ready {
            onConfirm()
        }
    }

    var body: some View {
        FmDialog(
            title: title,
            message: message,
            content: {
                FmTextField(text: $text, hint: hint, autofocus: true, onSubmit: submit)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled(true)
            },
            actions: {
                FmDialogActions(
                    onCancel: onCancel,
                    onConfirm: ready ? onConfirm : nil,
                    confirmLabel: confirmLabel
                )
            }
        )
    }
}

/// Presents a dialog over the view it is applied to, centred on Material's
/// black54 barrier. Apply it to the screen's root so the barrier covers the
/// whole screen. Tapping the barrier dismisses unless `barrierDismissible` is
/// false.
struct FmDialogPresenter<DialogContent: View>: ViewModifier {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Binding var isPresented: Bool

    let barrierDismissible: Bool
    let dialog: () -> DialogContent

    @ViewBuilder private var layer: some View {
        if isPresented {
            ZStack {
                Color.black.opacity(0.54)
                    .ignoresSafeArea()
                    .onTapGesture {
                        if barrierDismissible {
                            isPresented = false
                        }
                    }
                    .accessibilityHidden(true)
                dialog()
                    .padding(FmSpace.x5)
            }
            .transition(AnyTransition.opacity)
        }
    }

    func body(content: Content) -> some View {
        content
            .overlay { layer }
            .animation(FmMotion.easeOutCubic(FmMotion.normal, reduceMotion: reduceMotion), value: isPresented)
    }
}

private struct FmSheetHeightKey: PreferenceKey {
    static let defaultValue: CGFloat = 0

    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) {
        value = max(value, nextValue())
    }
}

/// A sheet under `bottomSheetTheme`, applied to the sheet's content: the raised
/// layer in light and the overlay layer in dark, 12pt top corners, no drag
/// handle. With `fitsContent` the sheet is as tall as its content, which is how
/// showModalBottomSheet opens; otherwise it offers the medium and large detents.
struct FmSheetStyleModifier: ViewModifier {
    @Environment(\.fmPalette) private var fm
    @State private var measured: CGFloat = 0

    let fitsContent: Bool
    let background: Color?

    private var detents: Set<PresentationDetent> {
        if fitsContent && measured > 0 {
            return [PresentationDetent.height(measured)]
        }
        if fitsContent {
            return [PresentationDetent.medium]
        }
        return [PresentationDetent.medium, PresentationDetent.large]
    }

    private func measuring(_ content: Content) -> some View {
        content
            .fixedSize(horizontal: false, vertical: true)
            .background {
                GeometryReader { (proxy: GeometryProxy) in
                    Color.clear.preference(key: FmSheetHeightKey.self, value: proxy.size.height)
                }
            }
            .onPreferenceChange(FmSheetHeightKey.self) { (height: CGFloat) in
                measured = height
            }
    }

    /// Only a fitted sheet is pinned to its natural height; a detent sheet
    /// leaves its content free to fill and scroll.
    @ViewBuilder private func sized(_ content: Content) -> some View {
        if fitsContent {
            measuring(content)
        } else {
            content
        }
    }

    func body(content: Content) -> some View {
        sized(content)
            .presentationDetents(detents)
            .presentationDragIndicator(Visibility.hidden)
            .presentationCornerRadius(FmRadius.lg)
            .fmSheetBackground(background ?? fm.sheetSurface)
    }
}

struct FmSheetBackground: ViewModifier {
    let color: Color

    func body(content: Content) -> some View {
        if #available(iOS 26.0, *) {
            content
        } else {
            content.presentationBackground(color)
        }
    }
}

struct FmPreGlassBackground: ViewModifier {
    let color: Color

    func body(content: Content) -> some View {
        if #available(iOS 26.0, *) {
            content
        } else {
            content.background(color)
        }
    }
}

extension View {
    /// Presents `dialog` while `isPresented` is true.
    func fmDialog<DialogContent: View>(
        isPresented: Binding<Bool>,
        barrierDismissible: Bool = true,
        @ViewBuilder dialog: @escaping () -> DialogContent
    ) -> some View {
        return modifier(
            FmDialogPresenter(isPresented: isPresented, barrierDismissible: barrierDismissible, dialog: dialog)
        )
    }

    /// Apply to the content of a `.sheet` to give it the bottom sheet theme.
    func fmSheetStyle(fitsContent: Bool = true, background: Color? = nil) -> some View {
        return modifier(FmSheetStyleModifier(fitsContent: fitsContent, background: background))
    }

    func fmSheetBackground(_ color: Color) -> some View {
        modifier(FmSheetBackground(color: color))
    }

    func fmPreGlassBackground(_ color: Color) -> some View {
        modifier(FmPreGlassBackground(color: color))
    }
}
