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

static const char* kHtml = R"(<!doctype html>
<meta charset="utf-8"><title>PlantUML</title>
<style>
html,body,#wrap{height:100%;margin:0}
#wrap{display:flex;flex-direction:column;background:#1e1e1e;color:#ddd;font:13px/1.4 system-ui,Segoe UI,Roboto,Arial}
#err{display:none;align-items:center;justify-content:center;background:#2b2b2b;color:#f66;padding:12px}
#imgzone{flex:1;display:flex;align-items:center;justify-content:center;overflow:auto}
.svghost{max-width:100%;max-height:100%}
#hud{position:fixed;right:12px;bottom:12px;background:#000a;color:#fff;padding:4px 8px;border-radius:6px;font:12px/1.2 system-ui;opacity:0;transition:opacity .2s}
.pagehint{position:fixed;left:12px;bottom:12px;color:#aaa;font:12px/1.2 system-ui}
:root.dark{background:#0e0e0e}
</style>
<div id="wrap"><div id="err"></div><div id="imgzone"></div><div id="hud"></div><div class="pagehint">←/→ PgUp/PgDn • Ctrl+C • D=dark</div></div>
<script>
const st={server:"https://www.plantuml.com/plantuml",prefer:"svg",blocks:[],i:0};
const hud=document.getElementById('hud');function setHud(t,ms=900){hud.textContent=t;hud.style.opacity=1;clearTimeout(hud._t);hud._t=setTimeout(()=>hud.style.opacity=0,ms);}
function showErr(m){const e=document.getElementById('err');e.textContent=m;e.style.display='flex';}
function splitBlocks(s){const r=/@startuml[\\s\\S]*?@enduml/gmi;const m=s.match(r);return m&&m.length?m:[s];}
async function post(kind,txt){const r=await fetch(st.server+'/'+kind,{method:'POST',headers:{'Content-Type':'text/plain;charset=utf-8'},body:txt});if(!r.ok)throw new Error('HTTP '+r.status);return r;}
async function renderSvg(t){const r=await post('svg',t);if(!(r.headers.get('content-type')||'').includes('image/svg'))throw new Error('Not SVG');return r.text();}
async function renderPng(t){const r=await post('png',t);return r.blob();}
function showSvg(svg){const z=document.getElementById('imgzone');z.innerHTML='';const d=document.createElement('div');d.className='svghost';d.innerHTML=svg;z.appendChild(d);}
function showPng(b){const z=document.getElementById('imgzone');z.innerHTML='';const i=document.createElement('img');i.className='svghost';i.src=URL.createObjectURL(b);z.appendChild(i);}
async function render(){const t=st.blocks[st.i]||'';try{ if(st.prefer==='png'){showPng(await renderPng(t));} else {showSvg(await renderSvg(t));} }
catch(e){try{ if(st.prefer==='png'){showSvg(await renderSvg(t));} else {showPng(await renderPng(t));} }catch(e2){showErr('PlantUML fetch failed');console.error(e,e2);}}
setHud((st.i+1)+'/'+st.blocks.length);}
function nav(d){if(!st.blocks.length)return;st.i=(st.i+d+st.blocks.length)%st.blocks.length;render();}
async function copyCurrent(){const t=st.blocks[st.i]||'';try{const s=await renderSvg(t);await navigator.clipboard.write([new ClipboardItem({'image/svg+xml':new Blob([s],{type:'image/svg+xml'})})]);setHud('Copied SVG');return;}catch(e){}
try{const b=await renderPng(t);await navigator.clipboard.write([new ClipboardItem({'image/png':b})]);setHud('Copied PNG');return;}catch(e){}
setHud('Copy failed');}
document.addEventListener('keydown',e=>{if(e.key==='ArrowRight'||e.key==='PageDown'){e.preventDefault();nav(1);}if(e.key==='ArrowLeft'||e.key==='PageUp'){e.preventDefault();nav(-1);}if((e.ctrlKey||e.metaKey)&&(e.key==='c'||e.key==='C')){e.preventDefault();copyCurrent();}if(e.key==='d'||e.key==='D'){document.documentElement.classList.toggle('dark');}});
if(window.chrome&&chrome.webview){chrome.webview.addEventListener('message',e=>{const m=e.data;if(m&&m.type==='load'){st.server=m.server||st.server;st.prefer=(m.prefer||'svg').toLowerCase()==='png'?'png':'svg';st.blocks=splitBlocks(m.text||'');if(!st.blocks.length)st.blocks=['(empty)'];st.i=(m.index|0)||0;render();}});chrome.webview.postMessage({ready:true});}
</script>
)";

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
