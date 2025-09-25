// PlantUML WebView Lister (Tiny)
// Features:
//  - Local rendering via Java + plantuml.jar (bundled or configurable)
//  - WebView2 runtime loaded dynamically (no static import)
//
// Build: CMake -> PlantUmlWebView.wlx64

#include <windows.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <wrl.h>
#include <wrl/event.h>
#include <string>
#include <sstream>
#include <vector>
#include <memory>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cwchar>

#include <wincodec.h>

#include "WebView2.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "windowscodecs.lib")

using namespace Microsoft::WRL;

// ---------------------- Config ----------------------
static std::wstring g_prefer          = L"svg";           // "svg" or "png"
static std::wstring g_rendererSetting = L"java";          // "java" or "web"
static std::string  g_detectA         = R"(EXT="PUML" | EXT="PLANTUML" | EXT="UML" | EXT="WSD" | EXT="WS" | EXT="IUML")";

static std::wstring g_jarPath;                      // If empty: auto-detect moduleDir\plantuml.jar
static std::wstring g_javaPath;                     // Optional explicit java[w].exe
static std::wstring g_logPath;                      // If empty: moduleDir\plantumlwebview.log
static DWORD        g_jarTimeoutMs = 8000;
static bool         g_logEnabled = true;

static bool         g_cfgLoaded = false;

static std::mutex   g_logMutex;
static bool         g_logSessionStarted = false;

enum class RenderBackend {
    Java,
    Web,
};

static const wchar_t* RenderBackendName(RenderBackend backend) {
    switch (backend) {
    case RenderBackend::Java: return L"java";
    case RenderBackend::Web:  return L"web";
    }
    return L"unknown";
}

static RenderBackend ParseRendererSettingValue(const std::wstring& rendererText,
                                              RenderBackend fallback);
static RenderBackend GetConfiguredRenderer();
static std::wstring GetConfiguredRendererName();

static std::string ToUtf8(const std::wstring& w);

static std::wstring FormatTimestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf(buf, 64, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

static void AppendLog(const std::wstring& message) {
    if (!g_logEnabled || g_logPath.empty()) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    if (!g_logSessionStarted) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(h, &size) && size.QuadPart > 0) {
            std::string sep = "\r\n";
            DWORD written = 0;
            WriteFile(h, sep.c_str(), (DWORD)sep.size(), &written, nullptr);
        }
        std::wstring header = FormatTimestamp() + L"--- PlantUML WebView session start ---\r\n";
        std::string headerUtf8 = ToUtf8(header);
        if (!headerUtf8.empty()) {
            DWORD written = 0;
            WriteFile(h, headerUtf8.data(), (DWORD)headerUtf8.size(), &written, nullptr);
        }
        g_logSessionStarted = true;
    }
    std::wstring line = FormatTimestamp() + message + L"\r\n";
    std::string utf8 = ToUtf8(line);
    if (!utf8.empty()) {
        DWORD written = 0;
        WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    }
    CloseHandle(h);
}

static bool ClipboardSetUnicodeText(const std::wstring& text) {
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) {
        return false;
    }
    void* ptr = GlobalLock(mem);
    if (!ptr) {
        GlobalFree(mem);
        return false;
    }
    memcpy(ptr, text.c_str(), bytes);
    GlobalUnlock(mem);
    if (!SetClipboardData(CF_UNICODETEXT, mem)) {
        GlobalFree(mem);
        return false;
    }
    return true;
}

static bool ClipboardSetBinaryData(UINT format, const void* data, size_t size) {
    if (!data || !size) {
        return false;
    }
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem) {
        return false;
    }
    void* ptr = GlobalLock(mem);
    if (!ptr) {
        GlobalFree(mem);
        return false;
    }
    memcpy(ptr, data, size);
    GlobalUnlock(mem);
    if (!SetClipboardData(format, mem)) {
        GlobalFree(mem);
        return false;
    }
    return true;
}

static bool CreateDibFromPng(const std::vector<unsigned char>& png,
                             std::vector<unsigned char>& outDib) {
    outDib.clear();
    if (png.empty()) {
        return false;
    }

    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninit = false;
    if (hrInit == RPC_E_CHANGED_MODE) {
        hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hrInit == RPC_E_CHANGED_MODE) {
            hrInit = S_OK;
        } else if (SUCCEEDED(hrInit)) {
            needUninit = (hrInit == S_OK || hrInit == S_FALSE);
        }
    } else if (SUCCEEDED(hrInit)) {
        needUninit = (hrInit == S_OK || hrInit == S_FALSE);
    }

    if (FAILED(hrInit)) {
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        if (needUninit) CoUninitialize();
        return false;
    }

    IStream* rawStream = SHCreateMemStream(png.data(), static_cast<UINT>(png.size()));
    if (!rawStream) {
        if (needUninit) CoUninitialize();
        return false;
    }
    ComPtr<IStream> stream;
    stream.Attach(rawStream);

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                          &decoder);
    if (FAILED(hr) || !decoder) {
        if (needUninit) CoUninitialize();
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        if (needUninit) CoUninitialize();
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter) {
        if (needUninit) CoUninitialize();
        return false;
    }

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                               WICBitmapDitherTypeNone, nullptr, 0.0f,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        if (needUninit) CoUninitialize();
        return false;
    }

    UINT width = 0, height = 0;
    hr = converter->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0) {
        if (needUninit) CoUninitialize();
        return false;
    }

    const UINT stride = width * 4;
    const UINT imageSize = stride * height;
    outDib.resize(sizeof(BITMAPV5HEADER) + imageSize);
    auto* header = reinterpret_cast<BITMAPV5HEADER*>(outDib.data());
    ZeroMemory(header, sizeof(BITMAPV5HEADER));
    header->bV5Size = sizeof(BITMAPV5HEADER);
    header->bV5Width = static_cast<LONG>(width);
    header->bV5Height = -static_cast<LONG>(height);
    header->bV5Planes = 1;
    header->bV5BitCount = 32;
    header->bV5Compression = BI_BITFIELDS;
    header->bV5RedMask   = 0x00FF0000;
    header->bV5GreenMask = 0x0000FF00;
    header->bV5BlueMask  = 0x000000FF;
    header->bV5AlphaMask = 0xFF000000;
    header->bV5SizeImage = imageSize;

    BYTE* pixels = outDib.data() + sizeof(BITMAPV5HEADER);
    hr = converter->CopyPixels(nullptr, stride, imageSize, pixels);

    if (needUninit) {
        CoUninitialize();
    }

    if (FAILED(hr)) {
        outDib.clear();
        return false;
    }

    return true;
}

// ---------------------- Helpers ----------------------
static std::wstring GetModuleDir() {
    wchar_t path[MAX_PATH]{}; HMODULE hm{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&GetModuleDir, &hm);
    GetModuleFileNameW(hm, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

static bool FileExistsW(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void ReplaceAll(std::wstring& inout, const std::wstring& from, const std::wstring& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = inout.find(from, pos)) != std::wstring::npos) {
        inout.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static std::wstring ExtractJsonStringField(const std::wstring& json, const std::wstring& field) {
    if (json.empty() || field.empty()) {
        return std::wstring();
    }
    const std::wstring needle = L"\"" + field + L"\"";
    size_t pos = json.find(needle);
    if (pos == std::wstring::npos) {
        return std::wstring();
    }
    pos = json.find(L':', pos + needle.size());
    if (pos == std::wstring::npos) {
        return std::wstring();
    }
    size_t quote = json.find_first_of(L"\"'", pos + 1);
    if (quote == std::wstring::npos) {
        return std::wstring();
    }
    const wchar_t delimiter = json[quote];
    size_t end = json.find(delimiter, quote + 1);
    if (end == std::wstring::npos) {
        return std::wstring();
    }
    return json.substr(quote + 1, end - quote - 1);
}

static std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring ReadFileUtf16OrAnsi(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        AppendLog(L"ReadFileUtf16OrAnsi: failed to open file " + std::wstring(path ? path : L"<null>") +
                  L" (error=" + std::to_wstring(GetLastError()) + L")");
        return L"";
    }
    DWORD size = GetFileSize(h, nullptr);
    std::string bytes; bytes.resize(size ? size : 0);
    DWORD read = 0;
    if (size && (!ReadFile(h, bytes.data(), size, &read, nullptr) || read != size)) {
        AppendLog(L"ReadFileUtf16OrAnsi: short read for file " + std::wstring(path ? path : L"<null>") +
                  L" (wanted=" + std::to_wstring(size) + L", got=" + std::to_wstring(read) + L")");
    }
    CloseHandle(h);

    if (bytes.size() >= 2 && (unsigned char)bytes[0]==0xFF && (unsigned char)bytes[1]==0xFE) {
        std::wstring w((wchar_t*)(bytes.data()+2), (bytes.size()-2)/2); return w;
    }
    if (bytes.size() >= 3 && (unsigned char)bytes[0]==0xEF && (unsigned char)bytes[1]==0xBB && (unsigned char)bytes[2]==0xBF) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, bytes.data()+3, (int)bytes.size()-3, nullptr, 0);
        std::wstring w(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, bytes.data()+3, (int)bytes.size()-3, w.data(), wlen);
        return w;
    }
    int wlen = MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), w.data(), wlen);
    return w;
}

static bool WriteBufferToFile(const std::wstring& path, const void* data, size_t size) {
    if (size > MAXDWORD) {
        AppendLog(L"WriteBufferToFile: payload too large for Win32 WriteFile: " + std::to_wstring(static_cast<unsigned long long>(size)) + L" bytes");
        return false;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        AppendLog(L"WriteBufferToFile: failed to create file " + path + L" (error=" + std::to_wstring(GetLastError()) + L")");
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr);
    DWORD lastErr = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    if (!ok || written != size) {
        AppendLog(L"WriteBufferToFile: failed to write file " + path + L" (error=" + std::to_wstring(lastErr) + L", written=" + std::to_wstring(written) + L"/" + std::to_wstring(static_cast<unsigned long long>(size)) + L")");
        return false;
    }
    return true;
}

static bool TryAutoDetectPlantUmlJar(std::wstring& outPath) {
    const std::wstring dir = GetModuleDir();
    const std::wstring exact = dir + L"\\plantuml.jar";
    if (FileExistsW(exact)) {
        outPath = exact;
        return true;
    }

    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = dir + L"\\plantuml*.jar";
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                outPath = dir + L"\\" + fd.cFileName;
                FindClose(hFind);
                return true;
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    return false;
}

