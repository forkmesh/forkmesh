import Foundation

public enum PhoneSecurityPolicy {
    public static let newestIOSMajor: Int = 26
}

public enum PhoneSecurityPaths {
    public static let jailbreakApps: [String] = [
        "/Applications/Cydia.app",
        "/Applications/Sileo.app",
        "/Applications/Zebra.app",
        "/Applications/Installer.app",
        "/Applications/Filza.app",
        "/Applications/blackra1n.app",
        "/Applications/FakeCarrier.app",
        "/Applications/Icy.app",
        "/Applications/IntelliScreen.app",
        "/Applications/SBSettings.app",
        "/Applications/Dopamine.app",
        "/Applications/palera1nLoader.app",
        "/Applications/TrollStore.app",
        "/var/jb/Applications/Sileo.app",
        "/var/jb/Applications/Zebra.app",
        "/var/jb/Applications/Filza.app",
    ]

    public static let jailbreakTools: [String] = [
        "/bin/bash",
        "/usr/sbin/sshd",
        "/usr/bin/ssh",
        "/usr/libexec/sftp-server",
        "/usr/libexec/ssh-keysign",
        "/etc/ssh/sshd_config",
        "/etc/apt",
        "/private/var/lib/apt",
        "/private/var/lib/cydia",
        "/private/var/cache/apt",
        "/private/var/stash",
        "/usr/bin/cycript",
        "/usr/local/bin/cycript",
        "/usr/lib/libcycript.dylib",
    ]

    public static let rootlessJailbreak: [String] = [
        "/var/jb",
        "/private/var/jb",
        "/private/preboot/jb",
        "/var/binpack",
        "/private/var/binpack",
    ]

    public static let tweakFrameworks: [String] = [
        "/Library/MobileSubstrate/MobileSubstrate.dylib",
        "/Library/MobileSubstrate/DynamicLibraries",
        "/usr/lib/substitute-inserter.dylib",
        "/usr/lib/libsubstitute.dylib",
        "/usr/lib/substrate",
        "/usr/lib/TweakInject",
        "/usr/lib/libhooker.dylib",
        "/usr/lib/ellekit",
        "/var/jb/usr/lib/libellekit.dylib",
        "/var/jb/usr/lib/TweakInject",
    ]

    public static let systemLinks: [String] = [
        "/Applications",
        "/Library/Ringtones",
        "/Library/Wallpaper",
        "/usr/arm-apple-darwin9",
        "/usr/include",
        "/usr/libexec",
        "/usr/share",
    ]

    public static var probed: [String] { jailbreakApps + jailbreakTools + rootlessJailbreak + tweakFrameworks }

    public static let injectedImageMarkers: [String] = [
        "mobilesubstrate",
        "substrateloader",
        "substrateinserter",
        "substratebootstrap",
        "libsubstitute",
        "substitute-inserter",
        "libhooker",
        "tweakinject",
        "ellekit",
        "fridagadget",
        "frida-agent",
        "frida-gadget",
        "libfrida",
        "cynject",
        "libcycript",
        "sslkillswitch",
        "shadow.dylib",
        "libertylite",
        "abypass",
        "flyjb",
        "choicy",
        "rocketbootstrap",
    ]

    public static let injectionVariables: [String] = [
        "DYLD_INSERT_LIBRARIES",
        "DYLD_LIBRARY_PATH",
        "DYLD_FRAMEWORK_PATH",
        "_MSSafeMode",
        "_SafeMode",
    ]
}

