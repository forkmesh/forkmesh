// device_info_plus for AuthService's `defaultDeviceLabel()` (the mobile-node
// bind's "iPhone 15 Pro · iOS 19.0"): FMAccount's DeviceInfo filled from
// UIDevice and the machine identifier.

import FMAccount
import Foundation
import UIKit

enum DeviceInfoProvider {
    /// This phone. Never the user-assigned device name, which can carry a
    /// person's name and is not the relay's business.
    @MainActor
    static func current() -> DeviceInfo {
        let device: UIDevice = UIDevice.current
        let machine: String = machineIdentifier()
        return DeviceInfo(
            platform: "ios",
            modelName: marketingName(machine),
            model: machine.isEmpty ? device.model : machine,
            systemName: device.systemName,
            systemVersion: device.systemVersion,
            vendorId: device.identifierForVendor?.uuidString ?? ""
        )
    }

    /// `utsname.machine` ("iPhone16,1"); on the Simulator, the simulated one.
    static func machineIdentifier() -> String {
        if let simulated = ProcessInfo.processInfo.environment["SIMULATOR_MODEL_IDENTIFIER"], !simulated.isEmpty {
            return simulated
        }
        var info = utsname()
        uname(&info)
        let mirror = Mirror(reflecting: info.machine)
        var text: String = ""
        for child in mirror.children {
            guard let value = child.value as? Int8, value != 0 else { break }
            text.append(Character(UnicodeScalar(UInt8(bitPattern: value))))
        }
        return text
    }

    /// device_info_plus's `modelName` for recent iPhones; "" for anything not
    /// listed (the label then uses the machine identifier).
    static func marketingName(_ machine: String) -> String {
        return names[machine] ?? ""
    }

    private static let names: [String: String] = [
        "iPhone13,1": "iPhone 12 mini",
        "iPhone13,2": "iPhone 12",
        "iPhone13,3": "iPhone 12 Pro",
        "iPhone13,4": "iPhone 12 Pro Max",
        "iPhone14,4": "iPhone 13 mini",
        "iPhone14,5": "iPhone 13",
        "iPhone14,2": "iPhone 13 Pro",
        "iPhone14,3": "iPhone 13 Pro Max",
        "iPhone14,6": "iPhone SE (3rd generation)",
        "iPhone14,7": "iPhone 14",
        "iPhone14,8": "iPhone 14 Plus",
        "iPhone15,2": "iPhone 14 Pro",
        "iPhone15,3": "iPhone 14 Pro Max",
        "iPhone15,4": "iPhone 15",
        "iPhone15,5": "iPhone 15 Plus",
        "iPhone16,1": "iPhone 15 Pro",
        "iPhone16,2": "iPhone 15 Pro Max",
        "iPhone17,1": "iPhone 16 Pro",
        "iPhone17,2": "iPhone 16 Pro Max",
        "iPhone17,3": "iPhone 16",
        "iPhone17,4": "iPhone 16 Plus",
        "iPhone17,5": "iPhone 16e",
        "iPhone18,1": "iPhone 17 Pro",
        "iPhone18,2": "iPhone 17 Pro Max",
        "iPhone18,3": "iPhone 17",
        "iPhone18,4": "iPhone Air",
    ]
}