static void LoadConfigIfNeeded() {
    if (g_cfgLoaded) return;
    g_cfgLoaded = true;

    const std::wstring moduleDir = GetModuleDir();
    const std::wstring ini = moduleDir + L"\\plantumlwebview.ini";
    wchar_t buf[2048];

    if (GetPrivateProfileStringW(L"render", L"prefer", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) g_prefer = buf;

    RenderBackend rendererChoice = GetConfiguredRenderer();
    if (GetPrivateProfileStringW(L"render", L"renderer", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) {
        rendererChoice = ParseRendererSettingValue(buf, rendererChoice);
    } else if (GetPrivateProfileStringW(L"render", L"pipeline", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) {
        rendererChoice = ParseRendererSettingValue(buf, rendererChoice);
    }
    g_rendererSetting = RenderBackendName(rendererChoice);

    if (GetPrivateProfileStringW(L"detect", L"string", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) {
        int need = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(need ? need - 1 : 0, '\0');
        if (need > 1) WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8.data(), need - 1, nullptr, nullptr);
        if (!utf8.empty()) g_detectA = utf8;
    }

    if (GetPrivateProfileStringW(L"plantuml", L"jar", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) {
        g_jarPath = buf;
        if (PathIsRelativeW(g_jarPath.c_str())) {
            g_jarPath = moduleDir + L"\\" + g_jarPath;
        }
    }
    if (GetPrivateProfileStringW(L"plantuml", L"java", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) {
        g_javaPath = buf;
        if (PathIsRelativeW(g_javaPath.c_str())) {
            g_javaPath = moduleDir + L"\\" + g_javaPath;
        }
    }
    DWORD tmo = GetPrivateProfileIntW(L"plantuml", L"timeout_ms", 0, ini.c_str());
    if (tmo > 0) g_jarTimeoutMs = tmo;

    int logEnabled = GetPrivateProfileIntW(L"debug", L"log_enabled", 1, ini.c_str());
    g_logEnabled = (logEnabled != 0);

    if (GetPrivateProfileStringW(L"debug", L"log", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) {
        g_logPath = buf;
        if (PathIsRelativeW(g_logPath.c_str())) {
            g_logPath = moduleDir + L"\\" + g_logPath;
        }
    }

    if (g_logEnabled) {
        if (g_logPath.empty()) {
            g_logPath = moduleDir + L"\\plantumlwebview.log";
        }
    } else {
        g_logPath.clear();
    }

    bool needDetectJar = g_jarPath.empty();
    if (!g_jarPath.empty() && !FileExistsW(g_jarPath)) {
        AppendLog(L"LoadConfig: configured jar not found at " + g_jarPath + L". Attempting auto-detect.");
        needDetectJar = true;
    }
    if (needDetectJar) {
        std::wstring detected;
        if (TryAutoDetectPlantUmlJar(detected)) {
            g_jarPath.swap(detected);
        }
    }

    std::wstringstream cfg;
    cfg << L"Config loaded. prefer=" << g_prefer
        << L", renderer=" << GetConfiguredRendererName()
        << L", jar=" << (g_jarPath.empty() ? L"<auto>" : g_jarPath)
        << L", java=" << (g_javaPath.empty() ? L"<auto>" : g_javaPath)
        << L", timeoutMs=" << g_jarTimeoutMs
        << L", logEnabled=" << (g_logEnabled ? L"1" : L"0")
        << L", log=" << (g_logPath.empty() ? L"<disabled>" : g_logPath);
    AppendLog(cfg.str());
}

static std::wstring ToLowerTrim(const std::wstring& in) {
    std::wstring s = in;
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](wchar_t c){ return !iswspace(c); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [](wchar_t c){ return !iswspace(c); }).base(), s.end());
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c){ return (wchar_t)towlower(c); });
    return s;
}

static RenderBackend ParseRendererSettingValue(const std::wstring& rendererText,
                                              RenderBackend fallback) {
    std::wstring token = ToLowerTrim(rendererText);
    if (token.empty()) {
        return fallback;
    }

    size_t comma = token.find(L',');
    if (comma != std::wstring::npos) {
        token = ToLowerTrim(token.substr(0, comma));
    }

    if (token == L"web") {
        return RenderBackend::Web;
    }
    if (token == L"java") {
        return RenderBackend::Java;
    }
    return fallback;
}

static RenderBackend GetConfiguredRenderer() {
    return ParseRendererSettingValue(g_rendererSetting, RenderBackend::Java);
}

static std::wstring GetConfiguredRendererName() {
    return std::wstring(RenderBackendName(GetConfiguredRenderer()));
}

static std::wstring HtmlEscape(const std::wstring& text) {
    std::wstring out = text;
    ReplaceAll(out, L"&", L"&amp;");
    ReplaceAll(out, L"<", L"&lt;");
    ReplaceAll(out, L">", L"&gt;");
    ReplaceAll(out, L"\"", L"&quot;");
    ReplaceAll(out, L"'", L"&#39;");
    return out;
}

static std::wstring HtmlAttributeEscape(const std::wstring& text) {
    return HtmlEscape(text);
}

static std::wstring ExtractFileStem(const std::wstring& path) {
    if (path.empty()) {
        return std::wstring();
    }
    const wchar_t* fileName = PathFindFileNameW(path.c_str());
    if (!fileName || !*fileName) {
        return std::wstring();
    }
    std::wstring stem(fileName);
    size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        stem.erase(dot);
    }
    return stem;
}
static bool FindJavaExecutable(std::wstring& outPath) {
    if (!g_javaPath.empty() && FileExistsW(g_javaPath)) { outPath = g_javaPath; return true; }
    wchar_t found[MAX_PATH]{};
    if (SearchPathW(nullptr, L"java.exe", nullptr, MAX_PATH, found, nullptr))  { outPath = found; return true; }
    if (SearchPathW(nullptr, L"javaw.exe", nullptr, MAX_PATH, found, nullptr)) { outPath = found; return true; }
    return false;
}

// Simple base64 encoder (for PNG data URLs)
static std::string Base64(const std::vector<unsigned char>& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0, n = in.size();
    out.reserve(((n + 2) / 3) * 4);
    while (i + 2 < n) {
        unsigned v = (in[i] << 16) | (in[i+1] << 8) | in[i+2];
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back(tbl[v & 63]);
        i += 3;
    }
    if (i + 1 == n) {
        unsigned v = (in[i] << 16);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == n) {
        unsigned v = (in[i] << 16) | (in[i+1] << 8);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

static int Base64DecodeChar(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return int(c - L'A');
    if (c >= L'a' && c <= L'z') return int(c - L'a') + 26;
    if (c >= L'0' && c <= L'9') return int(c - L'0') + 52;
    if (c == L'+') return 62;
    if (c == L'/') return 63;
    if (c == L'=') return -1;
    return -2;
}

static std::vector<unsigned char> Base64Decode(const std::wstring& in) {
    std::vector<unsigned char> out;
    if (in.empty()) {
        return out;
    }

    unsigned int buffer = 0;
    int bitsCollected = 0;
    bool hitPadding = false;
    for (wchar_t wc : in) {
        int decoded = Base64DecodeChar(wc);
        if (decoded < 0) {
            if (decoded == -1) {
                if (bitsCollected == 18) {
                    out.push_back(static_cast<unsigned char>((buffer >> 10) & 0xFF));
                    out.push_back(static_cast<unsigned char>((buffer >> 2) & 0xFF));
                } else if (bitsCollected == 12) {
                    out.push_back(static_cast<unsigned char>((buffer >> 4) & 0xFF));
                }
                hitPadding = true;
                break;
            }
            continue;
        }
        buffer = (buffer << 6) | static_cast<unsigned int>(decoded);
        bitsCollected += 6;
        if (bitsCollected >= 24) {
            out.push_back(static_cast<unsigned char>((buffer >> 16) & 0xFF));
            out.push_back(static_cast<unsigned char>((buffer >> 8) & 0xFF));
            out.push_back(static_cast<unsigned char>(buffer & 0xFF));
            buffer = 0;
            bitsCollected = 0;
        }
    }

    if (!hitPadding) {
        if (bitsCollected == 18) {
            out.push_back(static_cast<unsigned char>((buffer >> 10) & 0xFF));
            out.push_back(static_cast<unsigned char>((buffer >> 2) & 0xFF));
        } else if (bitsCollected == 12) {
            out.push_back(static_cast<unsigned char>((buffer >> 4) & 0xFF));
        }
    }

    return out;
}

// Run "java -jar plantuml.jar -pipe -t(svg|png)" and capture stdout.
static bool RunPlantUmlJar(const std::wstring& umlTextW, bool preferSvg,
                           std::wstring& outSvg, std::vector<unsigned char>& outPng)
{
    AppendLog(L"RunPlantUmlJar: start");

    if (g_jarPath.empty()) {
        AppendLog(L"RunPlantUmlJar: jar path is empty");
        return false;
    }
    if (!FileExistsW(g_jarPath)) {
        AppendLog(L"RunPlantUmlJar: jar not found at " + g_jarPath);
        return false;
    }

    std::wstring javaExe;
    if (!FindJavaExecutable(javaExe)) {
        AppendLog(L"RunPlantUmlJar: Java executable not found");
        return false;
    }

    AppendLog(L"RunPlantUmlJar: using java executable " + javaExe);


    std::wstring fmt = preferSvg ? L"-tsvg" : L"-tpng";

    SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE hInR=nullptr,  hInW=nullptr;
    HANDLE hOutR=nullptr, hOutW=nullptr;

    if (!CreatePipe(&hInR, &hInW, &sa, 0)) {
        AppendLog(L"RunPlantUmlJar: failed to create stdin pipe");
        return false;
    }
    if (!CreatePipe(&hOutR, &hOutW, &sa, 0)) {
        AppendLog(L"RunPlantUmlJar: failed to create stdout pipe");
        CloseHandle(hInR); CloseHandle(hInW);
        return false;
    }

    // Make only the child side inheritable
    SetHandleInformation(hInR,  HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(hOutW, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(hInW,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hOutR, HANDLE_FLAG_INHERIT, 0);

    std::wstringstream cmd;
    cmd << L"\"" << javaExe << L"\" -Djava.awt.headless=true -jar \"" << g_jarPath
        << L"\" -pipe " << fmt;

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput  = hInR;
    si.hStdOutput = hOutW;
    si.hStdError  = hOutW;

    PROCESS_INFORMATION pi{};
    std::wstring cmdline = cmd.str();
    BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hOutW);
    CloseHandle(hInR);

    if (!ok) {
        AppendLog(L"RunPlantUmlJar: CreateProcessW failed with error " + std::to_wstring(GetLastError()));
        CloseHandle(hInW); CloseHandle(hOutR);
        return false;
    }

    // Write UML as UTF-8 to child stdin
    std::string umlUtf8 = ToUtf8(umlTextW);
    DWORD written = 0;
    if (!umlUtf8.empty()) {
        if (!WriteFile(hInW, umlUtf8.data(), (DWORD)umlUtf8.size(), &written, nullptr)) {
            AppendLog(L"RunPlantUmlJar: failed to write UML to stdin (error=" + std::to_wstring(GetLastError()) + L")");
        }
    }
    CloseHandle(hInW); // signal EOF

    // Read child's stdout (up to 50MB)
    std::vector<unsigned char> buffer;
    buffer.reserve(64 * 1024);
    unsigned char tmp[16 * 1024];
    DWORD got = 0;
    for (;;) {
        if (!ReadFile(hOutR, tmp, sizeof(tmp), &got, nullptr) || got == 0) break;
        buffer.insert(buffer.end(), tmp, tmp + got);
        if (buffer.size() > (50 * 1024 * 1024)) break;
        DWORD wr = WaitForSingleObject(pi.hProcess, 0);
        if (wr == WAIT_OBJECT_0) {
            while (ReadFile(hOutR, tmp, sizeof(tmp), &got, nullptr) && got) {
                buffer.insert(buffer.end(), tmp, tmp + got);
            }
            break;
        }
    }
    CloseHandle(hOutR);

    DWORD wr = WaitForSingleObject(pi.hProcess, g_jarTimeoutMs);
    if (wr == WAIT_FAILED) {
        AppendLog(L"RunPlantUmlJar: WaitForSingleObject failed with error " + std::to_wstring(GetLastError()));
    } else if (wr == WAIT_TIMEOUT) {
        AppendLog(L"RunPlantUmlJar: timeout after " + std::to_wstring(g_jarTimeoutMs) + L" ms");
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hThread);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);

    if (buffer.empty()) {
        AppendLog(L"RunPlantUmlJar: process produced no output. exitCode=" + std::to_wstring(exitCode));
        return false;
    }

    if (preferSvg) {
        // interpret bytes as UTF-8 SVG
        int wlen = MultiByteToWideChar(CP_UTF8, 0,
                                       (const char*)buffer.data(), (int)buffer.size(),
                                       nullptr, 0);
        if (wlen <= 0) {
            AppendLog(L"RunPlantUmlJar: failed to decode SVG output");
            return false;
        }
        std::wstring svg(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, (const char*)buffer.data(), (int)buffer.size(),
                            svg.data(), wlen);
        outSvg.swap(svg);
    } else {
        outPng.swap(buffer);
    }
    AppendLog(L"RunPlantUmlJar: success. exitCode=" + std::to_wstring(exitCode) +
              L", outputLength=" + std::to_wstring((unsigned long long)(preferSvg ? outSvg.size() : outPng.size())));
    return true;
}


// PlantUML encoder script split into chunks to satisfy MSVC literal limits
static const char kPlantumlEncoderScriptPart1[] = R"ENC1(!function(t){if("object"==typeof exports&&"undefined"!=typeof module)module.exports=t();else if("function"==typeof define&&define.amd)define([],t);else{("undefined"!=typeof window?window:"undefined"!=typeof global?global:"undefined"!=typeof self?self:this).plantumlEncoder=t()}}(function(){return function i(s,h,o){function l(e,t){if(!h[e]){if(!s[e]){var a="function"==typeof require&&require;if(!t&&a)return a(e,!0);if(_)return _(e,!0);var n=new Error("Cannot find module '"+e+"'");throw n.code="MODULE_NOT_FOUND",n}var r=h[e]={exports:{}};s[e][0].call(r.exports,function(t){return l(s[e][1][t]||t)},r,r.exports,i,s,h,o)}return h[e].exports}for(var _="function"==typeof require&&require,t=0;t<o.length;t++)l(o[t]);return l}({1:[function(t,e,a){"use strict";var n=t("pako/lib/deflate.js");e.exports=function(t){return n.deflateRaw(t,{level:9,to:"string"})}},{"pako/lib/deflate.js":4}],2:[function(t,e,a){"use strict";function h(t){return t<10?String.fromCharCode(48+t):(t-=10)<26?String.fromCharCode(65+t):(t-=26)<26?String.fromCharCode(97+t):0===(t-=26)?"-":1===t?"_":"?"}function n(t,e,a){var n=(3&t)<<4|e>>4,r=(15&e)<<2|a>>6,i=63&a,s="";return s+=h(63&t>>2),s+=h(63&n),s+=h(63&r),s+=h(63&i)}e.exports=function(t){for(var e="",a=0;a<t.length;a+=3)a+2===t.length?e+=n(t.charCodeAt(a),t.charCodeAt(a+1),0):a+1===t.length?e+=n(t.charCodeAt(a),0,0):e+=n(t.charCodeAt(a),t.charCodeAt(a+1),t.charCodeAt(a+2));return e}},{}],3:[function(t,e,a){"use strict";var n=t("./deflate"),r=t("./encode64");e.exports.encode=function(t){var e=n(t);return r(e)}},{"./deflate":1,"./encode64":2}],4:[function(t,e,a){"use strict";var s=t("./zlib/deflate"),h=t("./utils/common"),o=t("./utils/strings"),r=t("./zlib/messages"),i=t("./zlib/zstream"),l=Object.prototype.toString,_=0,d=-1,u=0,f=8;function c(t){if(!(this instanceof c))return new c(t);this.options=h.assign({level:d,method:f,chunkSize:16384,windowBits:15,memLevel:8,strategy:u,to:""},t||{});var e=this.options;e.raw&&0<e.windowBits?e.windowBits=-e.windowBits:e.)ENC1";
static const char kPlantumlEncoderScriptPart2[] = R"ENC2(gzip&&0<e.windowBits&&e.windowBits<16&&(e.windowBits+=16),this.err=0,this.msg="",this.ended=!1,this.chunks=[],this.strm=new i,this.strm.avail_out=0;var a=s.deflateInit2(this.strm,e.level,e.method,e.windowBits,e.memLevel,e.strategy);if(a!==_)throw new Error(r[a]);if(e.header&&s.deflateSetHeader(this.strm,e.header),e.dictionary){var n;if(n="string"==typeof e.dictionary?o.string2buf(e.dictionary):"[object ArrayBuffer]"===l.call(e.dictionary)?new Uint8Array(e.dictionary):e.dictionary,(a=s.deflateSetDictionary(this.strm,n))!==_)throw new Error(r[a]);this._dict_set=!0}}function n(t,e){var a=new c(e);if(a.push(t,!0),a.err)throw a.msg||r[a.err];return a.result}c.prototype.push=function(t,e){var a,n,r=this.strm,i=this.options.chunkSize;if(this.ended)return!1;n=e===~~e?e:!0===e?4:0,"string"==typeof t?r.input=o.string2buf(t):"[object ArrayBuffer]"===l.call(t)?r.input=new Uint8Array(t):r.input=t,r.next_in=0,r.avail_in=r.input.length;do{if(0===r.avail_out&&(r.output=new h.Buf8(i),r.next_out=0,r.avail_out=i),1!==(a=s.deflate(r,n))&&a!==_)return this.onEnd(a),!(this.ended=!0);0!==r.avail_out&&(0!==r.avail_in||4!==n&&2!==n)||("string"===this.options.to?this.onData(o.buf2binstring(h.shrinkBuf(r.output,r.next_out))):this.onData(h.shrinkBuf(r.output,r.next_out)))}while((0<r.avail_in||0===r.avail_out)&&1!==a);return 4===n?(a=s.deflateEnd(this.strm),this.onEnd(a),this.ended=!0,a===_):2!==n||(this.onEnd(_),!(r.avail_out=0))},c.prototype.onData=function(t){this.chunks.push(t)},c.prototype.onEnd=function(t){t===_&&("string"===this.options.to?this.result=this.chunks.join(""):this.result=h.flattenChunks(this.chunks)),this.chunks=[],this.err=t,this.msg=this.strm.msg},a.Deflate=c,a.deflate=n,a.deflateRaw=function(t,e){return(e=e||{}).raw=!0,n(t,e)},a.gzip=function(t,e){return(e=e||{}).gzip=!0,n(t,e)}},{"./utils/common":5,"./utils/strings":6,"./zlib/deflate":9,"./zlib/messages":10,"./zlib/zstream":12}],5:[function(t,e,a){"use strict";var n="undefined"!=typeof Uint8Array&&"undefined"!=typeof Uin)ENC2";
static const char kPlantumlEncoderScriptPart3[] = R"ENC3(t16Array&&"undefined"!=typeof Int32Array;a.assign=function(t){for(var e,a,n=Array.prototype.slice.call(arguments,1);n.length;){var r=n.shift();if(r){if("object"!=typeof r)throw new TypeError(r+"must be non-object");for(var i in r)e=r,a=i,Object.prototype.hasOwnProperty.call(e,a)&&(t[i]=r[i])}}return t},a.shrinkBuf=function(t,e){return t.length===e?t:t.subarray?t.subarray(0,e):(t.length=e,t)};var r={arraySet:function(t,e,a,n,r){if(e.subarray&&t.subarray)t.set(e.subarray(a,a+n),r);else for(var i=0;i<n;i++)t[r+i]=e[a+i]},flattenChunks:function(t){var e,a,n,r,i,s;for(e=n=0,a=t.length;e<a;e++)n+=t[e].length;for(s=new Uint8Array(n),e=r=0,a=t.length;e<a;e++)i=t[e],s.set(i,r),r+=i.length;return s}},i={arraySet:function(t,e,a,n,r){for(var i=0;i<n;i++)t[r+i]=e[a+i]},flattenChunks:function(t){return[].concat.apply([],t)}};a.setTyped=function(t){t?(a.Buf8=Uint8Array,a.Buf16=Uint16Array,a.Buf32=Int32Array,a.assign(a,r)):(a.Buf8=Array,a.Buf16=Array,a.Buf32=Array,a.assign(a,i))},a.setTyped(n)},{}],6:[function(t,e,a){"use strict";var o=t("./common"),r=!0,i=!0;try{String.fromCharCode.apply(null,[0])}catch(t){r=!1}try{String.fromCharCode.apply(null,new Uint8Array(1))}catch(t){i=!1}for(var l=new o.Buf8(256),n=0;n<256;n++)l[n]=252<=n?6:248<=n?5:240<=n?4:224<=n?3:192<=n?2:1;function _(t,e){if(e<65534&&(t.subarray&&i||!t.subarray&&r))return String.fromCharCode.apply(null,o.shrinkBuf(t,e));for(var a="",n=0;n<e;n++)a+=String.fromCharCode(t[n]);return a}l[254]=l[254]=1,a.string2buf=function(t){var e,a,n,r,i,s=t.length,h=0;for(r=0;r<s;r++)55296==(64512&(a=t.charCodeAt(r)))&&r+1<s&&56320==(64512&(n=t.charCodeAt(r+1)))&&(a=65536+(a-55296<<10)+(n-56320),r++),h+=a<128?1:a<2048?2:a<65536?3:4;for(e=new o.Buf8(h),r=i=0;i<h;r++)55296==(64512&(a=t.charCodeAt(r)))&&r+1<s&&56320==(64512&(n=t.charCodeAt(r+1)))&&(a=65536+(a-55296<<10)+(n-56320),r++),a<128?e[i++]=a:(a<2048?e[i++]=192|a>>>6:(a<65536?e[i++]=224|a>>>12:(e[i++]=240|a>>>18,e[i++]=128|a>>>12&63),e[i++]=128|a>>>6&63),e[i++]=128|63&a);return e},a)ENC3";
static const char kPlantumlEncoderScriptPart4[] = R"ENC4(.buf2binstring=function(t){return _(t,t.length)},a.binstring2buf=function(t){for(var e=new o.Buf8(t.length),a=0,n=e.length;a<n;a++)e[a]=t.charCodeAt(a);return e},a.buf2string=function(t,e){var a,n,r,i,s=e||t.length,h=new Array(2*s);for(a=n=0;a<s;)if((r=t[a++])<128)h[n++]=r;else if(4<(i=l[r]))h[n++]=65533,a+=i-1;else{for(r&=2===i?31:3===i?15:7;1<i&&a<s;)r=r<<6|63&t[a++],i--;1<i?h[n++]=65533:r<65536?h[n++]=r:(r-=65536,h[n++]=55296|r>>10&1023,h[n++]=56320|1023&r)}return _(h,n)},a.utf8border=function(t,e){var a;for((e=e||t.length)>t.length&&(e=t.length),a=e-1;0<=a&&128==(192&t[a]);)a--;return a<0?e:0===a?e:a+l[t[a]]>e?a:e}},{"./common":5}],7:[function(t,e,a){"use strict";e.exports=function(t,e,a,n){for(var r=65535&t|0,i=t>>>16&65535|0,s=0;0!==a;){for(a-=s=2e3<a?2e3:a;i=i+(r=r+e[n++]|0)|0,--s;);r%=65521,i%=65521}return r|i<<16|0}},{}],8:[function(t,e,a){"use strict";var h=function(){for(var t,e=[],a=0;a<256;a++){t=a;for(var n=0;n<8;n++)t=1&t?3988292384^t>>>1:t>>>1;e[a]=t}return e}();e.exports=function(t,e,a,n){var r=h,i=n+a;t^=-1;for(var s=n;s<i;s++)t=t>>>8^r[255&(t^e[s])];return-1^t}},{}],9:[function(t,e,a){"use strict";var o,u=t("../utils/common"),l=t("./trees"),f=t("./adler32"),c=t("./crc32"),n=t("./messages"),_=0,d=4,p=0,g=-2,m=-1,b=4,r=2,v=8,w=9,i=286,s=30,h=19,y=2*i+1,k=15,z=3,x=258,A=x+z+1,B=42,C=113,S=1,E=2,j=3,U=4;function D(t,e){return t.msg=n[e],e}function I(t){return(t<<1)-(4<t?9:0)}function O(t){for(var e=t.length;0<=--e;)t[e]=0}function q(t){var e=t.state,a=e.pending;a>t.avail_out&&(a=t.avail_out),0!==a&&(u.arraySet(t.output,e.pending_buf,e.pending_out,a,t.next_out),t.next_out+=a,e.pending_out+=a,t.total_out+=a,t.avail_out-=a,e.pending-=a,0===e.pending&&(e.pending_out=0))}function R(t,e){l._tr_flush_block(t,0<=t.block_start?t.block_start:-1,t.strstart-t.block_start,e),t.block_start=t.strstart,q(t.strm)}function T(t,e){t.pending_buf[t.pending++]=e}function L(t,e){t.pending_buf[t.pending++]=e>>>8&255,t.pending_buf[t.pending++]=255&e}function N(t,e){var a,n,r=)ENC4";
static const char kPlantumlEncoderScriptPart5[] = R"ENC5(t.max_chain_length,i=t.strstart,s=t.prev_length,h=t.nice_match,o=t.strstart>t.w_size-A?t.strstart-(t.w_size-A):0,l=t.window,_=t.w_mask,d=t.prev,u=t.strstart+x,f=l[i+s-1],c=l[i+s];t.prev_length>=t.good_match&&(r>>=2),h>t.lookahead&&(h=t.lookahead);do{if(l[(a=e)+s]===c&&l[a+s-1]===f&&l[a]===l[i]&&l[++a]===l[i+1]){i+=2,a++;do{}while(l[++i]===l[++a]&&l[++i]===l[++a]&&l[++i]===l[++a]&&l[++i]===l[++a]&&l[++i]===l[++a]&&l[++i]===l[++a]&&l[++i]===l[++a]&&l[++i]===l[++a]&&i<u);if(n=x-(u-i),i=u-x,s<n){if(t.match_start=e,h<=(s=n))break;f=l[i+s-1],c=l[i+s]}}}while((e=d[e&_])>o&&0!=--r);return s<=t.lookahead?s:t.lookahead}function H(t){var e,a,n,r,i,s,h,o,l,_,d=t.w_size;do{if(r=t.window_size-t.lookahead-t.strstart,t.strstart>=d+(d-A)){for(u.arraySet(t.window,t.window,d,d,0),t.match_start-=d,t.strstart-=d,t.block_start-=d,e=a=t.hash_size;n=t.head[--e],t.head[e]=d<=n?n-d:0,--a;);for(e=a=d;n=t.prev[--e],t.prev[e]=d<=n?n-d:0,--a;);r+=d}if(0===t.strm.avail_in)break;if(s=t.strm,h=t.window,o=t.strstart+t.lookahead,l=r,_=void 0,_=s.avail_in,l<_&&(_=l),a=0===_?0:(s.avail_in-=_,u.arraySet(h,s.input,s.next_in,_,o),1===s.state.wrap?s.adler=f(s.adler,h,_,o):2===s.state.wrap&&(s.adler=c(s.adler,h,_,o)),s.next_in+=_,s.total_in+=_,_),t.lookahead+=a,t.lookahead+t.insert>=z)for(i=t.strstart-t.insert,t.ins_h=t.window[i],t.ins_h=(t.ins_h<<t.hash_shift^t.window[i+1])&t.hash_mask;t.insert&&(t.ins_h=(t.ins_h<<t.hash_shift^t.window[i+z-1])&t.hash_mask,t.prev[i&t.w_mask]=t.head[t.ins_h],t.head[t.ins_h]=i,i++,t.insert--,!(t.lookahead+t.insert<z)););}while(t.lookahead<A&&0!==t.strm.avail_in)}function F(t,e){for(var a,n;;){if(t.lookahead<A){if(H(t),t.lookahead<A&&e===_)return S;if(0===t.lookahead)break}if(a=0,t.lookahead>=z&&(t.ins_h=(t.ins_h<<t.hash_shift^t.window[t.strstart+z-1])&t.hash_mask,a=t.prev[t.strstart&t.w_mask]=t.head[t.ins_h],t.head[t.ins_h]=t.strstart),0!==a&&t.strstart-a<=t.w_size-A&&(t.match_length=N(t,a)),t.match_length>=z)if(n=l._tr_tally(t,t.strstart-t.match_start,t.match_length-z),t.loo)ENC5";
static const char kPlantumlEncoderScriptPart6[] = R"ENC6(kahead-=t.match_length,t.match_length<=t.max_lazy_match&&t.lookahead>=z){for(t.match_length--;t.strstart++,t.ins_h=(t.ins_h<<t.hash_shift^t.window[t.strstart+z-1])&t.hash_mask,a=t.prev[t.strstart&t.w_mask]=t.head[t.ins_h],t.head[t.ins_h]=t.strstart,0!=--t.match_length;);t.strstart++}else t.strstart+=t.match_length,t.match_length=0,t.ins_h=t.window[t.strstart],t.ins_h=(t.ins_h<<t.hash_shift^t.window[t.strstart+1])&t.hash_mask;else n=l._tr_tally(t,0,t.window[t.strstart]),t.lookahead--,t.strstart++;if(n&&(R(t,!1),0===t.strm.avail_out))return S}return t.insert=t.strstart<z-1?t.strstart:z-1,e===d?(R(t,!0),0===t.strm.avail_out?j:U):t.last_lit&&(R(t,!1),0===t.strm.avail_out)?S:E}function K(t,e){for(var a,n,r;;){if(t.lookahead<A){if(H(t),t.lookahead<A&&e===_)return S;if(0===t.lookahead)break}if(a=0,t.lookahead>=z&&(t.ins_h=(t.ins_h<<t.hash_shift^t.window[t.strstart+z-1])&t.hash_mask,a=t.prev[t.strstart&t.w_mask]=t.head[t.ins_h],t.head[t.ins_h]=t.strstart),t.prev_length=t.match_length,t.prev_match=t.match_start,t.match_length=z-1,0!==a&&t.prev_length<t.max_lazy_match&&t.strstart-a<=t.w_size-A&&(t.match_length=N(t,a),t.match_length<=5&&(1===t.strategy||t.match_length===z&&4096<t.strstart-t.match_start)&&(t.match_length=z-1)),t.prev_length>=z&&t.match_length<=t.prev_length){for(r=t.strstart+t.lookahead-z,n=l._tr_tally(t,t.strstart-1-t.prev_match,t.prev_length-z),t.lookahead-=t.prev_length-1,t.prev_length-=2;++t.strstart<=r&&(t.ins_h=(t.ins_h<<t.hash_shift^t.window[t.strstart+z-1])&t.hash_mask,a=t.prev[t.strstart&t.w_mask]=t.head[t.ins_h],t.head[t.ins_h]=t.strstart),0!=--t.prev_length;);if(t.match_available=0,t.match_length=z-1,t.strstart++,n&&(R(t,!1),0===t.strm.avail_out))return S}else if(t.match_available){if((n=l._tr_tally(t,0,t.window[t.strstart-1]))&&R(t,!1),t.strstart++,t.lookahead--,0===t.strm.avail_out)return S}else t.match_available=1,t.strstart++,t.lookahead--}return t.match_available&&(n=l._tr_tally(t,0,t.window[t.strstart-1]),t.match_available=0),t.insert=t.strstar)ENC6";
static const char kPlantumlEncoderScriptPart7[] = R"ENC7(t<z-1?t.strstart:z-1,e===d?(R(t,!0),0===t.strm.avail_out?j:U):t.last_lit&&(R(t,!1),0===t.strm.avail_out)?S:E}function M(t,e,a,n,r){this.good_length=t,this.max_lazy=e,this.nice_length=a,this.max_chain=n,this.func=r}function P(){this.strm=null,this.status=0,this.pending_buf=null,this.pending_buf_size=0,this.pending_out=0,this.pending=0,this.wrap=0,this.gzhead=null,this.gzindex=0,this.method=v,this.last_flush=-1,this.w_size=0,this.w_bits=0,this.w_mask=0,this.window=null,this.window_size=0,this.prev=null,this.head=null,this.ins_h=0,this.hash_size=0,this.hash_bits=0,this.hash_mask=0,this.hash_shift=0,this.block_start=0,this.match_length=0,this.prev_match=0,this.match_available=0,this.strstart=0,this.match_start=0,this.lookahead=0,this.prev_length=0,this.max_chain_length=0,this.max_lazy_match=0,this.level=0,this.strategy=0,this.good_match=0,this.nice_match=0,this.dyn_ltree=new u.Buf16(2*y),this.dyn_dtree=new u.Buf16(2*(2*s+1)),this.bl_tree=new u.Buf16(2*(2*h+1)),O(this.dyn_ltree),O(this.dyn_dtree),O(this.bl_tree),this.l_desc=null,this.d_desc=null,this.bl_desc=null,this.bl_count=new u.Buf16(k+1),this.heap=new u.Buf16(2*i+1),O(this.heap),this.heap_len=0,this.heap_max=0,this.depth=new u.Buf16(2*i+1),O(this.depth),this.l_buf=0,this.lit_bufsize=0,this.last_lit=0,this.d_buf=0,this.opt_len=0,this.static_len=0,this.matches=0,this.insert=0,this.bi_buf=0,this.bi_valid=0}function G(t){var e;return t&&t.state?(t.total_in=t.total_out=0,t.data_type=r,(e=t.state).pending=0,e.pending_out=0,e.wrap<0&&(e.wrap=-e.wrap),e.status=e.wrap?B:C,t.adler=2===e.wrap?0:1,e.last_flush=_,l._tr_init(e),p):D(t,g)}function J(t){var e=G(t);return e===p&&function(t){t.window_size=2*t.w_size,O(t.head),t.max_lazy_match=o[t.level].max_lazy,t.good_match=o[t.level].good_length,t.nice_match=o[t.level].nice_length,t.max_chain_length=o[t.level].max_chain,t.strstart=0,t.block_start=0,t.lookahead=0,t.insert=0,t.match_length=t.prev_length=z-1,t.match_available=0,t.ins_h=0}(t.state),e}function Q(t,e,a,n,r,i){if(!t)retu)ENC7";
static const char kPlantumlEncoderScriptPart8[] = R"ENC8(rn g;var s=1;if(e===m&&(e=6),n<0?(s=0,n=-n):15<n&&(s=2,n-=16),r<1||w<r||a!==v||n<8||15<n||e<0||9<e||i<0||b<i)return D(t,g);8===n&&(n=9);var h=new P;return(t.state=h).strm=t,h.wrap=s,h.gzhead=null,h.w_bits=n,h.w_size=1<<h.w_bits,h.w_mask=h.w_size-1,h.hash_bits=r+7,h.hash_size=1<<h.hash_bits,h.hash_mask=h.hash_size-1,h.hash_shift=~~((h.hash_bits+z-1)/z),h.window=new u.Buf8(2*h.w_size),h.head=new u.Buf16(h.hash_size),h.prev=new u.Buf16(h.w_size),h.lit_bufsize=1<<r+6,h.pending_buf_size=4*h.lit_bufsize,h.pending_buf=new u.Buf8(h.pending_buf_size),h.d_buf=1*h.lit_bufsize,h.l_buf=3*h.lit_bufsize,h.level=e,h.strategy=i,h.method=a,J(t)}o=[new M(0,0,0,0,function(t,e){var a=65535;for(a>t.pending_buf_size-5&&(a=t.pending_buf_size-5);;){if(t.lookahead<=1){if(H(t),0===t.lookahead&&e===_)return S;if(0===t.lookahead)break}t.strstart+=t.lookahead,t.lookahead=0;var n=t.block_start+a;if((0===t.strstart||t.strstart>=n)&&(t.lookahead=t.strstart-n,t.strstart=n,R(t,!1),0===t.strm.avail_out))return S;if(t.strstart-t.block_start>=t.w_size-A&&(R(t,!1),0===t.strm.avail_out))return S}return t.insert=0,e===d?(R(t,!0),0===t.strm.avail_out?j:U):(t.strstart>t.block_start&&(R(t,!1),t.strm.avail_out),S)}),new M(4,4,8,4,F),new M(4,5,16,8,F),new M(4,6,32,32,F),new M(4,4,16,16,K),new M(8,16,32,32,K),new M(8,16,128,128,K),new M(8,32,128,256,K),new M(32,128,258,1024,K),new M(32,258,258,4096,K)],a.deflateInit=function(t,e){return Q(t,e,v,15,8,0)},a.deflateInit2=Q,a.deflateReset=J,a.deflateResetKeep=G,a.deflateSetHeader=function(t,e){return t&&t.state?2!==t.state.wrap?g:(t.state.gzhead=e,p):g},a.deflate=function(t,e){var a,n,r,i;if(!t||!t.state||5<e||e<0)return t?D(t,g):g;if(n=t.state,!t.output||!t.input&&0!==t.avail_in||666===n.status&&e!==d)return D(t,0===t.avail_out?-5:g);if(n.strm=t,a=n.last_flush,n.last_flush=e,n.status===B)if(2===n.wrap)t.adler=0,T(n,31),T(n,139),T(n,8),n.gzhead?(T(n,(n.gzhead.text?1:0)+(n.gzhead.hcrc?2:0)+(n.gzhead.extra?4:0)+(n.gzhead.name?8:0)+(n.gzhead.comment?16:0)),T(n,255&n.gz)ENC8";
static const char kPlantumlEncoderScriptPart9[] = R"ENC9(head.time),T(n,n.gzhead.time>>8&255),T(n,n.gzhead.time>>16&255),T(n,n.gzhead.time>>24&255),T(n,9===n.level?2:2<=n.strategy||n.level<2?4:0),T(n,255&n.gzhead.os),n.gzhead.extra&&n.gzhead.extra.length&&(T(n,255&n.gzhead.extra.length),T(n,n.gzhead.extra.length>>8&255)),n.gzhead.hcrc&&(t.adler=c(t.adler,n.pending_buf,n.pending,0)),n.gzindex=0,n.status=69):(T(n,0),T(n,0),T(n,0),T(n,0),T(n,0),T(n,9===n.level?2:2<=n.strategy||n.level<2?4:0),T(n,3),n.status=C);else{var s=v+(n.w_bits-8<<4)<<8;s|=(2<=n.strategy||n.level<2?0:n.level<6?1:6===n.level?2:3)<<6,0!==n.strstart&&(s|=32),s+=31-s%31,n.status=C,L(n,s),0!==n.strstart&&(L(n,t.adler>>>16),L(n,65535&t.adler)),t.adler=1}if(69===n.status)if(n.gzhead.extra){for(r=n.pending;n.gzindex<(65535&n.gzhead.extra.length)&&(n.pending!==n.pending_buf_size||(n.gzhead.hcrc&&n.pending>r&&(t.adler=c(t.adler,n.pending_buf,n.pending-r,r)),q(t),r=n.pending,n.pending!==n.pending_buf_size));)T(n,255&n.gzhead.extra[n.gzindex]),n.gzindex++;n.gzhead.hcrc&&n.pending>r&&(t.adler=c(t.adler,n.pending_buf,n.pending-r,r)),n.gzindex===n.gzhead.extra.length&&(n.gzindex=0,n.status=73)}else n.status=73;if(73===n.status)if(n.gzhead.name){r=n.pending;do{if(n.pending===n.pending_buf_size&&(n.gzhead.hcrc&&n.pending>r&&(t.adler=c(t.adler,n.pending_buf,n.pending-r,r)),q(t),r=n.pending,n.pending===n.pending_buf_size)){i=1;break}i=n.gzindex<n.gzhead.name.length?255&n.gzhead.name.charCodeAt(n.gzindex++):0,T(n,i)}while(0!==i);n.gzhead.hcrc&&n.pending>r&&(t.adler=c(t.adler,n.pending_buf,n.pending-r,r)),0===i&&(n.gzindex=0,n.status=91)}else n.status=91;if(91===n.status)if(n.gzhead.comment){r=n.pending;do{if(n.pending===n.pending_buf_size&&(n.gzhead.hcrc&&n.pending>r&&(t.adler=c(t.adler,n.pending_buf,n.pending-r,r)),q(t),r=n.pending,n.pending===n.pending_buf_size)){i=1;break}i=n.gzindex<n.gzhead.comment.length?255&n.gzhead.comment.charCodeAt(n.gzindex++):0,T(n,i)}while(0!==i);n.gzhead.hcrc&&n.pending>r&&(t.adler=c(t.adler,n.pending_buf,n.pending-r,r)),0===i&&(n.status=103))ENC9";
static const char kPlantumlEncoderScriptPart10[] = R"ENC10(}else n.status=103;if(103===n.status&&(n.gzhead.hcrc?(n.pending+2>n.pending_buf_size&&q(t),n.pending+2<=n.pending_buf_size&&(T(n,255&t.adler),T(n,t.adler>>8&255),t.adler=0,n.status=C)):n.status=C),0!==n.pending){if(q(t),0===t.avail_out)return n.last_flush=-1,p}else if(0===t.avail_in&&I(e)<=I(a)&&e!==d)return D(t,-5);if(666===n.status&&0!==t.avail_in)return D(t,-5);if(0!==t.avail_in||0!==n.lookahead||e!==_&&666!==n.status){var h=2===n.strategy?function(t,e){for(var a;;){if(0===t.lookahead&&(H(t),0===t.lookahead)){if(e===_)return S;break}if(t.match_length=0,a=l._tr_tally(t,0,t.window[t.strstart]),t.lookahead--,t.strstart++,a&&(R(t,!1),0===t.strm.avail_out))return S}return t.insert=0,e===d?(R(t,!0),0===t.strm.avail_out?j:U):t.last_lit&&(R(t,!1),0===t.strm.avail_out)?S:E}(n,e):3===n.strategy?function(t,e){for(var a,n,r,i,s=t.window;;){if(t.lookahead<=x){if(H(t),t.lookahead<=x&&e===_)return S;if(0===t.lookahead)break}if(t.match_length=0,t.lookahead>=z&&0<t.strstart&&(n=s[r=t.strstart-1])===s[++r]&&n===s[++r]&&n===s[++r]){i=t.strstart+x;do{}while(n===s[++r]&&n===s[++r]&&n===s[++r]&&n===s[++r]&&n===s[++r]&&n===s[++r]&&n===s[++r]&&n===s[++r]&&r<i);t.match_length=x-(i-r),t.match_length>t.lookahead&&(t.match_length=t.lookahead)}if(t.match_length>=z?(a=l._tr_tally(t,1,t.match_length-z),t.lookahead-=t.match_length,t.strstart+=t.match_length,t.match_length=0):(a=l._tr_tally(t,0,t.window[t.strstart]),t.lookahead--,t.strstart++),a&&(R(t,!1),0===t.strm.avail_out))return S}return t.insert=0,e===d?(R(t,!0),0===t.strm.avail_out?j:U):t.last_lit&&(R(t,!1),0===t.strm.avail_out)?S:E}(n,e):o[n.level].func(n,e);if(h!==j&&h!==U||(n.status=666),h===S||h===j)return 0===t.avail_out&&(n.last_flush=-1),p;if(h===E&&(1===e?l._tr_align(n):5!==e&&(l._tr_stored_block(n,0,0,!1),3===e&&(O(n.head),0===n.lookahead&&(n.strstart=0,n.block_start=0,n.insert=0))),q(t),0===t.avail_out))return n.last_flush=-1,p}return e!==d?p:n.wrap<=0?1:(2===n.wrap?(T(n,255&t.adler),T(n,t.adler>>8&255),T(n,t.adler>>16&255),T(n,)ENC10";
static const char kPlantumlEncoderScriptPart11[] = R"ENC11(t.adler>>24&255),T(n,255&t.total_in),T(n,t.total_in>>8&255),T(n,t.total_in>>16&255),T(n,t.total_in>>24&255)):(L(n,t.adler>>>16),L(n,65535&t.adler)),q(t),0<n.wrap&&(n.wrap=-n.wrap),0!==n.pending?p:1)},a.deflateEnd=function(t){var e;return t&&t.state?(e=t.state.status)!==B&&69!==e&&73!==e&&91!==e&&103!==e&&e!==C&&666!==e?D(t,g):(t.state=null,e===C?D(t,-3):p):g},a.deflateSetDictionary=function(t,e){var a,n,r,i,s,h,o,l,_=e.length;if(!t||!t.state)return g;if(2===(i=(a=t.state).wrap)||1===i&&a.status!==B||a.lookahead)return g;for(1===i&&(t.adler=f(t.adler,e,_,0)),a.wrap=0,_>=a.w_size&&(0===i&&(O(a.head),a.strstart=0,a.block_start=0,a.insert=0),l=new u.Buf8(a.w_size),u.arraySet(l,e,_-a.w_size,a.w_size,0),e=l,_=a.w_size),s=t.avail_in,h=t.next_in,o=t.input,t.avail_in=_,t.next_in=0,t.input=e,H(a);a.lookahead>=z;){for(n=a.strstart,r=a.lookahead-(z-1);a.ins_h=(a.ins_h<<a.hash_shift^a.window[n+z-1])&a.hash_mask,a.prev[n&a.w_mask]=a.head[a.ins_h],a.head[a.ins_h]=n,n++,--r;);a.strstart=n,a.lookahead=z-1,H(a)}return a.strstart+=a.lookahead,a.block_start=a.strstart,a.insert=a.lookahead,a.lookahead=0,a.match_length=a.prev_length=z-1,a.match_available=0,t.next_in=h,t.input=o,t.avail_in=s,a.wrap=i,p},a.deflateInfo="pako deflate (from Nodeca project)"},{"../utils/common":5,"./adler32":7,"./crc32":8,"./messages":10,"./trees":11}],10:[function(t,e,a){"use strict";e.exports={2:"need dictionary",1:"stream end",0:"","-1":"file error","-2":"stream error","-3":"data error","-4":"insufficient memory","-5":"buffer error","-6":"incompatible version"}},{}],11:[function(t,e,a){"use strict";var r=t("../utils/common"),h=0,o=1;function n(t){for(var e=t.length;0<=--e;)t[e]=0}var i=0,s=29,l=256,_=l+1+s,d=30,u=19,g=2*_+1,m=15,f=16,c=7,p=256,b=16,v=17,w=18,y=[0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0],k=[0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13],z=[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,3,7],x=[16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15],A=new Array(2*(_+2));n(A);)ENC11";
static const char kPlantumlEncoderScriptPart12[] = R"ENC12(var B=new Array(2*d);n(B);var C=new Array(512);n(C);var S=new Array(256);n(S);var E=new Array(s);n(E);var j,U,D,I=new Array(d);function O(t,e,a,n,r){this.static_tree=t,this.extra_bits=e,this.extra_base=a,this.elems=n,this.max_length=r,this.has_stree=t&&t.length}function q(t,e){this.dyn_tree=t,this.max_code=0,this.stat_desc=e}function R(t){return t<256?C[t]:C[256+(t>>>7)]}function T(t,e){t.pending_buf[t.pending++]=255&e,t.pending_buf[t.pending++]=e>>>8&255}function L(t,e,a){t.bi_valid>f-a?(t.bi_buf|=e<<t.bi_valid&65535,T(t,t.bi_buf),t.bi_buf=e>>f-t.bi_valid,t.bi_valid+=a-f):(t.bi_buf|=e<<t.bi_valid&65535,t.bi_valid+=a)}function N(t,e,a){L(t,a[2*e],a[2*e+1])}function H(t,e){for(var a=0;a|=1&t,t>>>=1,a<<=1,0<--e;);return a>>>1}function F(t,e,a){var n,r,i=new Array(m+1),s=0;for(n=1;n<=m;n++)i[n]=s=s+a[n-1]<<1;for(r=0;r<=e;r++){var h=t[2*r+1];0!==h&&(t[2*r]=H(i[h]++,h))}}function K(t){var e;for(e=0;e<_;e++)t.dyn_ltree[2*e]=0;for(e=0;e<d;e++)t.dyn_dtree[2*e]=0;for(e=0;e<u;e++)t.bl_tree[2*e]=0;t.dyn_ltree[2*p]=1,t.opt_len=t.static_len=0,t.last_lit=t.matches=0}function M(t){8<t.bi_valid?T(t,t.bi_buf):0<t.bi_valid&&(t.pending_buf[t.pending++]=t.bi_buf),t.bi_buf=0,t.bi_valid=0}function P(t,e,a,n){var r=2*e,i=2*a;return t[r]<t[i]||t[r]===t[i]&&n[e]<=n[a]}function G(t,e,a){for(var n=t.heap[a],r=a<<1;r<=t.heap_len&&(r<t.heap_len&&P(e,t.heap[r+1],t.heap[r],t.depth)&&r++,!P(e,n,t.heap[r],t.depth));)t.heap[a]=t.heap[r],a=r,r<<=1;t.heap[a]=n}function J(t,e,a){var n,r,i,s,h=0;if(0!==t.last_lit)for(;n=t.pending_buf[t.d_buf+2*h]<<8|t.pending_buf[t.d_buf+2*h+1],r=t.pending_buf[t.l_buf+h],h++,0===n?N(t,r,e):(N(t,(i=S[r])+l+1,e),0!==(s=y[i])&&L(t,r-=E[i],s),N(t,i=R(--n),a),0!==(s=k[i])&&L(t,n-=I[i],s)),h<t.last_lit;);N(t,p,e)}function Q(t,e){var a,n,r,i=e.dyn_tree,s=e.stat_desc.static_tree,h=e.stat_desc.has_stree,o=e.stat_desc.elems,l=-1;for(t.heap_len=0,t.heap_max=g,a=0;a<o;a++)0!==i[2*a]?(t.heap[++t.heap_len]=l=a,t.depth[a]=0):i[2*a+1]=0;for(;t.heap_len<2;)i[2*(r=t.heap[++t.heap_len]=l<)ENC12";
static const char kPlantumlEncoderScriptPart13[] = R"ENC13(2?++l:0)]=1,t.depth[r]=0,t.opt_len--,h&&(t.static_len-=s[2*r+1]);for(e.max_code=l,a=t.heap_len>>1;1<=a;a--)G(t,i,a);for(r=o;a=t.heap[1],t.heap[1]=t.heap[t.heap_len--],G(t,i,1),n=t.heap[1],t.heap[--t.heap_max]=a,t.heap[--t.heap_max]=n,i[2*r]=i[2*a]+i[2*n],t.depth[r]=(t.depth[a]>=t.depth[n]?t.depth[a]:t.depth[n])+1,i[2*a+1]=i[2*n+1]=r,t.heap[1]=r++,G(t,i,1),2<=t.heap_len;);t.heap[--t.heap_max]=t.heap[1],function(t,e){var a,n,r,i,s,h,o=e.dyn_tree,l=e.max_code,_=e.stat_desc.static_tree,d=e.stat_desc.has_stree,u=e.stat_desc.extra_bits,f=e.stat_desc.extra_base,c=e.stat_desc.max_length,p=0;for(i=0;i<=m;i++)t.bl_count[i]=0;for(o[2*t.heap[t.heap_max]+1]=0,a=t.heap_max+1;a<g;a++)c<(i=o[2*o[2*(n=t.heap[a])+1]+1]+1)&&(i=c,p++),o[2*n+1]=i,l<n||(t.bl_count[i]++,s=0,f<=n&&(s=u[n-f]),h=o[2*n],t.opt_len+=h*(i+s),d&&(t.static_len+=h*(_[2*n+1]+s)));if(0!==p){do{for(i=c-1;0===t.bl_count[i];)i--;t.bl_count[i]--,t.bl_count[i+1]+=2,t.bl_count[c]--,p-=2}while(0<p);for(i=c;0!==i;i--)for(n=t.bl_count[i];0!==n;)l<(r=t.heap[--a])||(o[2*r+1]!==i&&(t.opt_len+=(i-o[2*r+1])*o[2*r],o[2*r+1]=i),n--)}}(t,e),F(i,l,t.bl_count)}function V(t,e,a){var n,r,i=-1,s=e[1],h=0,o=7,l=4;for(0===s&&(o=138,l=3),e[2*(a+1)+1]=65535,n=0;n<=a;n++)r=s,s=e[2*(n+1)+1],++h<o&&r===s||(h<l?t.bl_tree[2*r]+=h:0!==r?(r!==i&&t.bl_tree[2*r]++,t.bl_tree[2*b]++):h<=10?t.bl_tree[2*v]++:t.bl_tree[2*w]++,i=r,l=(h=0)===s?(o=138,3):r===s?(o=6,3):(o=7,4))}function W(t,e,a){var n,r,i=-1,s=e[1],h=0,o=7,l=4;for(0===s&&(o=138,l=3),n=0;n<=a;n++)if(r=s,s=e[2*(n+1)+1],!(++h<o&&r===s)){if(h<l)for(;N(t,r,t.bl_tree),0!=--h;);else 0!==r?(r!==i&&(N(t,r,t.bl_tree),h--),N(t,b,t.bl_tree),L(t,h-3,2)):h<=10?(N(t,v,t.bl_tree),L(t,h-3,3)):(N(t,w,t.bl_tree),L(t,h-11,7));i=r,l=(h=0)===s?(o=138,3):r===s?(o=6,3):(o=7,4)}}n(I);var X=!1;function Y(t,e,a,n){L(t,(i<<1)+(n?1:0),3),function(t,e,a,n){M(t),n&&(T(t,a),T(t,~a)),r.arraySet(t.pending_buf,t.window,e,a,t.pending),t.pending+=a}(t,e,a,!0)}a._tr_init=function(t){X||(function(){var t,e,a,n,r,i=new Array(m+1);fo)ENC13";
static const char kPlantumlEncoderScriptPart14[] = R"ENC14(r(n=a=0;n<s-1;n++)for(E[n]=a,t=0;t<1<<y[n];t++)S[a++]=n;for(S[a-1]=n,n=r=0;n<16;n++)for(I[n]=r,t=0;t<1<<k[n];t++)C[r++]=n;for(r>>=7;n<d;n++)for(I[n]=r<<7,t=0;t<1<<k[n]-7;t++)C[256+r++]=n;for(e=0;e<=m;e++)i[e]=0;for(t=0;t<=143;)A[2*t+1]=8,t++,i[8]++;for(;t<=255;)A[2*t+1]=9,t++,i[9]++;for(;t<=279;)A[2*t+1]=7,t++,i[7]++;for(;t<=287;)A[2*t+1]=8,t++,i[8]++;for(F(A,_+1,i),t=0;t<d;t++)B[2*t+1]=5,B[2*t]=H(t,5);j=new O(A,y,l+1,_,m),U=new O(B,k,0,d,m),D=new O(new Array(0),z,0,u,c)}(),X=!0),t.l_desc=new q(t.dyn_ltree,j),t.d_desc=new q(t.dyn_dtree,U),t.bl_desc=new q(t.bl_tree,D),t.bi_buf=0,t.bi_valid=0,K(t)},a._tr_stored_block=Y,a._tr_flush_block=function(t,e,a,n){var r,i,s=0;0<t.level?(2===t.strm.data_type&&(t.strm.data_type=function(t){var e,a=4093624447;for(e=0;e<=31;e++,a>>>=1)if(1&a&&0!==t.dyn_ltree[2*e])return h;if(0!==t.dyn_ltree[18]||0!==t.dyn_ltree[20]||0!==t.dyn_ltree[26])return o;for(e=32;e<l;e++)if(0!==t.dyn_ltree[2*e])return o;return h}(t)),Q(t,t.l_desc),Q(t,t.d_desc),s=function(t){var e;for(V(t,t.dyn_ltree,t.l_desc.max_code),V(t,t.dyn_dtree,t.d_desc.max_code),Q(t,t.bl_desc),e=u-1;3<=e&&0===t.bl_tree[2*x[e]+1];e--);return t.opt_len+=3*(e+1)+5+5+4,e}(t),r=t.opt_len+3+7>>>3,(i=t.static_len+3+7>>>3)<=r&&(r=i)):r=i=a+5,a+4<=r&&-1!==e?Y(t,e,a,n):4===t.strategy||i===r?(L(t,2+(n?1:0),3),J(t,A,B)):(L(t,4+(n?1:0),3),function(t,e,a,n){var r;for(L(t,e-257,5),L(t,a-1,5),L(t,n-4,4),r=0;r<n;r++)L(t,t.bl_tree[2*x[r]+1],3);W(t,t.dyn_ltree,e-1),W(t,t.dyn_dtree,a-1)}(t,t.l_desc.max_code+1,t.d_desc.max_code+1,s+1),J(t,t.dyn_ltree,t.dyn_dtree)),K(t),n&&M(t)},a._tr_tally=function(t,e,a){return t.pending_buf[t.d_buf+2*t.last_lit]=e>>>8&255,t.pending_buf[t.d_buf+2*t.last_lit+1]=255&e,t.pending_buf[t.l_buf+t.last_lit]=255&a,t.last_lit++,0===e?t.dyn_ltree[2*a]++:(t.matches++,e--,t.dyn_ltree[2*(S[a]+l+1)]++,t.dyn_dtree[2*R(e)]++),t.last_lit===t.lit_bufsize-1},a._tr_align=function(t){L(t,2,3),N(t,p,A),function(t){16===t.bi_valid?(T(t,t.bi_buf),t.bi_buf=0,t.bi_valid=0):8<=t.bi_valid&&(t.pendi)ENC14";
static const char kPlantumlEncoderScriptPart15[] = R"ENC15(ng_buf[t.pending++]=255&t.bi_buf,t.bi_buf>>=8,t.bi_valid-=8)}(t)}},{"../utils/common":5}],12:[function(t,e,a){"use strict";e.exports=function(){this.input=null,this.next_in=0,this.avail_in=0,this.total_in=0,this.output=null,this.next_out=0,this.avail_out=0,this.total_out=0,this.msg="",this.state=null,this.data_type=2,this.adler=0}},{}]},{},[3])(3)});)ENC15";

static const std::wstring& PlantumlEncoderScript() {
    static const std::wstring script = []() {
        std::string combined;
        combined.reserve(30000);
        combined.append(kPlantumlEncoderScriptPart1);
        combined.append(kPlantumlEncoderScriptPart2);
        combined.append(kPlantumlEncoderScriptPart3);
        combined.append(kPlantumlEncoderScriptPart4);
        combined.append(kPlantumlEncoderScriptPart5);
        combined.append(kPlantumlEncoderScriptPart6);
        combined.append(kPlantumlEncoderScriptPart7);
        combined.append(kPlantumlEncoderScriptPart8);
        combined.append(kPlantumlEncoderScriptPart9);
        combined.append(kPlantumlEncoderScriptPart10);
        combined.append(kPlantumlEncoderScriptPart11);
        combined.append(kPlantumlEncoderScriptPart12);
        combined.append(kPlantumlEncoderScriptPart13);
        combined.append(kPlantumlEncoderScriptPart14);
        combined.append(kPlantumlEncoderScriptPart15);
        return FromUtf8(combined);
    }();
    return script;
}

static std::wstring BuildShellHtmlWithBody(const std::wstring& body, bool preferSvg);

static bool BuildHtmlFromJavaRender(const std::wstring& umlText,
                                    bool preferSvg,
                                    std::wstring& outHtml,
                                    std::wstring* outSvg,
                                    std::vector<unsigned char>* outPng,
                                    std::wstring* outErrorMessage) {
    auto setError = [&](const std::wstring& message) {
        if (outErrorMessage) {
            *outErrorMessage = message;
        }
    };

    std::wstring svgOut;
    std::vector<unsigned char> pngOut;
    if (!RunPlantUmlJar(umlText, preferSvg, svgOut, pngOut)) {
        setError(L"Local Java/JAR rendering failed. Check Java installation and plantuml.jar path in the INI file.");
        return false;
    }

    if (preferSvg) {
        outHtml = BuildShellHtmlWithBody(svgOut, true);
    } else {
        const std::string b64 = Base64(pngOut);
        std::wstring body = L"<img alt=\"diagram\" src=\"data:image/png;base64,";
        body += FromUtf8(b64);
        body += L"\"/>";
        outHtml = BuildShellHtmlWithBody(body, false);
    }
    if (outSvg) {
        *outSvg = std::move(svgOut);
    }
    if (outPng) {
        *outPng = std::move(pngOut);
    }
    setError(std::wstring());
    return true;
}

// Build minimal HTML wrapper with injected BODY (svg markup or <img src="data:...">)
static std::wstring BuildShellHtmlWithBody(const std::wstring& body, bool preferSvg) {
    std::wstring html = LR"HTML(<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta http-equiv="X-UA-Compatible" content="IE=edge"/>
  <meta name="viewport" content="width=device-width, initial-scale=1"/>
  <title>PlantUML Viewer</title>
  <style>
    :root { color-scheme: light dark; }
    html, body { height: 100%; }
    body { margin: 0; background: canvas; color: CanvasText; font: 13px system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; position: relative; }
    #toolbar { position: fixed; top: 8px; left: 8px; display: flex; gap: 6px; z-index: 10; }
    #toolbar button, #toolbar select { padding: 6px 10px; border-radius: 6px; border: 1px solid color-mix(in oklab, Canvas 70%, CanvasText 30%); background: color-mix(in oklab, Canvas 92%, CanvasText 8%); color: inherit; font: inherit; cursor: pointer; }
    #toolbar button:hover, #toolbar select:hover { background: color-mix(in oklab, Canvas 88%, CanvasText 12%); }
    #toolbar button:disabled, #toolbar select:disabled { opacity: 0.6; cursor: not-allowed; }
    #root { padding: 56px 8px 8px 8px; display: grid; place-items: start center; }
    img, svg { max-width: 100%; height: auto; }
    .err { padding: 12px 14px; border-radius: 10px; background: color-mix(in oklab, Canvas 85%, red 15%); }
  </style>
</head>
<body data-format="{{FORMAT}}">
  <div id="toolbar">
    <button id="btn-refresh" type="button">Refresh</button>
    <button id="btn-save" type="button">Save as...</button>
    <select id="format-select">
      <option value="svg">SVG</option>
      <option value="png">PNG</option>
    </select>
    <button id="btn-copy" type="button">Copy to clipboard</button>
  </div>
  <div id="root">
    {{BODY}}
  </div>
  <script>
    const hookButton = (btn, messageType) => {
      if (!btn) {
        return;
      }
      const update = () => {
        const connected = !!(window.chrome && window.chrome.webview);
        btn.disabled = !connected;
        if (!connected) {
          btn.title = 'Available inside Total Commander';
          window.setTimeout(update, 1000);
        } else {
          btn.removeAttribute('title');
        }
      };
      update();
      btn.addEventListener('click', () => {
        if (window.chrome && window.chrome.webview) {
          window.chrome.webview.postMessage({ type: messageType });
        }
      });
    };
    hookButton(document.getElementById('btn-refresh'), 'refresh');
    hookButton(document.getElementById('btn-save'), 'saveAs');
    const select = document.getElementById('format-select');
    if (select) {
      const setDisabled = (disabled) => {
        if (disabled) {
          select.setAttribute('disabled', 'disabled');
        } else {
          select.removeAttribute('disabled');
        }
      };
      const update = () => {
        const connected = !!(window.chrome && window.chrome.webview);
        setDisabled(!connected);
        if (!connected) {
          select.title = 'Available inside Total Commander';
          window.setTimeout(update, 1000);
        } else {
          select.removeAttribute('title');
        }
      };
      const initial = document.body?.dataset?.format;
      if (initial) {
        select.value = initial;
      }
      select.addEventListener('change', () => {
        if (document.body && document.body.dataset) {
          document.body.dataset.format = select.value;
        }
        if (typeof updateCopyState === 'function') {
          updateCopyState();
        }
        if (window.chrome && window.chrome.webview) {
          window.chrome.webview.postMessage({ type: 'setFormat', format: select.value });
        }
      });
      update();
    }
    const copyButton = document.getElementById('btn-copy');
    const copyWithWebApi = async () => {
      if (!navigator.clipboard) {
        return false;
      }
      const svg = document.querySelector('svg');
      if (svg) {
        const s = new XMLSerializer().serializeToString(svg);
        await navigator.clipboard.writeText(s);
        return true;
      }
      const img = document.querySelector('img');
      if (img) {
        if (!window.ClipboardItem) {
          return false;
        }
        const c = document.createElement('canvas');
        c.width = img.naturalWidth;
        c.height = img.naturalHeight;
        const g = c.getContext('2d');
        g.drawImage(img, 0, 0);
        const blob = await new Promise(r => c.toBlob(r, 'image/png'));
        await navigator.clipboard.write([new ClipboardItem({ 'image/png': blob })]);
        return true;
      }
      return false;
    };
    const triggerCopy = async () => {
      try {
        if (window.chrome && window.chrome.webview) {
          window.chrome.webview.postMessage({ type: 'copy' });
        } else {
          await copyWithWebApi();
        }
      } catch (e) {}
    };
    let updateCopyState = null;
    if (copyButton) {
      updateCopyState = () => {
        const connected = !!(window.chrome && window.chrome.webview);
        const format = document.body?.dataset?.format || 'svg';
        const clipboardItemAvailable = typeof window.ClipboardItem !== 'undefined';
        const webApiAvailable = !!navigator.clipboard && (format !== 'png' || clipboardItemAvailable);
        if (connected) {
          copyButton.disabled = false;
          copyButton.removeAttribute('title');
        } else if (webApiAvailable) {
          copyButton.disabled = false;
          copyButton.title = 'Host unavailable – using browser clipboard';
          window.setTimeout(updateCopyState, 1000);
        } else {
          copyButton.disabled = true;
          copyButton.title = format === 'png' && !clipboardItemAvailable
            ? 'Clipboard image support is unavailable'
            : 'Clipboard access is unavailable';
          window.setTimeout(updateCopyState, 1000);
        }
      };
      updateCopyState();
      copyButton.addEventListener('click', triggerCopy);
    }
    // Ctrl+C copies SVG or PNG
    document.addEventListener('keydown', async ev => {
      if ((ev.ctrlKey || ev.metaKey) && ev.key.toLowerCase() === 'c') {
        ev.preventDefault();
        await triggerCopy();
      }
    });
  </script>
</body>
</html>)HTML";
    ReplaceAll(html, L"{{BODY}}", body);
    ReplaceAll(html, L"{{FORMAT}}", preferSvg ? L"svg" : L"png");
    ReplaceAll(html, L"{{PLANTUML_ENCODER}}", PlantumlEncoderScript());
    return html;
}

static std::wstring BuildErrorHtml(const std::wstring& message, bool preferSvg) {
    std::wstring safe = message;
    ReplaceAll(safe, L"<", L"&lt;");
    ReplaceAll(safe, L">", L"&gt;");
    return BuildShellHtmlWithBody(L"<div class='err'>"+safe+L"</div>", preferSvg);
}

static bool BuildHtmlFromWebRender(const std::wstring& umlText,
                                   const std::wstring& sourcePath,
                                   bool preferSvg,
                                   std::wstring& outHtml,
                                   std::wstring* outErrorMessage) {
    const std::wstring escaped = HtmlEscape(umlText);
    std::wstring sourceName = ExtractFileStem(sourcePath);
    if (sourceName.empty()) {
        sourceName = L"plantuml-diagram";
    }
    const std::wstring safeSourceName = HtmlAttributeEscape(sourceName);

    static const wchar_t kWebShellPart1[] = LR"HTML1(<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta http-equiv="X-UA-Compatible" content="IE=edge"/>
  <meta name="viewport" content="width=device-width, initial-scale=1"/>
  <title>PlantUML Viewer</title>
  <style>
    :root { color-scheme: light dark; }
    html, body { height: 100%; }
    body { margin: 0; background: canvas; color: CanvasText; font: 13px system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; position: relative; }
    #toolbar { position: fixed; top: 8px; left: 8px; display: flex; gap: 6px; z-index: 10; }
    #toolbar button, #toolbar select { padding: 6px 10px; border-radius: 6px; border: 1px solid color-mix(in oklab, Canvas 70%, CanvasText 30%); background: color-mix(in oklab, Canvas 92%, CanvasText 8%); color: inherit; font: inherit; cursor: pointer; }
    #toolbar button:hover, #toolbar select:hover { background: color-mix(in oklab, Canvas 88%, CanvasText 12%); }
    #toolbar button:disabled, #toolbar select:disabled { opacity: 0.6; cursor: not-allowed; }
    #root { padding: 56px 8px 8px 8px; display: grid; place-items: start center; }
    #diagram-container { width: min(1100px, 100%); display: grid; gap: 12px; place-items: center; }
    #diagram-container svg { max-width: 100%; height: auto; }
    #png-image { display: none; max-width: 100%; height: auto; }
    .err { padding: 12px 14px; border-radius: 10px; background: color-mix(in oklab, Canvas 85%, red 15%); display: none; text-align: center; }
    pre.hidden-source { display: none; }
  </style>
  <script>{{PLANTUML_ENCODER}}</script>
</head>
<body data-format="{{FORMAT}}" data-source-name="{{SOURCE_NAME}}">
  <div id="toolbar">
    <button id="btn-refresh" type="button">Refresh</button>
    <button id="btn-save" type="button">Save as...</button>
    <select id="format-select">
      <option value="svg">SVG</option>
      <option value="png">PNG</option>
    </select>
    <button id="btn-copy" type="button">Copy to clipboard</button>
  </div>
  <div id="root">
    <div id="diagram-container">
      <div id="svg-container"></div>
      <img id="png-image" alt="PlantUML Diagram"/>
      <div id="error-box" class="err"></div>
    </div>
  </div>
  <pre id="plantuml-source" class="hidden-source">{{PLANTUML_SOURCE}}</pre>
  <script>
    (function() {
      const PLANTUML_SERVER_URL = 'https://www.plantuml.com/plantuml';

      const bodyEl = document.body;
      const sourceEl = document.getElementById('plantuml-source');
      const svgContainer = document.getElementById('svg-container');
      const pngImage = document.getElementById('png-image');
      const errorBox = document.getElementById('error-box');
      const refreshButton = document.getElementById('btn-refresh');
      const saveButton = document.getElementById('btn-save');
      const copyButton = document.getElementById('btn-copy');
      const formatSelect = document.getElementById('format-select');

      const storageKey = 'plantuml-web-format';

      const state = {
        svgText: '',
        pngDataUrl: '',
        loading: false,
      };

      let lastSentSvg = '';
      let lastSentPng = '';
      let lastSentFormat = '';

      const isConnected = () => !!(window.chrome && window.chrome.webview);
      const getSource = () => (sourceEl ? sourceEl.textContent : '') || '';
      const getFormat = () => (bodyEl && bodyEl.dataset && bodyEl.dataset.format) ? bodyEl.dataset.format : 'svg';
      const setFormat = (value) => {
        if (bodyEl && bodyEl.dataset) {
          bodyEl.dataset.format = value;
        }
      };

      const setError = (message) => {
        if (!errorBox) {
          return;
        }
        if (message) {
          errorBox.textContent = message;
          errorBox.style.display = 'block';
        } else {
          errorBox.textContent = '';
          errorBox.style.display = 'none';
        }
      };

      const clearDiagram = () => {
        if (svgContainer) {
          svgContainer.innerHTML = '';
        }
        if (pngImage) {
          pngImage.removeAttribute('src');
          pngImage.style.display = 'none';
        }
      };

      const hasRenderable = () => {
        if (state.loading) {
          return false;
        }
        if (getFormat() === 'png') {
          return !!state.pngDataUrl;
        }
        return !!state.svgText;
      };

      const encodeBase64 = (text) => {
        try {
          if (typeof TextEncoder !== 'undefined') {
            const bytes = new TextEncoder().encode(text);
            let binary = '';
            bytes.forEach((b) => { binary += String.fromCharCode(b); });
            return window.btoa(binary);
          }
          return window.btoa(unescape(encodeURIComponent(text)));
        } catch (err) {
          console.warn('Unable to encode payload as base64', err);
          return '';
        }
      };

      const extractBase64FromDataUrl = (dataUrl) => {
        if (!dataUrl) {
          return '';
        }
        const comma = dataUrl.indexOf(',');
        return comma >= 0 ? dataUrl.slice(comma + 1) : '';
      };

      const notifyHost = () => {
        if (!isConnected()) {
          return;
        }
        const format = getFormat();
        const svgBase64 = state.svgText ? encodeBase64(state.svgText) : '';
        const pngBase64 = state.pngDataUrl ? extractBase64FromDataUrl(state.pngDataUrl) : '';
        if (svgBase64 === lastSentSvg && pngBase64 === lastSentPng && format === lastSentFormat) {
          return;
        }
        lastSentSvg = svgBase64;
        lastSentPng = pngBase64;
        lastSentFormat = format;
        try {
          window.chrome.webview.postMessage({
            type: 'rendered',
            format,
            svgBase64,
            pngBase64,
          });
        } catch (err) {
          console.warn('Failed to notify host about rendered diagram', err);
        }
      };
)HTML1";

    static const wchar_t kWebShellPart2[] = LR"HTML2(
      const updateSaveState = () => {
        if (!saveButton) {
          return;
        }
        if (hasRenderable()) {
          saveButton.disabled = false;
          saveButton.removeAttribute('title');
        } else if (state.loading) {
          saveButton.disabled = true;
          saveButton.title = 'Rendering diagram...';
        } else {
          saveButton.disabled = true;
          saveButton.title = 'Diagram not rendered yet';
        }
      };

      const updateCopyState = () => {
        if (!copyButton) {
          return;
        }
        const connected = isConnected();
        const clipboardItemAvailable = typeof window.ClipboardItem !== 'undefined';
        const webApiAvailable = !!navigator.clipboard && (getFormat() !== 'png' || clipboardItemAvailable);
        if (connected) {
          copyButton.disabled = false;
          copyButton.removeAttribute('title');
        } else if (webApiAvailable && hasRenderable()) {
          copyButton.disabled = false;
          copyButton.title = 'Host unavailable – using browser clipboard';
        } else if (!webApiAvailable) {
          copyButton.disabled = true;
          copyButton.title = getFormat() === 'png' && !clipboardItemAvailable
            ? 'Clipboard image support is unavailable'
            : 'Clipboard access is unavailable';
        } else {
          copyButton.disabled = true;
          copyButton.title = 'Diagram not rendered yet';
        }
      };

      const updateRefreshState = () => {
        if (!refreshButton) {
          return;
        }
        const connected = isConnected();
        refreshButton.disabled = !connected;
        if (!connected) {
          refreshButton.title = 'Available inside Total Commander';
          window.setTimeout(updateRefreshState, 1000);
        } else {
          refreshButton.removeAttribute('title');
        }
      };

      const dataUrlToBlob = async (dataUrl) => {
        try {
          const res = await fetch(dataUrl);
          return await res.blob();
        } catch (err) {
          console.warn('Failed to convert data URL to blob', err);
          return null;
        }
      };

      const saveWithWebApi = () => {
        if (!hasRenderable()) {
          return;
        }
        const format = getFormat();
        const fileBase = (bodyEl && bodyEl.dataset && bodyEl.dataset.sourceName) ? bodyEl.dataset.sourceName : 'plantuml-diagram';
        const filename = (fileBase || 'plantuml-diagram') + '.' + (format === 'png' ? 'png' : 'svg');
        if (format === 'png' && state.pngDataUrl) {
          const link = document.createElement('a');
          link.href = state.pngDataUrl;
          link.download = filename;
          link.click();
        } else if (state.svgText) {
          const blob = new Blob([state.svgText], { type: 'image/svg+xml;charset=utf-8' });
          const url = URL.createObjectURL(blob);
          const link = document.createElement('a');
          link.href = url;
          link.download = filename;
          link.click();
          window.setTimeout(() => URL.revokeObjectURL(url), 1000);
        }
      };

      const copyWithWebApi = async () => {
        if (!navigator.clipboard || !hasRenderable()) {
          return false;
        }
        const format = getFormat();
        try {
          if (format === 'png' && state.pngDataUrl) {
            if (typeof window.ClipboardItem === 'undefined') {
              return false;
            }
            const blob = await dataUrlToBlob(state.pngDataUrl);
            if (!blob) {
              return false;
            }
            await navigator.clipboard.write([new ClipboardItem({ 'image/png': blob })]);
            return true;
          }
          if (state.svgText) {
            await navigator.clipboard.writeText(state.svgText);
            return true;
          }
        } catch (err) {
          console.warn('Failed to copy diagram to clipboard', err);
        }
        return false;
      };

      const requestFallback = (message) => {
        if (!isConnected()) {
          return;
        }
        try {
          window.chrome.webview.postMessage({ type: 'renderFailed', message });
        } catch (err) {
          console.warn('Failed to notify host about render failure', err);
        }
      };
)HTML2";

    static const wchar_t kWebShellPart3[] = LR"HTML3(
      const renderDiagram = async () => {
        const format = getFormat();
        const source = getSource();
        if (!plantumlEncoder || typeof plantumlEncoder.encode !== 'function') {
          const message = 'PlantUML encoder library not available.';
          state.svgText = '';
          state.pngDataUrl = '';
          state.loading = false;
          clearDiagram();
          setError(message);
          notifyHost();
          requestFallback(message);
          updateSaveState();
          updateCopyState();
          return;
        }
        if (!source.trim()) {
          clearDiagram();
          state.svgText = '';
          state.pngDataUrl = '';
          state.loading = false;
          setError('PlantUML source is empty.');
          notifyHost();
          updateSaveState();
          updateCopyState();
          return;
        }
        state.loading = true;
        setError('');
        updateSaveState();
        updateCopyState();
        clearDiagram();
        const encoded = plantumlEncoder.encode(source);
        const imageURL = PLANTUML_SERVER_URL + '/' + format + '/' + encoded;
        try {
          if (format === 'png') {
            const response = await fetch(imageURL, { cache: 'no-store' });
            if (!response.ok) {
              throw new Error('HTTP ' + response.status);
            }
            const blob = await response.blob();
            const reader = new FileReader();
            const dataUrl = await new Promise((resolve, reject) => {
              reader.onload = () => resolve(reader.result || '');
              reader.onerror = () => reject(new Error('Failed to decode PNG response'));
              reader.readAsDataURL(blob);
            });
            state.svgText = '';
            state.pngDataUrl = typeof dataUrl === 'string' ? dataUrl : '';
            if (pngImage) {
              if (state.pngDataUrl) {
                pngImage.src = state.pngDataUrl;
                pngImage.style.display = 'block';
              } else {
                pngImage.removeAttribute('src');
                pngImage.style.display = 'none';
              }
            }
            if (svgContainer) {
              svgContainer.innerHTML = '';
            }
          } else {
            const response = await fetch(imageURL, { cache: 'no-store' });
            if (!response.ok) {
              throw new Error('HTTP ' + response.status);
            }
            const svgText = await response.text();
            state.svgText = svgText;
            state.pngDataUrl = '';
            if (svgContainer) {
              svgContainer.innerHTML = svgText;
            }
            if (pngImage) {
              pngImage.removeAttribute('src');
              pngImage.style.display = 'none';
            }
          }
          setError('');
          notifyHost();
        } catch (err) {
          console.error('Failed to fetch PlantUML diagram', err);
          const message = 'Unable to load diagram from PlantUML server.';
          state.svgText = '';
          state.pngDataUrl = '';
          clearDiagram();
          setError(message);
          notifyHost();
          requestFallback(message);
        } finally {
          state.loading = false;
          updateSaveState();
          updateCopyState();
        }
      };
)HTML3";

    static const wchar_t kWebShellPart4[] = LR"HTML4(
      if (formatSelect) {
        const stored = (() => {
          try {
            return window.localStorage ? window.localStorage.getItem(storageKey) : null;
          } catch (err) {
            return null;
          }
        })();
        const initial = (stored === 'png' || stored === 'svg') ? stored : getFormat();
        setFormat(initial);
        formatSelect.value = initial;
        formatSelect.addEventListener('change', () => {
          const value = formatSelect.value === 'png' ? 'png' : 'svg';
          setFormat(value);
          try {
            if (window.localStorage) {
              window.localStorage.setItem(storageKey, value);
            }
          } catch (err) {}
          state.svgText = '';
          state.pngDataUrl = '';
          notifyHost();
          renderDiagram();
          updateSaveState();
          updateCopyState();
          if (isConnected()) {
            try {
              window.chrome.webview.postMessage({ type: 'setFormat', format: value });
            } catch (err) {
              console.warn('Failed to notify host about format change', err);
            }
          }
        });
      } else {
        setFormat(getFormat());
      }

      if (refreshButton) {
        refreshButton.addEventListener('click', () => {
          if (isConnected()) {
            window.chrome.webview.postMessage({ type: 'refresh' });
          } else {
            renderDiagram();
          }
        });
      }

      if (saveButton) {
        saveButton.addEventListener('click', () => {
          if (isConnected()) {
            window.chrome.webview.postMessage({ type: 'saveAs' });
          } else {
            saveWithWebApi();
          }
        });
      }

      if (copyButton) {
        copyButton.addEventListener('click', async () => {
          if (isConnected()) {
            window.chrome.webview.postMessage({ type: 'copy' });
          } else {
            await copyWithWebApi();
          }
        });
      }

      document.addEventListener('keydown', async (ev) => {
        if ((ev.ctrlKey || ev.metaKey) && ev.key.toLowerCase() === 'c') {
          ev.preventDefault();
          if (isConnected()) {
            window.chrome.webview.postMessage({ type: 'copy' });
          } else {
            await copyWithWebApi();
          }
        }
      });

      updateRefreshState();
      updateSaveState();
      updateCopyState();
      renderDiagram();
    })();
  </script>
