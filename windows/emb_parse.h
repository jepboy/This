// emb_parse.h - find the preview picture inside .EMB (Wilcom), .CDR (CorelDRAW)
// and .CST (Tajima DG/ML by Pulse) files.
//
// Portable C++17, no OS calls, so it can be unit-tested anywhere.
// An EMB file is an OLE2 / Compound File Binary container. Wilcom stores a
// small preview in a stream (usually DESIGN_ICON); some files also carry a
// thumbnail in the SummaryInformation property set. This returns the best
// picture it can find as a complete image file (BMP, PNG, JPEG or GIF), or a
// Windows metafile (WMF/EMF) that the caller has to draw.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace emb {

using Bytes = std::vector<uint8_t>;

enum class Kind { None, Bmp, Png, Jpg, Gif, Wmf, Emf };

struct Picture {
    Kind kind = Kind::None;
    Bytes data;
    std::string where;  // which stream it came from (for diagnostics)
};

inline uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline uint32_t b32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]); }
inline int32_t i32(const uint8_t* p) { return int32_t(u32(p)); }

// ---------------------------------------------------------------------------
// Compound File Binary reader (read-only, tolerant of damage)
// ---------------------------------------------------------------------------

struct Entry {
    std::string name;  // ASCII-folded, upper case
    int type = 0;      // 1 storage, 2 stream, 5 root
    uint32_t start = 0;
    uint64_t size = 0;
};

