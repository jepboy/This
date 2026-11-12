// EmbThumbnailProvider.cpp - Windows Explorer thumbnail handler for .EMB files.
//
// Explorer (through its thumbnail cache) hands us the file as an IStream; we
// find the preview picture Wilcom stored inside it and return an HBITMAP.
// Registration is per-user (HKCU), so no administrator rights are needed.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <new>
#include <objbase.h>
#include <propsys.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <wincodec.h>

#include "emb_parse.h"

// {0678313A-B65C-412B-B332-4A248F11483E}
static const CLSID CLSID_EmbThumb = {0x0678313a, 0xb65c, 0x412b, {0xb3, 0x32, 0x4a, 0x24, 0x8f, 0x11, 0x48, 0x3e}};
// Not exported by MinGW's libuuid, so define it here.
static const IID IID_ThumbProvider = {0xe357fccd, 0xa995, 0x4576, {0xb0, 0x1f, 0x23, 0x46, 0x30, 0x15, 0x4e, 0x96}};
#define CLSID_STR L"{0678313A-B65C-412B-B332-4A248F11483E}"
#define THUMB_HANDLER_IID L"{E357FCCD-A995-4576-B01F-234630154E96}"

static HMODULE g_module;
static LONG g_refs;

// ---------------------------------------------------------------------------
// Picture -> HBITMAP
// ---------------------------------------------------------------------------

static HBITMAP makeDib(UINT w, UINT h, void** bits) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}

static void fitSize(UINT w, UINT h, UINT cx, UINT* ow, UINT* oh) {
    // Scale (up or down) so the longer side is cx. Embroidery previews are
    // often tiny, and Explorer would otherwise show a postage stamp.
    if (w >= h) { *ow = cx; *oh = (UINT)((double)h * cx / w + 0.5); }
    else        { *oh = cx; *ow = (UINT)((double)w * cx / h + 0.5); }
    if (*ow < 1) *ow = 1;
    if (*oh < 1) *oh = 1;
}

static HRESULT rasterToBitmap(const emb::Bytes& img, UINT cx, HBITMAP* out) {
    IWICImagingFactory* f = nullptr;
    IStream* s = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    IWICBitmapScaler* sc = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr)) {
        s = SHCreateMemStream(img.data(), (UINT)img.size());
        hr = s ? S_OK : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) hr = f->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = f->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    UINT w = 0, h = 0, ow = 0, oh = 0;
    if (SUCCEEDED(hr)) hr = conv->GetSize(&w, &h);
    if (SUCCEEDED(hr) && (w == 0 || h == 0)) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        fitSize(w, h, cx, &ow, &oh);
        hr = f->CreateBitmapScaler(&sc);
    }
    if (SUCCEEDED(hr)) {
        // Nearest-neighbour when enlarging a tiny icon keeps it crisp; Fant when shrinking.
        WICBitmapInterpolationMode mode = (ow >= 2 * w) ? WICBitmapInterpolationModeNearestNeighbor : WICBitmapInterpolationModeFant;
        hr = sc->Initialize(conv, ow, oh, mode);
    }
    if (SUCCEEDED(hr)) {
        void* bits = nullptr;
        HBITMAP bmp = makeDib(ow, oh, &bits);
        if (!bmp) hr = E_OUTOFMEMORY;
        else {
            hr = sc->CopyPixels(nullptr, ow * 4, ow * 4 * oh, (BYTE*)bits);
            if (SUCCEEDED(hr)) *out = bmp;
            else DeleteObject(bmp);
        }
    }
    if (sc) sc->Release();
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (s) s->Release();
    if (f) f->Release();
    return hr;
}

static HRESULT metafileToBitmap(const emb::Picture& pic, UINT cx, HBITMAP* out) {
    HENHMETAFILE emf = nullptr;
    if (pic.kind == emb::Kind::Emf)
        emf = SetEnhMetaFileBits((UINT)pic.data.size(), pic.data.data());
    else
        emf = SetWinMetaFileBits((UINT)pic.data.size(), pic.data.data(), nullptr, nullptr);
    if (!emf) return E_FAIL;
    ENHMETAHEADER hdr = {};
    GetEnhMetaFileHeader(emf, sizeof(hdr), &hdr);
    LONG bw = hdr.rclBounds.right - hdr.rclBounds.left, bh = hdr.rclBounds.bottom - hdr.rclBounds.top;
    UINT ow = cx, oh = cx;
    if (bw > 0 && bh > 0) fitSize((UINT)bw, (UINT)bh, cx, &ow, &oh);
    void* bits = nullptr;
    HBITMAP bmp = makeDib(ow, oh, &bits);
    HRESULT hr = E_FAIL;
    if (bmp) {
        memset(bits, 0xFF, (size_t)ow * oh * 4);  // white, opaque
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ old = SelectObject(dc, bmp);
        RECT r = {0, 0, (LONG)ow, (LONG)oh};
        PlayEnhMetaFile(dc, emf, &r);
        GdiFlush();
        SelectObject(dc, old);
        DeleteDC(dc);
        // GDI leaves alpha at 0 where it drew; make every pixel opaque.
        BYTE* px = (BYTE*)bits;
        for (size_t i = 0; i < (size_t)ow * oh; i++) px[i * 4 + 3] = 0xFF;
        *out = bmp;
        hr = S_OK;
    }
    DeleteEnhMetaFile(emf);
    return hr;
}