</body>
</html>
)HTML4";

    std::wstring html(kWebShellPart1);
    html.append(kWebShellPart2);
    html.append(kWebShellPart3);
    html.append(kWebShellPart4);

    ReplaceAll(html, L"{{FORMAT}}", preferSvg ? L"svg" : L"png");
    ReplaceAll(html, L"{{SOURCE_NAME}}", safeSourceName);
    ReplaceAll(html, L"{{PLANTUML_ENCODER}}", PlantumlEncoderScript());
    ReplaceAll(html, L"{{PLANTUML_SOURCE}}", escaped);

    outHtml.swap(html);
    if (outErrorMessage) {
        *outErrorMessage = std::wstring();
    }
    return true;
}

struct RenderPipelineResult {
    bool success = false;
    RenderBackend backend = RenderBackend::Java;
    std::wstring html;
    std::wstring svg;
    std::vector<unsigned char> png;
    std::wstring errorMessage;
};

static RenderPipelineResult ExecuteRenderBackend(RenderBackend backend,
                                                 const std::wstring& text,
                                                 const std::wstring& sourcePath,
                                                 bool preferSvg) {
    RenderPipelineResult result;
    result.backend = backend;

    if (backend == RenderBackend::Java) {
        std::wstring html;
        std::wstring svg;
        std::vector<unsigned char> png;
        std::wstring error;
        if (BuildHtmlFromJavaRender(text, preferSvg, html, &svg, &png, &error)) {
            result.success = true;
            result.html = std::move(html);
            result.svg = std::move(svg);
            result.png = std::move(png);
            return result;
        }
        result.errorMessage = !error.empty() ? error : std::wstring(L"Local Java rendering failed.");
        return result;
    }

    if (backend == RenderBackend::Web) {
        std::wstring html;
        std::wstring error;
        if (BuildHtmlFromWebRender(text, sourcePath, preferSvg, html, &error)) {
            result.success = true;
            result.html = std::move(html);
            if (!error.empty()) {
                result.errorMessage = error;
            }
            return result;
        }
        result.errorMessage = !error.empty() ? error : std::wstring(L"PlantUML web rendering failed.");
        return result;
    }

    result.errorMessage = std::wstring(L"Unsupported renderer selected.");
    return result;
}

