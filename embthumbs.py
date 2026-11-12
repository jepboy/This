#!/usr/bin/env python3
"""
embthumbs - see the thumbnails of Wilcom .EMB embroidery files.

Most EMB files are Microsoft "compound documents" (OLE2 / CFB), the same
container old .doc/.xls files use. Wilcom stores a small preview picture
inside it (usually in a stream called DESIGN_ICON, sometimes as the
SummaryInformation thumbnail). This tool opens the container read-only,
finds those pictures and builds a browsable HTML gallery.

Pure Python 3 standard library. It never modifies your EMB files.

Usage:
    python3 embthumbs.py gallery  <folder-or-files...> [-o out_dir] [--no-recurse]
    python3 embthumbs.py extract  <file.emb> [-o out_dir]
    python3 embthumbs.py info     <file.emb>
"""

import argparse
import html
import os
import struct
import sys
import zlib

# ---------------------------------------------------------------------------
# Minimal read-only OLE2 / Compound File Binary reader
# ---------------------------------------------------------------------------

CFB_MAGIC = b"\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1"
FREESECT, ENDOFCHAIN, FATSECT, DIFSECT = 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFD, 0xFFFFFFFC
NOSTREAM = 0xFFFFFFFF


class CFBError(Exception):
    pass


class CompoundFile:
    def __init__(self, data):
        if data[:8] != CFB_MAGIC:
            raise CFBError("not an OLE2 compound file")
        if len(data) < 512:
            raise CFBError("file too short")
        self.data = data
        (sec_shift, mini_shift) = struct.unpack_from("<HH", data, 0x1E)
        self.sec_size = 1 << sec_shift
        self.mini_size = 1 << mini_shift
        (n_fat, first_dir, _tx, self.mini_cutoff, first_mini_fat, n_mini_fat,
         first_difat, n_difat) = struct.unpack_from("<IIIIIIII", data, 0x2C)
        if self.sec_size not in (512, 4096):
            raise CFBError("bad sector size")

        # DIFAT -> list of FAT sectors
        difat = list(struct.unpack_from("<109I", data, 0x4C))
        sec, seen = first_difat, set()
        per = self.sec_size // 4 - 1
        while sec not in (ENDOFCHAIN, FREESECT) and sec not in seen and len(seen) < max(n_difat, 1) + 1000:
            seen.add(sec)
            raw = self._sector(sec)
            vals = struct.unpack_from("<%dI" % (per + 1), raw)
            difat.extend(vals[:per])
            sec = vals[per]
        fat_secs = [s for s in difat if s < 0xFFFFFFFA][:max(n_fat, 0) or None]

        fat = []
        for s in fat_secs:
            raw = self._sector(s)
            fat.extend(struct.unpack_from("<%dI" % (len(raw) // 4), raw))
        self.fat = fat

        self.entries = self._read_dir(first_dir)
        root = self.entries[0] if self.entries else None
        if root is None:
            raise CFBError("no root directory entry")
        self.ministream = self._chain_bytes(root["start"], root["size"]) if root["start"] < 0xFFFFFFFA else b""
        mf = self._chain_bytes(first_mini_fat, None) if first_mini_fat < 0xFFFFFFFA else b""
        self.minifat = list(struct.unpack_from("<%dI" % (len(mf) // 4), mf)) if mf else []
        self._build_paths()

    def _sector(self, n):
        off = (n + 1) * self.sec_size
        return self.data[off:off + self.sec_size]

    def _chain(self, start, table):
        out, seen, s = [], set(), start
        while s < len(table) and s not in seen and s < 0xFFFFFFFA:
            seen.add(s)
            out.append(s)
            s = table[s]
        return out

    def _chain_bytes(self, start, size):
        buf = b"".join(self._sector(s) for s in self._chain(start, self.fat))
        if not buf and start < 0xFFFFFFFA:          # FAT damaged: read linearly
            off = (start + 1) * self.sec_size
            buf = self.data[off:off + (size or self.sec_size)]
        return buf if size is None else buf[:size]

    def _read_dir(self, first):
        raw = self._chain_bytes(first, None)
        entries = []
        for i in range(len(raw) // 128):
            e = raw[i * 128:(i + 1) * 128]
            nlen = struct.unpack_from("<H", e, 64)[0]
            name = e[:max(0, min(nlen, 64) - 2)].decode("utf-16-le", "replace")
            etype = e[66]
            left, right, child = struct.unpack_from("<III", e, 68)
            start, size = struct.unpack_from("<IQ", e, 116)
            if self.sec_size == 512:
                size &= 0xFFFFFFFF
            entries.append(dict(id=i, name=name, type=etype, left=left, right=right,
                                child=child, start=start, size=size, path=None))
        return entries

    def _build_paths(self):
        def walk(idx, prefix, depth=0):
            stack = [idx]
            seen = set()
            while stack:
                i = stack.pop()
                if i == NOSTREAM or i >= len(self.entries) or i in seen or depth > 32:
                    continue
                seen.add(i)
                e = self.entries[i]
                e["path"] = prefix + e["name"]
                stack += [e["left"], e["right"]]
                if e["type"] in (1, 5) and e["child"] != NOSTREAM:
                    walk(e["child"], e["path"] + "/", depth + 1)
        root = self.entries[0]
        root["path"] = ""
        walk(root["child"], "")

    def streams(self):
        return [e for e in self.entries if e["type"] == 2 and e["path"]]

    def read(self, e):
        if e["size"] < self.mini_cutoff and self.ministream:
            out = b"".join(
                self.ministream[s * self.mini_size:(s + 1) * self.mini_size]
                for s in self._chain(e["start"], self.minifat))
            return out[:e["size"]]
        return self._chain_bytes(e["start"], e["size"])


# ---------------------------------------------------------------------------
# Finding pictures inside bytes
# ---------------------------------------------------------------------------

def _png_end(buf, start):
    i = start + 8
    while i + 12 <= len(buf):
        ln = struct.unpack_from(">I", buf, i)[0]
        typ = buf[i + 4:i + 8]
        i += 12 + ln
        if typ == b"IEND":
            return min(i, len(buf))
        if ln > len(buf):
            break
    return None


def _jpg_end(buf, start):
    j = buf.find(b"\xFF\xD9", start + 2)
    return j + 2 if j > 0 else None


def _gif_end(buf, start):
    j = buf.find(b"\x00\x3B", start + 6)
    return j + 2 if j > 0 else None


def _bmp_end(buf, start):
    if start + 54 > len(buf):
        return None
    size = struct.unpack_from("<I", buf, start + 2)[0]
    if 54 < size <= len(buf) - start and buf[start + 14:start + 18] in (b"\x28\0\0\0", b"\x6C\0\0\0", b"\x7C\0\0\0"):
        return start + size
    return None


def dib_to_bmp(buf, start=0):
    """A raw DIB (BITMAPINFOHEADER + pixels, as Windows clipboards and OLE
    thumbnails store it) -> a .bmp file any browser can show."""
    if start + 40 > len(buf):
        return None
    hsize, w, h, planes, bpp, comp, imgsize, _, _, clrused = struct.unpack_from("<IiiHHIIiiI", buf, start)
    if hsize not in (40, 108, 124) or planes != 1 or bpp not in (1, 4, 8, 16, 24, 32):
        return None
    if not (0 < w <= 4096 and 0 < abs(h) <= 4096) or comp not in (0, 1, 2, 3):
        return None
    ncol = clrused or (1 << bpp if bpp <= 8 else 0)
    extra = 12 if comp == 3 and hsize == 40 else 0
    off = 14 + hsize + extra + ncol * 4
    stride = ((w * bpp + 31) // 32) * 4
    pix = imgsize if (comp in (1, 2) and imgsize) else stride * abs(h)
    total = hsize + extra + ncol * 4 + pix
    if start + total > len(buf):
        if comp != 0 or start + total - pix // 4 > len(buf):
            return None
        total = len(buf) - start          # slightly short: keep what's there
    body = buf[start:start + total]
    return b"BM" + struct.pack("<IHHI", 14 + len(body), 0, 0, off) + body


SIGS = [
    (b"\x89PNG\r\n\x1a\n", "png", _png_end),
    (b"\xFF\xD8\xFF", "jpg", _jpg_end),
    (b"GIF87a", "gif", _gif_end),
    (b"GIF89a", "gif", _gif_end),
    (b"BM", "bmp", _bmp_end),
]


def find_images(buf, label, depth=0):
    """Yield (ext, bytes, how) for every picture found in buf."""
    found = []
    for magic, ext, ender in SIGS:
        i = buf.find(magic)
        while i >= 0:
            end = ender(buf, i)
            if end and end - i > 64:
                found.append((i, ext, buf[i:end], "%s @%d" % (label, i)))
            i = buf.find(magic, i + 1)
    # raw DIB headers (no 'BM' file header)
    for hs in (b"\x28\0\0\0", b"\x6C\0\0\0", b"\x7C\0\0\0"):
        i = buf.find(hs)
        while i >= 0:
            if not (i >= 14 and buf[i - 14:i - 12] == b"BM"):
                bmp = dib_to_bmp(buf, i)
                if bmp:
                    found.append((i, "bmp", bmp, "%s DIB @%d" % (label, i)))
            i = buf.find(hs, i + 1)
    # Windows metafiles (browsers can't show these, but save them anyway)
    for magic, ext in ((b"\xD7\xCD\xC6\x9A", "wmf"), (b"\x01\0\0\0", "emf")):
        i = buf.find(magic)
        if ext == "emf":
            while i >= 0 and buf[i + 40:i + 44] != b" EMF":
                i = buf.find(magic, i + 1)
        if i >= 0:
            found.append((i, ext, buf[i:], "%s %s @%d" % (label, ext.upper(), i)))

    # compressed blobs (zlib) may hide a picture
    if depth == 0 and not found:
        for i in range(0, min(len(buf), 4096)):
            if buf[i] == 0x78 and buf[i + 1:i + 2] in (b"\x01", b"\x5E", b"\x9C", b"\xDA"):
                try:
                    inner = zlib.decompressobj().decompress(buf[i:], 32 << 20)
                except zlib.error:
                    continue
                if len(inner) > 64:
                    found += [(i, *x) for x in find_images(inner, label + " zlib@%d" % i, 1)]
                    if found:
                        break

    found.sort(key=lambda t: (-len(t[2]) if t[1] in ("png", "jpg", "gif", "bmp") else 0, t[0]))
    return [(ext, data, how) for _, ext, data, how in found]


def summary_thumbnail(buf):
    """PIDSI_THUMBNAIL (property 17) in a SummaryInformation stream."""
    try:
        if len(buf) < 48:
            return None
        sec_off = struct.unpack_from("<I", buf, 44)[0]
        _size, nprops = struct.unpack_from("<II", buf, sec_off)
        for k in range(min(nprops, 256)):
            pid, poff = struct.unpack_from("<II", buf, sec_off + 8 + 8 * k)
            if pid != 17:
                continue
            p = sec_off + poff
            vt, cb, fmt = struct.unpack_from("<IIi", buf, p)
            if vt != 71:                       # VT_CF
                return None
            payload = buf[p + 12:p + 8 + cb]
            if fmt == -1 and len(payload) >= 4:
                cf = struct.unpack_from("<I", payload)[0]
                payload = payload[4:]
                if cf == 8:                    # CF_DIB
                    return ("bmp", dib_to_bmp(payload), "SummaryInformation thumbnail (DIB)")
                if cf == 3:                    # CF_METAFILEPICT: 8-byte header + WMF
                    return ("wmf", payload[8:], "SummaryInformation thumbnail (WMF)")
                if cf == 14:
                    return ("emf", payload, "SummaryInformation thumbnail (EMF)")
            imgs = find_images(payload, "SummaryInformation")
            return imgs[0] if imgs else None
    except struct.error:
        return None
    return None


# ---------------------------------------------------------------------------
# Per-file logic
# ---------------------------------------------------------------------------

PREFERRED = ("DESIGN_ICON", "THUMBNAIL", "PREVIEW", "ICON")
BROWSER_OK = ("png", "jpg", "gif", "bmp")


def thumbnails_for(path):
    """Return (list_of (ext, bytes, how), notes)."""
    with open(path, "rb") as f:
        data = f.read()
    notes = []
    results = []
    try:
        cf = CompoundFile(data)
    except CFBError as ex:
        notes.append("not a compound file (%s); scanning raw bytes" % ex)
        return find_images(data, "raw"), notes

    streams = cf.streams()
    streams.sort(key=lambda e: next((i for i, k in enumerate(PREFERRED) if k in e["name"].upper()), 99))
    for e in streams:
        buf = cf.read(e)
        if "SummaryInformation" in e["name"]:
            t = summary_thumbnail(buf)
            if t and t[1]:
                results.append(t)
            continue
        is_icon = any(k in e["name"].upper() for k in PREFERRED)
        if not is_icon and e["size"] > 8 << 20:
            continue
        imgs = find_images(buf, e["path"])
        if not imgs and is_icon:
            notes.append("%s: %d bytes, unknown picture format (starts %s)"
                         % (e["path"], len(buf), buf[:16].hex(" ")))
        results += imgs
    if not results:
        notes.append("no embedded picture found")
    return results, notes


def best(results):
    for r in results:
        if r[0] in BROWSER_OK:
            return r
    return results[0] if results else None


def iter_emb(paths, recurse=True):
    for p in paths:
        if os.path.isdir(p):
            if recurse:
                for root, dirs, files in os.walk(p):
                    dirs.sort()
                    for fn in sorted(files):
                        if fn.lower().endswith(".emb"):
                            yield os.path.join(root, fn)
            else:
                for fn in sorted(os.listdir(p)):
                    if fn.lower().endswith(".emb"):
                        yield os.path.join(p, fn)
        else:
            yield p


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

GALLERY_HEAD = """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EMB thumbnails</title><style>
:root{--bg:#f6f6f4;--card:#fff;--fg:#222;--muted:#777;--line:#ddd}
@media (prefers-color-scheme:dark){:root{--bg:#1b1b1b;--card:#262626;--fg:#eee;--muted:#999;--line:#3a3a3a}}
body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font:14px system-ui,sans-serif}
header{display:flex;gap:12px;align-items:center;flex-wrap:wrap;margin-bottom:16px}
h1{font-size:18px;margin:0}input{flex:1;min-width:180px;padding:8px;border:1px solid var(--line);border-radius:6px;background:var(--card);color:var(--fg)}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(var(--sz,160px),1fr));gap:12px}
.c{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:8px;text-align:center;overflow:hidden}
.c .im{height:var(--sz,160px);display:flex;align-items:center;justify-content:center;background:#fff;border-radius:4px}
.c img{width:100%;height:100%;object-fit:contain}
.n{margin-top:6px;word-break:break-all;font-size:12px}.m{color:var(--muted);font-size:11px}
.none{color:#999;font-size:12px}
</style></head><body><header><h1>EMB thumbnails (COUNT)</h1>
<input id="q" placeholder="Filter by name..." oninput="f()">
<label>Size <input type="range" min="80" max="400" value="160" style="flex:none;width:120px"
oninput="document.body.style.setProperty('--sz',this.value+'px')"></label></header><div class="grid" id="g">
"""
GALLERY_TAIL = """</div><script>function f(){var q=document.getElementById('q').value.toLowerCase();
document.querySelectorAll('.c').forEach(function(c){c.style.display=c.dataset.n.indexOf(q)<0?'none':''})}</script></body></html>
"""


def cmd_gallery(a):
    out = a.output
    tdir = os.path.join(out, "thumbs")
    os.makedirs(tdir, exist_ok=True)
    cards, n_ok, n = [], 0, 0
    for path in iter_emb(a.paths, not a.no_recurse):
        n += 1
        rel = os.path.relpath(path, a.paths[0]) if os.path.isdir(a.paths[0]) else os.path.basename(path)
        try:
            results, notes = thumbnails_for(path)
        except OSError as ex:
            results, notes = [], [str(ex)]
        pick = best(results)
        name = html.escape(rel)
        href = html.escape(os.path.relpath(os.path.abspath(path), os.path.abspath(out)).replace(os.sep, "/"))
        if pick and pick[0] in BROWSER_OK:
            fn = "%05d.%s" % (n, pick[0])
            with open(os.path.join(tdir, fn), "wb") as f:
                f.write(pick[1])
            inner = '<img loading="lazy" src="thumbs/%s" alt="">' % fn
            n_ok += 1
        else:
            inner = '<span class="none">%s</span>' % html.escape(
                "preview is %s (not viewable in browser)" % pick[0].upper() if pick else "no preview")
        cards.append('<div class="c" data-n="%s"><a class="im" href="%s" title="%s">%s</a>'
                     '<div class="n">%s</div><div class="m">%s KB</div></div>\n'
                     % (name.lower(), href, html.escape("; ".join(notes)), inner, name,
                        os.path.getsize(path) // 1024 if os.path.exists(path) else "?"))
        print("[%s] %s" % ("ok" if inner.startswith("<img") else "--", rel), file=sys.stderr)
    page = os.path.join(out, "index.html")
    with open(page, "w", encoding="utf-8") as f:
        f.write(GALLERY_HEAD.replace("COUNT", str(n)) + "".join(cards) + GALLERY_TAIL)
    print("\n%d of %d files have a viewable thumbnail.\nOpen: %s" % (n_ok, n, os.path.abspath(page)))


def cmd_extract(a):
    os.makedirs(a.output, exist_ok=True)
    results, notes = thumbnails_for(a.file)
    base = os.path.splitext(os.path.basename(a.file))[0]
    for i, (ext, data, how) in enumerate(results):
        fn = os.path.join(a.output, "%s_%d.%s" % (base, i, ext))
        with open(fn, "wb") as f:
            f.write(data)
        print("%s  <- %s (%d bytes)" % (fn, how, len(data)))
    for n in notes:
        print("note:", n)


def cmd_info(a):
    with open(a.file, "rb") as f:
        data = f.read()
    print("%s: %d bytes, starts %s" % (a.file, len(data), data[:8].hex(" ")))
    try:
        cf = CompoundFile(data)
    except CFBError as ex:
        print("Not a compound file:", ex)
        return
    for e in cf.streams():
        buf = cf.read(e)
        print("  %-40s %9d bytes  %s" % (e["path"], e["size"], buf[:16].hex(" ")))
    results, notes = thumbnails_for(a.file)
    print("Pictures found:")
    for ext, d, how in results:
        print("  %-4s %8d bytes  %s" % (ext, len(d), how))
    for n in notes:
        print("note:", n)


def main(argv=None):
    p = argparse.ArgumentParser(description="See thumbnails of Wilcom .EMB embroidery files.")
    sub = p.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gallery", help="build an HTML thumbnail gallery for a folder of EMB files")
    g.add_argument("paths", nargs="+")
    g.add_argument("-o", "--output", default="emb_gallery")
    g.add_argument("--no-recurse", action="store_true", help="don't look in subfolders")
    e = sub.add_parser("extract", help="save every embedded picture of one EMB file")
    e.add_argument("file")
    e.add_argument("-o", "--output", default=".")
    i = sub.add_parser("info", help="list the streams inside an EMB file (for troubleshooting)")
    i.add_argument("file")
    a = p.parse_args(argv)
    {"gallery": cmd_gallery, "extract": cmd_extract, "info": cmd_info}[a.cmd](a)


if __name__ == "__main__":
    main()
