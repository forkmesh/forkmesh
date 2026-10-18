// The one prompt input the app uses. A floating, frosted card holding a growing
// text field, attached photos, and one row of round controls: "+" (photos), the
// model chip, and a primary button that sends, or stops the run in progress. A
// prompt that starts with "/" offers the composer's commands above it.
//
// Each surface opts into the parts it can honour: one whose destination
// cannot receive photos passes no `attachments` binding and "+" is not drawn
// at all, rather than accepting photos it would drop.
//
// The field is a UITextView (ComposerTextView) so a surface can raise the
// keyboard synchronously from a tap (`ComposerFocus.focus()`), with no
// SwiftUI focus round trip in between: the shell's prompt button relies on it.

import FMAccount
import FMAgents
import FMApi
import FMBase
import PhotosUI
import SwiftUI
import UIKit
import UniformTypeIdentifiers

/// A command offered when a prompt starts with "/".
struct PromptSlashCommand: Identifiable {
    /// Without the leading slash, e.g. `model`.
    let name: String
    let description: String
    let run: () -> Void

    var id: String { name }
}

/// One entry of the model chip's menu.
struct PromptModelOption: Identifiable, Equatable {
    let id: String
    let label: String
    let note: String
}

struct PromptModeOption: Identifiable, Equatable {
    let id: String
    let label: String
    let note: String
    let systemImage: String
}

/// An extra entry in the composer's "+" menu.
struct PromptAttachAction {
    let label: String
    let systemImage: String
    let run: () -> Void
}

/// The field's first responder, reachable from outside the view so the
/// keyboard can come up in the same run-loop turn as the tap that asked.
@MainActor
final class ComposerFocus {
    fileprivate weak var textView: UITextView?

    var isFocused: Bool { textView?.isFirstResponder ?? false }

    /// Raise the keyboard now. True when the field took focus.
    @discardableResult
    func focus() -> Bool {
        return textView?.becomeFirstResponder() ?? false
    }

    func blur() {
        textView?.resignFirstResponder()
    }
}

