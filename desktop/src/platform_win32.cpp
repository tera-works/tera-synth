#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include "platform.h"

namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring filterString(const std::vector<std::pair<std::string, std::string>>& filter) {
    std::wstring f;
    for (auto& [desc, pat] : filter) {
        f += widen(desc);
        f.push_back(L'\0');
        f += widen(pat);
        f.push_back(L'\0');
    }
    f.push_back(L'\0');
    return f;
}
}  // namespace

namespace platform {

std::vector<std::string> openFiles(const std::vector<std::pair<std::string, std::string>>& filter, bool multi) {
    std::vector<wchar_t> buf(64 * 1024, L'\0');
    const std::wstring f = filterString(filter);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = f.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR | (multi ? OFN_ALLOWMULTISELECT : 0);
    std::vector<std::string> out;
    if (!GetOpenFileNameW(&ofn)) return out;
    // multi-select: "dir\0file1\0file2\0\0", single: "full path\0"
    std::wstring first(buf.data());
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!multi || *p == L'\0') {
        out.push_back(narrow(first));
        return out;
    }
    while (*p) {
        std::wstring name(p);
        out.push_back(narrow(first + L"\\" + name));
        p += name.size() + 1;
    }
    return out;
}

std::string pickFolder(const std::string& title) {
    std::string result;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return result;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog, (void**)&dlg))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(widen(title).c_str());
        if (SUCCEEDED(dlg->Show(GetActiveWindow()))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = narrow(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    CoUninitialize();
    return result;
}

std::string saveFile(const std::vector<std::pair<std::string, std::string>>& filter, const std::string& defaultName,
                     const std::string& defaultExt) {
    std::vector<wchar_t> buf(4096, L'\0');
    const std::wstring def = widen(defaultName);
    std::copy(def.begin(), def.end(), buf.begin());
    const std::wstring f = filterString(filter);
    const std::wstring e = widen(defaultExt);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = f.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrDefExt = e.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return {};
    return narrow(buf.data());
}

void openInExplorer(const std::string& path) {
    ShellExecuteW(nullptr, L"open", widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string executableDir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    return narrow(slash == std::wstring::npos ? p : p.substr(0, slash));
}

}  // namespace platform