class Cfb {
public:
    bool open(const Bytes& d) {
        static const uint8_t magic[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
        if (d.size() < 512 || memcmp(d.data(), magic, 8) != 0) return false;
        data_ = &d;
        const uint8_t* h = d.data();
        unsigned shift = u16(h + 0x1E), mshift = u16(h + 0x20);
        if (shift != 9 && shift != 12) return false;
        if (mshift > 12) return false;
        ss_ = 1u << shift;
        ms_ = 1u << mshift;
        uint32_t nfat = u32(h + 0x2C), firstDir = u32(h + 0x30);
        cutoff_ = u32(h + 0x38);
        uint32_t firstMiniFat = u32(h + 0x3C), firstDifat = u32(h + 0x44), nDifat = u32(h + 0x48);

        std::vector<uint32_t> difat;
        for (int i = 0; i < 109; i++) difat.push_back(u32(h + 0x4C + 4 * i));
        uint32_t s = firstDifat;
        for (uint32_t n = 0; s < 0xFFFFFFFA && n <= nDifat && n < 10000; n++) {
            const uint8_t* p = sector(s);
            if (!p) break;
            unsigned per = ss_ / 4 - 1;
            for (unsigned i = 0; i < per; i++) difat.push_back(u32(p + 4 * i));
            s = u32(p + 4 * per);
        }
        for (uint32_t fs : difat) {
            if (fs >= 0xFFFFFFFA) continue;
            if (nfat && fat_.size() >= size_t(nfat) * (ss_ / 4)) break;
            const uint8_t* p = sector(fs);
            if (!p) continue;
            for (unsigned i = 0; i < ss_ / 4; i++) fat_.push_back(u32(p + 4 * i));
        }

        Bytes dir = chain(firstDir, fat_, ss_, nullptr, UINT64_MAX);
        for (size_t off = 0; off + 128 <= dir.size(); off += 128) {
            const uint8_t* e = dir.data() + off;
            Entry en;
            unsigned nlen = std::min<unsigned>(u16(e + 64), 64);
            for (unsigned i = 0; i + 1 < nlen; i += 2) {
                uint16_t c = u16(e + i);
                if (!c) break;
                char ch = c < 128 ? char(c) : '?';
                en.name += char(toupper((unsigned char)ch));
            }
            en.type = e[66];
            en.start = u32(e + 116);
            en.size = ss_ == 512 ? u32(e + 120) : (uint64_t(u32(e + 124)) << 32 | u32(e + 120));
            entries_.push_back(en);
        }
        if (entries_.empty() || entries_[0].type != 5) return false;

        if (firstMiniFat < 0xFFFFFFFA) {
            Bytes mf = chain(firstMiniFat, fat_, ss_, nullptr, UINT64_MAX);
            for (size_t i = 0; i + 4 <= mf.size(); i += 4) minifat_.push_back(u32(mf.data() + i));
        }
        if (entries_[0].start < 0xFFFFFFFA)
            mini_ = chain(entries_[0].start, fat_, ss_, nullptr, entries_[0].size);
        return true;
    }

    const std::vector<Entry>& entries() const { return entries_; }

    Bytes read(const Entry& e) const {
        const uint64_t cap = 64ull << 20;
        uint64_t size = std::min<uint64_t>(e.size, cap);
        if (e.size < cutoff_ && !mini_.empty()) return chain(e.start, minifat_, ms_, &mini_, size);
        return chain(e.start, fat_, ss_, nullptr, size);
    }

private:
    const uint8_t* sector(uint32_t n) const {
        uint64_t off = (uint64_t(n) + 1) * ss_;
        if (off + ss_ > data_->size()) return nullptr;
        return data_->data() + off;
    }
    // Follow a sector chain; if 'pool' is set, read mini sectors from it.
    Bytes chain(uint32_t s, const std::vector<uint32_t>& table, unsigned unit, const Bytes* pool, uint64_t limit) const {
        Bytes out;
        std::vector<bool> seen(table.size(), false);
        while (s < table.size() && !seen[s] && out.size() < limit) {
            seen[s] = true;
            const uint8_t* p;
            if (pool) {
                uint64_t off = uint64_t(s) * unit;
                if (off + unit > pool->size()) break;
                p = pool->data() + off;
            } else {
                p = sector(s);
                if (!p) break;
            }
            out.insert(out.end(), p, p + unit);
            s = table[s];
        }
        if (limit != UINT64_MAX && out.size() > limit) out.resize(size_t(limit));
        return out;
    }

    const Bytes* data_ = nullptr;
    unsigned ss_ = 512, ms_ = 64;
    uint32_t cutoff_ = 4096;
    std::vector<uint32_t> fat_, minifat_;
    std::vector<Entry> entries_;
    Bytes mini_;
};

// ---------------------------------------------------------------------------
// zlib / DEFLATE decompression (RFC 1950/1951). Wilcom compresses the
// DESIGN_ICON picture: 4-byte little-endian size, then a zlib stream.
// ---------------------------------------------------------------------------

class Inflater {
public:
    // Returns false on any error. 'out' gets at most 'cap' bytes.
    static bool zlib(const uint8_t* p, size_t n, Bytes& out, size_t cap) {
        if (n < 2) return false;
        if ((p[0] & 0x0F) != 8 || (p[0] >> 4) > 7 || ((p[0] << 8) | p[1]) % 31 != 0 || (p[1] & 0x20)) return false;
        Inflater in(p + 2, n - 2, out, cap);
        return in.run();
    }
    // Raw DEFLATE (as used inside ZIP files).
    static bool raw(const uint8_t* p, size_t n, Bytes& out, size_t cap) {
        Inflater in(p, n, out, cap);
        return in.run();
    }

private:
    struct Huff {
        uint16_t count[16];
        uint16_t sym[320];
    };

    Inflater(const uint8_t* p, size_t n, Bytes& out, size_t cap) : p_(p), n_(n), out_(out), cap_(cap) {}

