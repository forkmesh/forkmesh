#!/usr/bin/env python3
import subprocess
import sys
from pathlib import Path

SOURCE_ANCHOR = '        "objc/api/peerconnection/RTCRtpSender.mm",\n'
SOURCE_LINES = (
    '        "objc/api/peerconnection/RTCFmFrameCryptor.h",\n'
    '        "objc/api/peerconnection/RTCFmFrameCryptor.mm",\n'
    '        "objc/api/peerconnection/RTCFmVoice.h",\n'
    '        "objc/api/peerconnection/RTCFmVoice.mm",\n'
)
DEPS_ANCHOR = '        "../api/crypto:frame_encryptor_interface",\n'
DEPS_LINE = (
    '        "//fm:fm_frame_crypto",\n'
    '        "../modules/audio_processing:audio_buffer",\n'
)
HEADER_ANCHOR = '          "objc/api/peerconnection/RTCRtpSender.h",\n'
HEADER_LINE = (
    '          "objc/api/peerconnection/RTCFmFrameCryptor.h",\n'
    '          "objc/api/peerconnection/RTCFmVoice.h",\n'
)
TARGET = 'rtc_library("peerconnectionfactory_base_objc") {'


def patch_sdk(text):
    start = text.index(TARGET)
    end = text.index("\n    }\n", start)
    body = text[start:end]
    if body.count(SOURCE_ANCHOR) != 1 or body.count(DEPS_ANCHOR) != 1:
        raise SystemExit("peerconnectionfactory_base_objc changed shape; update make_patches.py")
    body = body.replace(SOURCE_ANCHOR, SOURCE_ANCHOR + SOURCE_LINES, 1)
    body = body.replace(DEPS_ANCHOR, DEPS_ANCHOR + DEPS_LINE, 1)
    text = text[:start] + body + text[end:]
    if text.count(HEADER_ANCHOR) < 1:
        raise SystemExit("framework header lists changed shape; update make_patches.py")
    return text.replace(HEADER_ANCHOR, HEADER_ANCHOR + HEADER_LINE)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: make_patches.py <libwebrtc src> <patch output>")
    src = Path(sys.argv[1])
    out = Path(sys.argv[2])
    gn = src / "sdk" / "BUILD.gn"
    subprocess.run(["git", "-C", str(src), "checkout", "--", "sdk/BUILD.gn"], check=True)
    gn.write_text(patch_sdk(gn.read_text()))
    diff = subprocess.run(
        ["git", "-C", str(src), "diff", "--", "sdk/BUILD.gn"], check=True, capture_output=True, text=True).stdout
    subprocess.run(["git", "-C", str(src), "checkout", "--", "sdk/BUILD.gn"], check=True)
    out.write_text(diff)
    print("wrote", out, len(diff.splitlines()), "lines")


if __name__ == "__main__":
    main()
