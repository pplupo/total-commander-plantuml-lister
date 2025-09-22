# PlantUmlWebView — Total Commander Lister

A tiny, modern PlantUML viewer for **Total Commander (64-bit)**.
Renders diagrams **locally via Java + `plantuml.jar`**.
Powered by **WebView2** — no Qt or zlib required.

---

## Why this plugin?

* 🪶 **Small footprint** – just a WLX DLL, a config INI, and `WebView2Loader.dll`.
* ⚡ **Fast preview** – render via local Java + `plantuml.jar`.
* 🔧 **Configurable** – choose **SVG** or **PNG**, and configure Java/JAR paths.
* 📋 **Copy to clipboard** – **Ctrl+C** copies **SVG text** or a **PNG bitmap** from Lister.

---

## Requirements

* **Total Commander 64-bit** (Lister/WLX plugin support).
* **Microsoft Edge WebView2 Runtime** (evergreen).
  👉 Download from Microsoft: <https://developer.microsoft.com/en-us/microsoft-edge/webview2/#download>
* **Java** (`javaw.exe`/`java.exe`) and **`plantuml.jar`** (shipped with releases).

---

## Installation

1. Download the latest **release ZIP** from this repository’s **Releases** page.
2. Open the ZIP in **Total Commander**.
3. TC will offer **“Install plugin”** → click **OK** and follow the prompts.

That’s it. The plugin will be installed to your TC plugins folder.

---

## Usage

* Select a PlantUML file (`.puml`, `.plantuml`, `.uml`, `.wsd`, `.ws`, `.iuml`) and press **F3** (Lister).
* The plugin renders diagrams locally via Java + `plantuml.jar`. Configure `[plantuml]` in the INI if you need explicit paths.
* **Ctrl+C** inside the preview:
  * **SVG mode:** copies the SVG markup as text.
  * **PNG mode:** copies a PNG bitmap.

---

## Configuration

**INI path (after install):**  
`%COMMANDER_PATH%\Plugins\wlx\PlantUmlWebView\plantumlwebview.ini`

Default contents:

```ini
; Rendering is performed locally via Java + plantuml.jar.

[render]
; "svg" (default) or "png"
prefer=svg

[plantuml]
; If empty, the plugin auto-tries "plantuml.jar" next to PlantUmlWebView.wlx64.
; You can also point to a custom jar path here.
jar=plantuml-mit-1.2025.7.jar

; Optional explicit path to javaw.exe or java.exe. If empty, PATH is searched.
java=

; Kill the java process if it hangs (milliseconds)
timeout_ms=8000

[detect]
; Detect string reported to Total Commander during installation.
string=EXT="PUML" | EXT="PLANTUML" | EXT="UML" | EXT="WSD" | EXT="WS" | EXT="IUML"

[debug]
; Optional log file path (defaults next to the plugin DLL)
; Set log_enabled=0 to disable logging entirely
log_enabled=1
log=
```

### SVG vs PNG

* **SVG (default):** crisp, scalable, selectable text, small output.
* **PNG:** universal compatibility; larger bitmap output.

---

## Data handling

All rendering happens locally via Java and `plantuml.jar`; the plugin does not perform any network requests.

---

## Troubleshooting

* **Blank panel / “Render error”**

  * Verify Java and `plantuml.jar` paths in `[plantuml]` are correct.
* **Logging** – keep `[debug] log_enabled=1` (default) and inspect `plantumlwebview.log` (or a custom `[debug] log=` path) for details.
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
