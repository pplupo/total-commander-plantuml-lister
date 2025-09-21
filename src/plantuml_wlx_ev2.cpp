// PlantUML WebView Lister (Tiny)
// Features:
//  - Render order selectable in INI: java,web | web,java | java | web
//  - Local rendering via Java + plantuml.jar (bundled or configurable)
//  - Fallback to PlantUML server POST when requested
//  - WebView2 runtime loaded dynamically (no static import)
//
// Build: CMake -> PlantUmlWebView.wlx64

#include <windows.h>
#include <shlwapi.h>
#include <wrl.h>
#include <string>
#include <sstream>
#include <vector>
#include <memory>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cwchar>

#include "WebView2.h"

#pragma comment(lib, "shlwapi.lib")

using namespace Microsoft::WRL;

// ---------------------- Config ----------------------
static std::wstring g_serverUrl = L"https://www.plantuml.com/plantuml";
static std::wstring g_prefer    = L"svg";           // "svg" or "png"
static std::wstring g_order     = L"java,web";      // "java,web" | "web,java" | "java" | "web"
static std::string  g_detectA   = R"(EXT="PUML" | EXT="PLANTUML" | EXT="UML" | EXT="WSD" | EXT="WS" | EXT="IUML")";

static std::wstring g_jarPath;                      // If empty: auto-detect moduleDir\plantuml.jar
static std::wstring g_javaPath;                     // Optional explicit java[w].exe
static std::wstring g_logPath;                      // If empty: moduleDir\plantumlwebview.log
static DWORD        g_jarTimeoutMs = 8000;

static bool         g_cfgLoaded = false;