struct PromptComposer: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.colorScheme) private var scheme

    @Binding var text: String
    let hint: String
    /// Nil hides "+": this surface's destination cannot take photos.
    let attachments: Binding<[PromptAttachment]>?
    /// The chip's label ("Opus 5.5"); nil hides the chip.
    let modelLabel: String?
    /// The chip's menu; empty makes the chip open `onPickModel` instead.
    let modelOptions: [PromptModelOption]
    let selectedModelId: String
    let onSelectModel: ((String) -> Void)?
    let onPickModel: (() -> Void)?
    let modeOptions: [PromptModeOption]
    let selectedModeId: String
    let onSelectMode: ((String) -> Void)?
    /// An agent run is in progress: with nothing typed the primary button
    /// stops it; typing turns it back into send (which steers).
    let running: Bool
    /// A send is in flight: the primary button waits.
    let busy: Bool
    let onStop: (() -> Void)?
    /// Nil disables sending (signed out, closed session).
    let onSend: (() -> Void)?
    let offersFix: Bool
    let dictationEnabled: Bool
    let isOpen: Bool
    let listensOnOpen: Bool
    /// Return sends (chat) instead of inserting a newline (agents).
    let submitOnReturn: Bool
    let slashCommands: [PromptSlashCommand]
    /// Drawn after "+", e.g. the shell's Agent/Chat switch.
    let extraControls: AnyView?
    /// Drawn inside the card above the text, e.g. the run's context line.
    let header: AnyView?
    /// An extra entry in the "+" menu (the on-device agent's file reference).
    let extraAttachAction: PromptAttachAction?
    let uploadsFiles: Bool
    let focus: ComposerFocus?
    let maxInputHeight: CGFloat
    let accessibilityPrefix: String
    let onSendFrame: ((CGRect) -> Void)?
    let onClose: (() -> Void)?

    init(
        text: Binding<String>,
        hint: String = "Type / for commands",
        attachments: Binding<[PromptAttachment]>? = nil,
        modelLabel: String? = nil,
        modelOptions: [PromptModelOption] = [],
        selectedModelId: String = "",
        onSelectModel: ((String) -> Void)? = nil,
        onPickModel: (() -> Void)? = nil,
        modeOptions: [PromptModeOption] = [],
        selectedModeId: String = "",
        onSelectMode: ((String) -> Void)? = nil,
        running: Bool = false,
        busy: Bool = false,
        onStop: (() -> Void)? = nil,
        onSend: (() -> Void)?,
        offersFix: Bool = false,
        dictationEnabled: Bool = true,
        isOpen: Bool = true,
        listensOnOpen: Bool = false,
        submitOnReturn: Bool = false,
        slashCommands: [PromptSlashCommand] = [],
        extraControls: AnyView? = nil,
        header: AnyView? = nil,
        extraAttachAction: PromptAttachAction? = nil,
        uploadsFiles: Bool = false,
        focus: ComposerFocus? = nil,
        maxInputHeight: CGFloat = 168,
        accessibilityPrefix: String = "composer",
        onSendFrame: ((CGRect) -> Void)? = nil,
        onClose: (() -> Void)? = nil
    ) {
        _text = text
        self.hint = hint
        self.attachments = attachments
        self.modelLabel = modelLabel
        self.modelOptions = modelOptions
        self.selectedModelId = selectedModelId
        self.onSelectModel = onSelectModel
        self.onPickModel = onPickModel
        self.modeOptions = modeOptions
        self.selectedModeId = selectedModeId
        self.onSelectMode = onSelectMode
        self.running = running
        self.busy = busy
        self.onStop = onStop
        self.onSend = onSend
        self.offersFix = offersFix
        self.dictationEnabled = dictationEnabled
        self.isOpen = isOpen
        self.listensOnOpen = listensOnOpen
        self.submitOnReturn = submitOnReturn
        self.slashCommands = slashCommands
        self.extraControls = extraControls
        self.header = header
        self.extraAttachAction = extraAttachAction
        self.uploadsFiles = uploadsFiles
        self.focus = focus
        self.maxInputHeight = maxInputHeight
        self.accessibilityPrefix = accessibilityPrefix
        self.onSendFrame = onSendFrame
        self.onClose = onClose
    }

    @State private var dictation: PromptDictation?
    @State private var listening: Bool = false
    @State private var dictationBase: String = ""
    @State private var dictated: String = ""
    @State private var picking: Bool = false
    @State private var showLibrary: Bool = false
    @State private var showCamera: Bool = false
    @State private var showFiles: Bool = false
    @State private var importsText: Bool = false
    @State private var textFiles: [PromptTextFile] = []
    @State private var libraryItems: [PhotosPickerItem] = []
    @State private var markup: PromptMarkupTarget?
    @State private var problem: String?
    @State private var note: String?
    @State private var launches: Int = 0
    @State private var localFocus: ComposerFocus = ComposerFocus()
    @ScaledMetric(relativeTo: .body) private var control: CGFloat = 44

    private var canAttach: Bool { attachments != nil }
    private var photos: [PromptAttachment] { attachments?.wrappedValue ?? [] }
    private var room: Int { PromptAttachmentLimits.maxCount - photos.count }
    private var hasContent: Bool { !text.dartTrimmed.isEmpty || !photos.isEmpty || !textFiles.isEmpty }
    private var showStop: Bool { running && !hasContent && onStop != nil }
    private var fieldFocus: ComposerFocus { focus ?? localFocus }

    // MARK: Body

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            let matches: [PromptSlashCommand] = matching(text)
            if !matches.isEmpty {
                slashMenu(matches)
            }
            card
                .fullScreenCover(item: $markup) { (target: PromptMarkupTarget) in
                    PromptMarkupEditor(target: target, confirmLabel: target.replacing == nil ? "Add" : "Done") {
                        (result: PromptAttachment?) in
                        finishMarkup(target, result)
                    }
                }
        }
        .fmKeepsKeyboard()
        .photosPicker(
            isPresented: $showLibrary, selection: $libraryItems, maxSelectionCount: max(1, room),
            matching: .images, preferredItemEncoding: .compatible)
        .onChange(of: libraryItems) { (_: [PhotosPickerItem], items: [PhotosPickerItem]) in
            if items.isEmpty {
                return
            }
            libraryItems = []
            Task { await add(PromptLibraryLoader.load(items)) }
        }
        .fullScreenCover(isPresented: $showCamera) {
            PromptCameraPicker { (data: Data?) in
                showCamera = false
                if let data = data {
                    Task { await add([("photo", data)]) }
                }
            }
            .ignoresSafeArea()
        }
        .fileImporter(
            isPresented: $showFiles, allowedContentTypes: importsText ? [.item] : [.image], allowsMultipleSelection: true
        ) { (result: Result<[URL], Error>) in
            guard case .success(let urls) = result else {
                return
            }
            if importsText {
                Task { await addText(urls) }
            } else {
                Task { await add(PromptFileLoader.load(Array(urls.prefix(room)))) }
            }
        }
        .onAppear {
            if isOpen {
                listenOnOpen()
            }
        }
        .onChange(of: isOpen) { (_: Bool, open: Bool) in
            if open {
                listenOnOpen()
            } else {
                stopDictation()
                note = nil
            }
        }
        .onChange(of: text) { (_: String, value: String) in
            if listening && value != dictated {
                stopDictation()
            }
        }
        .onDisappear {
            stopDictation()
        }
    }

    private var card: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            if header != nil || onClose != nil {
                HStack(alignment: .top, spacing: FmSpace.x2) {
                    if let header = header {
                        header
                            .frame(maxWidth: .infinity, alignment: .leading)
                    } else {
                        Spacer(minLength: 0)
                    }
                    if let onClose = onClose {
                        closeButton(onClose)
                    }
                }
            }
            if !photos.isEmpty || picking {
                thumbnails
            }
            if !textFiles.isEmpty {
                fileChips
            }
            if let problem = problem {
                Text(problem)
                    .fmText(.bodySmall, color: fm.danger)
                    .fixedSize(horizontal: false, vertical: true)
                    .onTapGesture { self.problem = nil }
                    .fmMessageTools(problem)
            }
            if let note = note {
                Text(note)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .onTapGesture { self.note = nil }
            }
            ComposerTextView(
                text: $text,
                placeholder: listening ? "Listening…" : hint,
                textColor: UIColor(fm.textPrimary),
                placeholderColor: UIColor(fm.textTertiary),
                tint: UIColor(fm.accent),
                submitOnReturn: submitOnReturn,
                maxHeight: maxInputHeight,
                focus: fieldFocus,
                onSubmit: submit,
                onPasteImages: canAttach ? { (images: [PastedImage]) in paste(images) } : nil)
                .padding(.horizontal, FmSpace.x1)
                .accessibilityLabel(Text(hint))
                .accessibilityIdentifier("\(accessibilityPrefix)-field")
            controls
        }
        .padding(FmSpace.x3)
        .modifier(PromptCardSurface(shape: RoundedRectangle(cornerRadius: 28, style: .continuous)))
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("\(accessibilityPrefix)-card")
    }

    // MARK: Controls

    private var controls: some View {
        HStack(spacing: FmSpace.x2) {
            if canAttach || uploadsFiles || extraAttachAction != nil {
                attachMenu
            }
            if let extra = extraControls {
                extra
            }
            if !modeOptions.isEmpty, let onSelectMode = onSelectMode {
                modeChip(onSelectMode)
            }
            if let label = modelLabel {
                modelChip(label)
                    .layoutPriority(-1)
            }
            Spacer(minLength: 0)
            if listening {
                listeningChip
            }
            if offersFix && onSend != nil && !listening {
                fixButton
            }
            primaryButton
        }
    }

    private var attachMenu: some View {
        Menu {
            if canAttach {
                Button {
                    open(.library)
                } label: {
                    Label("Photo Library", systemImage: "photo.on.rectangle")
                }
                if PromptCameraPicker.isAvailable {
                    Button {
                        open(.camera)
                    } label: {
                        Label("Take Photo", systemImage: "camera")
                    }
                }
                Button {
                    captureScreen()
                } label: {
                    Label("Capture Screen", systemImage: "camera.viewfinder")
                }
                Button {
                    open(.file)
                } label: {
                    Label("Choose File", systemImage: "folder")
                }
            }
            if uploadsFiles {
                Button {
                    openTextFile()
                } label: {
                    Label("Upload Text File", systemImage: "doc.plaintext")
                }
            }
            if let extra = extraAttachAction {
                Button {
                    extra.run()
                } label: {
                    Label(extra.label, systemImage: extra.systemImage)
                }
            }
        } label: {
            roundFace(systemImage: "plus", active: false, enabled: !picking)
        }
        .disabled(picking)
        .accessibilityLabel(Text(canAttach ? "Add photos" : "Attach"))
        .accessibilityIdentifier("\(accessibilityPrefix)-attach")
    }

    private func modelChipFace(_ label: String) -> some View {
        HStack(spacing: FmSpace.x1) {
            Text(label)
                .fmText(.bodyMedium, color: fm.textPrimary, weight: FmType.medium)
                .lineLimit(1)
                .truncationMode(.tail)
            if onSelectModel != nil || onPickModel != nil {
                Image(systemName: "chevron.down")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(fm.textTertiary)
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .frame(minHeight: control)
        .background(Capsule().fill(controlFill(active: false)))
        .contentShape(Capsule())
    }

    private func modelChip(_ label: String) -> some View {
        Group {
            if !modelOptions.isEmpty, let onSelectModel = onSelectModel {
                Menu {
                    Picker("Model", selection: Binding<String>(
                        get: { selectedModelId },
                        set: { (id: String) in
                            FmHaptics.selection()
                            onSelectModel(id)
                        })
                    ) {
                        ForEach(modelOptions) { (option: PromptModelOption) in
                            if option.note.isEmpty {
                                Text(option.label).tag(option.id)
                            } else {
                                Text("\(option.label)\n\(option.note)").tag(option.id)
                            }
                        }
                    }
                } label: {
                    modelChipFace(label)
                }
            } else if let onPickModel = onPickModel {
                Button {
                    FmHaptics.tap()
                    onPickModel()
                } label: {
                    modelChipFace(label)
                }
                .buttonStyle(.plain)
            } else {
                modelChipFace(label)
            }
        }
        .accessibilityLabel(Text("Model: \(label)"))
        .accessibilityIdentifier("\(accessibilityPrefix)-model-chip")
    }

    private func modeChip(_ onSelectMode: @escaping (String) -> Void) -> some View {
        let current: PromptModeOption? = modeOptions.first { $0.id == selectedModeId } ?? modeOptions.first
        return Menu {
            Picker("Mode", selection: Binding<String>(
                get: { current?.id ?? "" },
                set: { (id: String) in
                    FmHaptics.selection()
                    onSelectMode(id)
                })
            ) {
                ForEach(modeOptions) { (option: PromptModeOption) in
                    Label(
                        option.note.isEmpty ? option.label : "\(option.label)\n\(option.note)",
                        systemImage: option.systemImage
                    )
                    .tag(option.id)
                }
            }
        } label: {
            Image(systemName: current?.systemImage ?? "slider.horizontal.3")
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(fm.accent)
                .frame(width: control, height: control)
                .background(Circle().fill(fm.accentSubtle))
                .contentShape(Circle())
        }
        .fixedSize()
        .accessibilityLabel(Text("Mode: \(current?.label ?? "")"))
        .accessibilityIdentifier("\(accessibilityPrefix)-mode-chip")
    }

    private var listeningChip: some View {
        listeningFace
            .onTapGesture {
                FmHaptics.selection()
                stopDictation()
            }
            .onLongPressGesture(minimumDuration: 0.5) {
                CaptureHold.noteHandled()
                stopListeningOnOpen()
            }
            .accessibilityElement(children: .ignore)
            .accessibilityAddTraits(.isButton)
            .accessibilityLabel(Text("Listening. Tap to stop dictation"))
            .accessibilityHint(Text("Touch and hold to stop listening whenever the prompt opens"))
            .accessibilityAction { stopDictation() }
            .accessibilityAction(named: Text("Stop listening when the prompt opens")) { stopListeningOnOpen() }
            .accessibilityIdentifier("\(accessibilityPrefix)-listening")
    }

    private func closeButton(_ onClose: @escaping () -> Void) -> some View {
        Button {
            FmHaptics.tap()
            onClose()
        } label: {
            Image(systemName: "xmark")
                .font(.caption.weight(.bold))
                .foregroundStyle(fm.textSecondary)
                .frame(width: 26, height: 26)
                .background(Circle().fill(controlFill(active: false)))
                .frame(width: 32, height: 32)
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .padding(.top, -FmSpace.x1)
        .padding(.trailing, -FmSpace.x1)
        .accessibilityLabel(Text("Close"))
        .accessibilityIdentifier("\(accessibilityPrefix)-close")
    }

    private var fixButton: some View {
        let canFix: Bool = hasContent && !busy
        return Button(action: submitFix) {
            fixFace(enabled: canFix)
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text("Send as Please fix"))
        .accessibilityIdentifier("\(accessibilityPrefix)-fix")
    }

    private var listeningFace: some View {
        Image(systemName: "waveform")
            .font(.subheadline.weight(.semibold))
            .symbolEffect(.variableColor.iterative)
            .foregroundStyle(fm.danger)
            .frame(minWidth: control, minHeight: control)
            .background(Capsule().fill(fm.danger.opacity(0.12)))
            .contentShape(Capsule())
            .fixedSize()
    }

    private func fixFace(enabled: Bool) -> some View {
        Image(systemName: "wrench.and.screwdriver")
            .font(.subheadline.weight(.semibold))
            .foregroundStyle(enabled ? fm.textPrimary : fm.textTertiary)
            .frame(width: control, height: control)
            .background(Circle().fill(controlFill(active: false)))
            .contentShape(Circle())
    }

    @ViewBuilder private var primaryButton: some View {
        if showStop {
            Button {
                FmHaptics.commit(.click)
                onStop?()
            } label: {
                primaryFace(systemImage: "stop.fill", enabled: true)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text("Stop"))
            .accessibilityIdentifier("\(accessibilityPrefix)-stop")
        } else {
            let canSend: Bool = hasContent && !busy && onSend != nil
            Button(action: submit) {
                if busy {
                    ProgressView()
                        .frame(width: control, height: control)
                        .background(Circle().fill(controlFill(active: false)))
                } else {
                    primaryFace(systemImage: "arrow.up", enabled: canSend)
                }
            }
            .buttonStyle(.plain)
            .onGeometryChange(for: CGRect.self, of: { $0.frame(in: .global) }) { (frame: CGRect) in
                onSendFrame?(frame)
            }
            .keyframeAnimator(initialValue: 1.0, trigger: launches) { face, scale in
                face.scaleEffect(scale)
            } keyframes: { (_: Double) in
                KeyframeTrack {
                    SpringKeyframe(0.8, duration: 0.09)
                    SpringKeyframe(1.14, duration: 0.16)
                    SpringKeyframe(1.0, duration: 0.3)
                }
            }
            // Dimmed rather than inert: the tap re-checks the field itself, so
            // a tap in the same frame as the last keystroke still sends.
            .disabled(onSend == nil)
            .accessibilityLabel(Text("Send"))
            .accessibilityIdentifier("\(accessibilityPrefix)-send")
        }
    }

    private func primaryFace(systemImage: String, enabled: Bool) -> some View {
        Image(systemName: systemImage)
            .font(.body.weight(.bold))
            .foregroundStyle(enabled ? fm.bgBase : fm.textTertiary)
            .frame(width: control, height: control)
            .background(Circle().fill(enabled ? fm.textPrimary : controlFill(active: false)))
            .contentShape(Circle())
    }

    private func roundFace(systemImage: String, active: Bool, enabled: Bool) -> some View {
        Image(systemName: systemImage)
            .font(.body.weight(.medium))
            .foregroundStyle(enabled ? fm.textPrimary : fm.textTertiary)
            .frame(width: control, height: control)
            .background(Circle().fill(controlFill(active: active)))
            .contentShape(Circle())
    }

    private func controlFill(active: Bool) -> Color {
        if active {
            return fm.accentSubtle
        }
        return scheme == .dark ? Color.white.opacity(0.08) : Color.black.opacity(0.05)
    }


    // MARK: Thumbnails

    private var thumbnails: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: FmSpace.x2) {
                ForEach(Array(photos.enumerated()), id: \.element.id) { (entry: (offset: Int, element: PromptAttachment)) in
                    PromptThumbnail(attachment: entry.element, onMarkUp: { markUp(entry.element) }) { remove(entry.element) }
                        .accessibilityIdentifier("\(accessibilityPrefix)-attachment-\(entry.offset)")
                }
                if picking {
                    ProgressView()
                        .frame(width: 64, height: 64)
                        .background(RoundedRectangle(cornerRadius: FmRadius.lg).fill(controlFill(active: false)))
                        .accessibilityLabel(Text("Preparing photo"))
                }
            }
            .padding(.top, 6)
            .padding(.trailing, 6)
        }
    }

    private var fileChips: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: FmSpace.x2) {
                ForEach(Array(textFiles.enumerated()), id: \.element.id) { (entry: (offset: Int, element: PromptTextFile)) in
                    PromptTextFileChip(file: entry.element) { removeText(entry.element) }
                        .accessibilityIdentifier("\(accessibilityPrefix)-file-\(entry.offset)")
                }
            }
        }
    }

    // MARK: Slash commands

    private var commands: [PromptSlashCommand] {
        var all: [PromptSlashCommand] = []
        if let onPickModel = onPickModel {
            all.append(PromptSlashCommand(name: "model", description: "Choose the model", run: onPickModel))
        }
        if canAttach {
            all.append(PromptSlashCommand(name: "photo", description: "Attach photos from your library") { open(.library) })
            if PromptCameraPicker.isAvailable {
                all.append(PromptSlashCommand(name: "camera", description: "Take a photo to attach") { open(.camera) })
            }
            all.append(PromptSlashCommand(name: "screenshot", description: "Capture this screen and mark it up") { captureScreen() })
        }
        if uploadsFiles {
            all.append(PromptSlashCommand(name: "file", description: "Upload a text file into the prompt") { openTextFile() })
        }
        if dictationEnabled {
            all.append(PromptSlashCommand(name: "dictate", description: "Speak instead of typing") { toggleDictation() })
        }
        return all + slashCommands
    }

    /// The commands matching what is typed, when the prompt is a bare "/word".
    private func matching(_ value: String) -> [PromptSlashCommand] {
        guard value.hasPrefix("/"), !value.contains(where: { $0.isWhitespace }) else {
            return []
        }
        let query: String = String(value.dropFirst()).lowercased()
        return commands.filter { $0.name.hasPrefix(query) }
    }

    private func slashMenu(_ matches: [PromptSlashCommand]) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            ForEach(matches) { (command: PromptSlashCommand) in
                Button {
                    FmHaptics.selection()
                    text = ""
                    command.run()
                } label: {
                    HStack(spacing: FmSpace.x3) {
                        Text("/\(command.name)").fmMono(size: FmType.small, color: fm.textPrimary)
                        Text(command.description)
                            .fmText(.bodySmall, color: fm.textTertiary)
                            .lineLimit(1)
                        Spacer(minLength: 0)
                    }
                    .padding(.horizontal, FmSpace.x4)
                    .frame(minHeight: 44)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("\(accessibilityPrefix)-command-\(command.name)")
            }
        }
        .padding(.vertical, FmSpace.x1)
        .background {
            RoundedRectangle(cornerRadius: FmRadius.lg + 4, style: .continuous)
                .fill(fm.sheetSurface.opacity(scheme == .dark ? 0.72 : 0.78))
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: FmRadius.lg + 4, style: .continuous))
        }
        .overlay(RoundedRectangle(cornerRadius: FmRadius.lg + 4, style: .continuous).strokeBorder(fm.border, lineWidth: 1))
        .accessibilityIdentifier("\(accessibilityPrefix)-slash-menu")
    }

    // MARK: Behaviour

    private func open(_ source: PromptAttachmentSource) {
        guard canAttach, !picking else {
            return
        }
        if room <= 0 {
            problem = "A prompt can carry \(PromptAttachmentLimits.maxCount) photos. Remove one to add another."
            return
        }
        switch source {
        case .library: showLibrary = true
        case .camera: showCamera = true
        case .file:
            importsText = false
            showFiles = true
        }
    }

    private func openTextFile() {
        guard uploadsFiles, !picking else {
            return
        }
        if textFiles.count >= PromptTextFiles.maxCount {
            problem = "A prompt can carry \(PromptTextFiles.maxCount) files. Remove one to add another."
            return
        }
        importsText = true
        showFiles = true
    }

    private func addText(_ urls: [URL]) async {
        guard uploadsFiles, !urls.isEmpty else {
            return
        }
        picking = true
        defer { picking = false }
        problem = nil
        var added: Bool = false
        for result in await PromptTextFileLoader.load(urls) {
            switch result {
            case .failure(let error):
                problem = error.message
            case .success(let file):
                if let refusal = PromptTextFiles.problem(adding: file, to: textFiles, prompt: text) {
                    problem = refusal
                } else {
                    textFiles.append(file)
                    added = true
                }
            }
        }
        if added {
            FmHaptics.selection()
        }
    }

    private func removeText(_ file: PromptTextFile) {
        FmHaptics.tap()
        textFiles.removeAll { $0.id == file.id }
    }

    private func paste(_ images: [PastedImage]) {
        guard canAttach, !picking else {
            return
        }
        let admitted = PromptPasteRules.admit(images, existing: photos.count)
        if let overflow = admitted.problem {
            problem = overflow
        }
        let raw: [(String, Data)] = admitted.accepted.map { (image: PastedImage) in (image.name, image.data) }
        Task { await add(raw, keepProblem: admitted.problem != nil) }
    }

    private func add(_ raw: [(String, Data)], keepProblem: Bool = false) async {
        guard let binding = attachments, !raw.isEmpty else {
            return
        }
        picking = true
        defer { picking = false }
        if !keepProblem {
            problem = nil
        }
        var prepared: [PromptAttachment] = []
        for (name, data) in raw.prefix(max(0, room)) {
            do {
                prepared.append(try await PromptPhotoPreparer.prepare(data, name: name))
            } catch let error as PromptAttachmentError {
                problem = error.message
            } catch {
                problem = "Could not read that photo: \(error.localizedDescription)"
            }
        }
        if prepared.isEmpty {
            return
        }
        FmHaptics.selection()
        binding.wrappedValue = Array((binding.wrappedValue + prepared).prefix(PromptAttachmentLimits.maxCount))
    }

    private func captureScreen() {
        guard canAttach, !picking, markup == nil else {
            return
        }
        if room <= 0 {
            problem = "A prompt can carry \(PromptAttachmentLimits.maxCount) photos. Remove one to add another."
            return
        }
        fieldFocus.blur()
        Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(350))
            guard let shot = PromptScreenCapture.keyWindow() else {
                problem = "This screen could not be captured."
                return
            }
            markup = PromptMarkupTarget(
                id: PromptPhotoRules.newId(), image: shot, name: PromptMarkupRules.captureName, replacing: nil)
        }
    }

    private func markUp(_ attachment: PromptAttachment) {
        guard canAttach, !picking, markup == nil else {
            return
        }
        guard let image = UIImage(data: attachment.bytes) else {
            problem = "That photo could not be opened for markup."
            return
        }
        markup = PromptMarkupTarget(id: attachment.id, image: image, name: attachment.name, replacing: attachment.id)
    }

    private func finishMarkup(_ target: PromptMarkupTarget, _ result: PromptAttachment?) {
        markup = nil
        guard let result = result, let binding = attachments else {
            return
        }
        FmHaptics.selection()
        if let old = target.replacing, let index = binding.wrappedValue.firstIndex(where: { $0.id == old }) {
            binding.wrappedValue[index] = result
        } else if binding.wrappedValue.count < PromptAttachmentLimits.maxCount {
            binding.wrappedValue.append(result)
        } else {
            problem = "A prompt can carry \(PromptAttachmentLimits.maxCount) photos. Remove one to add another."
        }
    }

    private func remove(_ attachment: PromptAttachment) {
        FmHaptics.tap()
        attachments?.wrappedValue.removeAll { $0.id == attachment.id }
    }

    private func toggleDictation() {
        if listening {
            stopDictation()
            return
        }
        startDictation()
    }

    private func stopListeningOnOpen() {
        FmHaptics.commit(.click)
        stopDictation()
        if let settings: SettingsService = Services.get(SettingsService.self) {
            settings.setDictatesOnOpen(false)
        } else if let prefs: KeyValueStore = Services.get(KeyValueStore.self) {
            prefs.set(false, forKey: SettingsService.dictateOnOpenKey)
        }
        note = "The prompt no longer listens when it opens. Type /dictate to speak, or turn it back on in Settings."
    }

    private func listenOnOpen() {
        guard listensOnOpen, dictationEnabled, !listening, !busy, !PromptDictation.refused,
              let prefs: KeyValueStore = Services.get(KeyValueStore.self), SettingsService.dictatesOnOpen(prefs)
        else {
            return
        }
        startDictation()
    }

    private func stopDictation() {
        if listening {
            dictation?.stop()
            listening = false
        }
    }

    private func startDictation() {
        let recognizer: PromptDictation = dictation ?? PromptDictation()
        dictation = recognizer
        dictationBase = text
        dictated = text
        listening = true
        Task { @MainActor in
            _ = await recognizer.start(
                onWords: { (words: String) in
                    let base: String = dictationBase
                    let joiner: String = base.isEmpty || base.last?.isWhitespace == true ? "" : " "
                    let value: String = words.isEmpty ? base : base + joiner + words
                    dictated = value
                    text = value
                },
                onDone: { (quiet: Bool) in
                    let spoke: Bool = listening && dictated != dictationBase
                    listening = false
                    if quiet && spoke {
                        submit()
                    }
                },
                onError: { (message: String) in problem = message })
        }
    }

    private func submit() {
        guard hasContent, !busy, let onSend = onSend else {
            return
        }
        stopDictation()
        if !textFiles.isEmpty {
            if let over = PromptTextFiles.overBudget(text, textFiles) {
                problem = over
                return
            }
            text = PromptTextFiles.fold(text, textFiles)
            textFiles = []
        }
        problem = nil
        FmHaptics.commit(.send)
        if onSendFrame != nil {
            launches += 1
        }
        onSend()
    }

    private func submitFix() {
        guard hasContent, !busy, onSend != nil else {
            return
        }
        stopDictation()
        text = PromptFix.prompt(text)
        submit()
    }
}