    int bit() {
        if (!bitcnt_) {
            if (pos_ >= n_) { err_ = true; return 0; }
            bitbuf_ = p_[pos_++];
            bitcnt_ = 8;
        }
        int b = bitbuf_ & 1;
        bitbuf_ >>= 1;
        bitcnt_--;
        return b;
    }
    unsigned bits(int k) {
        unsigned v = 0;
        for (int i = 0; i < k; i++) v |= unsigned(bit()) << i;
        return v;
    }
    static bool build(Huff& h, const uint8_t* len, int n) {
        uint16_t offs[16];
        memset(h.count, 0, sizeof(h.count));
        for (int i = 0; i < n; i++) h.count[len[i]]++;
        h.count[0] = 0;
        int left = 1;
        for (int i = 1; i < 16; i++) {
            left <<= 1;
            left -= h.count[i];
            if (left < 0) return false;
        }
        offs[1] = 0;
        for (int i = 1; i < 15; i++) offs[i + 1] = uint16_t(offs[i] + h.count[i]);
        for (int i = 0; i < n; i++)
            if (len[i]) h.sym[offs[len[i]]++] = uint16_t(i);
        return true;
    }
    int decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++) {
            code |= bit();
            int count = h.count[len];
            if (code - count < first) return h.sym[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
            if (err_) return -1;
        }
        return -1;
    }
    bool codes(const Huff& lit, const Huff& dist) {
        static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int s = decode(lit);
            if (s < 0 || err_) return false;
            if (s < 256) {
                if (out_.size() >= cap_) return false;
                out_.push_back(uint8_t(s));
            } else if (s == 256) {
                return true;
            } else {
                s -= 257;
                if (s >= 29) return false;
                size_t len = lbase[s] + bits(lext[s]);
                int d = decode(dist);
                if (d < 0 || d >= 30) return false;
                size_t back = dbase[d] + bits(dext[d]);
                if (err_ || back > out_.size() || out_.size() + len > cap_) return false;
                size_t from = out_.size() - back;
                for (size_t i = 0; i < len; i++) out_.push_back(out_[from + i]);
            }
        }
    }
    bool stored() {
        bitbuf_ = 0;
        bitcnt_ = 0;
        if (pos_ + 4 > n_) return false;
        unsigned len = p_[pos_] | (p_[pos_ + 1] << 8);
        unsigned nlen = p_[pos_ + 2] | (p_[pos_ + 3] << 8);
        pos_ += 4;
        if (len != (~nlen & 0xFFFF) || pos_ + len > n_ || out_.size() + len > cap_) return false;
        out_.insert(out_.end(), p_ + pos_, p_ + pos_ + len);
        pos_ += len;
        return true;
    }
    bool fixed() {
        uint8_t l[288];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        Huff lit, dist;
        build(lit, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        build(dist, l, 30);
        return codes(lit, dist);
    }
    bool dynamic() {
        static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        uint8_t l[320] = {};
        int nlen = int(bits(5)) + 257, ndist = int(bits(5)) + 1, ncode = int(bits(4)) + 4;
        if (nlen > 286 || ndist > 30) return false;
        for (int i = 0; i < ncode; i++) l[order[i]] = uint8_t(bits(3));
        Huff lencode, lit, dist;
        if (!build(lencode, l, 19)) return false;
        memset(l, 0, sizeof(l));
        int idx = 0;
        while (idx < nlen + ndist) {
            int s = decode(lencode);
            if (s < 0 || err_) return false;
            if (s < 16) { l[idx++] = uint8_t(s); continue; }
            int len = 0, rep;
            if (s == 16) {
                if (idx == 0) return false;
                len = l[idx - 1];
                rep = 3 + int(bits(2));
            } else if (s == 17) rep = 3 + int(bits(3));
            else rep = 11 + int(bits(7));
            if (idx + rep > nlen + ndist) return false;
            while (rep--) l[idx++] = uint8_t(len);
        }
        if (l[256] == 0) return false;
        if (!build(lit, l, nlen) || !build(dist, l + nlen, ndist)) return false;
        return codes(lit, dist);
    }
    bool run() {
        int last;
        do {
            last = bit();
            unsigned type = bits(2);
            if (err_) return false;
            bool ok = type == 0 ? stored() : type == 1 ? fixed() : type == 2 ? dynamic() : false;
            if (!ok) return false;
        } while (!last);
        return !out_.empty();
    }

    const uint8_t* p_;
    size_t n_, pos_ = 0;
    Bytes& out_;
    size_t cap_;
    unsigned bitbuf_ = 0;
    int bitcnt_ = 0;
    bool err_ = false;
};

