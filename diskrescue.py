#!/usr/bin/env python3
"""
diskrescue - recover data from a failing disk or a disk with damaged partitions.

Safe workflow (the source disk is only ever opened READ-ONLY):

  1. info    SRC                 show the partition tables (MBR, primary + backup GPT)
  2. image   SRC disk.img        copy the whole disk, skipping and retrying bad sectors
  3. scan    disk.img            find lost/damaged partitions by filesystem signatures
  4. extract disk.img --offset N --length L part.img   pull one partition out
     (or mount it read-only:  sudo mount -o ro,loop,offset=N disk.img /mnt/rescue)
  5. carve   disk.img outdir/    last resort: pull files out by their signatures

Pure Python 3 standard library. Linux and macOS; on Windows pass --size.
"""
import argparse
import json
import os
import signal
import stat
import struct
import sys
import time
import uuid

SECTOR = 512
MiB = 1024 * 1024


# --------------------------------------------------------------------------- utils

def human(n):
    n = float(n)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if abs(n) < 1024 or unit == "TiB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{int(n)} B"
        n /= 1024


def device_size(fd):
    """Size in bytes of a file or block device."""
    try:
        size = os.lseek(fd, 0, os.SEEK_END)
        if size > 0:
            return size
    except OSError:
        pass
    try:
        import fcntl
        if sys.platform.startswith("linux"):
            buf = fcntl.ioctl(fd, 0x80081272, b"\0" * 8)  # BLKGETSIZE64
            return struct.unpack("Q", buf)[0]
        if sys.platform == "darwin":
            bs = struct.unpack("I", fcntl.ioctl(fd, 0x40046418, b"\0" * 4))[0]  # DKIOCGETBLOCKSIZE
            bc = struct.unpack("Q", fcntl.ioctl(fd, 0x40086419, b"\0" * 8))[0]  # DKIOCGETBLOCKCOUNT
            return bs * bc
    except (OSError, ImportError):
        pass
    return 0


def open_ro(path):
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0)
    return os.open(path, flags)


def safe_pread(fd, n, off):
    try:
        return os.pread(fd, n, off)
    except OSError:
        return b""


class Progress:
    def __init__(self, total, label):
        self.total, self.label = max(total, 1), label
        self.t0 = self.last = time.time()
        self.start_done = None

    def update(self, done, extra="", force=False):
        now = time.time()
        if not force and now - self.last < 0.5:
            return
        self.last = now
        if self.start_done is None:
            self.start_done = done
        rate = (done - self.start_done) / max(now - self.t0, 1e-6)
        eta = (self.total - done) / rate if rate > 0 else 0
        sys.stderr.write(
            f"\r{self.label}: {done / self.total * 100:6.2f}%  {human(done)}/{human(self.total)}"
            f"  {human(rate)}/s  ETA {int(eta // 3600)}h{int(eta % 3600 // 60):02d}m {extra}   ")
        sys.stderr.flush()

    def finish(self):
        sys.stderr.write("\n")


def refuse_dangerous_output(src, dst, force=False):
    if os.path.exists(dst):
        if os.path.realpath(dst) == os.path.realpath(src):
            sys.exit("ERROR: output is the same as the source. Refusing.")
        if stat.S_ISBLK(os.stat(dst).st_mode) and not force:
            sys.exit("ERROR: output is a block device. Writing to a disk can destroy data. "
                     "Write to an image file on a DIFFERENT, healthy disk (or pass --force).")


# --------------------------------------------------------------------------- info

GPT_TYPES = {
    "c12a7328-f81f-11d2-ba4b-00a0c93ec93b": "EFI System",
    "e3c9e316-0b5c-4db8-817d-f92df00215ae": "Microsoft Reserved",
    "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7": "Microsoft Basic Data (NTFS/FAT/exFAT)",
    "de94bba4-06d1-4d40-a16a-bfd50179d6ac": "Windows Recovery",
    "0fc63daf-8483-4772-8e79-3d69d8477de4": "Linux filesystem",
    "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f": "Linux swap",
    "e6d6d379-f507-44c2-a23c-238f2a3df928": "Linux LVM",
    "a19d880f-05fc-4d3b-a006-743f0f84911e": "Linux RAID",
    "48465300-0000-11aa-aa11-00306543ecac": "Apple HFS+",
    "7c3457ef-0000-11aa-aa11-00306543ecac": "Apple APFS",
}