// ---------------------- WebView host ----------------------
static const wchar_t* kWndClass = L"PumlWebViewHost";

struct Host {
    std::atomic<long> refs{1};
    std::atomic<bool> closing{false};

    std::mutex stateMutex;

    HWND hwnd = nullptr;
    HINSTANCE hInst = nullptr;
    HMODULE   hWvLoader = nullptr;

    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller>  ctrl;
    ComPtr<ICoreWebView2>            web;
    EventRegistrationToken           navCompletedToken{};
    EventRegistrationToken           webMessageToken{};
    bool                             navCompletedRegistered = false;
    bool                             webMessageRegistered   = false;

    std::wstring initialHtml; // what we will NavigateToString()
    std::wstring sourceFilePath;
    std::wstring lastSvg;
    std::vector<unsigned char> lastPng;
    bool lastPreferSvg = true;
    bool hasRender = false;
    RenderBackend configuredRenderer = RenderBackend::Java;
    RenderBackend activeRenderer = RenderBackend::Java;
    std::wstring firstErrorMessage;
};

static void HostNavigateToInitialHtml(Host* host) {
    if (!host || !host->web) return;
    std::wstring html;
    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        html = host->initialHtml;
    }
    if (!html.empty()) {
        AppendLog(L"HostNavigateToInitialHtml: navigating with HTML length=" + std::to_wstring(html.size()));
        host->web->NavigateToString(html.c_str());
    }
}

