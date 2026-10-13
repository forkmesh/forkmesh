import CoreTransferable
import FMTools
import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct SugarContractImage: Transferable {
    let png: Data
    let name: String

    static var transferRepresentation: some TransferRepresentation {
        DataRepresentation(exportedContentType: .png) { (image: SugarContractImage) in image.png }
            .suggestedFileName { (image: SugarContractImage) in image.name }
    }
}

struct SugarContractSheet: View {
    @Environment(\.fmPalette) private var fm
    @State private var contract: SugarContract
    @State private var picture: UIImage?

    init(contract: SugarContract) {
        _contract = State(initialValue: contract)
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text(SugarContract.title)
                    .fmText(.titleMedium)
                preview
                FmCard {
                    VStack(alignment: .leading, spacing: FmSpace.x2) {
                        DatePicker("From", selection: startBinding, displayedComponents: .date)
                            .accessibilityIdentifier("sugar-contract-start")
                        DatePicker("Through", selection: endBinding, in: startDate..., displayedComponents: .date)
                            .accessibilityIdentifier("sugar-contract-end")
                        Stepper(value: $contract.signers, in: SugarContract.signerRange) {
                            Text(contract.signers == 1 ? "1 signature" : "\(contract.signers) signatures")
                                .fmText(.bodyLarge)
                        }
                        .accessibilityIdentifier("sugar-contract-signers")
                    }
                    .tint(fm.accent)
                }
                FmField(label: "Names under the lines (optional)") {
                    VStack(spacing: FmSpace.x2) {
                        ForEach(0..<contract.signers, id: \.self) { (index: Int) in
                            FmTextField(text: nameBinding(index), hint: "Signer \(index + 1)")
                                .textInputAutocapitalization(.words)
                                .accessibilityIdentifier("sugar-contract-name-\(index)")
                        }
                    }
                }
                shareButton
            }
            .padding(FmSpace.x4)
        }
        .fmSheetStyle(fitsContent: false)
        .onAppear { picture = SugarContractCard.render(contract) }
        .onChange(of: contract) { (_: SugarContract, next: SugarContract) in
            picture = SugarContractCard.render(next)
        }
    }

    @ViewBuilder private var preview: some View {
        if let picture {
            Image(uiImage: picture)
                .resizable()
                .scaledToFit()
                .frame(maxWidth: .infinity)
                .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
                .shadow(color: Color.black.opacity(0.18), radius: 10, x: 0, y: 4)
                .accessibilityLabel("\(SugarContract.title), \(contract.dateLine), \(contract.signers) signature lines")
                .accessibilityIdentifier("sugar-contract-preview")
        } else {
            FmCircularProgressIndicator()
                .frame(maxWidth: .infinity, minHeight: 240)
        }
    }

    @ViewBuilder private var shareButton: some View {
        if let picture, let png = picture.pngData() {
            ShareLink(
                item: SugarContractImage(png: png, name: contract.fileName),
                subject: Text(SugarContract.title),
                message: Text(contract.message),
                preview: SharePreview(SugarContract.title, image: Image(uiImage: picture))
            ) {
                HStack(spacing: FmSpace.x2) {
                    FmIcon("ios_share", size: 18, color: fm.onPrimary)
                    Text("Send contract")
                        .font(Font.system(size: 17, weight: .semibold))
                }
                .foregroundStyle(fm.onPrimary)
                .frame(maxWidth: .infinity, minHeight: 52)
                .background(fm.success)
                .clipShape(Capsule())
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("sugar-contract-share")
        }
    }

    private var startDate: Date { CaloriesText.parseDay(contract.startDay) }

    private var startBinding: Binding<Date> {
        Binding<Date>(
            get: { CaloriesText.parseDay(contract.startDay) },
            set: { (date: Date) in contract.setStart(CaloriesText.isoDay(date)) })
    }

    private var endBinding: Binding<Date> {
        Binding<Date>(
            get: { CaloriesText.parseDay(contract.endDay) },
            set: { (date: Date) in contract.setEnd(CaloriesText.isoDay(date)) })
    }

    private func nameBinding(_ index: Int) -> Binding<String> {
        Binding<String>(
            get: { contract.names.indices.contains(index) ? contract.names[index] : "" },
            set: { (name: String) in contract.setName(index, name) })
    }
}

struct SugarContractCard: View {
    let contract: SugarContract

    static let width: CGFloat = 360
    static let paper = Color(red: 0.984, green: 0.969, blue: 0.933)
    static let ink = Color(red: 0.122, green: 0.165, blue: 0.141)
    static let green = Color(red: 0.184, green: 0.490, blue: 0.357)
    static let muted = Color(red: 0.42, green: 0.478, blue: 0.44)

