import plistlib
import struct
import sys
from pathlib import Path


def symdef_bounds(data: bytearray, path: str) -> tuple[int, int, bool]:
    if data[:8] != b"!<arch>\n":
        raise SystemExit(f"{path}: not an ar archive")
    header = data[8:68]
    name = header[0:16].decode("ascii").strip()
    size = int(header[48:58].decode("ascii").strip())
    start = 68
    if name.startswith("#1/"):
        name_len = int(name[3:])
        name = data[start:start + name_len].rstrip(b"\0").decode("ascii")
        start += name_len
        size -= name_len
    if not name.startswith("__.SYMDEF"):
        raise SystemExit(f"{path}: the first member is {name!r}, not a symbol table")
    return start, start + size, name.startswith("__.SYMDEF_64")


def normalize_archive(path: str) -> None:
    data = bytearray(Path(path).read_bytes())
    start, end, wide = symdef_bounds(data, path)
    word, entry = ("<Q", 16) if wide else ("<I", 8)
    step = struct.calcsize(word)
    ranlib_size = struct.unpack_from(word, data, start)[0]
    entries = start + step
    strtab_size = struct.unpack_from(word, data, entries + ranlib_size)[0]
    strtab = entries + ranlib_size + step
    if strtab + strtab_size > end or ranlib_size % entry:
        raise SystemExit(f"{path}: malformed symbol table")
    used = 0
    for offset in range(entries, entries + ranlib_size, entry):
        strx = struct.unpack_from(word, data, offset)[0]
        used = max(used, data.index(0, strtab + strx) + 1 - strtab)
    data[strtab + used:end] = bytes(end - strtab - used)
    Path(path).write_bytes(bytes(data))


def normalize_plist(path: str) -> None:
    with open(path, "rb") as handle:
        info = plistlib.load(handle)
    info["AvailableLibraries"].sort(key=lambda library: library["LibraryIdentifier"])
    with open(path, "wb") as handle:
        plistlib.dump(info, handle, fmt=plistlib.FMT_XML, sort_keys=True)


def main() -> None:
    if len(sys.argv) < 3 or sys.argv[1] not in ("archive", "plist"):
        raise SystemExit("usage: normalize.py archive|plist <path>...")
    for path in sys.argv[2:]:
        (normalize_archive if sys.argv[1] == "archive" else normalize_plist)(path)


if __name__ == "__main__":
    main()
