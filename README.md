# PlantUML WebView Lister (Total Commander)

A tiny, fast **Lister plugin for Total Commander (64-bit)** that renders PlantUML files using the **Edge WebView2** engine and a **PlantUML HTTP server**.

Works great with `.puml`, `.plantuml`, `.uml`, `.wsd`, `.ws`, `.iuml`.

---

## Why this plugin?

* 🪶 **Small footprint** – ships only the Lister DLL, a config file, and `WebView2Loader.dll`.
* ⚡ **Fast preview** – renders via your PlantUML server using modern HTML.
* 🧩 **Simple install** – standard `pluginst.inf` for click-to-install from ZIP.
* 🔧 **Configurable** – point to your own PlantUML server; choose **SVG** or **PNG** output.
* 📋 **Copy to clipboard** – **Ctrl+C** in Lister copies SVG text or a PNG bitmap.

---

## Requirements

* **Total Commander 64-bit** (Lister/WLX plugin support).

* **Microsoft Edge WebView2 Runtime** (evergreen).
  If it’s missing, Windows will show a message the first time you open the plugin.
  (You can install the runtime from Microsoft; search for “WebView2 Runtime Evergreen”.)

* **PlantUML HTTP server** reachable from your machine (defaults to the public server).

---

## Installation

### Easiest (recommended)

1. Open the plugin ZIP in Total Commander.
2. TC should show **“Install plugin”** – click **OK** and follow the prompt.

Your ZIP must have these files **at the root**:

```
pluginst.inf
PlantUmlWebView.wlx64
plantumlwebview.ini
WebView2Loader.dll
README.md   (optional)
```

## Usage

* Select a PlantUML file and press **F3** (Lister).
* The plugin sends the file text to your PlantUML server using **HTTP POST** to `/svg` (default) or `/png`.
* Press **Ctrl+C**:

  * **SVG mode:** copies the SVG markup as text.
  * **PNG mode:** copies a PNG bitmap.

---

## Configuration (`%COMMANDER_PATH%\plugins\wfx\plantumlwebview.ini`)

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

[detect]
; Optional: override the detect string that TC shows by default.
; (This does not auto-update TC’s settings; it only affects the
; string the plugin reports if TC asks again.)
string=EXT="PUML" | EXT="PLANTUML" | EXT="UML" | EXT="WSD" | EXT="WS" | EXT="IUML"
```

### Choosing SVG vs PNG

* **SVG (default):** crisp, scalable, selectable text, small size.
* **PNG:** for environments where SVG isn’t preferred—larger but universal.

---

## Server requires HTTP POST!

Why HTTP **POST** instead of GET?

The traditional PlantUML URL style encodes your UML text as a compressed token in the path (GET). That requires client-side **zlib/deflate** and has practical URL length limits.

This plugin uses **POST** with raw text to `/{svg|png}`:

* ✅ No client compression code (keeps the plugin tiny).
* ✅ Avoids long URL limits and tricky encoding.
* ✅ Supported by the official server and most self-hosted servers.

---

## Privacy & security

* Your diagram text is sent to the configured PlantUML server.
* Use an **internal** or **local** server if your diagrams are sensitive.
* HTTPS is recommended for remote servers.

---

## Troubleshooting

* **Blank/white panel or error text**

  * Check network/proxy to the PlantUML server.
  * If using a self-signed TLS cert, the WebView may block it.

* **“WebView2 Runtime not found”**

  * Install Microsoft Edge WebView2 Runtime (evergreen) and retry.

* **No install prompt from ZIP**

  * Ensure `pluginst.inf` is at the ZIP **root** (not inside a subfolder or nested ZIP).

* **No copy to clipboard**

  * Click once inside the preview, then press **Ctrl+C**.

---

## Building (for developers)

* Toolchain: MSVC (x64), CMake + Ninja.
* Dependencies:

  * `WebView2Loader.dll` and headers (`WebView2.h`) from the WebView2 SDK.
* Minimal CMake outline:

  * Create a DLL named `PlantUmlWebView.wlx64`
  * Include `third_party/WebView2/build/native/include` for headers
  * Link delay-loaded `WebView2Loader.dll` or load it dynamically

> This repository also ships a ready-to-use **GitHub Actions** workflow that fetches the WebView2 SDK, builds the DLL, and packages the plugin ZIP.

---

## File list (delivered)

```
PlantUmlWebView.wlx64
WebView2Loader.dll
plantumlwebview.ini
pluginst.inf
README.md
```

---

## License

MIT (or your preferred license).

---

## Acknowledgements

* [PlantUML](https://plantuml.com/) for the rendering engine.
* Microsoft **WebView2** for modern, lightweight HTML rendering inside Lister.
* Total Commander for its extensible Lister plugin interface.