// Try to inflate a zlib stream near the start of 's' (Wilcom puts a 4-byte
// size first; also try a few other small offsets).
inline bool tryInflate(const Bytes& s, Bytes& out) {
    const size_t cap = 64u << 20;
    static const size_t offs[] = {4, 0, 8, 2, 6, 12, 16};
    for (size_t o : offs) {
        if (o + 2 >= s.size()) continue;
        out.clear();
        if (Inflater::zlib(s.data() + o, s.size() - o, out, cap) && out.size() > 32) return true;
    }
    for (size_t o = 0; o + 2 < s.size() && o < 64; o++) {
        if (s[o] != 0x78) continue;
        out.clear();
        if (Inflater::zlib(s.data() + o, s.size() - o, out, cap) && out.size() > 32) return true;
    }
    out.clear();
    return false;
}

// ---------------------------------------------------------------------------
// Picture detection
// ---------------------------------------------------------------------------

// Raw DIB (BITMAPINFOHEADER + palette + pixels) -> BMP file. Empty if invalid.
inline Bytes dibToBmp(const uint8_t* p, size_t avail) {
    if (avail < 40) return {};
    uint32_t hs = u32(p);
    int32_t w = i32(p + 4), h = i32(p + 8);
    uint16_t planes = u16(p + 12), bpp = u16(p + 14);
    uint32_t comp = u32(p + 16), imgsize = u32(p + 20), clrused = u32(p + 32);
    if (hs != 40 && hs != 108 && hs != 124) return {};
    if (planes != 1) return {};
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return {};
    if (w <= 0 || w > 4096 || h == 0 || h > 4096 || h < -4096) return {};
    if (comp > 3) return {};
    if (clrused > 256) return {};
    uint64_t ncol = clrused ? clrused : (bpp <= 8 ? (1u << bpp) : 0);
    uint64_t extra = (comp == 3 && hs == 40) ? 12 : 0;
    uint64_t stride = ((uint64_t(w) * bpp + 31) / 32) * 4;
    uint64_t ah = h < 0 ? uint64_t(-int64_t(h)) : uint64_t(h);
    uint64_t pix = (comp == 1 || comp == 2) && imgsize ? imgsize : stride * ah;
    uint64_t hdr = hs + extra + ncol * 4;
    uint64_t total = hdr + pix;
    if (total > avail) {
        // Tolerate a slightly truncated uncompressed bitmap (keep >= 3/4 of rows).
        if (comp != 0 || hdr + pix * 3 / 4 > avail) return {};
        total = avail;
    }
    Bytes out(14 + size_t(total));
    out[0] = 'B'; out[1] = 'M';
    uint32_t fsz = uint32_t(out.size()), off = uint32_t(14 + hdr);
    memcpy(&out[2], &fsz, 4);
    memcpy(&out[10], &off, 4);
    memcpy(&out[14], p, size_t(total));
    return out;
}

inline bool isEmf(const uint8_t* p, size_t n) { return n >= 88 && u32(p) == 1 && memcmp(p + 40, " EMF", 4) == 0; }

struct Hit { size_t at; Kind kind; Bytes data; };

