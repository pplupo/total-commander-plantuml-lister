# PlantUmlWebView — Total Commander Lister

A tiny, modern PlantUML viewer for **Total Commander (64-bit)**.  
Renders diagrams either **locally via Java + `plantuml.jar`** (offline) or **via a PlantUML HTTP server** (POST).  
Powered by **WebView2** — no Qt or zlib required.

---

## Why this plugin?

* 🪶 **Small footprint** – just a WLX DLL, a config INI, and `WebView2Loader.dll`.
* ⚡ **Fast preview** – render via local Java or a remote PlantUML server.
* 🔧 **Configurable** – set your server, choose **SVG** or **PNG**, and pick render **order** (`java,web` / `web,java` / `java` / `web`).
* 📋 **Copy to clipboard** – **Ctrl+C** copies **SVG text** or a **PNG bitmap** from Lister.

---

## Requirements

* **Total Commander 64-bit** (Lister/WLX plugin support).
* **Microsoft Edge WebView2 Runtime** (evergreen).  
  👉 Download from Microsoft: <https://developer.microsoft.com/en-us/microsoft-edge/webview2/?form=MA13LH#download>
* For **server rendering**: a reachable **PlantUML HTTP server** (defaults to the public service).
* For **local rendering**: **Java** (`javaw.exe`/`java.exe`) and **`plantuml.jar`** (shipped with releases).

---

## Installation

1. Download the latest **release ZIP** from this repository’s **Releases** page.
2. Open the ZIP in **Total Commander**.
3. TC will offer **“Install plugin”** → click **OK** and follow the prompts.

That’s it. The plugin will be installed to your TC plugins folder.

---

## Usage

* Select a PlantUML file (`.puml`, `.plantuml`, `.uml`, `.wsd`, `.ws`, `.iuml`) and press **F3** (Lister).
* The plugin chooses the render path according to `render.order` in the INI:
  * **`java,web` (default):** try local Java + `plantuml.jar`; if it fails, POST to the server.
  * **`web,java`:** try server first, then local Java if needed.
  * **`java`:** only local Java; shows an error if Java/JAR not available.
  * **`web`:** only server POST.
* **Ctrl+C** inside the preview:
  * **SVG mode:** copies the SVG markup as text.
  * **PNG mode:** copies a PNG bitmap.

---

## Configuration

**INI path (after install):**  
`%COMMANDER_PATH%\Plugins\wlx\PlantUmlWebView\plantumlwebview.ini`

Default contents:

```ini
[server]
; PlantUML server base URL (no trailing slash required).
; Official public service (rate-limited):
;   https://www.plantuml.com/plantuml
; Your own server examples:
;   http://localhost:8080/plantuml
;   http://intranet.example/plantuml
url=https://www.plantuml.com/plantuml

[render]
; "svg" (default) or "png"
prefer=svg

; Render order (comma-separated, case-insensitive):
;   java,web  -> try local JAR first; fall back to server
;   web,java  -> try server first; fall back to local JAR
;   java      -> only local JAR (error if Java/JAR missing)
;   web       -> only server
order=java,web

[plantuml]
; If empty, the plugin auto-tries "plantuml.jar" next to PlantUmlWebView.wlx64.
jar=
; Optional explicit path to javaw.exe or java.exe; if empty, PATH is searched.
java=
; Kill the java process if it hangs (milliseconds)
timeout_ms=8000

[detect]
; Detect string reported to Total Commander during installation.
string=EXT="PUML" | EXT="PLANTUML" | EXT="UML" | EXT="WSD" | EXT="WS" | EXT="IUML"```

````
### SVG vs PNG

* **SVG (default):** crisp, scalable, selectable text, small output.
* **PNG:** universal compatibility; larger bitmap output.

---

## Why HTTP POST (server mode)?

Classic PlantUML URLs compress and encode your UML text into the path (GET), which needs client-side deflate/zlib and hits URL length limits.
This plugin uses **plain-text POST** to `/svg` or `/png`:

* ✅ Keeps the plugin tiny (no compression code).
* ✅ Avoids URL length/encoding issues.
* ✅ Supported by the official and most self-hosted servers.

---

## Privacy & Security

* When using **server mode**, your diagram text is sent to the configured **PlantUML server**.
* Use an **internal** server for sensitive diagrams.
* Prefer **HTTPS** for remote servers.

---

## Troubleshooting

* **Blank panel / “Render error”**

  * Check server reachability and proxy settings.
  * Self-signed HTTPS may be blocked by WebView.
* **“WebView2 Runtime not found”**

  * Install the **WebView2 Runtime (Evergreen)** from Microsoft (link above) and retry.
* **Java mode fails**

  * Ensure Java is on PATH or set `[plantuml] java=...`.
  * Ensure `plantuml.jar` is present (or set `[plantuml] jar=...`).
  * Increase `[plantuml] timeout_ms` for large diagrams.
* **Copy to clipboard doesn’t work**

  * Click inside the preview to focus, then press **Ctrl+C**.

---

## Development

* License: **MIT** — contributions welcome.
* Toolchain: **MSVC x64**, **CMake + Ninja**.
* Dependencies:

  * Headers: `WebView2.h` from the WebView2 SDK.
  * Runtime: `WebView2Loader.dll` is **loaded dynamically** (no import library needed).

Minimal CMake outline:

```cmake
add_library(PlantUmlWebView SHARED src/plantuml_wlx_ev2.cpp)
target_include_directories(PlantUmlWebView PRIVATE third_party/WebView2/build/native/include)
target_link_libraries(PlantUmlWebView PRIVATE shlwapi)
set_target_properties(PlantUmlWebView PROPERTIES OUTPUT_NAME "PlantUmlWebView" SUFFIX ".wlx64")
```

---

## Acknowledgements

* [PlantUML](https://plantuml.com/) – the rendering engine.
* Microsoft **WebView2** – lightweight HTML rendering inside Lister.
* **Total Commander** – for the flexible Lister plugin interface.
