// setup.cpp - "EMB Thumbnails Setup.exe": double-click to install or remove
// the EMB thumbnail handler for Windows Explorer. No admin rights needed:
// everything goes into the current user's profile and registry.
//
//   EMB Thumbnails Setup.exe              ask, then install / repair / remove
//   EMB Thumbnails Setup.exe /install     install without asking (add /quiet for no messages)
//   EMB Thumbnails Setup.exe /uninstall   remove (add /quiet for no messages)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <string>

#define APP_NAME L"EMB Thumbnails"
#define APP_VERSION L"1.1.0"
#define CLSID_STR L"{0678313A-B65C-412B-B332-4A248F11483E}"
#define UNINSTALL_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\EmbThumbnails"
#define IDR_DLL 101

static bool g_quiet = false;

static void say(const std::wstring& msg, UINT icon = MB_ICONINFORMATION) {
    if (!g_quiet) MessageBoxW(nullptr, msg.c_str(), APP_NAME, MB_OK | icon);
}

static std::wstring installDir() {
    wchar_t* p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    return dir + L"\\EMB Thumbnails";
}

static std::wstring selfPath() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

static bool isInstalled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\" CLSID_STR L"\\InprocServer32", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    RegCloseKey(k);
    return true;
}

static void setStr(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

static void deleteLeftovers(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.old").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    while (FindNextFileW(h, &fd));
    FindClose(h);
}

static bool writeDll(const std::wstring& path, std::wstring& err) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_DLL), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const void* data = g ? LockResource(g) : nullptr;
    DWORD size = r ? SizeofResource(nullptr, r) : 0;
    if (!data || !size) { err = L"The installer is damaged (missing component)."; return false; }

    // Explorer may still have the old version loaded; a loaded DLL can't be
    // overwritten but can be renamed out of the way.
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(path.c_str())) {
        std::wstring old = path + L"." + std::to_wstring(GetTickCount64()) + L".old";
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = L"Could not write " + path; return false; }
    DWORD wrote = 0;
    BOOL ok = WriteFile(f, data, size, &wrote, nullptr) && wrote == size;
    CloseHandle(f);
    if (!ok) err = L"Could not write " + path;
    return ok;
}

typedef HRESULT(__stdcall* RegFn)();

static HRESULT callDll(const std::wstring& dll, const char* fn) {
    HMODULE m = LoadLibraryW(dll.c_str());
    if (!m) return HRESULT_FROM_WIN32(GetLastError());
    RegFn f = (RegFn)(void*)GetProcAddress(m, fn);
    HRESULT hr = f ? f() : E_FAIL;
    FreeLibrary(m);
    return hr;
}

// Windows remembers thumbnails (and failures) in thumbcache_*.db, which
// Explorer keeps open. Close Explorer, delete the cache, start it again.
static void refreshThumbnailCache() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            DWORD sess = 0;
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") || !ProcessIdToSessionId(pe.th32ProcessID, &sess) || sess != mySession)
                continue;
            HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
            if (h) {
                TerminateProcess(h, 1);
                WaitForSingleObject(h, 5000);
                CloseHandle(h);
            }
        }
        CloseHandle(snap);
    }
    wchar_t* p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
        std::wstring dir = std::wstring(p) + L"\\Microsoft\\Windows\\Explorer";
        for (int attempt = 0; attempt < 5; attempt++) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dir + L"\\thumbcache_*.db").c_str(), &fd);
            bool locked = false;
            if (h != INVALID_HANDLE_VALUE) {
                do if (!DeleteFileW((dir + L"\\" + fd.cFileName).c_str())) locked = true;
                while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            if (!locked) break;
            Sleep(500);
        }
    }
    CoTaskMemFree(p);
    Sleep(300);
    ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
}