inline void scan(const uint8_t* b, size_t n, std::vector<Hit>& hits) {
    for (size_t i = 0; i + 8 <= n; i++) {
        const uint8_t* p = b + i;
        size_t left = n - i;
        if (p[0] == 0x89 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0) {
            size_t j = 8;
            while (j + 12 <= left) {
                uint32_t len = b32(p + j);
                bool iend = memcmp(p + j + 4, "IEND", 4) == 0;
                if (len > left) break;
                j += 12 + size_t(len);
                if (iend) { hits.push_back({i, Kind::Png, Bytes(p, p + std::min(j, left))}); break; }
            }
        } else if (p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) {
            for (size_t j = 2; j + 1 < left; j++)
                if (p[j] == 0xFF && p[j + 1] == 0xD9) {
                    if (j + 2 > 64) hits.push_back({i, Kind::Jpg, Bytes(p, p + j + 2)});
                    break;
                }
        } else if (p[0] == 'G' && (memcmp(p, "GIF87a", 6) == 0 || memcmp(p, "GIF89a", 6) == 0)) {
            for (size_t j = 6; j + 1 < left; j++)
                if (p[j] == 0 && p[j + 1] == 0x3B) { hits.push_back({i, Kind::Gif, Bytes(p, p + j + 2)}); break; }
        } else if (p[0] == 'B' && p[1] == 'M' && left >= 54) {
            uint32_t sz = u32(p + 2);
            Bytes bmp = dibToBmp(p + 14, std::min<size_t>(left - 14, sz > 14 ? sz - 14 : 0));
            if (!bmp.empty() && sz <= left) hits.push_back({i, Kind::Bmp, Bytes(p, p + sz)});
        } else if ((p[0] == 40 || p[0] == 108 || p[0] == 124) && p[1] == 0 && p[2] == 0 && p[3] == 0) {
            Bytes bmp = dibToBmp(p, left);
            if (!bmp.empty()) hits.push_back({i, Kind::Bmp, std::move(bmp)});
        } else if (p[0] == 0xD7 && memcmp(p, "\xD7\xCD\xC6\x9A", 4) == 0) {
            hits.push_back({i, Kind::Wmf, Bytes(p, b + n)});
        } else if (isEmf(p, left)) {
            uint32_t sz = u32(p + 48);
            hits.push_back({i, Kind::Emf, Bytes(p, p + (sz && sz <= left ? sz : left))});
        }
    }
}

inline bool isRaster(Kind k) { return k == Kind::Bmp || k == Kind::Png || k == Kind::Jpg || k == Kind::Gif; }

// Pick the best hit: raster pictures before metafiles, then the biggest.
inline bool better(const Hit& a, const Hit& b) {
    if (isRaster(a.kind) != isRaster(b.kind)) return isRaster(a.kind);
    return a.data.size() > b.data.size();
}

// PIDSI_THUMBNAIL (property 17, VT_CF) in a SummaryInformation stream.
inline bool summaryThumb(const Bytes& s, Picture& out) {
    if (s.size() < 48) return false;
    uint32_t sec = u32(&s[44]);
    if (sec + 8 > s.size()) return false;
    uint32_t n = std::min<uint32_t>(u32(&s[sec + 4]), 256);
    for (uint32_t k = 0; k < n; k++) {
        size_t q = size_t(sec) + 8 + 8 * k;
        if (q + 8 > s.size()) return false;
        if (u32(&s[q]) != 17) continue;
        size_t p = size_t(sec) + u32(&s[q + 4]);
        if (p + 16 > s.size() || u32(&s[p]) != 71) return false;  // VT_CF
        uint32_t cb = u32(&s[p + 4]);
        int32_t fmt = i32(&s[p + 8]);
        size_t end = std::min(s.size(), p + 8 + size_t(cb));
        if (fmt != -1 || p + 16 > end) return false;
        uint32_t cf = u32(&s[p + 12]);
        const uint8_t* pl = &s[p + 16];
        size_t len = end - (p + 16);
        if (cf == 8) {  // CF_DIB
            out.data = dibToBmp(pl, len);
            out.kind = out.data.empty() ? Kind::None : Kind::Bmp;
        } else if (cf == 3 && len > 8) {  // CF_METAFILEPICT: 8-byte header + WMF records
            out.kind = Kind::Wmf;
            out.data.assign(pl + 8, pl + len);
        } else if (cf == 14) {
            out.kind = Kind::Emf;
            out.data.assign(pl, pl + len);
        }
        out.where = "SummaryInformation";
        return out.kind != Kind::None;
    }
    return false;
}

