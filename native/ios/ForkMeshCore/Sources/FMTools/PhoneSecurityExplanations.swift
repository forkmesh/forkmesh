import Foundation

public enum PhoneSecurityExplanations {
    public static let texts: [String: String] = [
        "ios.passcode":
            "The passcode is what unlocks this iPhone's storage encryption and the Keychain, where ForkMesh keeps your node key and sign-in. Without one, anyone holding the phone can open every app and read its files.",
        "ios.biometrics":
            "Face ID or Touch ID lets you use a long passcode without typing it every time, and lets ForkMesh's vault and autofill ask for your face or finger instead of the master password.",
        "ios.notification-previews":
            "When previews show always, message text and agent replies appear on the lock screen, where anyone near the phone can read them without unlocking it.",
        "ios.jailbreak-apps":
            "Package managers such as Cydia, Sileo and Zebra exist only on jailbroken phones. A jailbreak turns off the protections that keep each app's files and keys private, so other software can read ForkMesh's keys and messages.",
        "ios.jailbreak-tools":
            "A stock iPhone has no shell, SSH server or apt. These files are left by a jailbreak and let software and remote users run commands outside the app sandbox.",
        "ios.rootless-jailbreak":
            "Modern jailbreaks such as Dopamine and palera1n keep their files under /var/jb or the preboot volume instead of the system folders. Their presence means the phone's protections can be switched off.",
        "ios.tweak-frameworks":
            "Tweak frameworks inject code into every app as it starts. They are how jailbreak tweaks change app behaviour, and how malicious tweaks read what apps show and store.",
        "ios.injected-libraries":
            "These are the libraries loaded into ForkMesh right now. A hooking framework, Frida or an SSL-unpinning tweak inside the app can read keys, messages and network traffic as ForkMesh uses them.",
        "ios.dyld-environment":
            "These variables tell the system loader to add extra libraries to an app. Xcode sets some while debugging; on an installed app they mean someone is injecting code.",
        "ios.sandbox":
            "Every iPhone app is confined to its own folder. If ForkMesh can write outside it, the sandbox is off and other apps can reach ForkMesh's files too.",
        "ios.system-links":
            "On a stock iPhone these system folders are real folders. Jailbreaks replace them with links to move files onto the larger data volume.",
        "ios.debugger":
            "A debugger can read and change everything in ForkMesh's memory, including keys and decrypted messages. It is normal while developing the app and not otherwise.",
        "ios.simulator":
            "The iOS Simulator runs on a Mac without the phone's secure hardware, so keys are not protected the way they are on a real iPhone.",
        "ios.os-version":
            "Apple fixes security holes in each iOS update and gives the newest version all fixes first. Older versions get only some fixes, or none.",
        "ios.install-source":
            "Apps from the App Store or TestFlight are signed and checked by Apple. A development or ad hoc build is signed by a developer certificate and may not be the released ForkMesh.",
        "ios.debug-build":
            "Debug builds include extra logging and checks for developers and are not what ForkMesh ships to the App Store.",
        "ios.data-protection":
            "iOS encrypts each app file with a key tied to your passcode. ForkMesh's files are protected at least until the phone is first unlocked after a restart; without protection they can be read from a locked phone.",
        "ios.transport-security":
            "App Transport Security makes every connection the app opens use HTTPS. Exceptions let some connections go unencrypted, where anyone on the network can read or change them.",
        "ios.relay-https":
            "The relay carries your chats, pings and agent traffic. Over plain HTTP anyone on the same Wi-Fi or network path can read or change it; local development relays are the exception.",
        "ios.screen-capture":
            "While the screen is recorded, mirrored with AirPlay or shared on a call, whatever ForkMesh shows, including vault passwords and messages, is visible to the recording or the other screen.",
        "ios.http-proxy":
            "A proxy sees every web request the phone makes. One you or your organisation set up is fine; an unexpected one, often added by a configuration profile, can watch traffic.",
        "ios.vpn":
            "A VPN carries the phone's traffic through another network. This shows whether one is on; whether you need it is up to you.",
        "ios.permission.camera":
            "With camera access ForkMesh can take photos and video while it is open. Turn it off in Settings if you do not use the features that need it.",
        "ios.permission.microphone":
            "With microphone access ForkMesh can record audio while it is open. Turn it off in Settings if you do not use the features that need it.",
        "ios.permission.photos":
            "Full photo library access lets ForkMesh read every photo and video on the phone. Limited access, where you pick photos one at a time, is enough for attachments.",
        "ios.permission.speech":
            "Speech recognition sends audio to Apple's servers, or handles it on the phone where supported, to turn it into text.",
        "ios.permission.bluetooth":
            "Bluetooth access lets ForkMesh find and talk to nearby devices. It never pairs on its own, but you can turn it off if you do not use the features that need it.",
        "ios.permission.motion":
            "Motion and fitness access lets ForkMesh read step counts and movement from the phone's sensors.",
    ]

    public static func text(for id: String) -> String? { texts[id] }
}
