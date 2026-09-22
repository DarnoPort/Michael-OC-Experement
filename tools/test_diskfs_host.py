#!/usr/bin/env python3

from __future__ import annotations

import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import diskfs_host  # noqa: E402


def make_minimal_elf(path: Path) -> None:
    header = struct.pack(
        diskfs_host.ELF_HEADER_FMT,
        diskfs_host.ELF_MAGIC,
        diskfs_host.ET_EXEC,
        diskfs_host.EM_386,
        diskfs_host.EV_CURRENT,
        diskfs_host.USER_VM_BASE,
        diskfs_host.ELF_HEADER_SIZE,
        0,
        0,
        diskfs_host.ELF_HEADER_SIZE,
        diskfs_host.ELF_PHDR_SIZE,
        1,
        0,
        0,
        0,
    )
    phdr = struct.pack(
        diskfs_host.ELF_PHDR_FMT,
        diskfs_host.PT_LOAD,
        0x100,
        diskfs_host.USER_VM_BASE,
        0,
        1,
        1,
        diskfs_host.PF_R | diskfs_host.PF_X,
        0x1000,
    )
    image = header + phdr
    image += b"\0" * (0x100 - len(image))
    image += b"\xC3"
    path.write_bytes(image)


def main() -> int:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        elf = root / "hello.elf"
        disk = root / "michaelos.disk"

        make_minimal_elf(elf)
        info = diskfs_host.validate_elf32_file(elf)

        assert info["entry"] == diskfs_host.USER_VM_BASE
        assert len(info["segments"]) == 1

        image = bytearray(diskfs_host.DISK_SIZE)
        diskfs_host.diskfs_format(disk)
        image = diskfs_host.load_image(disk)
        diskfs_host.atomic_write(disk, diskfs_host.import_file(
            image,
            elf,
            "/bin/hello.elf",
        ))

        image = diskfs_host.load_image(disk)
        payload = diskfs_host.read_diskfs_file(
            image,
            "/bin/hello.elf",
        )
        stored = diskfs_host.parse_elf32(payload)

        assert stored["entry"] == info["entry"]
        assert payload.endswith(b"\xC3")

        bad = bytearray(elf.read_bytes())
        bad[0] = 0
        bad_path = root / "bad.elf"
        bad_path.write_bytes(bad)

        try:
            diskfs_host.validate_elf32_file(bad_path)
        except diskfs_host.DiskFSError:
            pass
        else:
            raise AssertionError("invalid ELF magic was accepted")

    print("diskfs_host tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