static int install() {
    std::wstring dir = installDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    deleteLeftovers(dir);
    std::wstring dll = dir + L"\\EmbThumbnails.dll";
    std::wstring err;
    if (!writeDll(dll, err)) { say(err, MB_ICONERROR); return 1; }

    // Keep a copy of this setup program so it can be uninstalled from Settings > Apps.
    std::wstring self = selfPath(), setup = dir + L"\\EMB Thumbnails Setup.exe";
    if (_wcsicmp(self.c_str(), setup.c_str()) != 0) CopyFileW(self.c_str(), setup.c_str(), FALSE);

    HRESULT hr = callDll(dll, "DllRegisterServer");
    if (FAILED(hr)) {
        wchar_t msg[200];
        wsprintfW(msg, L"Could not register the thumbnail handler (error 0x%08X).", (unsigned)hr);
        say(msg, MB_ICONERROR);
        return 1;
    }

    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UNINSTALL_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        setStr(k, L"DisplayName", APP_NAME L" (Explorer previews for .EMB embroidery files)");
        setStr(k, L"DisplayVersion", APP_VERSION);
        setStr(k, L"DisplayIcon", setup);
        setStr(k, L"InstallLocation", dir);
        setStr(k, L"UninstallString", L"\"" + setup + L"\" /uninstall");
        setStr(k, L"QuietUninstallString", L"\"" + setup + L"\" /uninstall /quiet");
        DWORD one = 1;
        RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, 4);
        RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, 4);
        RegCloseKey(k);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    if (!g_quiet && MessageBoxW(nullptr,
            L"Installed!\n\nRefresh File Explorer now so the previews appear?\n\n"
            L"This clears Windows' saved thumbnails and restarts File Explorer.\n"
            L"Your taskbar will disappear for a few seconds. That's normal.\n"
            L"(Recommended, especially if you installed an earlier version.)",
            APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES)
        refreshThumbnailCache();

    say(L"Done! EMB thumbnails are now switched on.\n\n"
        L"Open a folder with .emb files in File Explorer and pick\n"
        L"View > Large icons (or Extra large icons).\n\n"
        L"If you still see plain icons, run this setup again and choose Yes (repair).\n"
        L"To remove it later: Settings > Apps > EMB Thumbnails > Uninstall.");
    return 0;
}

static void removeKeysDirectly() {
    const wchar_t* keys[] = {
        L"Software\\Classes\\.emb\\ShellEx\\{E357FCCD-A995-4576-B01F-234630154E96}",
        L"Software\\Classes\\SystemFileAssociations\\.emb\\ShellEx\\{E357FCCD-A995-4576-B01F-234630154E96}",
    };
    for (const wchar_t* key : keys) {
        wchar_t val[64] = {};
        DWORD cb = sizeof(val);
        if (RegGetValueW(HKEY_CURRENT_USER, key, nullptr, RRF_RT_REG_SZ, nullptr, val, &cb) == ERROR_SUCCESS &&
            _wcsicmp(val, CLSID_STR) == 0)
            RegDeleteTreeW(HKEY_CURRENT_USER, key);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\" CLSID_STR);
}

static int uninstall() {
    std::wstring dir = installDir(), dll = dir + L"\\EmbThumbnails.dll";
    if (FAILED(callDll(dll, "DllUnregisterServer"))) removeKeysDirectly();
    RegDeleteTreeW(HKEY_CURRENT_USER, UNINSTALL_KEY);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    // Files may be in use (Explorer, or this very program); remove the folder
    // a few seconds after we exit.
    deleteLeftovers(dir);
    DeleteFileW(dll.c_str());
    std::wstring cmd = L"cmd.exe /c ping 127.0.0.1 -n 6 >nul & rd /s /q \"" + dir + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    say(L"EMB thumbnails have been removed.");
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int action = 0;  // 0 ask, 1 install, 2 uninstall
    for (int i = 1; i < argc; i++) {
        if (!_wcsicmp(argv[i], L"/quiet") || !_wcsicmp(argv[i], L"/q")) g_quiet = true;
        else if (!_wcsicmp(argv[i], L"/install")) action = 1;
        else if (!_wcsicmp(argv[i], L"/uninstall")) action = 2;
    }
    LocalFree(argv);

    if (action == 0) {
        if (isInstalled()) {
            int r = MessageBoxW(nullptr,
                L"EMB Thumbnails is already installed.\n\n"
                L"Yes = reinstall / repair\nNo = uninstall (remove it)\nCancel = do nothing",
                APP_NAME, MB_YESNOCANCEL | MB_ICONQUESTION);
            action = r == IDYES ? 1 : r == IDNO ? 2 : 0;
        } else {
            int r = MessageBoxW(nullptr,
                L"Show thumbnails of .EMB embroidery files in Windows File Explorer?\n\n"
                L"This installs a small preview add-on for your user account only.\n"
                L"No administrator rights needed. Your EMB files are never changed.",
                APP_NAME L" Setup", MB_OKCANCEL | MB_ICONQUESTION);
            action = r == IDOK ? 1 : 0;
        }
    }
    int rc = action == 1 ? install() : action == 2 ? uninstall() : 0;
    CoUninitialize();
    return rc;
}