static void HostAddRef(Host* host) {
    if (host) host->refs.fetch_add(1, std::memory_order_relaxed);
}

static void HostRelease(Host* host) {
    if (!host) return;
    if (host->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete host;
    }
}

static bool HostRenderAndReload(Host* host,
                                bool preferSvg,
                                const std::wstring& logContext,
                                const std::wstring& failureDialogMessage,
                                bool showDialogOnFailure) {
    if (!host) {
        return false;
    }

    std::wstring sourcePath;
    RenderBackend renderer = RenderBackend::Java;
    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        sourcePath = host->sourceFilePath;
        renderer = host->configuredRenderer;
    }

    if (sourcePath.empty()) {
        AppendLog(logContext + L": no source path recorded");
        {
            std::lock_guard<std::mutex> lock(host->stateMutex);
            host->lastPreferSvg = preferSvg;
        }
        if (showDialogOnFailure && host->hwnd) {
            MessageBoxW(host->hwnd,
                        L"Unable to render because the original file path is unknown.",
                        L"PlantUML Viewer",
                        MB_OK | MB_ICONERROR);
        }
        return false;
    }

    AppendLog(logContext + L": reloading file " + sourcePath);
    const std::wstring text = ReadFileUtf16OrAnsi(sourcePath.c_str());
    AppendLog(logContext + L": file characters=" + std::to_wstring(text.size()));

    RenderPipelineResult renderResult = ExecuteRenderBackend(renderer,
                                                             text,
                                                             sourcePath,
                                                             preferSvg);

    std::wstring htmlToNavigate;

    if (renderResult.success) {
        std::wstringstream os;
        os << logContext << L": render succeeded via " << RenderBackendName(renderResult.backend);
        AppendLog(os.str());
        {
            std::lock_guard<std::mutex> lock(host->stateMutex);
            host->configuredRenderer = renderer;
            host->initialHtml = renderResult.html;
            host->lastPreferSvg = preferSvg;
            host->activeRenderer = renderResult.backend;
            host->firstErrorMessage.clear();
            if (renderResult.backend == RenderBackend::Java) {
                host->lastSvg = renderResult.svg;
                host->lastPng = renderResult.png;
                host->hasRender = preferSvg ? !host->lastSvg.empty() : !host->lastPng.empty();
            } else {
                host->lastSvg.clear();
                host->lastPng.clear();
                host->hasRender = false;
            }
            htmlToNavigate = host->initialHtml;
        }
    } else {
        std::wstring dialogMessage = failureDialogMessage.empty()
            ? std::wstring(L"Unable to render the diagram. Check the log for details.")
            : failureDialogMessage;
        if (!renderResult.errorMessage.empty()) {
            dialogMessage = renderResult.errorMessage;
        }
        AppendLog(logContext + L": render failed -> " + dialogMessage);
        {
            std::lock_guard<std::mutex> lock(host->stateMutex);
            host->initialHtml = BuildErrorHtml(dialogMessage, preferSvg);
            host->lastSvg.clear();
            host->lastPng.clear();
            host->lastPreferSvg = preferSvg;
            host->hasRender = false;
            host->activeRenderer = renderer;
            host->configuredRenderer = renderer;
            host->firstErrorMessage = dialogMessage;
            htmlToNavigate = host->initialHtml;
        }
        if (showDialogOnFailure && host->hwnd) {
            MessageBoxW(host->hwnd, dialogMessage.c_str(), L"PlantUML Viewer", MB_OK | MB_ICONERROR);
        }
    }

    if (host->web && !htmlToNavigate.empty()) {
        host->web->NavigateToString(htmlToNavigate.c_str());
    }

    return renderResult.success;
}