private struct PromptCardSurface: ViewModifier {
    @Environment(\.fmPalette) private var fm
    @Environment(\.colorScheme) private var scheme
    let shape: RoundedRectangle

    func body(content: Content) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            content.glassEffect(.regular, in: shape)
        } else {
            frosted(content)
        }
        #else
        frosted(content)
        #endif
    }

    private func frosted<Inner: View>(_ inner: Inner) -> some View {
        inner
            .background {
                shape
                    .fill(fm.sheetSurface.opacity(scheme == .dark ? 0.72 : 0.78))
                    .background(.ultraThinMaterial, in: shape)
            }
            .overlay {
                shape.strokeBorder(scheme == .dark ? Color.white.opacity(0.10) : Color.black.opacity(0.08), lineWidth: 1)
            }
            .shadow(color: Color.black.opacity(scheme == .dark ? 0.35 : 0.10), radius: 18, y: 6)
    }
}

private struct PromptTextFileChip: View {
    @Environment(\.fmPalette) private var fm
    let file: PromptTextFile
    let onRemove: () -> Void

    var body: some View {
        HStack(spacing: FmSpace.x1) {
            Image(systemName: "doc.plaintext")
                .font(.caption)
                .foregroundStyle(fm.textSecondary)
            VStack(alignment: .leading, spacing: 0) {
                Text(file.name)
                    .fmText(.bodySmall, color: fm.textPrimary, weight: FmType.medium)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Text(PromptTextFiles.sizeLabel(file))
                    .fmText(.labelSmall, color: fm.textTertiary)
                    .lineLimit(1)
            }
            .frame(maxWidth: 180, alignment: .leading)
            Button(action: onRemove) {
                Image(systemName: "xmark")
                    .font(.caption2.weight(.bold))
                    .foregroundStyle(fm.textSecondary)
                    .frame(width: 44, height: 44)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text("Remove \(file.name)"))
        }
        .padding(.leading, FmSpace.x3)
        .background(Capsule().fill(fm.bgOverlay))
        .accessibilityElement(children: .contain)
    }
}

