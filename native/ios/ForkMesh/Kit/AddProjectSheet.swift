import FMAccount
import FMBase
import FMChat
import FMNet
import PhotosUI
import SwiftUI
import UIKit

struct AddProjectSheet: View {
    @State private var model: AddProjectModel?
    private let onAdded: (RelayProfile) -> Void

    init(onAdded: @escaping (RelayProfile) -> Void) {
        self.onAdded = onAdded
        let settings: SettingsService? = Services.get(SettingsService.self)
        let http: HTTPClient = Services.get(HTTPClient.self) ?? URLSessionHTTPClient()
        _model = State(initialValue: settings.map { AddProjectModel(relays: $0.relays, probe: RelayProbe(http: http)) })
    }

    var body: some View {
        if let model = model {
            AddProjectForm(model: model, onAdded: onAdded)
                .fmSheetStyle()
        }
    }
}

private struct AddProjectForm: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: AddProjectModel
    let onAdded: (RelayProfile) -> Void
    @State private var pickerItem: PhotosPickerItem?
    @State private var logoData: Data?
    @State private var logoImage: UIImage?
    @State private var saving: Bool = false
    @State private var error: String?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                FmPanelHeader(title: "New project")
                Text(AddProjectModel.intro)
                    .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                    .padding(.top, FmSpace.x3)
                logoRow.padding(.top, FmSpace.x4)
                FmField(label: "Name") {
                    TextField("My project", text: $model.name)
                        .submitLabel(.done)
                        .accessibilityIdentifier("add-project-name")
                }
                .padding(.top, FmSpace.x4)
                FmField(label: "Instance (optional)") {
                    TextField("relay.example.com", text: $model.host)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled(true)
                        .keyboardType(.URL)
                        .onSubmit { Task { await model.check() } }
                        .accessibilityIdentifier("add-project-host")
                }
                .padding(.top, FmSpace.x3)
                verdict.padding(.top, FmSpace.x4)
                if let error {
                    RelayNoticeView(notice: RelayNotice(message: error, error: true)).padding(.top, FmSpace.x3)
                }
                HStack(spacing: FmSpace.x3) {
                    if model.hasHost {
                        FmOutlinedButton(model.checkLabel, fillWidth: true, onPressed: model.checking ? nil : {
                            Task { await model.check() }
                        })
                    }
                    FmFilledButton(
                        model.createLabel, fillWidth: true,
                        onPressed: model.canCreate && !saving ? { Task { await create() } } : nil)
                }
                .padding(.top, FmSpace.x4)
            }
            .padding(FmSpace.x4)
        }
        .animation(.easeOut(duration: FmMotion.normal), value: model.checking)
        .onChange(of: pickerItem) { (_: PhotosPickerItem?, item: PhotosPickerItem?) in
            guard let item else { return }
            Task { await pick(item) }
        }
    }

    private var preview: RelayProfile {
        RelayProfile(
            id: "preview", label: model.cleanName.isEmpty ? "?" : model.cleanName, serverUrl: "", hasInstance: false)
    }

    private var logoRow: some View {
        HStack(spacing: FmSpace.x3) {
            Group {
                if let logoImage {
                    Image(uiImage: logoImage)
                        .resizable()
                        .scaledToFit()
                        .frame(width: 48, height: 48)
                        .background(fm.bgRaised)
                        .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                } else {
                    ProjectIcon(project: preview, size: 48)
                }
            }
            .accessibilityHidden(true)
            PhotosPicker(selection: $pickerItem, matching: .images) {
                Text(logoImage == nil ? "Choose logo…" : "Change logo")
                    .fmText(.labelLarge, color: fm.accent)
                    .frame(minHeight: 44)
            }
            .accessibilityIdentifier("add-project-logo")
            if logoImage != nil {
                Button {
                    logoData = nil
                    logoImage = nil
                    pickerItem = nil
                } label: {
                    Text("Remove").fmText(.labelLarge, color: fm.textSecondary).frame(minHeight: 44)
                }
                .buttonStyle(.plain)
            }
            Spacer(minLength: 0)
        }
    }

    @ViewBuilder private var verdict: some View {
        if model.checking {
            HStack(spacing: FmSpace.x3) {
                FmCircularProgressIndicator(size: 16, strokeWidth: 2)
                Text("Checking that this is a ForkMesh instance…").fmText(.bodySmall, color: fm.textSecondary)
            }
        } else if let result = model.result {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                RelayNoticeView(notice: RelayNotice(message: result.message, error: !result.ok && model.known == nil))
                if let missing = model.missingLine {
                    Text(missing).fmText(.bodySmall, color: fm.warning, height: 1.35)
                        .fmMessageTools(missing)
                }
            }
        }
    }

    private func pick(_ item: PhotosPickerItem) async {
        error = nil
        guard let data = try? await item.loadTransferable(type: Data.self),
            let shrunk = ProjectLogoStore.shrink(data), let image = UIImage(data: shrunk)
        else {
            error = "That image could not be read. Try a PNG or JPEG."
            return
        }
        logoData = data
        logoImage = image
    }

    private func create() async {
        saving = true
        defer { saving = false }
        let previous: String = model.known?.logo ?? ""
        var logo: String = ""
        if let logoData {
            guard let saved = await ProjectLogoStore.save(logoData, projectKey: model.cleanName) else {
                error = "The logo could not be saved."
                return
            }
            logo = saved
        }
        guard let project = model.create(logo: logo) else {
            if !logo.isEmpty { ProjectLogoStore.delete(logo) }
            return
        }
        if !logo.isEmpty, let logoImage { ProjectImages.shared.setLogo(logo, image: logoImage) }
        if logo.isEmpty, project.hasInstance {
            await ProjectImages.shared.adoptInstanceLogo(project.id, logoUrl: model.result?.logoUrl ?? "")
        }
        if !logo.isEmpty, !previous.isEmpty, previous != logo { ProjectLogoStore.delete(previous) }
        FmHaptics.commit()
        onAdded(project)
    }
}
