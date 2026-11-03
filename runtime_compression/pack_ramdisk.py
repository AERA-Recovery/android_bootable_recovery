#!/usr/bin/env python3
"""Pack the AERA recovery runtime inside a normal LZ4 boot ramdisk.

Wraps mkbootfs without modifying its input tree. Init, its dependency closure,
early-init executables, shell, ADB and static BusyBox remain directly available.
The original CPIO uid/gid/mode/mtime metadata is retained in the runtime tar.
"""
import argparse
import hashlib
import io
import pathlib
import shlex
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
from dataclasses import dataclass


PAYLOAD = "aera-runtime.tar.xz"
BOOTSTRAP = "aera-runtime-unpack.sh"
# First-stage init detects recovery with access("/system/bin/recovery", F_OK),
# before early-init can unpack anything. Keep the real executable intact, also
# preserving running-root checksum/repacking behavior. Its libraries are only
# needed when the recovery service starts after extraction, not for this check.
RECOVERY = "system/bin/recovery"
PREFIXES = ("system/bin/", "system/lib/", "system/lib64/", "vendor/bin/", "vendor/lib/", "vendor/lib64/",
            "vendor/odm/lib/", "vendor/odm/lib64/", "twres/", "system/etc/terminfo/",
            "aera-device-runtime/", "vendor/aera-stock-lib64/",
            "vendor/aera-audio-abi/", "vendor/odm/firmware/",
            "vendor/odm/etc/acdbdata/", "vendor/etc/acdbdata/",
            "vendor/etc/audio/", "vendor/lib/rfsa/")
BOOT_FILES = {"init", "sbin/busybox", BOOTSTRAP, PAYLOAD,
              "system/bin/init", "system/bin/linker64", "system/bin/linker",
              "system/bin/sh", "system/bin/adbd", "system/bin/logd",
              "system/bin/ueventd", "system/bin/servicemanager",
              "system/bin/hwservicemanager", "system/bin/vndservicemanager"}


@dataclass
class Entry:
    name: str
    fields: list
    data: bytes


def parse_cpio(data):
    entries, seen, offset = [], set(), 0
    while offset + 110 <= len(data):
        header = data[offset:offset + 110]
        if header[:6] not in (b"070701", b"070702"):
            raise ValueError("Invalid newc header at byte %d" % offset)
        f = [int(header[6 + i * 8:14 + i * 8], 16) for i in range(13)]
        if not 1 <= f[11] <= 4096 or offset + 110 + f[11] > len(data):
            raise ValueError("Invalid CPIO filename length")
        raw_name = data[offset + 110:offset + 110 + f[11]]
        if raw_name[-1:] != b"\0":
            raise ValueError("Unterminated CPIO filename")
        name = raw_name[:-1].decode("utf-8")
        offset = (offset + 110 + f[11] + 3) & ~3
        if offset + f[6] > len(data):
            raise ValueError("Truncated CPIO entry: " + name)
        body = data[offset:offset + f[6]]
        offset = (offset + f[6] + 3) & ~3
        if name == "TRAILER!!!":
            entries.append(Entry(name, f, body))
            return entries
        canonical = name.removeprefix("./")
        if canonical.startswith("/") or ".." in pathlib.PurePosixPath(canonical).parts:
            raise ValueError("Unsafe CPIO path: " + name)
        if canonical in seen:
            raise ValueError("Duplicate CPIO path: " + name)
        seen.add(canonical)
        entries.append(Entry(canonical, f, body))
    raise ValueError("CPIO trailer not found")


def encode_cpio(entries):
    out = bytearray()
    for e in entries:
        fields = e.fields.copy()
        name = e.name.encode("utf-8") + b"\0"
        fields[6], fields[11], fields[12] = len(e.data), len(name), 0
        out += b"070701" + b"".join(("%08x" % x).encode() for x in fields) + name
        out += b"\0" * (-len(out) % 4)
        out += e.data
        out += b"\0" * (-len(out) % 4)
    out += b"\0" * (-len(out) % 512)
    return bytes(out)