static void HostHandleSaveAs(Host* host) {
    if (!host) return;

    std::wstring svgCopy;
    std::vector<unsigned char> pngCopy;
    std::wstring sourcePath;
    bool preferSvg = true;
    bool hasRender = false;
    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        hasRender = host->hasRender;
        preferSvg = host->lastPreferSvg;
        svgCopy = host->lastSvg;
        pngCopy = host->lastPng;
        sourcePath = host->sourceFilePath;
    }

    if (!hasRender) {
        AppendLog(L"HostHandleSaveAs: no render available");
        MessageBoxW(host->hwnd, L"There is no rendered diagram available to save.", L"PlantUML Viewer", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const std::wstring defaultExt = preferSvg ? L"svg" : L"png";
    std::wstring suggestedName = L"diagram." + defaultExt;
    if (!sourcePath.empty()) {
        const wchar_t* fileName = PathFindFileNameW(sourcePath.c_str());
        if (fileName && *fileName) {
            suggestedName.assign(fileName);
            size_t dot = suggestedName.find_last_of(L'.');
            if (dot != std::wstring::npos) {
                suggestedName.erase(dot);
            }
            suggestedName += L"." + defaultExt;
        }
    }

    std::wstring fileBuf(MAX_PATH, L'\0');
    lstrcpynW(fileBuf.data(), suggestedName.c_str(), static_cast<int>(fileBuf.size()));

    std::wstring filterSvg = L"Scalable Vector Graphics (*.svg)\0*.svg\0All Files (*.*)\0*.*\0\0";
    std::wstring filterPng = L"Portable Network Graphics (*.png)\0*.png\0All Files (*.*)\0*.*\0\0";

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = host->hwnd;
    ofn.lpstrFile = fileBuf.data();
    ofn.nMaxFile = static_cast<DWORD>(fileBuf.size());
    ofn.lpstrFilter = preferSvg ? filterSvg.c_str() : filterPng.c_str();
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = defaultExt.c_str();
    ofn.lpstrTitle = L"Save PlantUML Output";

    if (!GetSaveFileNameW(&ofn)) {
        DWORD dlgErr = CommDlgExtendedError();
        if (dlgErr != 0) {
            AppendLog(L"HostHandleSaveAs: GetSaveFileNameW failed (CommDlgExtendedError=" + std::to_wstring(dlgErr) + L")");
            MessageBoxW(host->hwnd, L"Unable to open the save dialog.", L"PlantUML Viewer", MB_OK | MB_ICONERROR);
        } else {
            AppendLog(L"HostHandleSaveAs: user cancelled save dialog");
        }
        return;
    }

    std::wstring savePath(ofn.lpstrFile);
    bool success = false;
    if (preferSvg) {
        std::string utf8 = ToUtf8(svgCopy);
        if (!svgCopy.empty() && utf8.empty()) {
            AppendLog(L"HostHandleSaveAs: failed to encode SVG as UTF-8");
            success = false;
        } else {
            success = WriteBufferToFile(savePath, utf8.data(), utf8.size());
        }
    } else {
        success = WriteBufferToFile(savePath, pngCopy.data(), pngCopy.size());
    }

    if (!success) {
        MessageBoxW(host->hwnd, L"Failed to save the file.", L"PlantUML Viewer", MB_OK | MB_ICONERROR);
        return;
    }

    AppendLog(L"HostHandleSaveAs: saved diagram to " + savePath);
}

static void HostHandleRefresh(Host* host) {
    if (!host) return;

    bool preferSvg = true;
    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        preferSvg = host->lastPreferSvg;
    }

    HostRenderAndReload(host,
                        preferSvg,
                        L"HostHandleRefresh",
                        L"Unable to refresh the diagram. Check the log for details.",
                        true);
}

