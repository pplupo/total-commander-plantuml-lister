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
static std::wstring g_rendererPipeline = L"java";        // e.g. "java", "web", "web,java"
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
    if (GetPrivateProfileStringW(L"render", L"pipeline", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) g_rendererPipeline = buf;

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
        << L", pipeline=" << g_rendererPipeline
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

static std::vector<RenderBackend> ParseRendererPipeline(const std::wstring& pipelineText) {
    std::vector<RenderBackend> pipeline;
    std::wstring text = pipelineText;
    if (text.empty()) {
        text = L"java";
    }

    size_t start = 0;
    while (start <= text.size()) {
        size_t comma = text.find(L',', start);
        std::wstring token = (comma == std::wstring::npos)
            ? text.substr(start)
            : text.substr(start, comma - start);
        token = ToLowerTrim(token);
        if (token == L"java") {
            pipeline.push_back(RenderBackend::Java);
        } else if (token == L"web") {
            pipeline.push_back(RenderBackend::Web);
        }
        if (comma == std::wstring::npos) {
            break;
        }
        start = comma + 1;
    }

    if (pipeline.empty()) {
        pipeline.push_back(RenderBackend::Java);
    }
    return pipeline;
}

static std::wstring JoinRendererPipeline(const std::vector<RenderBackend>& pipeline) {
    if (pipeline.empty()) {
        return L"";
    }
    std::wstring joined;
    for (size_t i = 0; i < pipeline.size(); ++i) {
        if (i > 0) joined += L",";
        joined += RenderBackendName(pipeline[i]);
    }
    return joined;
}

static std::vector<RenderBackend> GetRendererPipelineVector() {
    return ParseRendererPipeline(g_rendererPipeline);
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
    for (wchar_t wc : in) {
        int decoded = Base64DecodeChar(wc);
        if (decoded < 0) {
            if (decoded == -1) {
                if (bitsCollected == 18) {
                    out.push_back(static_cast<unsigned char>((buffer >> 16) & 0xFF));
                } else if (bitsCollected == 12) {
                    out.push_back(static_cast<unsigned char>((buffer >> 16) & 0xFF));
                    out.push_back(static_cast<unsigned char>((buffer >> 8) & 0xFF));
                }
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
  <script src="https://cdn.jsdelivr.net/npm/plantuml-encoder@1.2.5/dist/plantuml-encoder.min.js"></script>
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
)HTML1";

    static const wchar_t kWebShellPart2[] = LR"HTML2(
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

)HTML2";

    static const wchar_t kWebShellPart3[] = LR"HTML3(
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
)HTML3";

    std::wstring html(kWebShellPart1);
    html.append(kWebShellPart2);
    html.append(kWebShellPart3);

    ReplaceAll(html, L"{{FORMAT}}", preferSvg ? L"svg" : L"png");
    ReplaceAll(html, L"{{SOURCE_NAME}}", safeSourceName);
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
    size_t backendIndex = 0;
    std::wstring html;
    std::wstring svg;
    std::vector<unsigned char> png;
    std::wstring errorMessage;
};

static RenderPipelineResult ExecuteRenderPipeline(const std::vector<RenderBackend>& pipeline,
                                                  size_t startIndex,
                                                  const std::wstring& text,
                                                  const std::wstring& sourcePath,
                                                  bool preferSvg,
                                                  std::wstring& firstErrorMessage) {
    RenderPipelineResult result;
    if (pipeline.empty()) {
        std::vector<RenderBackend> defaultPipeline = { RenderBackend::Java };
        return ExecuteRenderPipeline(defaultPipeline, 0, text, sourcePath, preferSvg, firstErrorMessage);
    }

    for (size_t idx = startIndex; idx < pipeline.size(); ++idx) {
        const RenderBackend backend = pipeline[idx];
        if (backend == RenderBackend::Java) {
            std::wstring html;
            std::wstring svg;
            std::vector<unsigned char> png;
            std::wstring error;
            if (BuildHtmlFromJavaRender(text, preferSvg, html, &svg, &png, &error)) {
                result.success = true;
                result.backend = backend;
                result.backendIndex = idx;
                result.html = std::move(html);
                result.svg = std::move(svg);
                result.png = std::move(png);
                return result;
            }
            if (firstErrorMessage.empty()) {
                firstErrorMessage = !error.empty() ? error : std::wstring(L"Local Java rendering failed.");
            }
        } else if (backend == RenderBackend::Web) {
            std::wstring html;
            std::wstring error;
            if (BuildHtmlFromWebRender(text, sourcePath, preferSvg, html, &error)) {
                result.success = true;
                result.backend = backend;
                result.backendIndex = idx;
                result.html = std::move(html);
                if (firstErrorMessage.empty() && !error.empty()) {
                    firstErrorMessage = error;
                }
                return result;
            }
            if (firstErrorMessage.empty()) {
                firstErrorMessage = !error.empty() ? error : std::wstring(L"PlantUML web rendering failed.");
            }
        }
    }

    result.success = false;
    result.backendIndex = std::min(startIndex, pipeline.size() - 1);
    result.backend = pipeline[result.backendIndex];
    result.errorMessage = firstErrorMessage.empty()
        ? std::wstring(L"Unable to render the diagram. Check the log for details.")
        : firstErrorMessage;
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
    std::vector<RenderBackend> pipeline;
    size_t activeRendererIndex = 0;
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
                                bool showDialogOnFailure,
                                size_t startIndex = 0,
                                const std::wstring& preservedErrorMessage = std::wstring()) {
    if (!host) {
        return false;
    }

    std::wstring sourcePath;
    std::vector<RenderBackend> pipeline;
    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        sourcePath = host->sourceFilePath;
        pipeline = host->pipeline;
    }

    if (pipeline.empty()) {
        pipeline = GetRendererPipelineVector();
    }
    if (pipeline.empty()) {
        pipeline.push_back(RenderBackend::Java);
    }
    if (startIndex >= pipeline.size()) {
        startIndex = pipeline.size() - 1;
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

    std::wstring firstError = preservedErrorMessage;
    RenderPipelineResult renderResult = ExecuteRenderPipeline(pipeline,
                                                              startIndex,
                                                              text,
                                                              sourcePath,
                                                              preferSvg,
                                                              firstError);

    std::wstring htmlToNavigate;

    if (renderResult.success) {
        std::wstringstream os;
        os << logContext << L": render succeeded via " << RenderBackendName(renderResult.backend)
           << L" (index=" << static_cast<unsigned long long>(renderResult.backendIndex) << L")";
        AppendLog(os.str());
        {
            std::lock_guard<std::mutex> lock(host->stateMutex);
            host->pipeline = pipeline;
            host->initialHtml = renderResult.html;
            host->lastPreferSvg = preferSvg;
            host->activeRenderer = renderResult.backend;
            host->activeRendererIndex = renderResult.backendIndex;
            if (renderResult.backend == RenderBackend::Java) {
                host->firstErrorMessage.clear();
                host->lastSvg = renderResult.svg;
                host->lastPng = renderResult.png;
                host->hasRender = preferSvg ? !host->lastSvg.empty() : !host->lastPng.empty();
            } else {
                if (firstError.empty()) {
                    host->firstErrorMessage.clear();
                } else {
                    host->firstErrorMessage = firstError;
                }
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
        if (!firstError.empty()) {
            dialogMessage = firstError;
        }
        AppendLog(logContext + L": render failed -> " + dialogMessage);
        {
            std::lock_guard<std::mutex> lock(host->stateMutex);
            host->pipeline = pipeline;
            host->initialHtml = BuildErrorHtml(dialogMessage, preferSvg);
            host->lastSvg.clear();
            host->lastPng.clear();
            host->lastPreferSvg = preferSvg;
            host->hasRender = false;
            host->activeRendererIndex = std::min(startIndex, pipeline.size() - 1);
            host->activeRenderer = pipeline[host->activeRendererIndex];
            host->firstErrorMessage = firstError.empty() ? dialogMessage : firstError;
            htmlToNavigate = host->initialHtml;
        }
        if (showDialogOnFailure && host->hwnd) {
            MessageBoxW(host->hwnd, dialogMessage.c_str(), L"PlantUML Viewer", MB_OK | MB_ICONERROR);
        }
    }

    if (host->web && !htmlToNavigate.empty()) {
        host->web->NavigateToString(htmlToNavigate.c_str());
    }

    return true;
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

    std::vector<RenderBackend> pipeline;
    size_t nextIndex = 0;
    bool preferSvg = true;
    std::wstring preservedError;
    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        pipeline = host->pipeline;
        nextIndex = host->activeRendererIndex + 1;
        preferSvg = host->lastPreferSvg;
        if (host->firstErrorMessage.empty() && !message.empty()) {
            host->firstErrorMessage = message;
        }
        preservedError = host->firstErrorMessage;
    }

    if (pipeline.empty()) {
        pipeline = GetRendererPipelineVector();
    }

    std::wstringstream log;
    log << L"HostHandleRenderFailure: message='" << message
        << L"', nextIndex=" << static_cast<unsigned long long>(nextIndex)
        << L"/" << static_cast<unsigned long long>(pipeline.size());
    AppendLog(log.str());

    if (!pipeline.empty() && nextIndex < pipeline.size()) {
        const RenderBackend nextBackend = pipeline[nextIndex];
        AppendLog(L"HostHandleRenderFailure: attempting fallback to " + std::wstring(RenderBackendName(nextBackend)));
        const std::wstring dialogMessage = preservedError.empty() ? message : preservedError;
        HostRenderAndReload(host,
                            preferSvg,
                            L"HostHandleRenderFailure",
                            dialogMessage,
                            false,
                            nextIndex,
                            preservedError);
        return;
    }

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
        if (!pipeline.empty()) {
            size_t index = nextIndex == 0 ? 0 : std::min(nextIndex - 1, pipeline.size() - 1);
            host->activeRendererIndex = index;
            host->activeRenderer = pipeline[index];
        }
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

    std::vector<RenderBackend> pipeline = GetRendererPipelineVector();
    AppendLog(L"ListLoadW: renderer pipeline = " + JoinRendererPipeline(pipeline));

    {
        std::lock_guard<std::mutex> lock(host->stateMutex);
        host->sourceFilePath = FileToLoad ? std::wstring(FileToLoad) : std::wstring();
        host->pipeline = pipeline;
        host->activeRendererIndex = 0;
        host->activeRenderer = pipeline.empty() ? RenderBackend::Java : pipeline.front();
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