MBR_TYPES = {0x01: "FAT12", 0x04: "FAT16", 0x05: "Extended", 0x06: "FAT16", 0x07: "NTFS/exFAT",
             0x0b: "FAT32", 0x0c: "FAT32 LBA", 0x0e: "FAT16 LBA", 0x0f: "Extended LBA",
             0x82: "Linux swap", 0x83: "Linux", 0x8e: "Linux LVM", 0xaf: "Apple HFS+",
             0xee: "GPT protective", 0xef: "EFI"}


def parse_mbr(sec0):
    if len(sec0) < 512 or sec0[510:512] != b"\x55\xaa":
        return None
    parts = []
    for i in range(4):
        e = sec0[446 + 16 * i: 462 + 16 * i]
        ptype = e[4]
        lba, count = struct.unpack_from("<II", e, 8)
        if ptype and count:
            parts.append({"n": i + 1, "type": ptype, "type_name": MBR_TYPES.get(ptype, "?"),
                          "boot": e[0] == 0x80, "start": lba, "sectors": count})
    return parts


def parse_gpt(fd, header_lba, ss):
    hdr = safe_pread(fd, ss, header_lba * ss)
    if hdr[:8] != b"EFI PART":
        return None
    (hsize,) = struct.unpack_from("<I", hdr, 12)
    my_lba, alt_lba, first_usable, last_usable = struct.unpack_from("<QQQQ", hdr, 24)
    disk_guid = str(uuid.UUID(bytes_le=hdr[56:72]))
    entries_lba, n_entries, esize = struct.unpack_from("<QII", hdr, 72)
    if not (0 < esize <= 4096 and 0 < n_entries <= 4096):
        return {"valid": False, "my_lba": my_lba, "alt_lba": alt_lba, "parts": []}
    raw = safe_pread(fd, n_entries * esize, entries_lba * ss)
    parts = []
    for i in range(min(n_entries, len(raw) // esize)):
        e = raw[i * esize:(i + 1) * esize]
        if e[:16] == b"\0" * 16:
            continue
        tguid = str(uuid.UUID(bytes_le=e[:16]))
        first, last = struct.unpack_from("<QQ", e, 32)
        name = e[56:128].decode("utf-16-le", "replace").rstrip("\0")
        parts.append({"n": i + 1, "type": tguid, "type_name": GPT_TYPES.get(tguid, "?"),
                      "start": first, "sectors": last - first + 1, "name": name})
    import zlib
    crc_ok = zlib.crc32(raw[:n_entries * esize]) & 0xffffffff == struct.unpack_from("<I", hdr, 88)[0]
    h = bytearray(hdr[:hsize]); h[16:20] = b"\0\0\0\0"
    hcrc_ok = zlib.crc32(bytes(h)) & 0xffffffff == struct.unpack_from("<I", hdr, 16)[0]
    return {"valid": crc_ok and hcrc_ok, "header_crc_ok": hcrc_ok, "entries_crc_ok": crc_ok,
            "my_lba": my_lba, "alt_lba": alt_lba, "disk_guid": disk_guid,
            "entries_lba": entries_lba, "parts": parts}


def cmd_info(a):
    fd = open_ro(a.source)
    size = a.size or device_size(fd)
    print(f"Source: {a.source}   size: {human(size)} ({size} bytes)")
    sec0 = safe_pread(fd, 512, 0)
    if len(sec0) < 512:
        print("!! Could not read sector 0 (bad sector at the very start?). Image the disk first.")
    mbr = parse_mbr(sec0)
    print("\n== MBR ==")
    if mbr is None:
        print("  no valid MBR signature (55 AA) -> partition table is missing or damaged")
    elif not mbr:
        print("  MBR signature present but no partitions listed")
    for p in mbr or []:
        print(f"  #{p['n']} type 0x{p['type']:02x} {p['type_name']:<16} start {p['start'] * 512:>15}"
              f"  size {human(p['sectors'] * 512):>10}{'  (boot)' if p['boot'] else ''}")
    found = False
    for ss in (512, 4096):
        prim = parse_gpt(fd, 1, ss)
        last_lba = size // ss - 1 if size else 0
        backup = parse_gpt(fd, last_lba, ss) if last_lba else None
        if not prim and not backup:
            continue
        found = True
        for label, g in (("primary GPT (LBA 1)", prim), (f"backup GPT (LBA {last_lba})", backup)):
            print(f"\n== {label}, {ss}-byte sectors ==")
            if not g:
                print("  MISSING / unreadable")
                continue
            print(f"  CRC {'OK' if g['valid'] else 'BAD'}  disk GUID {g.get('disk_guid', '?')}")
            for p in g["parts"]:
                print(f"  #{p['n']:<3} {p['type_name']:<38} start {p['start'] * ss:>15}"
                      f"  size {human(p['sectors'] * ss):>10}  {p['name']}")
        if prim and backup and (not prim["valid"]) and backup["valid"]:
            print("\n>> Primary GPT is damaged but the BACKUP GPT is intact. The partitions listed in the "
                  "backup table can be used with 'extract' / mount offsets; gdisk ('r' then 'b' then 'c') "
                  "can rebuild the primary from it ON A COPY.")
        if not prim and backup:
            print("\n>> Primary GPT missing but backup GPT exists - partitions are recoverable from it.")
    if not found:
        print("\n== GPT == none found")
    print("\nNext: run 'scan' to look for partitions by filesystem signature (finds ones the tables lost).")
    os.close(fd)


# --------------------------------------------------------------------------- image

class MapFile:
    def __init__(self, path):
        self.path = path
        self.d = None
        if path and os.path.exists(path):
            with open(path) as f:
                self.d = json.load(f)

    def save(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.d, f)
        os.replace(tmp, self.path)


def merge_ranges(r):
    r = sorted(r)
    out = []
    for s, e in r:
        if out and s <= out[-1][1]:
            out[-1][1] = max(out[-1][1], e)
        else:
            out.append([s, e])
    return out


def cmd_image(a):
    refuse_dangerous_output(a.source, a.output, a.force)
    src = open_ro(a.source)
    size = a.size or device_size(src)
    if not size:
        sys.exit("ERROR: cannot determine source size; pass --size BYTES")
    mapf = MapFile(a.map or a.output + ".map.json")
    if mapf.d is None:
        mapf.d = {"source": a.source, "size": size, "pos": 0, "phase": 1, "bad": [], "unreadable": []}
    elif mapf.d["size"] != size:
        sys.exit("ERROR: map file belongs to a source of a different size.")
    d = mapf.d
    dst = os.open(a.output, os.O_RDWR | os.O_CREAT | getattr(os, "O_BINARY", 0), 0o644)
    if os.fstat(dst).st_size < size:
        os.ftruncate(dst, size)  # sparse; unread areas stay zero

    stop = {"flag": False}

    def on_sigint(*_):
        stop["flag"] = True
    signal.signal(signal.SIGINT, on_sigint)

    chunk = a.chunk * 1024
    ss = a.sector
    last_save = time.time()

    def bad_bytes():
        return sum(e - s for s, e in d["bad"]) + sum(e - s for s, e in d["unreadable"])

    # ---- phase 1: fast copy, skip over trouble
    if d["phase"] == 1:
        print(f"Phase 1: copying {human(size)} in {human(chunk)} blocks, skipping bad areas (Ctrl-C to pause)")
        prog = Progress(size, "copy ")
        skip = chunk
        pos = d["pos"]
        while pos < size and not stop["flag"]:
            n = min(chunk, size - pos)
            try:
                data = os.pread(src, n, pos)
            except OSError:
                data = None
            if data:
                os.pwrite(dst, data, pos)
                pos += len(data)
                skip = chunk
            else:
                end = min(size, pos + skip)
                d["bad"].append([pos, end])
                d["bad"] = merge_ranges(d["bad"])
                pos = end
                skip = min(skip * 2, a.max_skip * MiB)  # jump further away from a bad zone
            d["pos"] = pos
            prog.update(pos, f"bad {human(bad_bytes())}")
            if time.time() - last_save > 5:
                mapf.save(); last_save = time.time()
        prog.update(pos, f"bad {human(bad_bytes())}", force=True); prog.finish()
        if pos >= size:
            d["phase"] = 2
        mapf.save()

    # ---- phase 2: go back into the skipped areas, small blocks then single sectors
    if d["phase"] == 2 and not stop["flag"] and not a.no_retry:
        total = sum(e - s for s, e in d["bad"])
        print(f"Phase 2: retrying {human(total)} of skipped areas sector-by-sector ({a.retries} tries)")
        prog = Progress(total, "retry")
        done = 0
        while d["bad"] and not stop["flag"]:
            s, e = d["bad"][0]
            p = s
            sub = 64 * 1024
            while p < e and not stop["flag"]:
                n = min(sub, e - p)
                data = safe_pread(src, n, p)
                if len(data) == n:
                    os.pwrite(dst, data, p)
                else:
                    # read sector by sector inside this sub-block
                    q = p
                    while q < p + n:
                        m = min(ss, p + n - q)
                        got = b""
                        for _ in range(a.retries):
                            got = safe_pread(src, m, q)
                            if len(got) == m:
                                break
                        if len(got) == m:
                            os.pwrite(dst, got, q)
                        else:
                            d["unreadable"].append([q, q + m])
                        q += m
                    d["unreadable"] = merge_ranges(d["unreadable"])
                p += n
                done += n
                d["bad"][0][0] = p
                prog.update(done, f"unreadable {human(sum(y - x for x, y in d['unreadable']))}")
                if time.time() - last_save > 5:
                    mapf.save(); last_save = time.time()
            if p >= e:
                d["bad"].pop(0)
        prog.finish()
        if not d["bad"]:
            d["phase"] = 3
        mapf.save()

    os.fsync(dst); os.close(dst); os.close(src)
    unread = sum(e - s for s, e in d["unreadable"]) + sum(e - s for s, e in d["bad"])
    if stop["flag"]:
        print(f"\nPaused. Run the same command again to resume (map: {mapf.path}).")
    else:
        print(f"\nDone. Image: {a.output}   unrecovered: {human(unread)} "
              f"({unread / size * 100:.4f}% of disk). Map: {mapf.path}")
        print("Unrecovered areas are zero-filled in the image. Next: diskrescue.py scan", a.output)


# --------------------------------------------------------------------------- scan

def _aligned_hits(buf, sig, off_in_sector, align=SECTOR):
    i = buf.find(sig)
    while i != -1:
        if (i - off_in_sector) % align == 0:
            yield i - off_in_sector
        i = buf.find(sig, i + 1)


def check_ntfs(bs):
    if bs[3:11] != b"NTFS    " or bs[510:512] != b"\x55\xaa":
        return None
    bps, spc = struct.unpack_from("<HB", bs, 11)
    total = struct.unpack_from("<Q", bs, 40)[0]
    if bps not in (512, 1024, 2048, 4096) or spc == 0 or total == 0:
        return None
    serial = struct.unpack_from("<Q", bs, 72)[0]
    return {"fs": "NTFS", "size": (total + 1) * bps, "serial": f"{serial:016X}", "label": "",
            "backup_offset": total * bps}


def check_fat(bs):
    if bs[510:512] != b"\x55\xaa" or bs[0] not in (0xEB, 0xE9):
        return None
    bps, spc, reserved, nfats = struct.unpack_from("<HBHB", bs, 11)
    if bps not in (512, 1024, 2048, 4096) or spc == 0 or spc & (spc - 1) or nfats not in (1, 2):
        return None
    tot16 = struct.unpack_from("<H", bs, 19)[0]
    tot32 = struct.unpack_from("<I", bs, 32)[0]
    total = tot16 or tot32
    if not total:
        return None
    if bs[82:90] == b"FAT32   ":
        return {"fs": "FAT32", "size": total * bps, "serial": f"{struct.unpack_from('<I', bs, 67)[0]:08X}",
                "label": bs[71:82].decode("ascii", "replace").strip(), "is_backup_hint": True}
    if bs[54:59] in (b"FAT12", b"FAT16"):
        return {"fs": bs[54:59].decode(), "size": total * bps,
                "serial": f"{struct.unpack_from('<I', bs, 39)[0]:08X}",
                "label": bs[43:54].decode("ascii", "replace").strip()}
    return None


def check_exfat(bs):
    if bs[3:11] != b"EXFAT   " or bs[510:512] != b"\x55\xaa":
        return None
    length = struct.unpack_from("<Q", bs, 72)[0]
    bps_shift = bs[108]
    if not 9 <= bps_shift <= 12:
        return None
    return {"fs": "exFAT", "size": length << bps_shift,
            "serial": f"{struct.unpack_from('<I', bs, 100)[0]:08X}", "label": ""}


def check_ext(sb):
    # sb = 1024-byte superblock (found at partition_start + 1024)
    if len(sb) < 1024 or sb[56:58] != b"\x53\xef":
        return None
    blocks_lo, = struct.unpack_from("<I", sb, 4)
    log_bs, = struct.unpack_from("<I", sb, 24)
    bpg, = struct.unpack_from("<I", sb, 32)
    group_nr, = struct.unpack_from("<H", sb, 90)
    if log_bs > 6 or blocks_lo == 0 or bpg == 0 or bpg > 8 * (1024 << log_bs):
        return None
    feat_incompat, = struct.unpack_from("<I", sb, 96)
    blocks_hi = struct.unpack_from("<I", sb, 0x150)[0] if feat_incompat & 0x80 else 0
    bs = 1024 << log_bs
    fs = "ext4" if feat_incompat & 0x40 else ("ext3" if struct.unpack_from("<I", sb, 92)[0] & 4 else "ext2")
    return {"fs": fs, "size": ((blocks_hi << 32) | blocks_lo) * bs,
            "serial": str(uuid.UUID(bytes=sb[104:120])),
            "label": sb[120:136].split(b"\0")[0].decode("utf-8", "replace"),
            "group_nr": group_nr, "block_size": bs}


def check_hfs(vh):
    if vh[:2] not in (b"H+", b"HX"):
        return None
    bsz, total = struct.unpack_from(">II", vh, 40)
    if bsz == 0 or bsz & (bsz - 1) or total == 0:
        return None
    return {"fs": "HFS+", "size": bsz * total, "serial": "", "label": ""}


def check_apfs(b):
    if b[32:36] != b"NXSB":
        return None
    bsz, count = struct.unpack_from("<IQ", b, 36)
    if bsz == 0 or bsz & (bsz - 1):
        return None
    return {"fs": "APFS", "size": bsz * count, "serial": str(uuid.UUID(bytes=b[72:88])), "label": ""}


def scan_partitions(path, start=0, end=None, quiet=False):
    fd = open_ro(path)
    size = device_size(fd)
    end = min(end or size, size)
    block = 16 * MiB
    found = []
    prog = Progress(end - start, "scan ")
    pos = start - start % SECTOR
    while pos < end:
        buf = safe_pread(fd, min(block + 4096, end - pos + 4096), pos)
        if not buf:
            pos += block
            continue
        lim = min(block, len(buf))
        cands = []
        for off in _aligned_hits(buf, b"NTFS    ", 3):
            cands.append((off, "ntfs"))
        for off in _aligned_hits(buf, b"EXFAT   ", 3):
            cands.append((off, "exfat"))
        for off in _aligned_hits(buf, b"FAT32   ", 82):
            cands.append((off, "fat"))
        for sig in (b"FAT12   ", b"FAT16   "):
            for off in _aligned_hits(buf, sig, 54):
                cands.append((off, "fat"))
        for off in _aligned_hits(buf, b"NXSB", 32):
            cands.append((off, "apfs"))
        for sig in (b"H+\x00\x04", b"HX\x00\x05"):
            for off in _aligned_hits(buf, sig, 0):
                cands.append((off - 1024, "hfs"))
        # ext superblock magic at byte 56 of a sector; partition starts 1024 bytes earlier
        col = buf[56::SECTOR]
        i = col.find(b"\x53")
        while i != -1:
            o = i * SECTOR
            if buf[o + 57:o + 58] == b"\xef":
                cands.append((o - 1024, "ext"))
            i = col.find(b"\x53", i + 1)

        for off, kind in cands:
            if off < 0 and pos + off < 0:
                continue
            if off >= lim:
                continue
            abs_off = pos + off
            if kind == "ext":
                info = check_ext(safe_pread(fd, 1024, abs_off + 1024))
                if info and info["group_nr"] != 0:
                    continue  # backup superblock inside an ext fs, not a partition start
            elif kind == "hfs":
                info = check_hfs(safe_pread(fd, 512, abs_off + 1024))
            else:
                bs = safe_pread(fd, 512, abs_off)
                info = {"ntfs": check_ntfs, "exfat": check_exfat, "fat": check_fat,
                        "apfs": check_apfs}[kind](bs)
            if info:
                info["offset"] = abs_off
                if kind == "ntfs":
                    # primary boot sector? then its backup copy sits at the end of the volume
                    bk = check_ntfs(safe_pread(fd, 512, abs_off + info["backup_offset"]))
                    info["primary_confirmed"] = bool(bk and bk["serial"] == info["serial"])
                found.append(info)
        pos += block
        if not quiet:
            prog.update(min(pos, end) - start, f"found {len(found)}")
    if not quiet:
        prog.finish()
    os.close(fd)
    return dedupe(found)


def dedupe(found):
    """Drop backup boot sectors of filesystems whose primary we also found; keep orphaned backups."""
    out = []
    seen = {}
    for f in sorted(found, key=lambda x: x["offset"]):
        key = (f["fs"], f.get("serial"), f["size"])
        if f["fs"] == "NTFS":
            key = ("NTFS", f["serial"])
        if key in seen and f.get("serial"):
            continue
        seen[key] = f
        f["note"] = ""
        out.append(f)
    # an NTFS whose primary boot sector is gone: the backup sits at the END of the volume
    for f in out:
        if f["fs"] == "NTFS" and not f.get("primary_confirmed"):
            maybe_start = f["offset"] - f["backup_offset"]
            if maybe_start >= 0 and maybe_start != f["offset"]:
                f["alt_offset"] = maybe_start
                f["note"] = (f"may be the BACKUP boot sector; if mounting at this offset fails, "
                             f"try offset {maybe_start}")
    for f in out:
        if f["fs"] == "FAT32":
            f["note"] = "if this is a backup boot sector, the volume starts 3072 bytes (6 sectors) earlier"
    return out


def mount_hint(path, f):
    t = {"NTFS": "-t ntfs-3g ", "exFAT": "-t exfat ", "HFS+": "-t hfsplus "}.get(f["fs"], "")
    return f"sudo mount {t}-o ro,loop,offset={f['offset']} '{path}' /mnt/rescue"


def cmd_scan(a):
    print(f"Scanning {a.image} for filesystem signatures (read-only)...")
    found = scan_partitions(a.image, a.start, a.end)
    if not found:
        print("No filesystems found. Try 'carve' to pull files out by signature.")
        return
    print(f"\n{'#':>3}  {'offset (bytes)':>16}  {'fs':<6} {'size':>10}  label / id")
    for i, f in enumerate(found, 1):
        print(f"{i:>3}  {f['offset']:>16}  {f['fs']:<6} {human(f['size']):>10}  {f.get('label') or ''} {f.get('serial', '')}")
        if f.get("note"):
            print(f"{'':>24}note: {f['note']}")
    print("\nTo get files off a partition (read-only):")
    for i, f in enumerate(found, 1):
        print(f"  #{i}: {mount_hint(a.image, f)}")
        print(f"      or: {sys.argv[0]} extract '{a.image}' --offset {f['offset']} --length {f['size']} part{i}.img")
    if a.json:
        with open(a.json, "w") as fh:
            json.dump(found, fh, indent=2)
        print(f"\nSaved results to {a.json}")


# --------------------------------------------------------------------------- extract

def cmd_extract(a):
    refuse_dangerous_output(a.image, a.output, a.force)
    src = open_ro(a.image)
    size = device_size(src)
    length = min(a.length, size - a.offset)
    with open(a.output, "wb") as out:
        prog = Progress(length, "extract")
        done = 0
        while done < length:
            n = min(4 * MiB, length - done)
            data = safe_pread(src, n, a.offset + done)
            if len(data) < n:
                data = data + b"\0" * (n - len(data))
            out.write(data)
            done += n
            prog.update(done)
        prog.finish()
    print(f"Wrote {human(length)} to {a.output}. Mount with: sudo mount -o ro,loop '{a.output}' /mnt/rescue")


# --------------------------------------------------------------------------- carve

class Reader:
    def __init__(self, fd, size):
        self.fd, self.size = fd, size

    def read(self, off, n):
        if off >= self.size:
            return b""
        return safe_pread(self.fd, min(n, self.size - off), off)


def end_jpeg(r, off, maxlen):
    p = off + 2
    # walk marker segments until Start Of Scan
    while p < off + maxlen:
        h = r.read(p, 4)
        if len(h) < 4 or h[0] != 0xFF:
            return None
        m = h[1]
        if m == 0xD9:
            return p + 2
        if m == 0xDA:
            seglen = struct.unpack(">H", h[2:4])[0]
            p += 2 + seglen
            break
        if 0xD0 <= m <= 0xD7 or m == 0x01:
            p += 2
            continue
        seglen = struct.unpack(">H", h[2:4])[0]
        if seglen < 2:
            return None
        p += 2 + seglen
    # entropy-coded data: look for FF D9 (byte-stuffed FFs are FF 00)
    while p < off + maxlen:
        buf = r.read(p, MiB)
        if not buf:
            return None
        i = buf.find(b"\xff")
        while i != -1 and i + 1 < len(buf):
            nb = buf[i + 1]
            if nb == 0xD9:
                return p + i + 2
            if nb == 0xDA:  # progressive JPEG: another scan
                pass
            i = buf.find(b"\xff", i + 1)
        p += len(buf) - 1
    return None


def end_png(r, off, maxlen):
    p = off + 8
    while p < off + maxlen:
        h = r.read(p, 8)
        if len(h) < 8:
            return None
        ln, typ = struct.unpack(">I4s", h)
        if not typ.isalpha():
            return None
        p += 12 + ln
        if typ == b"IEND":
            return p
    return None


def end_gif(r, off, maxlen):
    buf = r.read(off, maxlen)
    i = buf.find(b"\x00\x3b", 13)
    return off + i + 2 if i != -1 else None


def end_pdf(r, off, maxlen):
    buf = r.read(off, maxlen)
    nxt = buf.find(b"%PDF-", 5)
    if nxt != -1:
        buf = buf[:nxt]
    i = buf.rfind(b"%%EOF")
    if i == -1:
        return None
    j = i + 5
    while j < len(buf) and buf[j:j + 1] in (b"\r", b"\n"):
        j += 1
    return off + j


def end_zip(r, off, maxlen):
    p = off
    step = 4 * MiB
    while p < off + maxlen:
        buf = r.read(p, step + 22)
        if not buf:
            return None
        i = buf.find(b"PK\x05\x06")
        while i != -1:
            if i + 22 <= len(buf):
                clen = struct.unpack_from("<H", buf, i + 20)[0]
                cd_size, cd_off = struct.unpack_from("<II", buf, i + 12)
                # sanity: central directory should end right where EOCD starts
                if cd_off + cd_size == p + i - off or cd_off == 0xFFFFFFFF:
                    return p + i + 22 + clen
            i = buf.find(b"PK\x05\x06", i + 1)
        p += step
    return None


def zip_kind(r, off, end):
    head = r.read(off, min(end - off, 8 * MiB))
    tail = r.read(max(off, end - 2 * MiB), min(end - off, 2 * MiB))
    blob = head + tail
    if b"word/" in blob:
        return "docx"
    if b"xl/" in blob:
        return "xlsx"
    if b"ppt/" in blob:
        return "pptx"
    if b"application/vnd.oasis.opendocument.text" in head:
        return "odt"
    if b"META-INF/MANIFEST.MF" in blob:
        return "jar"
    return "zip"


ISO_BOXES = {b"ftyp", b"moov", b"mdat", b"free", b"skip", b"wide", b"uuid", b"meta", b"pnot",
             b"udta", b"moof", b"mfra", b"sidx", b"styp", b"pdin", b"iinf", b"iloc"}


def end_iso(r, off, maxlen):
    p = off
    seen_data = False
    while p < off + maxlen:
        h = r.read(p, 16)
        if len(h) < 8:
            break
        sz, typ = struct.unpack(">I4s", h[:8])
        if typ not in ISO_BOXES:
            break
        if sz == 1:
            sz = struct.unpack(">Q", h[8:16])[0]
        elif sz == 0:
            return None
        if sz < 8:
            break
        if typ in (b"mdat", b"moov"):
            seen_data = True
        p += sz
    return p if seen_data and p > off + 8 else None


def iso_kind(r, off):
    brand = r.read(off + 8, 4)
    if brand in (b"heic", b"heix", b"mif1", b"msf1"):
        return "heic"
    if brand.startswith(b"qt"):
        return "mov"
    if brand.startswith(b"M4A"):
        return "m4a"
    if brand.startswith(b"3g"):
        return "3gp"
    return "mp4"


def end_riff(r, off, maxlen):
    h = r.read(off, 12)
    sz = struct.unpack_from("<I", h, 4)[0]
    if h[8:12] not in (b"WAVE", b"AVI ", b"WEBP") or sz < 4 or sz + 8 > maxlen:
        return None
    return off + 8 + sz


def riff_kind(r, off):
    return {b"WAVE": "wav", b"AVI ": "avi", b"WEBP": "webp"}[r.read(off + 8, 4)]


CARVERS = {
    # name: (signature, offset of signature in file, max size, end finder, kind fn)
    "jpg": ([b"\xff\xd8\xff"], 0, 64 * MiB, end_jpeg, None),
    "png": ([b"\x89PNG\r\n\x1a\n"], 0, 128 * MiB, end_png, None),
    "gif": ([b"GIF89a", b"GIF87a"], 0, 32 * MiB, end_gif, None),
    "pdf": ([b"%PDF-"], 0, 256 * MiB, end_pdf, None),
    "zip": ([b"PK\x03\x04"], 0, 2048 * MiB, end_zip, zip_kind),
    "mp4": ([b"ftyp"], 4, 64 * 1024 * MiB, end_iso, iso_kind),
    "riff": ([b"RIFF"], 0, 4096 * MiB, end_riff, riff_kind),
}


def cmd_carve(a):
    os.makedirs(a.outdir, exist_ok=True)
    types = a.types.split(",") if a.types else list(CARVERS)
    fd = open_ro(a.image)
    size = device_size(fd)
    r = Reader(fd, size)
    end_all = min(a.end or size, size)
    align = 1 if a.unaligned else SECTOR
    block = 16 * MiB
    pos = a.start - a.start % SECTOR
    counts = {}
    total_bytes = 0
    prog = Progress(end_all - a.start, "carve")
    skip_until = pos
    log = open(os.path.join(a.outdir, "carve-log.tsv"), "a")
    while pos < end_all:
        buf = r.read(pos, min(block, end_all - pos) + 16)
        if not buf:
            pos += block
            continue
        lim = min(block, end_all - pos)
        hits = []
        for name in types:
            sigs, sig_off, maxlen, ender, kinder = CARVERS[name]
            for sig in sigs:
                for o in _aligned_hits(buf, sig, sig_off, align):
                    if 0 <= o < lim:
                        hits.append((pos + o, name))
        for off, name in sorted(hits):
            if off < skip_until:
                continue
            sigs, sig_off, maxlen, ender, kinder = CARVERS[name]
            try:
                end = ender(r, off, min(maxlen, size - off))
            except (struct.error, KeyError, ValueError):
                end = None
            if not end or end - off < a.min_size:
                continue
            ext = kinder(r, off) if kinder and name != "zip" else name
            if name == "zip":
                ext = zip_kind(r, off, end)
            sub = os.path.join(a.outdir, ext)
            os.makedirs(sub, exist_ok=True)
            fname = os.path.join(sub, f"f{off:014d}.{ext}")
            with open(fname, "wb") as out:
                p = off
                while p < end:
                    chunk = r.read(p, min(8 * MiB, end - p))
                    if not chunk:
                        break
                    out.write(chunk)
                    p += len(chunk)
            counts[ext] = counts.get(ext, 0) + 1
            total_bytes += end - off
            log.write(f"{off}\t{end - off}\t{fname}\n")
            skip_until = end  # don't re-carve pieces inside a file we already recovered
        pos += block
        prog.update(min(pos, end_all) - a.start,
                    f"files {sum(counts.values())} ({human(total_bytes)})")
    prog.finish()
    log.close()
    os.close(fd)
    print(f"Recovered {sum(counts.values())} files ({human(total_bytes)}) into {a.outdir}")
    for k, v in sorted(counts.items()):
        print(f"  {k:<6} {v}")
    print("Note: carving cannot restore file names/folders and fails on fragmented files. "
          "For more formats, also run PhotoRec on the image.")


# --------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)

    p = sp.add_parser("info", help="show partition tables (MBR / primary+backup GPT)")
    p.add_argument("source")
    p.add_argument("--size", type=int, help="source size in bytes if it cannot be detected")
    p.set_defaults(fn=cmd_info)

    p = sp.add_parser("image", help="clone the disk to an image file, tolerating bad sectors (resumable)")
    p.add_argument("source", help="e.g. /dev/sdb (whole disk, not /dev/sdb1)")
    p.add_argument("output", help="image file on a DIFFERENT healthy disk")
    p.add_argument("--map", help="progress/bad-sector map (default: OUTPUT.map.json)")
    p.add_argument("--chunk", type=int, default=1024, help="phase-1 read size in KiB (default 1024)")
    p.add_argument("--max-skip", type=int, default=64, help="max MiB to jump past a bad area (default 64)")
    p.add_argument("--sector", type=int, default=512, help="sector size for retries (default 512)")
    p.add_argument("--retries", type=int, default=3, help="reads per sector in phase 2 (default 3)")
    p.add_argument("--no-retry", action="store_true", help="skip phase 2 (get the easy data first)")
    p.add_argument("--size", type=int, help="source size in bytes if it cannot be detected")
    p.add_argument("--force", action="store_true")
    p.set_defaults(fn=cmd_image)

    p = sp.add_parser("scan", help="find lost/damaged partitions by filesystem signature")
    p.add_argument("image")
    p.add_argument("--start", type=int, default=0)
    p.add_argument("--end", type=int)
    p.add_argument("--json", help="save results to this JSON file")
    p.set_defaults(fn=cmd_scan)

    p = sp.add_parser("extract", help="copy a byte range (a partition) out to its own image")
    p.add_argument("image")
    p.add_argument("output")
    p.add_argument("--offset", type=int, required=True)
    p.add_argument("--length", type=int, required=True)
    p.add_argument("--force", action="store_true")
    p.set_defaults(fn=cmd_extract)

    p = sp.add_parser("carve", help="recover files by signature (photos, video, pdf, office, zip, audio)")
    p.add_argument("image")
    p.add_argument("outdir")
    p.add_argument("--types", help="comma list from: " + ",".join(CARVERS))
    p.add_argument("--unaligned", action="store_true", help="also look for files not on sector boundaries")
    p.add_argument("--min-size", type=int, default=1024, help="ignore files smaller than this (bytes)")
    p.add_argument("--start", type=int, default=0)
    p.add_argument("--end", type=int)
    p.set_defaults(fn=cmd_carve)

    a = ap.parse_args()
    try:
        a.fn(a)
    except PermissionError as e:
        sys.exit(f"ERROR: {e}. Reading a raw disk needs root: run with sudo.")


if __name__ == "__main__":
    main()
