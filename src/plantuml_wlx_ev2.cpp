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
static DWORD        g_jarTimeoutMs = 8000;

static bool         g_cfgLoaded = false;

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
    if (h == INVALID_HANDLE_VALUE) return L"";
    DWORD size = GetFileSize(h, nullptr);
    std::string bytes; bytes.resize(size ? size : 0);
    DWORD read = 0;
    if (size && ReadFile(h, bytes.data(), size, &read, nullptr) && read == size) {}
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

static void LoadConfigIfNeeded() {
    if (g_cfgLoaded) return;
    g_cfgLoaded = true;

    const std::wstring ini = GetModuleDir() + L"\\plantumlwebview.ini";
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

    if (GetPrivateProfileStringW(L"plantuml", L"jar", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) g_jarPath = buf;
    if (GetPrivateProfileStringW(L"plantuml", L"java", L"", buf, 2048, ini.c_str()) > 0 && buf[0]) g_javaPath = buf;
    DWORD tmo = GetPrivateProfileIntW(L"plantuml", L"timeout_ms", 0, ini.c_str());
    if (tmo > 0) g_jarTimeoutMs = tmo;

    // If jar is not explicitly set, auto-try "plantuml.jar" next to the plugin
    if (g_jarPath.empty()) {
        const std::wstring guess = GetModuleDir() + L"\\plantuml.jar";
        if (FileExistsW(guess)) g_jarPath = guess;
    }
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
    if (SearchPathW(nullptr, L"javaw.exe", nullptr, MAX_PATH, found, nullptr)) { outPath = found; return true; }
    if (SearchPathW(nullptr, L"java.exe", nullptr, MAX_PATH, found, nullptr))  { outPath = found; return true; }
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
    if (g_jarPath.empty() || !FileExistsW(g_jarPath)) return false;

    std::wstring javaExe;
    if (!FindJavaExecutable(javaExe)) return false;

    std::wstring fmt = preferSvg ? L"-tsvg" : L"-tpng";

    SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE hInR=nullptr,  hInW=nullptr;
    HANDLE hOutR=nullptr, hOutW=nullptr;

    if (!CreatePipe(&hInR, &hInW, &sa, 0)) return false;
    if (!CreatePipe(&hOutR, &hOutW, &sa, 0)) { CloseHandle(hInR); CloseHandle(hInW); return false; }

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
        CloseHandle(hInW); CloseHandle(hOutR);
        return false;
    }

    // Write UML as UTF-8 to child stdin
    std::string umlUtf8 = ToUtf8(umlTextW);
    DWORD written = 0;
    if (!umlUtf8.empty()) {
        WriteFile(hInW, umlUtf8.data(), (DWORD)umlUtf8.size(), &written, nullptr);
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
    if (wr == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hThread);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);

    if (buffer.empty()) return false;

    if (preferSvg) {
        // interpret bytes as UTF-8 SVG
        int wlen = MultiByteToWideChar(CP_UTF8, 0,
                                       (const char*)buffer.data(), (int)buffer.size(),
                                       nullptr, 0);
        if (wlen <= 0) return false;
        std::wstring svg(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, (const char*)buffer.data(), (int)buffer.size(),
                            svg.data(), wlen);
        outSvg.swap(svg);
    } else {
        outPng.swap(buffer);
    }
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
                                    const std::wstring& umlText,
                                    const std::wstring& prevError)
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
    .err { padding: 12px 14px; border-radius: 10px; background: color-mix(in oklab, Canvas 85%, red 15%); }
  </style>