/// One attached photo with its remove button.
private struct PromptThumbnail: View {
    @Environment(\.fmPalette) private var fm
    let attachment: PromptAttachment
    let onMarkUp: () -> Void
    let onRemove: () -> Void
    @State private var image: UIImage?

    var body: some View {
        ZStack(alignment: .topTrailing) {
            Group {
                if let image = image {
                    Image(uiImage: image)
                        .resizable()
                        .scaledToFill()
                } else {
                    fm.bgOverlay
                }
            }
            .frame(width: 64, height: 64)
            .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
            .contentShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
            .onTapGesture(perform: onMarkUp)
            .accessibilityElement()
            .accessibilityLabel(Text("Mark up \(attachment.name)"))
            .accessibilityAddTraits(.isButton)
            .accessibilityAction { onMarkUp() }
            Button(action: onRemove) {
                Image(systemName: "xmark")
                    .font(.caption2.weight(.bold))
                    .foregroundStyle(fm.bgBase)
                    .frame(width: 22, height: 22)
                    .background(Circle().fill(fm.textPrimary))
                    .padding(4)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .offset(x: 8, y: -8)
            .accessibilityLabel(Text("Remove photo"))
        }
        .task(id: attachment.id) {
            let data: Data = attachment.bytes
            image = await Task.detached(priority: .utility) {
                UIImage(data: data)?.preparingThumbnail(of: CGSize(width: 192, height: 192))
            }.value
        }
    }
}

/// The composer's field: a UITextView that grows with its text up to
/// `maxHeight`, then scrolls. Dynamic Type follows the body style.
struct ComposerTextView: UIViewRepresentable {
    @Binding var text: String
    let placeholder: String
    let textColor: UIColor
    let placeholderColor: UIColor
    let tint: UIColor
    let submitOnReturn: Bool
    let maxHeight: CGFloat
    let focus: ComposerFocus
    let onSubmit: () -> Void
    var onPasteImages: (([PastedImage]) -> Void)? = nil

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeUIView(context: Context) -> ComposerUITextView {
        let view: ComposerUITextView = ComposerUITextView()
        view.delegate = context.coordinator
        view.backgroundColor = .clear
        view.font = UIFont.preferredFont(forTextStyle: .body)
        view.adjustsFontForContentSizeCategory = true
        view.textContainerInset = UIEdgeInsets(top: 6, left: 0, bottom: 6, right: 0)
        view.textContainer.lineFragmentPadding = 0
        view.autocapitalizationType = .sentences
        view.isScrollEnabled = true
        view.alwaysBounceVertical = false
        view.showsVerticalScrollIndicator = false
        view.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        focus.textView = view
        return view
    }