    @MainActor
    static func render(_ contract: SugarContract) -> UIImage? {
        let renderer = ImageRenderer(content: SugarContractCard(contract: contract).environment(\.colorScheme, .light))
        renderer.scale = 3
        renderer.isOpaque = true
        return renderer.uiImage
    }

    var body: some View {
        VStack(spacing: 14) {
            brandRule
            seal
            Text(SugarContract.title)
                .font(Font.system(size: 28, weight: .bold, design: .serif))
                .foregroundStyle(Self.ink)
            Text("\(contract.dateLine)  ·  \(contract.lengthLine)")
                .font(Font.system(size: 13, weight: .semibold))
                .foregroundStyle(Self.green)
                .padding(.horizontal, 12)
                .padding(.vertical, 5)
                .background(Capsule().fill(Self.green.opacity(0.12)))
            Text(contract.terms)
                .font(Font.system(size: 14, design: .serif))
                .foregroundStyle(Self.ink)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
            Text(SugarContract.fineprint)
                .font(Font.system(size: 10.5, weight: .medium))
                .foregroundStyle(Self.muted)
            signatures
                .padding(.top, 4)
            Text("Signed with ForkMesh Sugar Break")
                .font(Font.system(size: 8.5, weight: .medium))
                .tracking(0.6)
                .foregroundStyle(Self.muted.opacity(0.8))
                .padding(.top, 2)
        }
        .padding(.horizontal, 30)
        .padding(.vertical, 32)
        .frame(width: Self.width)
        .background(Self.paper)
        .overlay {
            Rectangle()
                .inset(by: 10)
                .stroke(Self.green.opacity(0.75), lineWidth: 1.5)
        }
        .overlay {
            Rectangle()
                .inset(by: 14)
                .stroke(Self.green.opacity(0.35), lineWidth: 0.75)
        }
    }

    private var brandRule: some View {
        HStack(spacing: 10) {
            Rectangle().fill(Self.green.opacity(0.45)).frame(height: 0.75)
            Text(SugarContract.brand)
                .font(Font.system(size: 9.5, weight: .bold))
                .tracking(2.4)
                .foregroundStyle(Self.green)
                .fixedSize()
            Rectangle().fill(Self.green.opacity(0.45)).frame(height: 0.75)
        }
    }

    private var seal: some View {
        ZStack {
            Circle()
                .fill(Self.green)
            Circle()
                .inset(by: 4)
                .stroke(Self.paper.opacity(0.85), style: StrokeStyle(lineWidth: 1, dash: [2, 2]))
            VStack(spacing: 0) {
                Text("NO")
                    .font(Font.system(size: 11, weight: .heavy))
                Text("SUGAR")
                    .font(Font.system(size: 9, weight: .heavy))
                    .tracking(0.8)
            }
            .foregroundStyle(Self.paper)
        }
        .frame(width: 56, height: 56)
        .rotationEffect(Angle.degrees(-8))
    }

    private var rows: [[SugarContractLine]] {
        let lines = contract.lines
        let perRow = lines.count == 1 ? 1 : 2
        return stride(from: 0, to: lines.count, by: perRow).map { (start: Int) in
            Array(lines[start..<min(start + perRow, lines.count)])
        }
    }

    private var signatures: some View {
        let rows: [[SugarContractLine]] = self.rows
        return VStack(spacing: 18) {
            ForEach(0..<rows.count, id: \.self) { (index: Int) in
                HStack(alignment: .bottom, spacing: 22) {
                    ForEach(rows[index]) { (line: SugarContractLine) in
                        signatureLine(line)
                    }
                    if rows[index].count == 1 && contract.signers > 1 {
                        Color.clear.frame(maxWidth: .infinity, maxHeight: 1)
                    }
                }
            }
        }
    }

    private func signatureLine(_ line: SugarContractLine) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(alignment: .bottom, spacing: 4) {
                Text("✕")
                    .font(Font.system(size: 10, weight: .semibold))
                    .foregroundStyle(Self.muted)
                Rectangle()
                    .fill(Self.ink.opacity(0.7))
                    .frame(height: 0.9)
            }
            .frame(height: 30, alignment: .bottom)
            HStack(spacing: 4) {
                Text(line.caption)
                    .font(Font.system(size: 9.5, weight: line.name.isEmpty ? .regular : .semibold))
                    .foregroundStyle(line.name.isEmpty ? Self.muted : Self.ink)
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
                Spacer(minLength: 4)
                Text("Date")
                    .font(Font.system(size: 9.5))
                    .foregroundStyle(Self.muted)
            }
        }
        .frame(maxWidth: .infinity)
    }
}