static void HostHandleFormatChange(Host* host, bool preferSvg) {
    if (!host) return;

    RenderBackend backend = RenderBackend::Java;
    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        backend = host->activeRenderer;
        host->lastPreferSvg = preferSvg;
        if (backend == RenderBackend::Web) {
            host->hasRender = false;
        }
    }

    if (backend == RenderBackend::Web) {
        AppendLog(L"HostHandleFormatChange: updated preferred format to " + std::wstring(preferSvg ? L"SVG" : L"PNG") + L" (web renderer)");
        return;
    }

    const std::wstring formatLabel = preferSvg ? L"svg" : L"png";
    const std::wstring logContext = std::wstring(L"HostHandleFormatChange(") + formatLabel + L")";
    const std::wstring errorMessage = preferSvg
        ? std::wstring(L"Unable to render the diagram as SVG. Check the log for details.")
        : std::wstring(L"Unable to render the diagram as PNG. Check the log for details.");

    HostRenderAndReload(host,
                        preferSvg,
                        logContext,
                        errorMessage,
                        true);
}

static void HostHandleRenderUpdate(Host* host,
                                   const std::wstring& format,
                                   const std::wstring& svgBase64,
                                   const std::wstring& pngBase64) {
    if (!host) {
        return;
    }

    std::vector<unsigned char> svgBytes = Base64Decode(svgBase64);
    const size_t svgByteCount = svgBytes.size();
    std::wstring svgText;
    if (!svgBytes.empty()) {
        std::string utf8(svgBytes.begin(), svgBytes.end());
        svgText = FromUtf8(utf8);
    }

    std::vector<unsigned char> pngBytes = Base64Decode(pngBase64);
    const size_t pngByteCount = pngBytes.size();

    const std::wstring loweredFormat = ToLowerTrim(format);
    const bool preferSvg = loweredFormat.empty() ? true : (loweredFormat != L"png");
    const bool hasRenderable = !svgText.empty() || !pngBytes.empty();

    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        host->lastSvg = std::move(svgText);
        host->lastPng = std::move(pngBytes);
        host->lastPreferSvg = preferSvg;
        host->hasRender = hasRenderable;
        if (hasRenderable) {
            host->firstErrorMessage.clear();
        }
    }

    std::wstringstream log;
    log << L"HostHandleRenderUpdate: received render payload (svgBytes="
        << static_cast<unsigned long long>(svgByteCount)
        << L", pngBytes=" << static_cast<unsigned long long>(pngByteCount)
        << L", preferSvg=" << (preferSvg ? L"true" : L"false") << L")";
    AppendLog(log.str());
}

static void HostHandleRenderFailure(Host* host, const std::wstring& message) {
    if (!host) {
        return;
    }

    bool preferSvg = true;
    std::wstring preservedError;
    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        preferSvg = host->lastPreferSvg;
        if (host->firstErrorMessage.empty() && !message.empty()) {
            host->firstErrorMessage = message;
        }
        preservedError = host->firstErrorMessage;
    }

    AppendLog(L"HostHandleRenderFailure: message='" + message + L"'");

    std::wstring finalMessage = preservedError.empty() ? message : preservedError;
    if (finalMessage.empty()) {
        finalMessage = L"Unable to render the diagram. Check the log for details.";
    }

    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        host->initialHtml = BuildErrorHtml(finalMessage, host->lastPreferSvg);
        host->lastSvg.clear();
        host->lastPng.clear();
        host->hasRender = false;
        host->firstErrorMessage = finalMessage;
        host->activeRenderer = host->configuredRenderer;
    }

    if (host->web && !host->initialHtml.empty()) {
        host->web->NavigateToString(host->initialHtml.c_str());
    }
}