def elf_needed(data):
    if data[:4] != b"\x7fELF":
        return []
    if data[5] != 1:
        raise ValueError("Only little-endian Android ELF is supported")
    if data[4] == 2:
        offset = struct.unpack_from("<Q", data, 40)[0]
        entsize, count = struct.unpack_from("<HH", data, 58)
        fmt, dynfmt = "<IIQQQQIIQQ", "<qQ"
    elif data[4] == 1:
        offset = struct.unpack_from("<I", data, 32)[0]
        entsize, count = struct.unpack_from("<HH", data, 46)
        fmt, dynfmt = "<IIIIIIIIII", "<iI"
    else:
        raise ValueError("Unknown ELF class")
    if not count:
        raise ValueError("Bootstrap ELF is missing section headers")
    sections = [struct.unpack_from(fmt, data, offset + i * entsize) for i in range(count)]
    result = []
    for section in sections:
        if section[1] != 6:  # SHT_DYNAMIC
            continue
        strings = sections[section[6]]
        table = data[strings[4]:strings[4] + strings[5]]
        size = struct.calcsize(dynfmt)
        for pos in range(section[4], section[4] + section[5], size):
            tag, value = struct.unpack_from(dynfmt, data, pos)
            if tag == 1:
                end = table.index(0, value)
                result.append(table[value:end].decode())
    return result


def protected_paths(files):
    protect = set(BOOT_FILES)
    services, early_starts, early_execs = {}, set(), set()
    for name, entry in files.items():
        if not name.endswith(".rc") or not stat.S_ISREG(entry.fields[1]):
            continue
        early = False
        for line in entry.data.decode(errors="replace").splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            try:
                words = shlex.split(stripped, comments=True)
            except ValueError:
                continue
            if not words:
                continue
            if not line[:1].isspace():
                early = words[:2] == ["on", "early-init"]
                if words[0] == "service" and len(words) >= 3:
                    services[words[1]] = words[2].lstrip("/")
                continue
            if early and words[0] in ("start", "restart") and len(words) > 1:
                early_starts.add(words[1])
            if early and words[0] in ("exec", "exec_background", "exec_start"):
                if words[0] == "exec_start" and len(words) > 1:
                    early_starts.add(words[1])
                else:
                    absolute = [w for w in words[1:] if w.startswith("/")]
                    if absolute:
                        early_execs.add(absolute[0].lstrip("/"))
    protect.update(services[s] for s in early_starts if s in services)
    protect.update(early_execs)
    # Init can dlopen these independently of its DT_NEEDED list.
    protect.update({"system/lib64/libprocessgroup_setup.so",
                    "system/lib64/libdl_android.so", "system/lib64/libgsi.so",
                    "system/lib/libprocessgroup_setup.so", "system/lib/libdl_android.so",
                    "system/lib/libgsi.so"})
    by_basename = {}
    for name in files:
        if name.endswith(".so"):
            by_basename.setdefault(pathlib.PurePosixPath(name).name, []).append(name)
    pending, visited = list(protect), set()
    while pending:
        name = pending.pop()
        if name in visited or name not in files:
            continue
        visited.add(name)
        e = files[name]
        if stat.S_ISLNK(e.fields[1]):
            target = e.data.decode()
            if target.startswith("/"):
                dest = target[1:]
            else:
                parts = []
                for part in (pathlib.PurePosixPath(name).parent / target).parts:
                    if part == "..":
                        if parts:
                            parts.pop()
                    elif part != ".":
                        parts.append(part)
                dest = "/".join(parts)
            protect.add(dest)
            pending.append(dest)
        elif stat.S_ISREG(e.fields[1]):
            for dep in elf_needed(e.data):
                matches = by_basename.get(dep, [])
                if not matches:
                    raise ValueError("Missing bootstrap dependency %s required by %s" % (dep, name))
                protect.update(matches)
                pending.extend(matches)
    return protect