    func updateUIView(_ view: ComposerUITextView, context: Context) {
        context.coordinator.parent = self
        focus.textView = view
        view.onPasteImages = onPasteImages
        if view.text != text {
            view.text = text
        }
        view.textColor = textColor
        view.tintColor = tint
        view.placeholder = placeholder
        view.placeholderColor = placeholderColor
        let key: UIReturnKeyType = submitOnReturn ? .send : .default
        if view.returnKeyType != key {
            view.returnKeyType = key
            view.reloadInputViews()
        }
        view.refreshPlaceholder()
    }

    func sizeThatFits(_ proposal: ProposedViewSize, uiView: ComposerUITextView, context: Context) -> CGSize? {
        let width: CGFloat = proposal.width ?? 280
        let fitted: CGSize = uiView.sizeThatFits(CGSize(width: width, height: .greatestFiniteMagnitude))
        let lineHeight: CGFloat = (uiView.font?.lineHeight ?? 20) + uiView.textContainerInset.top + uiView.textContainerInset.bottom
        return CGSize(width: width, height: min(maxHeight, max(lineHeight, fitted.height)))
    }

    final class Coordinator: NSObject, UITextViewDelegate {
        var parent: ComposerTextView

        init(_ parent: ComposerTextView) { self.parent = parent }