static void HostHandleCopy(Host* host) {
    if (!host) {
        return;
    }

    std::wstring svgCopy;
    std::vector<unsigned char> pngCopy;
    bool preferSvg = true;
    bool hasRender = false;
    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        hasRender = host->hasRender;
        preferSvg = host->lastPreferSvg;
        svgCopy = host->lastSvg;
        pngCopy = host->lastPng;
    }

    if (!hasRender) {
        AppendLog(L"HostHandleCopy: no render available");
        MessageBoxW(host->hwnd,
                    L"There is no rendered diagram available to copy.",
                    L"PlantUML Viewer",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (!OpenClipboard(host->hwnd)) {
        AppendLog(L"HostHandleCopy: OpenClipboard failed with error " + std::to_wstring(GetLastError()));
        MessageBoxW(host->hwnd,
                    L"Unable to access the clipboard.",
                    L"PlantUML Viewer",
                    MB_OK | MB_ICONERROR);
        return;
    }

    bool emptied = EmptyClipboard() != FALSE;
    if (!emptied) {
        AppendLog(L"HostHandleCopy: EmptyClipboard failed with error " + std::to_wstring(GetLastError()));
        CloseClipboard();
        MessageBoxW(host->hwnd,
                    L"Unable to clear the clipboard.",
                    L"PlantUML Viewer",
                    MB_OK | MB_ICONERROR);
        return;
    }

    bool success = false;

    if (preferSvg) {
        if (!svgCopy.empty()) {
            success = ClipboardSetUnicodeText(svgCopy);
            if (!success) {
                AppendLog(L"HostHandleCopy: failed to place SVG text on the clipboard");
            }
        } else {
            AppendLog(L"HostHandleCopy: SVG buffer is empty");
        }
    } else {
        if (!pngCopy.empty()) {
            std::vector<unsigned char> dib;
            bool dibOk = false;
            if (CreateDibFromPng(pngCopy, dib)) {
                dibOk = ClipboardSetBinaryData(CF_DIB, dib.data(), dib.size());
                if (!dibOk) {
                    AppendLog(L"HostHandleCopy: failed to place CF_DIB bitmap on the clipboard");
                }
            } else {
                AppendLog(L"HostHandleCopy: failed to convert PNG to DIB");
            }
            UINT pngFormat = RegisterClipboardFormatW(L"PNG");
            bool pngOk = false;
            if (pngFormat != 0) {
                pngOk = ClipboardSetBinaryData(pngFormat, pngCopy.data(), pngCopy.size());
                if (!pngOk) {
                    AppendLog(L"HostHandleCopy: failed to place PNG data on the clipboard");
                }
            } else {
                AppendLog(L"HostHandleCopy: RegisterClipboardFormatW(PNG) failed");
            }
            success = dibOk || pngOk;
        } else {
            AppendLog(L"HostHandleCopy: PNG buffer is empty");
        }
    }

    CloseClipboard();

    if (!success) {
        MessageBoxW(host->hwnd,
                    L"Failed to copy the diagram to the clipboard.",
                    L"PlantUML Viewer",
                    MB_OK | MB_ICONERROR);
    } else {
        AppendLog(L"HostHandleCopy: copied diagram as " + std::wstring(preferSvg ? L"SVG" : L"PNG"));
    }
}

typedef HRESULT (STDAPICALLTYPE *PFN_CreateCoreWebView2EnvironmentWithOptions)(
    PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

static LRESULT CALLBACK HostWndProc(HWND h, UINT m, WPARAM w, LPARAM l){
    if(m==WM_SIZE){
        auto* host = reinterpret_cast<Host*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if(host && host->ctrl){
            RECT rc; GetClientRect(h, &rc);
            host->ctrl->put_Bounds(rc);
        }
    }
    if(m==WM_NCDESTROY){
        auto* host = reinterpret_cast<Host*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if(host){
            host->closing.store(true, std::memory_order_release);
            host->hwnd = nullptr;
            if (host->web && host->navCompletedRegistered) {
                host->web->remove_NavigationCompleted(host->navCompletedToken);
                host->navCompletedRegistered = false;
                HostRelease(host);
            }
            if (host->web && host->webMessageRegistered) {
                host->web->remove_WebMessageReceived(host->webMessageToken);
                host->webMessageRegistered = false;
                HostRelease(host);
            }
            if(host->ctrl) host->ctrl->Close();
            if(host->hWvLoader) FreeLibrary(host->hWvLoader);
            host->ctrl.Reset();
            host->web.Reset();
            host->env.Reset();
            HostRelease(host);
        }
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(h, m, w, l);
}

static void EnsureWndClass(){
    static bool inited = false;
    if(inited) return;
    WNDCLASSW wc{}; wc.lpfnWndProc = HostWndProc;
    wc.hInstance   = GetModuleHandleW(nullptr);
    wc.hCursor     = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWndClass;
    RegisterClassW(&wc);
    inited = true;
}

static void InitWebView(struct Host* host){
    AppendLog(L"InitWebView: loading WebView2Loader.dll");
    std::wstring loaderPath = GetModuleDir() + L"\\WebView2Loader.dll";
    host->hWvLoader = LoadLibraryW(loaderPath.c_str());
    if(!host->hWvLoader){
        AppendLog(L"InitWebView: WebView2Loader.dll not found at " + loaderPath +
                  L" (error=" + std::to_wstring(GetLastError()) + L")");
        host->hWvLoader = LoadLibraryW(L"WebView2Loader.dll");
    }
    if(!host->hWvLoader){
        AppendLog(L"InitWebView: WebView2Loader.dll load failed");
        CreateWindowW(L"STATIC", L"WebView2 Runtime not found. Install Edge WebView2 Runtime.", WS_CHILD|WS_VISIBLE|SS_CENTER,
                      0,0,0,0, host->hwnd, nullptr, host->hInst, nullptr);
        return;
    }
    auto fn = reinterpret_cast<PFN_CreateCoreWebView2EnvironmentWithOptions>(
        GetProcAddress(host->hWvLoader, "CreateCoreWebView2EnvironmentWithOptions"));
    if(!fn){
        AppendLog(L"InitWebView: CreateCoreWebView2EnvironmentWithOptions entry not found");
        CreateWindowW(L"STATIC", L"WebView2 loader entry not found.", WS_CHILD|WS_VISIBLE|SS_CENTER,
                      0,0,0,0, host->hwnd, nullptr, host->hInst, nullptr);
        return;
    }

    AppendLog(L"InitWebView: creating environment");
    HostAddRef(host);
    auto envCompleted = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [host](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            std::unique_ptr<Host, decltype(&HostRelease)> guard(host, &HostRelease);
            if(!host || host->closing.load(std::memory_order_acquire)){
                AppendLog(L"InitWebView: host closing before environment callback");
                return S_OK;
            }
            if(FAILED(hr) || !env){
                AppendLog(L"InitWebView: environment creation failed with HRESULT=" + std::to_wstring(hr));
                return S_OK;
            }
            AppendLog(L"InitWebView: environment ready");
            host->env = env;

            HostAddRef(host);
            auto controllerCompleted = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [host](HRESULT hrCtrl, ICoreWebView2Controller* ctrl) -> HRESULT {
                    std::unique_ptr<Host, decltype(&HostRelease)> guard(host, &HostRelease);
                    if(!host || host->closing.load(std::memory_order_acquire)){
                        AppendLog(L"InitWebView: host closing before controller callback");
                        return S_OK;
                    }
                    if(FAILED(hrCtrl) || !ctrl){
                        AppendLog(L"InitWebView: controller creation failed with HRESULT=" + std::to_wstring(hrCtrl));
                        return S_OK;
                    }
                    AppendLog(L"InitWebView: controller ready");
                    host->ctrl = ctrl;
                    host->ctrl->get_CoreWebView2(&host->web);
                    if (!host->web) {
                        AppendLog(L"InitWebView: failed to get CoreWebView2 interface");
                        return S_OK;
                    }
                    AppendLog(L"InitWebView: CoreWebView2 obtained");
                    if(!host->hwnd){
                        AppendLog(L"InitWebView: window destroyed before bounds update");
                        return S_OK;
                    }
                    RECT rc; GetClientRect(host->hwnd, &rc);
                    host->ctrl->put_Bounds(rc);

                    HostAddRef(host);
                    auto webMessageHandler = Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [host](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            HostAddRef(host);
                            std::unique_ptr<Host, decltype(&HostRelease)> guard(host, &HostRelease);
                            if (!host || host->closing.load(std::memory_order_acquire)) {
                                return S_OK;
                            }
                            if (!args) {
                                return S_OK;
                            }

                            LPWSTR rawJson = nullptr;
                            HRESULT hrJson = args->get_WebMessageAsJson(&rawJson);
                            if (SUCCEEDED(hrJson) && rawJson) {
                                std::wstring json(rawJson);
                                CoTaskMemFree(rawJson);

                                const std::wstring type = ToLowerTrim(ExtractJsonStringField(json, L"type"));
                                if (type == L"saveas") {
                                    HostHandleSaveAs(host);
                                } else if (type == L"refresh") {
                                    HostHandleRefresh(host);
                                } else if (type == L"setformat") {
                                    std::wstring format = ToLowerTrim(ExtractJsonStringField(json, L"format"));
                                    const bool preferSvg = format != L"png";
                                    HostHandleFormatChange(host, preferSvg);
                                } else if (type == L"copy") {
                                    HostHandleCopy(host);
                                } else if (type == L"rendered") {
                                    std::wstring format = ExtractJsonStringField(json, L"format");
                                    std::wstring svgB64 = ExtractJsonStringField(json, L"svgBase64");
                                    std::wstring pngB64 = ExtractJsonStringField(json, L"pngBase64");
                                    HostHandleRenderUpdate(host, format, svgB64, pngB64);
                                } else if (type == L"renderfailed") {
                                    std::wstring message = ExtractJsonStringField(json, L"message");
                                    HostHandleRenderFailure(host, message);
                                }
                            } else if (rawJson) {
                                CoTaskMemFree(rawJson);
                            }
                            return S_OK;
                        });
                    EventRegistrationToken msgToken{};
                    HRESULT hrMsg = host->web->add_WebMessageReceived(webMessageHandler.Get(), &msgToken);
                    if (SUCCEEDED(hrMsg)) {
                        host->webMessageToken = msgToken;
                        host->webMessageRegistered = true;
                    } else {
                        AppendLog(L"InitWebView: add_WebMessageReceived failed with HRESULT=" + std::to_wstring(hrMsg));
                        HostRelease(host);
                    }

                    HostAddRef(host);
                    auto navCompletedHandler = Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [host](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                            HostAddRef(host);
                            std::unique_ptr<Host, decltype(&HostRelease)> guard(host, &HostRelease);
                            if(!host || host->closing.load(std::memory_order_acquire)){
                                return S_OK;
                            }
                            UINT64 navId = 0;
                            if (args) args->get_NavigationId(&navId);
                            BOOL isSuccess = FALSE;
                            if (args) args->get_IsSuccess(&isSuccess);
                            COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                            if (args) args->get_WebErrorStatus(&status);
                            std::wstringstream os;
                            os << L"InitWebView: NavigationCompleted id=" << navId
                               << L", success=" << (isSuccess ? L"true" : L"false")
                               << L", webErrorStatus=" << static_cast<int>(status);
                            AppendLog(os.str());
                            return S_OK;
                        });
                    EventRegistrationToken navToken{};
                    HRESULT hrNav = host->web->add_NavigationCompleted(navCompletedHandler.Get(), &navToken);
                    if (SUCCEEDED(hrNav)) {
                        host->navCompletedToken = navToken;
                        host->navCompletedRegistered = true;
                    } else {
                        AppendLog(L"InitWebView: add_NavigationCompleted failed with HRESULT=" + std::to_wstring(hrNav));
                        HostRelease(host);
                    }

                    {
                        std::lock_guard<std::mutex> lock(host->stateMutex);
                        if (!host->initialHtml.empty()){
                            AppendLog(L"InitWebView: navigating to initial HTML (" + std::to_wstring(host->initialHtml.size()) + L" chars)");
                        }
                    }
                    HostNavigateToInitialHtml(host);
                    return S_OK;
                });

            HRESULT hrCtrl = env->CreateCoreWebView2Controller(host->hwnd, controllerCompleted.Get());
            if(FAILED(hrCtrl)){
                AppendLog(L"InitWebView: CreateCoreWebView2Controller call failed with HRESULT=" + std::to_wstring(hrCtrl));
                HostRelease(host);
            }
            return S_OK;
        });

    HRESULT hrEnv = fn(nullptr, nullptr, nullptr, envCompleted.Get());
    if(FAILED(hrEnv)){
        AppendLog(L"InitWebView: CreateCoreWebView2EnvironmentWithOptions call failed with HRESULT=" + std::to_wstring(hrEnv));
        HostRelease(host);
    }
}


// ---------------------- WLX exports ----------------------
extern "C" {

__declspec(dllexport) int __stdcall ListGetDetectString(char* DetectString, int maxlen) {
    LoadConfigIfNeeded();
    lstrcpynA(DetectString, g_detectA.c_str(), maxlen);
    return 0;
}

__declspec(dllexport) HWND __stdcall ListLoadW(HWND ParentWin, wchar_t* FileToLoad, int /*ShowFlags*/) {
    LoadConfigIfNeeded();
    AppendLog(L"ListLoadW: start for file " + std::wstring(FileToLoad ? FileToLoad : L"<null>"));
    EnsureWndClass();

    auto* host = new Host();
    host->hInst = GetModuleHandleW(nullptr);
    host->hwnd  = CreateWindowExW(0, kWndClass, L"",
                                  WS_CHILD|WS_VISIBLE, 0,0,0,0,
                                  ParentWin, nullptr, host->hInst, nullptr);
    if(!host->hwnd){
        AppendLog(L"ListLoadW: CreateWindowExW failed with error " + std::to_wstring(GetLastError()));
        HostRelease(host);
        return nullptr;
    }
    SetWindowLongPtrW(host->hwnd, GWLP_USERDATA, (LONG_PTR)host);

    const bool preferSvg = (ToLowerTrim(g_prefer) == L"svg");
    AppendLog(L"ListLoadW: preferSvg=" + std::wstring(preferSvg ? L"true" : L"false"));

    RenderBackend renderer = GetConfiguredRenderer();
    AppendLog(L"ListLoadW: renderer = " + GetConfiguredRendererName());

    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        host->sourceFilePath = FileToLoad ? std::wstring(FileToLoad) : std::wstring();
        host->configuredRenderer = renderer;
        host->activeRenderer = renderer;
        host->lastPreferSvg = preferSvg;
        host->firstErrorMessage.clear();
        host->lastSvg.clear();
        host->lastPng.clear();
        host->hasRender = false;
    }

    const std::wstring failureMessage = L"Unable to render the diagram. Check the log for details.";
    HostRenderAndReload(host,
                        preferSvg,
                        L"ListLoadW",
                        failureMessage,
                        false);

    InitWebView(host);
    AppendLog(L"ListLoadW: InitWebView invoked");
    return host->hwnd;
}

__declspec(dllexport) int __stdcall ListSendCommand(HWND /*ListWin*/, int /*Command*/, int /*Parameter*/) {
    // Ctrl+C is handled by the web page JS.
    return 0;
}

__declspec(dllexport) void __stdcall ListCloseWindow(HWND ListWin) {
    AppendLog(L"ListCloseWindow: destroying window");
    DestroyWindow(ListWin);
}

} // extern "C"
