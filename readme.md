# PlantUML WebView Lister (Tiny, Server-POST)

A **small** Total Commander Lister plugin that renders PlantUML diagrams by
sending the raw diagram text via **HTTP POST** to a PlantUML server. It prefers
**SVG** and automatically falls back to **PNG**.

No Qt, no zlib, no bundling of huge runtimes. Just **WebView2** (Edge) and a bit of C++.

## Features
- Multi-diagram paging (`@startuml ... @enduml` blocks) with a mini HUD (`2/5`)
- Clipboard: **Ctrl+C** copies SVG (or PNG if needed)
- Dark background toggle: press **D**
- Configurable server URL and detect string via `plantumlwebview.ini`
- Minimal DLL size (no external runtime packaged)

## Requirements
- Windows 10/11 with **Microsoft Edge WebView2 Runtime** (usually already installed).
- WebView2 SDK **header** `WebView2.h` available to the compiler (put it in `third_party/` or use NuGet).

## Build
```powershell
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