def prepare_bootstrap(entries):
    """Add boot-only files to the CPIO stream, never the incremental OUT tree."""
    files = {e.name: e for e in entries}
    if PAYLOAD in files:
        raise ValueError("Input already contains a packed runtime")
    inode = max(e.fields[0] for e in entries) + 1

    def put(name, data, mode):
        nonlocal inode
        if name in files:
            entry = files[name]
            if not stat.S_ISREG(entry.fields[1]):
                raise ValueError("Bootstrap path is not a regular file: " + name)
            entry.data = data
            entry.fields[1] = stat.S_IFREG | mode
        else:
            fields = [inode, stat.S_IFREG | mode, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0]
            inode += 1
            entry = Entry(name, fields, data)
            entries.insert(-1, entry)
            files[name] = entry

    source = pathlib.Path(__file__).resolve().parent
    put(BOOTSTRAP, (source / BOOTSTRAP).read_bytes(), 0o755)
    init = files.get("system/etc/init/hw/init.rc")
    if not init or not stat.S_ISREG(init.fields[1]):
        raise ValueError("Recovery init.rc is missing; cannot order runtime extraction safely")
    rc = "init.recovery.aera-runtime.rc"
    directive = ("import /" + rc + "\n").encode()
    if directive in init.data:
        raise ValueError("Recovery init.rc already imports the runtime bootstrap")
    # Imported actions are registered in parse order: extract before every
    # device early-init action, including its ABI selector and HAL startup.
    init.data = directive + init.data
    put(rc, (source / rc).read_bytes(), 0o644)
    busybox = files.get("sbin/busybox")
    if not busybox or busybox.data[:4] != b"\x7fELF" or elf_needed(busybox.data):
        native_init = files.get("system/bin/init")
        machine = struct.unpack_from("<H", native_init.data, 18)[0] if native_init else 0
        arch = {183: "arm64", 40: "arm"}.get(machine)
        if not arch:
            raise ValueError("No static BusyBox available for this recovery architecture")
        prebuilt = next((parent / "vendor/recovery/prebuilt" / arch / "busybox"
                         for parent in source.parents
                         if (parent / "vendor/recovery/prebuilt" / arch / "busybox").is_file()), None)
        if not prebuilt:
            raise ValueError("AERA static BusyBox prebuilt is missing")
        put("sbin/busybox", prebuilt.read_bytes(), 0o755)
        busybox = files["sbin/busybox"]
    if busybox.data[:4] != b"\x7fELF" or elf_needed(busybox.data):
        raise ValueError("Runtime unpacking requires static BusyBox")
    return files


