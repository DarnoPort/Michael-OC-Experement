#!/usr/bin/env python3
"""Host-side tool for reading and modifying Michael OS DiskFS v1 images.

This tool intentionally implements only the stable on-disk format used by
Michael OS Phase 14 through Phase 25.2. It does not need the OS to be running.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import tempfile
from pathlib import Path


SECTOR_SIZE = 512
TOTAL_SECTORS = 32768
MAX_INODES = 128
NAME_MAX = 32
MAX_FILE_SIZE = 65536

NODE_FILE = 1
NODE_DIR = 2

SUPERBLOCK_SECTOR = 0
INODE_START = 1
INODE_SECTORS = 16
BITMAP_START = 17
BITMAP_SECTORS = 8
DATA_START = 25

MAGIC = b"NFS1"
VERSION = 1

SUPER_FMT = "<4s8I6I"
INODE_FMT = "<6I32s2I"
SUPER_SIZE = struct.calcsize(SUPER_FMT)
INODE_SIZE = struct.calcsize(INODE_FMT)

DISK_SIZE = TOTAL_SECTORS * SECTOR_SIZE
BITMAP_BYTES = BITMAP_SECTORS * SECTOR_SIZE
DATA_SECTORS = TOTAL_SECTORS - DATA_START

USER_VM_BASE = 0x80000000
USER_HEAP_BASE = 0x80100000

ELF_MAGIC = b"\\x7fELF"
ELFCLASS32 = 1
ELFDATA2LSB = 1
ET_EXEC = 2
EM_386 = 3
EV_CURRENT = 1
PT_LOAD = 1
PF_X = 1
PF_W = 2
PF_R = 4

ELF_HEADER_FMT = "<16sHHIIIIIHHHHHH"
ELF_PHDR_FMT = "<IIIIIIII"
ELF_HEADER_SIZE = struct.calcsize(ELF_HEADER_FMT)
ELF_PHDR_SIZE = struct.calcsize(ELF_PHDR_FMT)


class DiskFSError(Exception):
    pass


def die(message: str) -> int:
    print(f"diskfs: {message}", file=sys.stderr)
    return 1


def read_uint(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def pack_inode(
    used: int,
    node_type: int,
    size: int,
    data_start: int,
    data_sectors: int,
    parent: int,
    name: bytes,
) -> bytes:
    if len(name) >= NAME_MAX:
        raise DiskFSError("inode name is too long")
    raw_name = name + b"\0" * (NAME_MAX - len(name))
    return struct.pack(
        INODE_FMT,
        used,
        node_type,
        size,
        data_start,
        data_sectors,
        parent,
        raw_name,
        0,
        0,
    )


def unpack_inode(image: bytes, inode_number: int) -> dict:
    if not 0 <= inode_number < MAX_INODES:
        raise DiskFSError("inode number out of range")
    offset = INODE_START * SECTOR_SIZE + inode_number * INODE_SIZE
    values = struct.unpack_from(INODE_FMT, image, offset)
    name = values[6].split(b"\0", 1)[0]
    return {
        "used": values[0],
        "type": values[1],
        "size": values[2],
        "data_start": values[3],
        "data_sectors": values[4],
        "parent": values[5],
        "name": name,
    }


def write_inode(image: bytearray, inode_number: int, inode: dict) -> None:
    offset = INODE_START * SECTOR_SIZE + inode_number * INODE_SIZE
    image[offset : offset + INODE_SIZE] = pack_inode(
        inode["used"],
        inode["type"],
        inode["size"],
        inode["data_start"],
        inode["data_sectors"],
        inode["parent"],
        inode["name"],
    )


def bit_get(bitmap: bytearray, index: int) -> bool:
    return bool(bitmap[index >> 3] & (1 << (index & 7)))


def bit_set(bitmap: bytearray, index: int, used: bool) -> None:
    mask = 1 << (index & 7)
    byte_index = index >> 3
    if used:
        bitmap[byte_index] |= mask
    else:
        bitmap[byte_index] &= ~mask


def load_image(path: Path) -> bytearray:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise DiskFSError(f"cannot read disk image: {exc}") from exc

    if len(data) != DISK_SIZE:
        raise DiskFSError(
            f"disk image must be exactly {DISK_SIZE} bytes, got {len(data)}"
        )

    values = struct.unpack_from(SUPER_FMT, data, 0)
    if (
        values[0] != MAGIC
        or values[1] != VERSION
        or values[2] != SECTOR_SIZE
        or values[3] != TOTAL_SECTORS
        or values[4] != INODE_START
        or values[5] != MAX_INODES
        or values[6] != BITMAP_START
        or values[7] != BITMAP_SECTORS
        or values[8] != DATA_START
    ):
        raise DiskFSError("unsupported or invalid Michael OS DiskFS v1 image")

    if values[1] != VERSION:
        raise DiskFSError("unsupported DiskFS version")

    return bytearray(data)


def diskfs_format(path: Path) -> None:
    image = bytearray(DISK_SIZE)
    struct.pack_into(
        SUPER_FMT,
        image,
        0,
        MAGIC,
        VERSION,
        SECTOR_SIZE,
        TOTAL_SECTORS,
        INODE_START,
        MAX_INODES,
        BITMAP_START,
        BITMAP_SECTORS,
        DATA_START,
        0,
        0,
        0,
        0,
        0,
        0,
    )
    root = {
        "used": 1,
        "type": NODE_DIR,
        "size": 0,
        "data_start": 0,
        "data_sectors": 0,
        "parent": 0,
        "name": b"/",
    }
    write_inode(image, 0, root)
    atomic_write(path, image)


def bitmap_view(image: bytearray) -> bytearray:
    start = BITMAP_START * SECTOR_SIZE
    return bytearray(image[start : start + BITMAP_BYTES])


def write_bitmap(image: bytearray, bitmap: bytearray) -> None:
    start = BITMAP_START * SECTOR_SIZE
    image[start : start + BITMAP_BYTES] = bitmap


def name_of(inode: dict) -> str:
    try:
        return inode["name"].decode("utf-8")
    except UnicodeDecodeError:
        return inode["name"].decode("latin-1")


def validate_name(raw: bytes) -> None:
    if not raw:
        raise DiskFSError("empty DiskFS name")
    if len(raw) >= NAME_MAX:
        raise DiskFSError("DiskFS name is too long")
    if b"/" in raw:
        raise DiskFSError("DiskFS name may not contain '/'")
    if raw in (b".", b".."):
        raise DiskFSError("'.' and '..' are reserved")


def parse_disk_path(path: str) -> list[bytes]:
    if not path.startswith("/"):
        raise DiskFSError("destination path must be absolute (start with '/')")

    parts = []
    for text in path.split("/"):
        if text == "":
            continue
        raw = text.encode("utf-8")
        validate_name(raw)
        parts.append(raw)

    if not parts:
        raise DiskFSError("root directory cannot be used as a file path")

    return parts


def find_child(image: bytearray, parent: int, name: bytes) -> int | None:
    for inode_number in range(1, MAX_INODES):
        inode = unpack_inode(image, inode_number)
        if inode["used"] and inode["parent"] == parent and inode["name"] == name:
            return inode_number
    return None


def resolve_node(image: bytearray, path: str) -> int:
    parts = parse_disk_path(path)
    current = 0
    for part in parts:
        inode_number = find_child(image, current, part)
        if inode_number is None:
            raise DiskFSError(f"path not found: {path}")
        inode = unpack_inode(image, inode_number)
        if inode["type"] != NODE_DIR and part != parts[-1]:
            raise DiskFSError(f"component is not a directory: {part!r}")
        current = inode_number
    return current


def ensure_directory_path(image: bytearray, path: str) -> bytearray:
    """
    Create missing directory components in an absolute DiskFS path.
    Existing file components are rejected. The root directory always exists.
    """
    if path == "/":
        return image
    parts = parse_disk_path(path)
    current = 0

    for part in parts:
        existing = find_child(image, current, part)

        if existing is not None:
            inode = unpack_inode(image, existing)
            if inode["type"] != NODE_DIR:
                raise DiskFSError(
                    f"path component is not a directory: {part!r}"
                )
            current = existing
            continue

        inode_number = find_free_inode(image)
        write_inode(
            image,
            inode_number,
            {
                "used": 1,
                "type": NODE_DIR,
                "size": 0,
                "data_start": 0,
                "data_sectors": 0,
                "parent": current,
                "name": part,
            },
        )
        current = inode_number

    return image


def resolve_parent(image: bytearray, path: str) -> tuple[int, bytes]:
    parts = parse_disk_path(path)
    leaf = parts[-1]
    if len(parts) == 1:
        return 0, leaf

    current = 0
    for part in parts[:-1]:
        inode_number = find_child(image, current, part)
        if inode_number is None:
            raise DiskFSError(f"parent directory not found: {'/' + '/'.join(p.decode('utf-8', 'replace') for p in parts[:-1])}")
        inode = unpack_inode(image, inode_number)
        if inode["type"] != NODE_DIR:
            raise DiskFSError(f"parent component is not a directory: {part!r}")
        current = inode_number

    return current, leaf


def find_free_inode(image: bytearray) -> int:
    for inode_number in range(1, MAX_INODES):
        if not unpack_inode(image, inode_number)["used"]:
            return inode_number
    raise DiskFSError("DiskFS inode table is full")


def clear_extent(bitmap: bytearray, start: int, count: int) -> None:
    if count == 0:
        return
    if start < DATA_START or start + count > TOTAL_SECTORS:
        raise DiskFSError("existing file has an invalid data extent")
    for sector in range(count):
        bit_set(bitmap, start - DATA_START + sector, False)


def find_free_run(bitmap: bytearray, required: int) -> int:
    if required == 0:
        return 0
    if required > DATA_SECTORS:
        raise DiskFSError("file is too large for DiskFS")

    run = 0
    run_start = 0
    for index in range(DATA_SECTORS):
        if not bit_get(bitmap, index):
            if run == 0:
                run_start = index
            run += 1
            if run >= required:
                return DATA_START + run_start
        else:
            run = 0

    raise DiskFSError("not enough contiguous free data sectors")


def validate_destination(
    image: bytearray,
    parent: int,
    name: bytes,
) -> int | None:
    parent_inode = unpack_inode(image, parent)
    if not parent_inode["used"] or parent_inode["type"] != NODE_DIR:
        raise DiskFSError("destination parent is not a directory")
    return find_child(image, parent, name)


def import_file(image: bytearray, source: Path, destination: str) -> bytearray:
    try:
        payload = source.read_bytes()
    except OSError as exc:
        raise DiskFSError(f"cannot read source file: {exc}") from exc

    if len(payload) > MAX_FILE_SIZE:
        raise DiskFSError(
            f"source is {len(payload)} bytes; DiskFS maximum is {MAX_FILE_SIZE}"
        )

    destination_parts = parse_disk_path(destination)
    parent_text = "/" + "/".join(
        part.decode("utf-8") for part in destination_parts[:-1]
    )
    if parent_text != "/":
        ensure_directory_path(image, parent_text)

    parent, name = resolve_parent(image, destination)
    existing = validate_destination(image, parent, name)

    if existing is None:
        inode_number = find_free_inode(image)
        old_extent = (0, 0)
    else:
        inode_number = existing
        inode = unpack_inode(image, inode_number)
        if inode["type"] != NODE_FILE:
            raise DiskFSError("destination already exists and is not a file")
        old_extent = (inode["data_start"], inode["data_sectors"])

    bitmap = bitmap_view(image)
    if old_extent[1]:
        clear_extent(bitmap, *old_extent)

    required = (len(payload) + SECTOR_SIZE - 1) // SECTOR_SIZE
    new_start = find_free_run(bitmap, required)

    if required:
        for sector in range(required):
            absolute = new_start + sector
            offset = sector * SECTOR_SIZE
            chunk = payload[offset : offset + SECTOR_SIZE]
            image[absolute * SECTOR_SIZE : (absolute + 1) * SECTOR_SIZE] = b"\0" * SECTOR_SIZE
            image[absolute * SECTOR_SIZE : absolute * SECTOR_SIZE + len(chunk)] = chunk
            bit_set(bitmap, absolute - DATA_START, True)

    write_bitmap(image, bitmap)
    write_inode(
        image,
        inode_number,
        {
            "used": 1,
            "type": NODE_FILE,
            "size": len(payload),
            "data_start": new_start if required else 0,
            "data_sectors": required,
            "parent": parent,
            "name": name,
        },
    )

    return image


def export_file(image: bytearray, source: str, destination: Path) -> None:
    inode_number = resolve_node(image, source)
    inode = unpack_inode(image, inode_number)

    if not inode["used"] or inode["type"] != NODE_FILE:
        raise DiskFSError("source is not a file")

    if inode["size"] > MAX_FILE_SIZE:
        raise DiskFSError("source inode has an invalid size")

    required = (inode["size"] + SECTOR_SIZE - 1) // SECTOR_SIZE
    if inode["data_sectors"] != required:
        raise DiskFSError("source inode has inconsistent sector count")

    if required:
        start = inode["data_start"]
        if start < DATA_START or start + required > TOTAL_SECTORS:
            raise DiskFSError("source inode has an invalid data extent")
        payload = bytes(
            image[start * SECTOR_SIZE : start * SECTOR_SIZE + inode["size"]]
        )
    else:
        payload = b""

    try:
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(payload)
    except OSError as exc:
        raise DiskFSError(f"cannot write exported file: {exc}") from exc


def list_directory(image: bytearray, path: str) -> None:
    inode_number = 0 if path == "/" else resolve_node(image, path)
    inode = unpack_inode(image, inode_number)
    if not inode["used"] or inode["type"] != NODE_DIR:
        raise DiskFSError("path is not a directory")

    entries = []
    for i in range(1, MAX_INODES):
        child = unpack_inode(image, i)
        if child["used"] and child["parent"] == inode_number:
            entries.append(child)

    entries.sort(key=lambda item: item["name"])
    for child in entries:
        kind = "DIR " if child["type"] == NODE_DIR else "FILE"
        print(f"[{kind}] {name_of(child)}  {child['size']} bytes")


def atomic_write(path: Path, image: bytearray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            dir=str(path.parent),
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as handle:
            temp_name = Path(handle.name)
            handle.write(image)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temp_name, path)
    except OSError as exc:
        try:
            temp_name.unlink(missing_ok=True)
        except UnboundLocalError:
            pass
        raise DiskFSError(f"cannot replace disk image: {exc}") from exc


def parse_elf32(image: bytes) -> dict:
    if len(image) < ELF_HEADER_SIZE:
        raise DiskFSError("ELF image is smaller than an ELF32 header")

    values = struct.unpack_from(ELF_HEADER_FMT, image, 0)
    ident = values[0]
    if (
        ident[:4] != ELF_MAGIC
        or ident[4] != ELFCLASS32
        or ident[5] != ELFDATA2LSB
        or ident[6] != EV_CURRENT
        or values[1] != ET_EXEC
        or values[2] != EM_386
        or values[3] != EV_CURRENT
        or values[8] != ELF_HEADER_SIZE
        or values[9] != ELF_PHDR_SIZE
        or values[10] == 0
    ):
        raise DiskFSError(
            "unsupported ELF: expected little-endian 32-bit x86 ET_EXEC"
        )

    entry = values[4]
    phoff = values[5]
    phnum = values[10]

    ph_end = phoff + phnum * ELF_PHDR_SIZE
    if ph_end < phoff or ph_end > len(image):
        raise DiskFSError("ELF program header table is outside the image")

    loaded = []
    page_bitmap = bytearray((USER_HEAP_BASE - USER_VM_BASE) // 0x1000 // 8)

    for index in range(phnum):
        offset = phoff + index * ELF_PHDR_SIZE
        ph = struct.unpack_from(ELF_PHDR_FMT, image, offset)

        p_type, p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz, p_flags, p_align = ph

        if p_type != PT_LOAD:
            continue

        if p_memsz < p_filesz:
            raise DiskFSError(f"PT_LOAD #{index}: p_memsz is smaller than p_filesz")

        if p_offset + p_filesz < p_offset or p_offset + p_filesz > len(image):
            raise DiskFSError(f"PT_LOAD #{index}: file range is outside the image")

        if p_memsz == 0:
            continue

        segment_end = p_vaddr + p_memsz
        if segment_end < p_vaddr:
            raise DiskFSError(f"PT_LOAD #{index}: virtual address range overflows")

        if p_vaddr < USER_VM_BASE or segment_end > USER_HEAP_BASE:
            raise DiskFSError(
                f"PT_LOAD #{index}: virtual range must stay inside "
                f"0x{USER_VM_BASE:08x}-0x{USER_HEAP_BASE:08x}"
            )

        if p_flags & ~(PF_R | PF_W | PF_X):
            raise DiskFSError(f"PT_LOAD #{index}: unsupported permission flags")

        page_start = p_vaddr & 0xFFFFF000
        page_end = (segment_end + 0xFFF) & 0xFFFFF000
        for address in range(page_start, page_end, 0x1000):
            page = (address - USER_VM_BASE) // 0x1000
            byte_index = page >> 3
            bit = 1 << (page & 7)
            if page_bitmap[byte_index] & bit:
                raise DiskFSError(
                    f"PT_LOAD #{index}: overlaps another loadable segment"
                )
            page_bitmap[byte_index] |= bit

        loaded.append({
            "index": index,
            "offset": p_offset,
            "vaddr": p_vaddr,
            "filesz": p_filesz,
            "memsz": p_memsz,
            "flags": p_flags,
            "align": p_align,
        })

    if not loaded:
        raise DiskFSError("ELF image contains no PT_LOAD segments")

    if not any(segment["vaddr"] <= entry < segment["vaddr"] + segment["memsz"] for segment in loaded):
        raise DiskFSError("ELF entry point is not inside a PT_LOAD segment")

    return {
        "entry": entry,
        "phnum": phnum,
        "segments": loaded,
    }


def validate_elf32_file(source: Path) -> dict:
    try:
        image = source.read_bytes()
    except OSError as exc:
        raise DiskFSError(f"cannot read ELF source: {exc}") from exc

    if len(image) > MAX_FILE_SIZE:
        raise DiskFSError(
            f"ELF source is {len(image)} bytes; DiskFS maximum is {MAX_FILE_SIZE}"
        )

    return parse_elf32(image)


def format_elf_info(path: Path, info: dict) -> None:
    print(f"{path}: valid ELF32 i386 executable")
    print(f"Entry: 0x{info['entry']:08x}")
    print(f"Loadable segments: {len(info['segments'])}")
    for segment in info["segments"]:
        permissions = (
            ("R" if segment["flags"] & PF_R else "-")
            + ("W" if segment["flags"] & PF_W else "-")
            + ("X" if segment["flags"] & PF_X else "-")
        )
        print(
            f"  PT_LOAD #{segment['index']}: "
            f"vaddr=0x{segment['vaddr']:08x} "
            f"file={segment['filesz']} mem={segment['memsz']} "
            f"flags={permissions}"
        )


def read_diskfs_file(image: bytearray, source: str) -> bytes:
    inode_number = resolve_node(image, source)
    inode = unpack_inode(image, inode_number)

    if not inode["used"] or inode["type"] != NODE_FILE:
        raise DiskFSError("source is not a file")

    if inode["size"] > MAX_FILE_SIZE:
        raise DiskFSError("source inode has an invalid size")

    required = (inode["size"] + SECTOR_SIZE - 1) // SECTOR_SIZE
    if inode["data_sectors"] != required:
        raise DiskFSError("source inode has inconsistent sector count")

    if required:
        start = inode["data_start"]
        if start < DATA_START or start + required > TOTAL_SECTORS:
            raise DiskFSError("source inode has an invalid data extent")
        return bytes(
            image[start * SECTOR_SIZE : start * SECTOR_SIZE + inode["size"]]
        )

    return b""


def command_import(args: argparse.Namespace) -> None:
    disk = Path(args.disk)
    if not disk.exists():
        diskfs_format(disk)
        print(f"Created new {disk}.")

    image = load_image(disk)
    image = import_file(image, Path(args.source), args.destination)
    atomic_write(disk, image)

    print(f"Imported {args.source} -> {args.destination}")


def command_install_elf(args: argparse.Namespace) -> None:
    source = Path(args.source)
    info = validate_elf32_file(source)

    disk = Path(args.disk)
    if not disk.exists():
        diskfs_format(disk)
        print(f"Created new {disk}.")

    image = load_image(disk)
    image = import_file(image, source, args.destination)
    atomic_write(disk, image)

    print(f"Installed ELF32 {source} -> {args.destination}")
    print(f"Entry: 0x{info['entry']:08x}")
    print(f"Loadable segments: {len(info['segments'])}")


def command_check_elf(args: argparse.Namespace) -> None:
    source = Path(args.source)
    info = validate_elf32_file(source)
    format_elf_info(source, info)


def command_check_disk_elf(args: argparse.Namespace) -> None:
    image = load_image(Path(args.disk))
    payload = read_diskfs_file(image, args.source)
    info = parse_elf32(payload)
    format_elf_info(Path(args.source), info)


def command_export(args: argparse.Namespace) -> None:
    image = load_image(Path(args.disk))
    export_file(image, args.source, Path(args.destination))
    print(f"Exported {args.source} -> {args.destination}")


def command_list(args: argparse.Namespace) -> None:
    image = load_image(Path(args.disk))
    list_directory(image, args.path)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Michael OS DiskFS v1 host utility"
    )
    parser.add_argument(
        "--disk",
        default="michaelos.disk",
        help="DiskFS image path (default: michaelos.disk)",
    )

    subparsers = parser.add_subparsers(dest="command", required=True)

    install_parser = subparsers.add_parser(
        "install-elf",
        help="validate and install a host ELF32 executable into DiskFS",
    )
    install_parser.add_argument("source")
    install_parser.add_argument("destination")
    install_parser.set_defaults(handler=command_install_elf)

    elf_check_parser = subparsers.add_parser(
        "elf-check",
        help="validate a host ELF32 executable",
    )
    elf_check_parser.add_argument("source")
    elf_check_parser.set_defaults(handler=command_check_elf)

    disk_elf_parser = subparsers.add_parser(
        "disk-elf-check",
        help="validate an ELF32 executable already stored in DiskFS",
    )
    disk_elf_parser.add_argument("source")
    disk_elf_parser.set_defaults(handler=command_check_disk_elf)

    import_parser = subparsers.add_parser(
        "import",
        help="import a host file into DiskFS",
    )
    import_parser.add_argument("source")
    import_parser.add_argument("destination")
    import_parser.set_defaults(handler=command_import)

    export_parser = subparsers.add_parser(
        "export",
        help="export a DiskFS file to the host",
    )
    export_parser.add_argument("source")
    export_parser.add_argument("destination")
    export_parser.set_defaults(handler=command_export)

    list_parser = subparsers.add_parser(
        "ls",
        help="list a DiskFS directory",
    )
    list_parser.add_argument("path", nargs="?", default="/")
    list_parser.set_defaults(handler=command_list)

    args = parser.parse_args()

    try:
        args.handler(args)
    except DiskFSError as exc:
        return die(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
