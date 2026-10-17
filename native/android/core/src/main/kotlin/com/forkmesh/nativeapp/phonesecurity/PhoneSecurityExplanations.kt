package com.forkmesh.nativeapp.phonesecurity

object PhoneSecurityExplanations {
    private val texts: Map<String, String> = mapOf(
        "android.screen-lock" to
            "A screen lock is the PIN, pattern or password that unlocks the phone. Android encrypts your storage with a key tied to it, so without a lock anyone holding the phone can open ForkMesh, read your chats and use your node identity.",
        "android.biometrics" to
            "Fingerprint or face unlock lets you unlock with your body instead of typing the PIN every time. It makes a long PIN practical, and ForkMesh can use it to confirm sensitive actions such as opening the vault.",
        "android.lock-timeout" to
            "The screen timeout is how long the phone waits before the screen turns off and locks. A long timeout leaves an unlocked phone open to anyone who picks it up while you are away.",
        "android.lock-notifications" to
            "Android can show the full text of notifications on the lock screen. Anyone who glances at a locked phone can then read chat messages, sign-in codes and pings without unlocking it.",
        "android.stay-awake" to
            "Stay awake is a developer option that keeps the screen on and unlocked while the phone charges. A phone left on a charger then never locks.",
        "android.su-binary" to
            "The su program gives an app full administrator (root) rights over the phone. It is not part of a normal Android phone; it appears when the phone has been rooted. With root, any app granted it can read ForkMesh's private files, including the node key.",
        "android.root-apps" to
            "Root managers such as Magisk, SuperSU, KernelSU and APatch hand out root rights to other apps. Their presence means the phone is rooted or was rooted, and Android's app sandbox no longer keeps ForkMesh's data private.",
        "android.magisk-files" to
            "Magisk, KernelSU and APatch keep their modules and settings in folders a normal phone does not have. Finding them means a root framework is or was installed, even if its app is hidden.",
        "android.busybox" to
            "BusyBox is a bundle of Unix command-line tools. Phones do not ship it in their system folders; it is usually installed alongside root so that scripts can change the system.",
        "android.test-keys" to
            "Every Android build is signed by its maker. Official builds use private release keys; test-keys are public keys anyone can sign with, so a system built with them cannot prove it came from your phone's maker.",
        "android.build-type" to
            "Phones ship a user build. userdebug and eng builds are for developers: they allow root access over USB, relax security checks and are usually custom or pre-release firmware.",
        "android.system-rw" to
            "Android mounts its system partitions read-only so apps cannot change the operating system. A writable system partition means something has remounted it, which normally requires root.",
        "android.hooking-frameworks" to
            "Xposed, LSPosed and EdXposed let modules rewrite how any app behaves while it runs. A module could read what you type into ForkMesh, change what it shows, or copy its keys.",
        "android.injected-libraries" to
            "ForkMesh lists the code libraries loaded into its own process. Libraries from Frida, Substrate or Xposed are not part of Android or ForkMesh; they mean another tool is hooking into the app and can read or change it.",
        "android.frida-server" to
            "Frida is a toolkit for inspecting and changing running apps. Its server runs with root on the phone and lets a connected computer attach to any app, including ForkMesh.",
        "android.selinux" to
            "SELinux is the Android security layer that limits what each app and system service may touch. When it is not enforcing, the walls between apps are much weaker, and this almost always means a custom or rooted system.",
        "android.emulator" to
            "An emulator is a virtual phone running on a computer. The computer's owner can read everything inside it, so it is fine for testing but should not hold a real account's keys.",
        "android.debugger" to
            "A debugger attached to ForkMesh can pause the app and read or change anything in its memory, including decrypted messages and keys. Developers attach one on purpose; nobody else should.",
        "android.app-debuggable" to
            "A debuggable build of ForkMesh lets anyone with a USB cable inspect its memory and files. Store releases are never debuggable; a debuggable copy is a development build.",
        "android.install-source" to
            "Android records which app store installed each app. An app store checks the developer's signature on every update; a copy installed from a file or by another app has had no such check.",
        "android.cleartext" to
            "Cleartext means unencrypted network traffic. If ForkMesh allowed it, someone on the same Wi-Fi could read or change what it sends and receives.",
        "android.relay-https" to
            "The relay is the ForkMesh server this phone connects to for chat, sync and calls. An encrypted (https or wss) address keeps the connection private; a plain address is only safe when it points at this phone or your local network for testing.",
        "android.hardware-keystore" to
            "Android can keep encryption keys inside a separate secure chip (StrongBox) or a protected area of the processor. Keys stored there cannot be copied out, even by malware with root, so ForkMesh's stored secrets stay on this phone.",
        "android.patch-level" to
            "The security patch level is the date of the latest Android security fixes installed on the phone. Every month fixes flaws that attackers already know about, so an old patch level leaves those flaws open.",
        "android.os-version" to
            "Each Android version adds protections and only recent versions keep receiving security fixes. An old version stops getting fixes for newly found flaws.",
        "android.encryption" to
            "Storage encryption scrambles everything saved on the phone so it cannot be read without your screen lock. Per-user (file-based) encryption is the strongest; a default key protects against little because the key is not tied to your lock.",
        "android.developer-options" to
            "Developer options is a hidden settings menu for app developers. It holds switches such as USB debugging that weaken security; most people never need it on.",
        "android.usb-debugging" to
            "USB debugging lets a connected computer install apps, read files and control the phone over a cable. A computer you plug into, such as a public charging station, could use it.",
        "android.wireless-debugging" to
            "Wireless debugging gives the same control as USB debugging over Wi-Fi. Anyone on the same network who pairs with the phone can install apps and read data.",
        "android.auto-time" to
            "Secure connections check certificates against the current date. With automatic time off, a wrong clock can make expired or not-yet-valid certificates look fine, and sign-in codes stop matching.",
        "android.http-proxy" to
            "A proxy sends web traffic through another computer first. A proxy you did not set up can watch which sites you visit and, with an added certificate, read encrypted traffic.",
        "android.vpn" to
            "A VPN sends all of the phone's traffic through a tunnel to another network. It hides your traffic from the local Wi-Fi, but the VPN provider can see where it goes.",
        "android.user-certificates" to
            "Certificate authorities decide which secure websites the phone trusts. One you or an app added can be used to impersonate any website or server and read encrypted traffic, unless an app such as ForkMesh refuses user-added certificates.",
        "android.private-dns" to
            "DNS turns names such as forkmesh.com into addresses. Private DNS encrypts those lookups so the network you are on cannot see or change which servers you contact.",
        "android.accessibility-services" to
            "Accessibility services help people use the phone by reading the screen and acting for them. That power means a service can read everything on screen, including chats and passwords, and tap buttons on your behalf.",
        "android.notification-listeners" to
            "Apps with notification access read every notification as it arrives, including ForkMesh messages, sign-in codes and pings from other apps.",
        "android.device-admins" to
            "Device admin apps can lock or wipe the phone, enforce password rules and resist being uninstalled. Work profiles and find-my-phone tools use this; anything else should be questioned.",
        "android.keyboard" to
            "The keyboard sees every character you type, including passwords and messages. Keyboards that come with the phone or from large vendors are widely checked; others may send what you type elsewhere.",
        "android.permission.camera" to
            "Camera access lets ForkMesh take photos and video. Android shows a green dot whenever an app uses the camera.",
        "android.permission.microphone" to
            "Microphone access lets ForkMesh record sound. Android shows a green dot whenever an app uses the microphone.",
        "android.permission.nearby" to
            "Nearby devices access lets ForkMesh scan for and connect to Bluetooth devices around you. It does not reveal your location.",
        "android.permission.notifications" to
            "Notification permission lets ForkMesh alert you about calls, pings and messages. Without it the phone stays silent when someone calls.",
    )

    fun text(id: String): String = texts[id].orEmpty()

    val ids: Set<String> get() = texts.keys
}
