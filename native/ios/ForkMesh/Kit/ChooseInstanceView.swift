import FMAccount
import FMBase
import FMChat
import FMNet
import SwiftUI

struct ChooseInstanceView: View {
    @Environment(\.fmPalette) private var fm
    let project: RelayProfile
    let onClose: () -> Void
    @State private var model: AttachInstanceModel

    init(project: RelayProfile, onClose: @escaping () -> Void) {
        self.project = project
        self.onClose = onClose
        let http: HTTPClient = Services.get(HTTPClient.self) ?? URLSessionHTTPClient()
        let projectId = project.id
        _model = State(
            initialValue: AttachInstanceModel(probe: RelayProbe(http: http)) { instance in
                guard let switcher = Services.get(RelaySwitchController.self) else {
                    return RelaySwitchResult(ok: false, relay: nil, error: "Projects are not available right now.")
                }
                return await switcher.attach(projectId, to: instance)
            })
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                ProjectIcon(project: project, size: 48).accessibilityHidden(true)
                FmPanelHeader(title: "Choose an instance").padding(.top, FmSpace.x3)
                Text(AttachInstanceModel.intro(project.label))
                    .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                    .padding(.top, FmSpace.x3)
                FmField(label: "Instance") {
                    TextField("relay.example.com", text: $model.host)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled(true)
                        .keyboardType(.URL)
                        .onSubmit { Task { await model.check() } }
                        .accessibilityIdentifier("choose-instance-host")
                }
                .padding(.top, FmSpace.x4)
                if let result = model.result {
                    RelayNoticeView(notice: RelayNotice(message: result.message, error: !result.ok))
                        .padding(.top, FmSpace.x3)
                }
                if !model.error.isEmpty {
                    RelayNoticeView(notice: RelayNotice(message: model.error, error: true)).padding(.top, FmSpace.x3)
                }
                VStack(spacing: FmSpace.x3) {
                    if let verified = model.verified {
                        FmFilledButton(
                            "Connect to \(verified.host)", fillWidth: true,
                            onPressed: model.checking ? nil : { Task { await connect() } })
                    } else {
                        FmFilledButton(
                            model.checkLabel, fillWidth: true,
                            onPressed: model.checking || !model.hasHost ? nil : { Task { await model.check() } })
                    }
                    FmOutlinedButton("Not now", fillWidth: true, onPressed: onClose)
                }
                .padding(.top, FmSpace.x4)
            }
            .padding(FmSpace.x4)
            .frame(maxWidth: 480)
            .frame(maxWidth: .infinity)
        }
        .background(fm.canvas.ignoresSafeArea())
        .accessibilityIdentifier("choose-instance")
    }

    private func connect() async {
        let logoUrl: String = model.result?.logoUrl ?? ""
        guard await model.attach() else { return }
        await ProjectImages.shared.adoptInstanceLogo(project.id, logoUrl: logoUrl)
        onClose()
    }
}