        func textViewDidChange(_ textView: UITextView) {
            if textView.text.utf16.count != parent.text.utf16.count {
                FmSoundCues.play(.keystroke)
            }
            parent.text = textView.text
            (textView as? ComposerUITextView)?.refreshPlaceholder()
            textView.invalidateIntrinsicContentSize()
        }

        func textView(_ textView: UITextView, shouldChangeTextIn range: NSRange, replacementText replacement: String) -> Bool {
            if parent.submitOnReturn && replacement == "\n" {
                parent.onSubmit()
                return false
            }
            return true
        }
    }
}

/// A UITextView with a placeholder label.
final class ComposerUITextView: UITextView {
    private let placeholderLabel: UILabel = UILabel()

    var placeholder: String = "" {
        didSet { placeholderLabel.text = placeholder }
    }

    var placeholderColor: UIColor = .placeholderText {
        didSet { placeholderLabel.textColor = placeholderColor }
    }

    var onPasteImages: (([PastedImage]) -> Void)?

    override func canPerformAction(_ action: Selector, withSender sender: Any?) -> Bool {
        if action == #selector(paste(_:)), onPasteImages != nil, PromptPasteboard.hasImages {
            return true
        }
        return super.canPerformAction(action, withSender: sender)
    }

    override func paste(_ sender: Any?) {
        guard let onPasteImages = onPasteImages else {
            super.paste(sender)
            return
        }
        let pasted: PromptPaste = PromptPasteboard.read()
        if pasted.images.isEmpty {
            super.paste(sender)
            return
        }
        onPasteImages(pasted.images)
        if !pasted.text.isEmpty {
            insertText(pasted.text)
        }
    }

    override init(frame: CGRect, textContainer: NSTextContainer?) {
        super.init(frame: frame, textContainer: textContainer)
        placeholderLabel.font = UIFont.preferredFont(forTextStyle: .body)
        placeholderLabel.adjustsFontForContentSizeCategory = true
        placeholderLabel.numberOfLines = 1
        placeholderLabel.isAccessibilityElement = false
        placeholderLabel.translatesAutoresizingMaskIntoConstraints = false
        addSubview(placeholderLabel)
        NSLayoutConstraint.activate([
            placeholderLabel.leadingAnchor.constraint(equalTo: leadingAnchor),
            placeholderLabel.topAnchor.constraint(equalTo: topAnchor, constant: 6),
            placeholderLabel.widthAnchor.constraint(lessThanOrEqualTo: widthAnchor),
        ])
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    func refreshPlaceholder() {
        placeholderLabel.isHidden = !(text ?? "").isEmpty
    }
}