</head>
<body>
  <div id="hud">rendering…</div>
  <div id="root"></div>
  <script>
    (function(){
      const SERVER = "{{SERVER}}".replace(/\/+$/,'');
      const FORMAT = "{{FORMAT}}";
      const UML    = `{{DATA}}`;
      const PREV_ERROR = `{{PREV_ERROR}}`;
      const root   = document.getElementById('root');
      const hud    = document.getElementById('hud');

      if (PREV_ERROR) {
        const d = document.createElement('div');
        d.className = 'err';
        d.style.marginBottom = '12px';
        d.textContent = 'Local render failed: ' + PREV_ERROR + ' Falling back to server…';
        root.appendChild(d);
      }

      const endpoint = SERVER + '/' + FORMAT;
      fetch(endpoint, {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain;charset=utf-8' },
        body: UML
      }).then(async res => {
        if (!res.ok) throw new Error('HTTP ' + res.status);
        if (FORMAT === 'svg') {
          const svg = await res.text();
          // Append to root to avoid removing the potential error message div
          if (root.children.length > 0) root.insertAdjacentHTML('beforeend', svg);
          else root.innerHTML = svg;
        } else {
          const blob = await res.blob();
          const url = URL.createObjectURL(blob);
          const img = new Image();
          img.onload = () => URL.revokeObjectURL(url);
          img.src = url;
          root.appendChild(img);
        }
        hud.textContent = 'done';
      }).catch(err => {
        const d = document.createElement('div');
        d.className = 'err';
        d.textContent = 'Server render error: ' + err.message;
        root.appendChild(d);
        hud.textContent = 'error';
      });

      document.addEventListener('keydown', async ev => {
        if ((ev.ctrlKey || ev.metaKey) && ev.key.toLowerCase() === 'c') {
          ev.preventDefault();
          try {
            const svg = root.querySelector('svg');
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

    auto jsEscape = [](const std::wstring& w) {
        if (w.empty()) return std::wstring();
        std::wstring jsEsc; jsEsc.reserve(w.size() + 16);
        for (wchar_t ch : w) {
            switch (ch) {
                case L'\\': jsEsc += L"\\\\"; break;
                case L'`':  jsEsc += L"\\`";  break; // For JS template literals
                case L'\n': jsEsc += L"\\n";  break;
                case L'\r':                break;
                case L'\t': jsEsc += L"\\t";  break;
                default:    jsEsc += ch;      break;
            }
        }
        return jsEsc;
    };

    ReplaceAll(html, L"{{DATA}}", jsEscape(umlText));
    ReplaceAll(html, L"{{PREV_ERROR}}", jsEscape(prevError));
    return html;
}

// ---------------------- WebView host ----------------------
static const wchar_t* kWndClass = L"PumlWebViewHost";

struct Host {
    HWND hwnd = nullptr;
    HINSTANCE hInst = nullptr;
    HMODULE   hWvLoader = nullptr;

    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller>  ctrl;
    ComPtr<ICoreWebView2>            web;

    std::wstring initialHtml; // what we will NavigateToString()
};

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
            if(host->ctrl) host->ctrl->Close();
            if(host->hWvLoader) FreeLibrary(host->hWvLoader);
            delete host;
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
    host->hWvLoader = LoadLibraryW(L"WebView2Loader.dll");
    if(!host->hWvLoader){
        CreateWindowW(L"STATIC", L"WebView2 Runtime not found. Install Edge WebView2 Runtime.", WS_CHILD|WS_VISIBLE|SS_CENTER,
                      0,0,0,0, host->hwnd, nullptr, host->hInst, nullptr);
        return;
    }
    auto fn = reinterpret_cast<PFN_CreateCoreWebView2EnvironmentWithOptions>(
        GetProcAddress(host->hWvLoader, "CreateCoreWebView2EnvironmentWithOptions"));
    if(!fn){
        CreateWindowW(L"STATIC", L"WebView2 loader entry not found.", WS_CHILD|WS_VISIBLE|SS_CENTER,
                      0,0,0,0, host->hwnd, nullptr, host->hInst, nullptr);
        return;
    }

    fn(nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [host](HRESULT hr, ICoreWebView2Environment* env)->HRESULT{
                if(FAILED(hr) || !env) return S_OK;
                host->env = env;
                env->CreateCoreWebView2Controller(host->hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [host](HRESULT hr, ICoreWebView2Controller* ctrl)->HRESULT{
                            if(FAILED(hr) || !ctrl) return S_OK;
                            host->ctrl = ctrl;
                            host->ctrl->get_CoreWebView2(&host->web);
                            RECT rc; GetClientRect(host->hwnd, &rc);
                            host->ctrl->put_Bounds(rc);
                            if (!host->initialHtml.empty())
                                host->web->NavigateToString(host->initialHtml.c_str());
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
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
    EnsureWndClass();

    auto* host = new Host();
    host->hInst = GetModuleHandleW(nullptr);
    host->hwnd  = CreateWindowExW(0, kWndClass, L"",
                                  WS_CHILD|WS_VISIBLE, 0,0,0,0,
                                  ParentWin, nullptr, host->hInst, nullptr);
    SetWindowLongPtrW(host->hwnd, GWLP_USERDATA, (LONG_PTR)host);

    const std::wstring text = ReadFileUtf16OrAnsi(FileToLoad);
    const bool preferSvg = (ToLowerTrim(g_prefer) == L"svg");

    // Decide render path by order
    const std::vector<std::wstring> order = SplitOrder(g_order.empty() ? L"java,web" : g_order);
    bool produced = false;
    std::wstring lastErr;

    for (const auto& step : order) {
        if (step == L"java") {
            std::wstring svgOut;
            std::vector<unsigned char> pngOut;
            if (RunPlantUmlJar(text, preferSvg, svgOut, pngOut)) {
                if (preferSvg) {
                    host->initialHtml = BuildShellHtmlWithBody(svgOut);
                } else {
                    const std::string b64 = Base64(pngOut);
                    std::wstring body = L"<img alt=\"diagram\" src=\"data:image/png;base64,";
                    body += FromUtf8(b64); body += L"\"/>";
                    host->initialHtml = BuildShellHtmlWithBody(body);
                }
                produced = true;
                break;
            } else {
                lastErr = L"Local Java/JAR rendering failed or Java/JAR not found.";
            }
        } else if (step == L"web") {
            host->initialHtml = BuildServerHtml(g_serverUrl, preferSvg, text, lastErr);
            produced = true;
            break;
        } else {
            // ignore unknown tokens
        }
    }

    if (!produced) {
        if (order.size() == 1 && order[0] == L"java") {
            host->initialHtml = BuildErrorHtml(lastErr.empty() ? L"Java-only mode selected, but Java/JAR not available." : lastErr);
        } else if (order.size() == 1 && order[0] == L"web") {
            host->initialHtml = BuildErrorHtml(L"Web-only mode selected, cannot proceed (unexpected).");
        } else {
            host->initialHtml = BuildErrorHtml(L"No valid render mode produced output.");
        }
    }

    InitWebView(host);
    return host->hwnd;
}

__declspec(dllexport) int __stdcall ListSendCommand(HWND /*ListWin*/, int /*Command*/, int /*Parameter*/) {
    // Ctrl+C is handled by the web page JS.
    return 0;
}

__declspec(dllexport) void __stdcall ListCloseWindow(HWND ListWin) {
    DestroyWindow(ListWin);
}

} // extern "C"