public enum PhoneSecurityCatalog {
    public static let passcode = PhoneSecurityCheck(id: "ios.passcode", title: "Device passcode", category: .lockScreen)
    public static let biometrics = PhoneSecurityCheck(id: "ios.biometrics", title: "Face ID or Touch ID", category: .lockScreen)
    public static let notificationPreviews = PhoneSecurityCheck(
        id: "ios.notification-previews", title: "Notification previews", category: .lockScreen)
    public static let jailbreakApps = PhoneSecurityCheck(id: "ios.jailbreak-apps", title: "Jailbreak apps", category: .deviceIntegrity)
    public static let jailbreakTools = PhoneSecurityCheck(
        id: "ios.jailbreak-tools", title: "Tools left by a jailbreak", category: .deviceIntegrity)
    public static let rootlessJailbreak = PhoneSecurityCheck(
        id: "ios.rootless-jailbreak", title: "Rootless jailbreak folders", category: .deviceIntegrity)
    public static let tweakFrameworks = PhoneSecurityCheck(
        id: "ios.tweak-frameworks", title: "Tweak frameworks on disk", category: .deviceIntegrity)
    public static let injectedLibraries = PhoneSecurityCheck(
        id: "ios.injected-libraries", title: "Code injected into ForkMesh", category: .deviceIntegrity)
    public static let dyldEnvironment = PhoneSecurityCheck(
        id: "ios.dyld-environment", title: "Library injection variables", category: .deviceIntegrity)
    public static let sandbox = PhoneSecurityCheck(id: "ios.sandbox", title: "App sandbox", category: .deviceIntegrity)
    public static let systemLinks = PhoneSecurityCheck(
        id: "ios.system-links", title: "System folders replaced by links", category: .deviceIntegrity)
    public static let debugger = PhoneSecurityCheck(id: "ios.debugger", title: "Debugger attached", category: .deviceIntegrity)
    public static let simulator = PhoneSecurityCheck(id: "ios.simulator", title: "Real device", category: .deviceIntegrity)
    public static let osVersion = PhoneSecurityCheck(id: "ios.os-version", title: "iOS version", category: .system)
    public static let installSource = PhoneSecurityCheck(
        id: "ios.install-source", title: "Where ForkMesh came from", category: .appProtection)
    public static let debugBuild = PhoneSecurityCheck(id: "ios.debug-build", title: "Release build", category: .appProtection)
    public static let dataProtection = PhoneSecurityCheck(
        id: "ios.data-protection", title: "ForkMesh file encryption", category: .appProtection)
    public static let transportSecurity = PhoneSecurityCheck(
        id: "ios.transport-security", title: "Encrypted connections only", category: .appProtection)
    public static let relayHTTPS = PhoneSecurityCheck(
        id: "ios.relay-https", title: "Relay address uses HTTPS", category: .appProtection)
    public static let screenCapture = PhoneSecurityCheck(
        id: "ios.screen-capture", title: "Screen recording or mirroring", category: .appProtection)
    public static let httpProxy = PhoneSecurityCheck(id: "ios.http-proxy", title: "HTTP proxy", category: .network)
    public static let vpn = PhoneSecurityCheck(id: "ios.vpn", title: "VPN", category: .network)

    public static func permission(_ permission: PhonePermission) -> PhoneSecurityCheck {
        PhoneSecurityCheck(id: "ios.permission.\(permission.rawValue)", title: permissionTitle(permission), category: .privacy)
    }

    public static func permissionTitle(_ permission: PhonePermission) -> String {
        switch permission {
        case .camera: return "Camera access"
        case .microphone: return "Microphone access"
        case .photos: return "Photo library access"
        case .speech: return "Speech recognition"
        case .bluetooth: return "Bluetooth access"
        case .motion: return "Motion and fitness"
        }
    }

    public static func permissionUse(_ permission: PhonePermission) -> String {
        switch permission {
        case .camera: return "ForkMesh uses the camera for video calls, photos in chats and agent prompts, food photos in Calories, and Face Changer."
        case .microphone: return "ForkMesh uses the microphone for calls, voice messages and dictated prompts."
        case .photos: return "ForkMesh uses the photo library to attach images to chats and agent prompts and to pick a background."
        case .speech: return "ForkMesh uses speech recognition to turn dictated prompts and video captions into text."
        case .bluetooth: return "ForkMesh uses Bluetooth for Glasses Alert and to find nearby mesh nodes."
        case .motion: return "ForkMesh uses motion data to count steps and walks."
        }
    }

    public static var all: [PhoneSecurityCheck] {
        [
            passcode, biometrics, notificationPreviews,
            jailbreakApps, jailbreakTools, rootlessJailbreak, tweakFrameworks, injectedLibraries, dyldEnvironment,
            sandbox, systemLinks, debugger, simulator,
            osVersion,
            installSource, debugBuild, dataProtection, transportSecurity, relayHTTPS, screenCapture,
            httpProxy, vpn,
        ] + PhonePermission.allCases.map(permission)
    }

    public static func check(id: String) -> PhoneSecurityCheck? { all.first { $0.id == id } }
}