static std::mutex   g_logMutex;
static bool         g_logSessionStarted = false;

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
    if (g_logPath.empty()) return;
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
    if (g_logPath.empty()) {
        g_logPath = moduleDir + L"\\plantumlwebview.log";
    }
    wchar_t buf[2048];

    if (GetPrivateProfileStringW(L"server", L"url", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) g_serverUrl = buf;
    if (GetPrivateProfileStringW(L"render", L"prefer", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) g_prefer = buf;
    if (GetPrivateProfileStringW(L"render", L"order", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) g_order = buf;

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

    if (GetPrivateProfileStringW(L"debug", L"log", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) {
        g_logPath = buf;
        if (PathIsRelativeW(g_logPath.c_str())) {
            g_logPath = moduleDir + L"\\" + g_logPath;
        }
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
    cfg << L"Config loaded. server=" << g_serverUrl
        << L", prefer=" << g_prefer
        << L", order=" << g_order
        << L", jar=" << (g_jarPath.empty() ? L"<auto>" : g_jarPath)
        << L", java=" << (g_javaPath.empty() ? L"<auto>" : g_javaPath)
        << L", timeoutMs=" << g_jarTimeoutMs
        << L", log=" << g_logPath;
    AppendLog(cfg.str());
}

static std::wstring ToLowerTrim(const std::wstring& in) {
    std::wstring s = in;
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](wchar_t c){ return !iswspace(c); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [](wchar_t c){ return !iswspace(c); }).base(), s.end());
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c){ return (wchar_t)towlower(c); });
    return s;
}
static std::vector<std::wstring> SplitOrder(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t comma = s.find(L',', pos);
        std::wstring token = (comma == std::wstring::npos) ? s.substr(pos) : s.substr(pos, comma - pos);
        out.push_back(ToLowerTrim(token));
        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
    // Remove empties
    out.erase(std::remove_if(out.begin(), out.end(), [](const std::wstring& t){ return t.empty(); }), out.end());
    return out;
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
    SetHandleInformation(hInW,  HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(hOutR, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(hInR,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hOutW, HANDLE_FLAG_INHERIT, 0);

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

// Build minimal HTML wrapper with injected BODY (svg markup or <img src="data:...">)
static std::wstring BuildShellHtmlWithBody(const std::wstring& body) {
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
    body { margin: 0; background: canvas; color: CanvasText; font: 13px system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
    #root { padding: 8px; display: grid; place-items: start center; }
    img, svg { max-width: 100%; height: auto; }
    .err { padding: 12px 14px; border-radius: 10px; background: color-mix(in oklab, Canvas 85%, red 15%); }
  </style>
</head>
<body>
  <div id="root">
    {{BODY}}
  </div>
  <script>
    // Ctrl+C copies SVG or PNG
    document.addEventListener('keydown', async ev => {
      if ((ev.ctrlKey || ev.metaKey) && ev.key.toLowerCase() === 'c') {
        ev.preventDefault();
        try {
          const svg = document.querySelector('svg');
          if (svg) {
            const s = new XMLSerializer().serializeToString(svg);
            await navigator.clipboard.writeText(s);
            return;
          }
          const img = document.querySelector('img');
          if (img) {
            const c = document.createElement('canvas');
            c.width = img.naturalWidth; c.height = img.naturalHeight;
            const g = c.getContext('2d');
            g.drawImage(img, 0, 0);
            const blob = await new Promise(r => c.toBlob(r, 'image/png'));
            await navigator.clipboard.write([new ClipboardItem({ 'image/png': blob })]);
          }
        } catch (e) {}
      }
    });
  </script>
</body>
</html>)HTML";
    ReplaceAll(html, L"{{BODY}}", body);
    return html;
}

static std::wstring BuildErrorHtml(const std::wstring& message) {
    std::wstring safe = message;
    ReplaceAll(safe, L"<", L"&lt;");
    ReplaceAll(safe, L">", L"&gt;");
    return BuildShellHtmlWithBody(L"<div class='err'>"+safe+L"</div>");
}

// Build HTML that fetches from server via POST (fallback path)
static std::wstring BuildServerHtml(const std::wstring& serverUrl,
                                    bool preferSvg,
                                    const std::wstring& umlText)
{
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
    body { margin: 0; background: canvas; color: CanvasText; font: 13px system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
    #root { padding: 8px; display: grid; place-items: start center; }
    img, svg { max-width: 100%; height: auto; }
    #hud { position: fixed; top:8px; right:8px; background: color-mix(in oklab, Canvas 80%, CanvasText 20%);
           padding: 4px 10px; border-radius: 999px; font-weight: 600; }
  </style>
</head>
<body>
  <div id="hud">rendering…</div>
  <div id="root"></div>
  <script>
    (function(){
      const SERVER = "{{SERVER}}".replace(/\/+$/,'');
      const FORMAT = "{{FORMAT}}";
      const UML    = "{{DATA}}";
      const root   = document.getElementById('root');
      const hud    = document.getElementById('hud');

      const endpoint = SERVER + '/' + FORMAT;
      fetch(endpoint, {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain;charset=utf-8' },
        body: UML
      }).then(async res => {
        if (!res.ok) throw new Error('HTTP ' + res.status);
        if (FORMAT === 'svg') {
          const svg = await res.text();
          root.innerHTML = svg;
        } else {
          const blob = await res.blob();
          const url = URL.createObjectURL(blob);
          const img = new Image();
          img.onload = () => URL.revokeObjectURL(url);
          img.src = url;
          root.replaceChildren(img);
        }
        hud.textContent = 'done';
      }).catch(err => {
        root.textContent = 'Render error: ' + err.message;
        hud.textContent = 'error';
      });

      document.addEventListener('keydown', async ev => {
        if ((ev.ctrlKey || ev.metaKey) && ev.key.toLowerCase() === 'c') {
          ev.preventDefault();
          try {
            const svg = document.querySelector('svg');
            if (svg) {
              const s = new XMLSerializer().serializeToString(svg);
              await navigator.clipboard.writeText(s);
              hud.textContent = 'copied SVG';
              return;
            }
            const img = document.querySelector('img');
            if (img) {
              const c = document.createElement('canvas');
              c.width = img.naturalWidth; c.height = img.naturalHeight;
              const g = c.getContext('2d'); g.drawImage(img, 0, 0);
              const blob = await new Promise(r => c.toBlob(r, 'image/png'));
              await navigator.clipboard.write([new ClipboardItem({ 'image/png': blob })]);
              hud.textContent = 'copied PNG';
            }
          } catch(e) { hud.textContent = 'copy failed'; }
        }
      });
    })();
  </script>
</body>
</html>)HTML";

    ReplaceAll(html, L"{{SERVER}}", serverUrl);
    ReplaceAll(html, L"{{FORMAT}}", preferSvg ? std::wstring(L"svg") : std::wstring(L"png"));

    // Escape Unicode for embedding as JS string literal
    std::wstring jsEsc; jsEsc.reserve(umlText.size()+16);
    for (wchar_t ch : umlText) {
        switch (ch) {
            case L'\\': jsEsc += L"\\\\"; break;
            case L'"':  jsEsc += L"\\\""; break;
            case L'\n': jsEsc += L"\\n";  break;
            case L'\r':                break;
            case L'\t': jsEsc += L"\\t";  break;
            default:    jsEsc += ch;      break;
        }
    }
    ReplaceAll(html, L"{{DATA}}", jsEsc);
    return html;
}

// ---------------------- WebView host ----------------------
static const wchar_t* kWndClass = L"PumlWebViewHost";

struct Host {
    std::atomic<long> refs{1};
    std::atomic<bool> closing{false};

    HWND hwnd = nullptr;
    HINSTANCE hInst = nullptr;
    HMODULE   hWvLoader = nullptr;

    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller>  ctrl;
    ComPtr<ICoreWebView2>            web;

    std::wstring initialHtml; // what we will NavigateToString()
};

static void HostAddRef(Host* host) {
    if (host) host->refs.fetch_add(1, std::memory_order_relaxed);
}

static void HostRelease(Host* host) {
    if (!host) return;
    if (host->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete host;
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
    host->hWvLoader = LoadLibraryW(L"WebView2Loader.dll");
    if(!host->hWvLoader){
        AppendLog(L"InitWebView: WebView2Loader.dll not found");
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
    HRESULT hrEnv = fn(nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [host](HRESULT hr, ICoreWebView2Environment* env)->HRESULT{
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
                HRESULT hrCtrl = env->CreateCoreWebView2Controller(host->hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [host](HRESULT hr, ICoreWebView2Controller* ctrl)->HRESULT{
                            std::unique_ptr<Host, decltype(&HostRelease)> guard(host, &HostRelease);
                            if(!host || host->closing.load(std::memory_order_acquire)){
                                AppendLog(L"InitWebView: host closing before controller callback");
                                return S_OK;
                            }
                            if(FAILED(hr) || !ctrl){
                                AppendLog(L"InitWebView: controller creation failed with HRESULT=" + std::to_wstring(hr));
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
                            if (!host->initialHtml.empty()){
                                AppendLog(L"InitWebView: navigating to initial HTML (" + std::to_wstring(host->initialHtml.size()) + L" chars)");
                                host->web->NavigateToString(host->initialHtml.c_str());
                            }
                            return S_OK;
                        }).Get());
                if(FAILED(hrCtrl)){
                    AppendLog(L"InitWebView: CreateCoreWebView2Controller call failed with HRESULT=" + std::to_wstring(hrCtrl));
                    HostRelease(host);
                }
                return S_OK;
            }).Get());
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

    const std::wstring text = ReadFileUtf16OrAnsi(FileToLoad);
    AppendLog(L"ListLoadW: loaded file characters=" + std::to_wstring(text.size()));
    const bool preferSvg = (ToLowerTrim(g_prefer) == L"svg");
    AppendLog(L"ListLoadW: preferSvg=" + std::wstring(preferSvg ? L"true" : L"false"));

    // Decide render path by order
    const std::vector<std::wstring> order = SplitOrder(g_order.empty() ? L"java,web" : g_order);
    {
        std::wstringstream os;
        os << L"ListLoadW: render order=";
        for (size_t i = 0; i < order.size(); ++i) {
            if (i) os << L",";
            os << order[i];
        }
        AppendLog(os.str());
    }
    bool produced = false;
    std::wstring lastErr;

    for (const auto& step : order) {
        AppendLog(L"ListLoadW: considering renderer step '" + step + L"'");
        if (step == L"java") {
            std::wstring svgOut;
            std::vector<unsigned char> pngOut;
            AppendLog(L"ListLoadW: attempting local Java render with jar=" + (g_jarPath.empty() ? std::wstring(L"<auto>") : g_jarPath));
            if (RunPlantUmlJar(text, preferSvg, svgOut, pngOut)) {
                if (preferSvg) {
                    host->initialHtml = BuildShellHtmlWithBody(svgOut);
                    AppendLog(L"ListLoadW: local render succeeded (SVG)");
                } else {
                    const std::string b64 = Base64(pngOut);
                    std::wstring body = L"<img alt=\"diagram\" src=\"data:image/png;base64,";
                    body += FromUtf8(b64); body += L"\"/>";
                    host->initialHtml = BuildShellHtmlWithBody(body);
                    AppendLog(L"ListLoadW: local render succeeded (PNG)");
                }
                produced = true;
                break; // Success, so we're done.
            } else {
                lastErr = L"Local Java/JAR rendering failed. Check Java installation and plantuml.jar path in the INI file.";
                AppendLog(L"ListLoadW: local render failed");
                continue; // Try next step in the order.
            }
        } else if (step == L"web") {
            host->initialHtml = BuildServerHtml(g_serverUrl, preferSvg, text);
            AppendLog(L"ListLoadW: prepared web fallback HTML with server=" + g_serverUrl);
            produced = true;
            break; // We can only try, so we're done.
        } else {
            AppendLog(L"ListLoadW: ignoring unknown renderer step '" + step + L"'");
        }
        // Unrecognized steps are ignored
    }

    if (!produced) {
        if (!lastErr.empty()) {
            host->initialHtml = BuildErrorHtml(lastErr);
            AppendLog(L"ListLoadW: showing error HTML");
        } else {
            host->initialHtml = BuildErrorHtml(L"No valid renderer specified in `render.order` of INI file. Check plantumlwebview.ini.");
            AppendLog(L"ListLoadW: no renderer produced output");
        }
    }

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