// ---------------------------------------------------------------------------
// ZIP containers (CorelDRAW X4 and later keep previews/*.png inside a ZIP)
// ---------------------------------------------------------------------------

inline bool endsWith(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

inline Kind magicKind(const Bytes& b) {
    if (b.size() < 16) return Kind::None;
    if (memcmp(b.data(), "\x89PNG", 4) == 0) return Kind::Png;
    if (b[0] == 0xFF && b[1] == 0xD8) return Kind::Jpg;
    if (b[0] == 'B' && b[1] == 'M') return Kind::Bmp;
    if (memcmp(b.data(), "GIF8", 4) == 0) return Kind::Gif;
    return Kind::None;
}

inline Picture findPictureZip(const Bytes& f) {
    Picture best;
    size_t n = f.size();
    if (n < 22) return best;
    // End of central directory: last 22..65557 bytes.
    size_t eocd = SIZE_MAX;
    for (size_t i = n - 22 + 1; i-- > 0 && n - i <= 65557;)
        if (u32(&f[i]) == 0x06054b50) { eocd = i; break; }
    if (eocd == SIZE_MAX) return best;
    uint32_t cdSize = u32(&f[eocd + 12]), cdOff = u32(&f[eocd + 16]);
    unsigned count = u16(&f[eocd + 10]);
    if (uint64_t(cdOff) + cdSize > n) return best;
    int bestScore = -1;
    size_t bestSize = 0;
    size_t q = cdOff;
    for (unsigned k = 0; k < count && q + 46 <= n && u32(&f[q]) == 0x02014b50; k++) {
        unsigned method = u16(&f[q + 10]);
        uint32_t csize = u32(&f[q + 20]), usize = u32(&f[q + 24]);
        unsigned nl = u16(&f[q + 28]), xl = u16(&f[q + 30]), cl = u16(&f[q + 32]);
        uint32_t lho = u32(&f[q + 42]);
        if (q + 46 + nl > n) break;
        std::string name;
        for (unsigned i = 0; i < nl; i++) name += char(tolower(f[q + 46 + i]));
        q += 46 + size_t(nl) + xl + cl;

        bool img = endsWith(name, ".png") || endsWith(name, ".bmp") || endsWith(name, ".jpg") || endsWith(name, ".jpeg");
        if (!img) continue;
        int score = 0;
        if (name.find("thumbnail") != std::string::npos) score = 3;
        else if (name.compare(0, 9, "previews/") == 0 || name.find("/thumbnails/") != std::string::npos) score = 2;
        else if (name.find("preview") != std::string::npos || name.find("thumb") != std::string::npos || name.find("icon") != std::string::npos) score = 1;
        else continue;  // other pictures in the archive are document content, not previews
        if (score < bestScore || (score == bestScore && usize <= bestSize)) continue;
        if (usize > (64u << 20) || uint64_t(lho) + 30 > n || u32(&f[lho]) != 0x04034b50) continue;
        size_t data = size_t(lho) + 30 + u16(&f[lho + 26]) + u16(&f[lho + 28]);
        if (uint64_t(data) + csize > n) continue;
        Bytes out;
        if (method == 0) out.assign(f.begin() + data, f.begin() + data + csize);
        else if (method != 8 || !Inflater::raw(&f[data], csize, out, size_t(usize) + 16)) continue;
        Kind kind = magicKind(out);
        if (kind == Kind::None) continue;
        bestScore = score;
        bestSize = usize;
        best.kind = kind;
        best.data = std::move(out);
        best.where = name;
    }
    return best;
}

// ---------------------------------------------------------------------------
// RIFF containers (CorelDRAW up to X3: the "DISP" chunk is the preview)
// ---------------------------------------------------------------------------

inline Picture findPictureRiff(const Bytes& f) {
    Picture p;
    for (size_t i = 12; i + 12 + 40 <= f.size(); i++) {
        if (f[i] != 'D' || memcmp(&f[i], "DISP", 4) != 0) continue;
        uint32_t len = u32(&f[i + 4]);
        size_t avail = std::min<size_t>(f.size() - (i + 12), len > 4 ? len - 4 : 0);
        Bytes bmp = dibToBmp(&f[i + 12], avail);
        if (bmp.empty()) bmp = dibToBmp(&f[i + 8], std::min<size_t>(f.size() - (i + 8), len));
        if (!bmp.empty()) {
            p.kind = Kind::Bmp;
            p.data = std::move(bmp);
            p.where = "DISP";
            return p;
        }
    }
    return p;
}

inline Picture findPictureRaw(const Bytes& file) {
    Picture best;
    std::vector<Hit> hits;
    Bytes inflated;
    if (tryInflate(file, inflated)) scan(inflated.data(), inflated.size(), hits);
    if (hits.empty()) scan(file.data(), file.size(), hits);
    if (!hits.empty()) {
        auto it = std::min_element(hits.begin(), hits.end(), better);
        best.kind = it->kind; best.data = std::move(it->data); best.where = "raw";
    }
    return best;
}

inline int streamRank(const std::string& name) {
    static const char* pref[] = {"DESIGN_ICON", "THUMBNAIL", "PREVIEW", "ICON"};
    for (int i = 0; i < 4; i++)
        if (name.find(pref[i]) != std::string::npos) return i;
    return 99;
}

// Main entry point.
inline Picture findPicture(const Bytes& file) {
    Picture best;
    if (file.size() >= 4 && u32(file.data()) == 0x04034b50) {  // ZIP (CorelDRAW X4+)
        best = findPictureZip(file);
        return best.kind != Kind::None ? best : findPictureRaw(file);
    }
    if (file.size() >= 12 && memcmp(file.data(), "RIFF", 4) == 0) {  // CorelDRAW <= X3
        best = findPictureRiff(file);
        return best.kind != Kind::None ? best : findPictureRaw(file);
    }
    Cfb cfb;
    if (!cfb.open(file)) return findPictureRaw(file);
    std::vector<const Entry*> streams;
    for (const Entry& e : cfb.entries())
        if (e.type == 2) streams.push_back(&e);
    std::stable_sort(streams.begin(), streams.end(),
                     [](const Entry* a, const Entry* b) { return streamRank(a->name) < streamRank(b->name); });

    Picture meta;  // best metafile seen, used only if no raster picture exists
    for (const Entry* e : streams) {
        bool preferred = streamRank(e->name) < 99;
        if (!preferred && e->size > (8u << 20)) continue;
        Bytes s = cfb.read(*e);
        if (e->name.find("SUMMARYINFORMATION") != std::string::npos) {
            Picture p;
            if (summaryThumb(s, p)) {
                if (isRaster(p.kind)) { if (best.kind == Kind::None) best = std::move(p); }
                else if (meta.kind == Kind::None) meta = std::move(p);
            }
            continue;
        }
        std::vector<Hit> hits;
        // Preferred streams (DESIGN_ICON) are normally zlib-compressed: try that first.
        Bytes inflated;
        if ((preferred || s.size() <= (1u << 20)) && tryInflate(s, inflated)) scan(inflated.data(), inflated.size(), hits);
        if (hits.empty()) scan(s.data(), s.size(), hits);
        if (hits.empty()) continue;
        auto it = std::min_element(hits.begin(), hits.end(), better);
        if (isRaster(it->kind)) {
            if (best.kind == Kind::None || (!preferred && it->data.size() > best.data.size() && streamRank(best.where) == 99)) {
                best.kind = it->kind; best.data = std::move(it->data); best.where = e->name;
            }
            if (preferred) return best;  // DESIGN_ICON etc. wins outright
        } else if (meta.kind == Kind::None) {
            meta.kind = it->kind; meta.data = std::move(it->data); meta.where = e->name;
        }
    }
    return best.kind != Kind::None ? best : meta;
}

}  // namespace emb