// ---------------------------------------------------------------------------
// The thumbnail provider COM object
// ---------------------------------------------------------------------------

class EmbThumbProvider : public IInitializeWithStream, public IThumbnailProvider {
public:
    EmbThumbProvider() { InterlockedIncrement(&g_refs); }
    ~EmbThumbProvider() { InterlockedDecrement(&g_refs); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IInitializeWithStream)
            *ppv = static_cast<IInitializeWithStream*>(this);
        else if (riid == IID_ThumbProvider)
            *ppv = static_cast<IThumbnailProvider*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }

    IFACEMETHODIMP Initialize(IStream* stream, DWORD) override {
        if (!stream) return E_INVALIDARG;
        if (!data_.empty()) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        const size_t cap = 128u << 20;
        BYTE buf[65536];
        for (;;) {
            ULONG got = 0;
            HRESULT hr = stream->Read(buf, sizeof(buf), &got);
            if (got) data_.insert(data_.end(), buf, buf + got);
            if (FAILED(hr)) return hr;
            if (got == 0 || hr == S_FALSE || data_.size() > cap) break;
        }
        return data_.empty() ? E_FAIL : S_OK;
    }

    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* alpha) override {
        if (!phbmp) return E_POINTER;
        *phbmp = nullptr;
        if (alpha) *alpha = WTSAT_ARGB;
        if (cx == 0 || cx > 2560) cx = 256;
        emb::Picture pic;
        try {
            pic = emb::findPicture(data_);
        } catch (...) {
            return E_FAIL;
        }
        if (pic.kind == emb::Kind::None) return E_FAIL;  // Explorer falls back to the normal icon
        if (emb::isRaster(pic.kind)) return rasterToBitmap(pic.data, cx, phbmp);
        return metafileToBitmap(pic, cx, phbmp);
    }

private:
    LONG ref_ = 1;
    emb::Bytes data_;
};

class ClassFactory : public IClassFactory {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { InterlockedIncrement(&g_refs); return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { InterlockedDecrement(&g_refs); return 1; }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        EmbThumbProvider* p = new (std::nothrow) EmbThumbProvider();
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_refs); else InterlockedDecrement(&g_refs);
        return S_OK;
    }
};

static ClassFactory g_factory;

// ---------------------------------------------------------------------------
// DLL exports
// ---------------------------------------------------------------------------

extern "C" BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid != CLSID_EmbThumb) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

extern "C" HRESULT __stdcall DllCanUnloadNow() { return g_refs == 0 ? S_OK : S_FALSE; }

static LONG setValue(const wchar_t* key, const wchar_t* name, const wchar_t* value) {
    HKEY k;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r;
}

static const wchar_t* kHandlerKeys[] = {
    L"Software\\Classes\\.emb\\ShellEx\\" THUMB_HANDLER_IID,
    L"Software\\Classes\\SystemFileAssociations\\.emb\\ShellEx\\" THUMB_HANDLER_IID,
};

extern "C" HRESULT __stdcall DllRegisterServer() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    const wchar_t* clsidKey = L"Software\\Classes\\CLSID\\" CLSID_STR;
    const wchar_t* inproc = L"Software\\Classes\\CLSID\\" CLSID_STR L"\\InprocServer32";
    LONG r = setValue(clsidKey, nullptr, L"EMB Embroidery Thumbnail Provider");
    if (!r) r = setValue(inproc, nullptr, path);
    if (!r) r = setValue(inproc, L"ThreadingModel", L"Apartment");
    for (const wchar_t* k : kHandlerKeys)
        if (!r) r = setValue(k, nullptr, CLSID_STR);
    // Mark as approved (only enforced on locked-down machines, harmless otherwise).
    if (!r) setValue(L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved", CLSID_STR, L"EMB Embroidery Thumbnail Provider");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return HRESULT_FROM_WIN32(r);
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
    // Only remove handler keys that still point at us.
    for (const wchar_t* k : kHandlerKeys) {
        wchar_t val[64] = {};
        DWORD cb = sizeof(val);
        if (RegGetValueW(HKEY_CURRENT_USER, k, nullptr, RRF_RT_REG_SZ, nullptr, val, &cb) == ERROR_SUCCESS &&
            _wcsicmp(val, CLSID_STR) == 0)
            RegDeleteTreeW(HKEY_CURRENT_USER, k);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\" CLSID_STR);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, CLSID_STR);
        RegCloseKey(k);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