def transform(data):
    entries = parse_cpio(data)
    files = prepare_bootstrap(entries)
    protect = protected_paths(files)
    recovery = files.get(RECOVERY)
    if not recovery or not stat.S_ISREG(recovery.fields[1]) or not recovery.data:
        raise ValueError("First-stage recovery detection requires the native recovery executable")
    selected = [e for e in entries if e.name.startswith(PREFIXES)
                and e.name not in protect and e.name != RECOVERY and stat.S_ISREG(e.fields[1])
                and e.fields[4] == 1 and e.data]
    def compression_order(entry):
        if entry.name.endswith('.ko') and entry.name.startswith('aera-device-runtime/'):
            parts = pathlib.PurePosixPath(entry.name).parts
            module = parts[2] if parts[1] == 'variants' else pathlib.PurePosixPath(entry.name).stem
            return (0, module.replace('-', '_'), entry.name)
        return (1, '', '')
    # Adjacent ABI variants share instruction/data bytes within XZ's dictionary.
    selected.sort(key=compression_order)
    tar_data = io.BytesIO()
    with tarfile.open(fileobj=tar_data, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for e in selected:
            info = tarfile.TarInfo(e.name)
            info.mode = stat.S_IMODE(e.fields[1])
            info.uid, info.gid, info.mtime = e.fields[2], e.fields[3], e.fields[5]
            info.size = len(e.data)
            tar.addfile(info, io.BytesIO(e.data))
    # Android's hermetic Python omits _lzma. Use its checked-in host XZ tool
    # instead; a PATH fallback also permits standalone packaging tests.
    xz = next((parent / "prebuilts/build-tools/linux-x86/bin/xz"
               for parent in pathlib.Path(__file__).resolve().parents
               if (parent / "prebuilts/build-tools/linux-x86/bin/xz").is_file()), None)
    if xz is None:
        xz = shutil.which("xz")
    if not xz:
        raise ValueError("Host XZ tool is missing")
    payload = subprocess.run([str(xz), "-9", "--threads=1", "--check=crc64", "-c"],
                             input=tar_data.getvalue(), stdout=subprocess.PIPE,
                             check=True).stdout
    chosen = {e.name for e in selected}
    result = [e for e in entries if e.name not in chosen and e.name != "TRAILER!!!"]
    inode = max(e.fields[0] for e in entries) + 1
    fields = [inode, stat.S_IFREG | 0o444, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0]
    result.append(Entry(PAYLOAD, fields, payload))
    by_name = {e.name: e for e in result}
    for manifest in ("ramdisk-files.txt", "ramdisk-files.sha256sum"):
        if manifest not in by_name:
            raise ValueError("Repacker manifest is missing: " + manifest)
    by_name["ramdisk-files.txt"].data = ("\n".join(e.name for e in result) + "\n").encode()
    hashes = []
    for e in result:
        if stat.S_ISREG(e.fields[1]) and e.name not in ("prop.default", "ramdisk-files.sha256sum"):
            hashes.append(hashlib.sha256(e.data).hexdigest() + "  " + e.name)
    by_name["ramdisk-files.sha256sum"].data = ("\n".join(hashes) + "\n").encode()
    result.append(entries[-1])
    summary = {"files_packed": len(selected), "raw_bytes_packed": sum(len(e.data) for e in selected),
               "xz_bytes": len(payload), "bootstrap_files_preserved": sorted(protect & files.keys())}
    return encode_cpio(result), summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mkbootfs")
    parser.add_argument("--input-cpio")
    parser.add_argument("--output-cpio")
    parser.add_argument("args", nargs=argparse.REMAINDER)
    native_args = sys.argv[1:]
    direct_wrapper = bool(native_args) and native_args[0] not in (
        "--mkbootfs", "--input-cpio", "--output-cpio", "--help", "-h")
    if direct_wrapper:
        root = pathlib.Path(native_args[-1]).resolve()
        parts = root.parts
        candidates = [i for i in range(len(parts) - 1) if parts[i:i + 2] == ("target", "product")]
        if not candidates:
            raise ValueError("Cannot locate host mkbootfs from Android output path: " + str(root))
        out = pathlib.Path(*parts[:candidates[-1]])
        native = out / "host/linux-x86/bin/mkbootfs"
        if not native.is_file():
            raise ValueError("Native host mkbootfs is missing: " + str(native))
        if root.name != "root" or root.parent.name != "recovery":
            # Target-specific Make variables propagate to prerequisites. Do not
            # change a generic/vendor ramdisk if its recipe inherits this one.
            raise SystemExit(subprocess.run([str(native)] + native_args).returncode)
        options = argparse.Namespace(mkbootfs=str(native), input_cpio=None,
                                     output_cpio=None, args=native_args)
    else:
        options = parser.parse_args()
    if options.input_cpio:
        original = pathlib.Path(options.input_cpio).read_bytes()
    else:
        if not options.mkbootfs:
            parser.error("--mkbootfs or --input-cpio is required")
        argv = options.args[1:] if options.args[:1] == ["--"] else options.args
        process = subprocess.run([options.mkbootfs] + argv, stdout=subprocess.PIPE, check=True)
        original = process.stdout
    packed, summary = transform(original)
    print("AERA runtime: %d files, %.2f MB -> %.2f MB XZ; bootstrap closure preserved" %
          (summary["files_packed"], summary["raw_bytes_packed"] / 1000000,
           summary["xz_bytes"] / 1000000), file=sys.stderr)
    if options.output_cpio:
        pathlib.Path(options.output_cpio).write_bytes(packed)
    else:
        sys.stdout.buffer.write(packed)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print("AERA runtime packing failed: %s" % error, file=sys.stderr)
        sys.exit(1)
