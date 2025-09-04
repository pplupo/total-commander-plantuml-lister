// PlantUML WebView Lister (Tiny, Server-POST, SVG-preferred)
// Build: CMake (produces PlantUmlWebView.wlx64)

#include <windows.h>
#include <shlwapi.h>
#include <wrl.h>
#include <string>
#include <sstream>
#include <vector>
#include <memory>

// WebView2 COM interfaces (header from SDK required)
#include "WebView2.h"

#pragma comment(lib, "shlwapi.lib")

using namespace Microsoft::WRL;

// ---------------------- Config ----------------------
static std::wstring g_serverUrl = L"https://www.plantuml.com/plantuml";
static std::wstring g_prefer    = L"svg"; // or "png"
static std::string  g_detectA   = R"(EXT="PUML" | EXT="PLANTUML" | EXT="UML" | EXT="WSD" | EXT="WS" | EXT="IUML")";
static bool         g_cfgLoaded = false;

// --- helpers for UTF-8 -> UTF-16, JS escaping, and string replace ---
static std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static void ReplaceAll(std::wstring& inout, const std::wstring& from, const std::wstring& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = inout.find(from, pos)) != std::wstring::npos) {
        inout.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static std::wstring JsEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 16);
    for (wchar_t ch : s) {
        switch (ch) {
            case L'\\': out += L"\\\\"; break;
            case L'"':  out += L"\\\""; break;
            case L'\'': out += L"\\\'"; break;
            case L'\n': out += L"\\n";  break;
            case L'\r': out += L"\\r";  break;
            case L'\t': out += L"\\t";  break;
            default:
                // keep printable BMP as-is; escape other control chars
                if (ch < 0x20) {
                    wchar_t buf[7];
                    swprintf(buf, 7, L"\\u%04X", (unsigned)ch);
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

static std::wstring GetModuleDir() {
    wchar_t path[MAX_PATH]{};
    HMODULE hm{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&GetModuleDir, &hm);
    GetModuleFileNameW(hm, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

static void LoadConfigIfNeeded() {
    if (g_cfgLoaded) return;
    g_cfgLoaded = true;

    std::wstring dir = GetModuleDir();
    std::wstring ini = dir + L"\\plantumlwebview.ini";

    wchar_t buf[2048];

    if (GetPrivateProfileStringW(L"server", L"url", L"", buf, 2048, ini.c_str()) > 0) {
        if (buf[0]) g_serverUrl = buf;
    }
    if (GetPrivateProfileStringW(L"render", L"prefer", L"", buf, 2048, ini.c_str()) > 0) {
        if (buf[0]) g_prefer = buf;
    }
    if (GetPrivateProfileStringW(L"detect", L"string", L"", buf, 2048, ini.c_str()) > 0) {
        int need = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(need ? need - 1 : 0, '\0');
        if (need > 1) WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8.data(), need - 1, nullptr, nullptr);
        if (!utf8.empty()) g_detectA = utf8;
    }
}

// ---------------------- Utils ----------------------
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

static std::wstring EscapeForJson(const std::wstring& w) {
    std::wstring out; out.reserve(w.size() + 64);
    for (wchar_t c : w) {
        switch (c) {
        case L'\\': out += L"\\\\"; break;
        case L'"':  out += L"\\\""; break;
        case L'\r': break;
        case L'\n': out += L"\\n"; break;
        case L'\t': out += L"\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

// ---------------------- WebView host ----------------------
static const wchar_t* kWndClass = L"PumlWebViewHost";

struct Host {
    HWND hwnd = nullptr;
    HINSTANCE hInst = nullptr;
    HMODULE   hWvLoader = nullptr;

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> env;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller>  ctrl;
    Microsoft::WRL::ComPtr<ICoreWebView2>            web;

    std::wstring payloadJson;
};

// server URL and format come from your settings; adjust names if different
const std::string serverUtf8 = state.serverUrl;           // e.g. "https://www.plantuml.com/plantuml"
const bool preferSvg = state.preferSvg;                   // true -> svg, false -> png
const std::string umlUtf8 = umlText;                      // your PlantUML source text (UTF-8)

// Create a single wide raw-string HTML document with placeholders.
std::wstring html = LR"PUMLHTML(<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta http-equiv="X-UA-Compatible" content="IE=edge"/>
  <meta name="viewport" content="width=device-width, initial-scale=1"/>
  <title>PlantUML Viewer</title>
  <style>
    :root {
      color-scheme: light dark;
    }
    html, body { height: 100%; }
    body {
      margin: 0;
      background: canvas;
      color: CanvasText;
      font: 13px system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
      overflow: auto;
    }
    /* SVG container theming + scroll */
    #root {
      display: flex;
      align-items: start;
      justify-content: center;
      padding: 8px;
      gap: 8px;
    }
    #hud {
      position: fixed;
      right: 8px; top: 8px;
      background: color-mix(in oklab, Canvas 80%, CanvasText 20%);
      color: CanvasText;
      border-radius: 999px;
      padding: 4px 10px;
      font-weight: 600;
      box-shadow: 0 1px 4px rgba(0,0,0,.2);
      user-select: none;
      -webkit-user-select: none;
      pointer-events: none;
    }
    img { max-width: 100%; height: auto; }
    svg { max-width: 100%; height: auto; }
  </style>
</head>
<body>
  <div id="hud"></div>
  <div id="root"></div>
  <script>
    (function(){
      const SERVER  = "{{SERVER}}";
      const FORMAT  = "{{FORMAT}}"; // "svg" or "png"
      const UML     = "{{DATA}}";   // JS-escaped PlantUML text
      const root    = document.getElementById('root');
      const hud     = document.getElementById('hud');

      function setHUD(text){ hud.textContent = text; }

      // POST plain text to /svg or /png (no compression needed)
      const endpoint = SERVER.replace(/\/+$/,'') + '/' + FORMAT;
      setHUD('rendering…');

      fetch(endpoint, {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain;charset=utf-8' },
        body: UML
      }).then(async (res) => {
        if (!res.ok) throw new Error('HTTP ' + res.status);
        if (FORMAT === 'svg') {
          const svgText = await res.text();
          root.innerHTML = svgText; // inject SVG markup
        } else {
          const blob = await res.blob();
          const url = URL.createObjectURL(blob);
          const img = new Image();
          img.onload = () => URL.revokeObjectURL(url);
          img.src = url;
          root.replaceChildren(img);
        }
        setHUD('done');
      }).catch(err => {
        root.textContent = 'Render error: ' + err.message;
        setHUD('error');
      });

      // Copy-to-clipboard: Ctrl+C exports SVG text or PNG bitmap
      document.addEventListener('keydown', async (ev) => {
        if ((ev.ctrlKey || ev.metaKey) && ev.key.toLowerCase() === 'c') {
          ev.preventDefault();
          try {
            if (FORMAT === 'svg') {
              const sel = root.querySelector('svg');
              if (sel) {
                const text = new XMLSerializer().serializeToString(sel);
                await navigator.clipboard.writeText(text);
                setHUD('copied SVG');
              }
            } else {
              const img = root.querySelector('img');
              if (img) {
                const c = document.createElement('canvas');
                c.width = img.naturalWidth; c.height = img.naturalHeight;
                const g = c.getContext('2d');
                g.drawImage(img, 0, 0);
                const blob = await new Promise(r => c.toBlob(r, 'image/png'));
                await navigator.clipboard.write([new ClipboardItem({ 'image/png': blob })]);
                setHUD('copied PNG');
              }
            }
          } catch(e) { setHUD('copy failed'); }
        }
      });
    })();
  </script>
</body>
</html>
)PUMLHTML";

// Fill placeholders.
ReplaceAll(html, L"{{SERVER}}",  FromUtf8(serverUtf8));
ReplaceAll(html, L"{{FORMAT}}",  preferSvg ? std::wstring(L"svg") : std::wstring(L"png"));
ReplaceAll(html, L"{{DATA}}",    JsEscape(FromUtf8(umlUtf8)));

// Navigate with UTF-16 as required by WebView2
webview->NavigateToString(html.c_str());

// Function pointer type for dynamic loading of WebView2 loader
typedef HRESULT (STDAPICALLTYPE *PFN_CreateCoreWebView2EnvironmentWithOptions)(
    PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

// Window proc
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
    WNDCLASSW wc{};
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance   = GetModuleHandleW(nullptr);
    wc.hCursor     = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PumlWebViewHost";
    RegisterClassW(&wc);
    inited = true;
}

static void InitWebView(struct Host* host){
    // Dynamically load WebView2Loader.dll to avoid import libs
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
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [host](HRESULT hr, ICoreWebView2Environment* env)->HRESULT{
                if(FAILED(hr) || !env) return S_OK;
                host->env = env;
                env->CreateCoreWebView2Controller(host->hwnd,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [host](HRESULT hr, ICoreWebView2Controller* ctrl)->HRESULT{
                            if(FAILED(hr) || !ctrl) return S_OK;
                            host->ctrl = ctrl;
                            host->ctrl->get_CoreWebView2(&host->web);
                            RECT rc; GetClientRect(host->hwnd, &rc);
                            host->ctrl->put_Bounds(rc);
                            host->web->NavigateToString(kHtml);

                            EventRegistrationToken token{};
                            host->web->add_WebMessageReceived(
                                Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [host](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* e)->HRESULT{
                                        LPWSTR j = nullptr;
                                        e->TryGetWebMessageAsString(&j);
                                        if(j){
                                            if(wcsstr(j, L"\"ready\":true")){
                                                if(!host->payloadJson.empty())
                                                    host->web->PostWebMessageAsJson(host->payloadJson.c_str());
                                            }
                                            CoTaskMemFree(j);
                                        }
                                        return S_OK;
                                    }).Get(), &token);

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
    host->hwnd  = CreateWindowExW(0, L"PumlWebViewHost", L"",
                                  WS_CHILD|WS_VISIBLE, 0,0,0,0,
                                  ParentWin, nullptr, host->hInst, nullptr);
    SetWindowLongPtrW(host->hwnd, GWLP_USERDATA, (LONG_PTR)host);

    std::wstring text = ReadFileUtf16OrAnsi(FileToLoad);
    std::wstring esc  = EscapeForJson(text);

    std::wstringstream js;
    js << L"{\\"type\\":\\"load\\",\\"server\\":\\"" << g_serverUrl
       << L"\\",\\"prefer\\":\\"" << g_prefer
       << L"\\",\\"index\\":0,\\"text\\":\\"" << esc << L"\\"}";
    host->payloadJson = js.str();

    InitWebView(host);
    return host->hwnd;
}

__declspec(dllexport) int __stdcall ListSendCommand(HWND ListWin, int Command, int /*Parameter*/) {
    // 2017 is lc_copy in WLX (Ctrl+C)
    if(Command == 2017) {
        auto* host = reinterpret_cast<Host*>(GetWindowLongPtrW(ListWin, GWLP_USERDATA));
        if(host && host->web){
            host->web->ExecuteScript(L"copyCurrent()", nullptr);
            return 1;
        }
    }
    return 0;
}

__declspec(dllexport) void __stdcall ListCloseWindow(HWND ListWin) {
    DestroyWindow(ListWin);
}

} // extern "C"

