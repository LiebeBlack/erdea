// EdgeDock Studio :: main.cpp
// Punto de entrada: conciencia de DPI por monitor, inicialización OLE (necesaria para el
// drag & drop de notas) y bucle de mensajes principal.

#include <windows.h>
#include <objbase.h>

#include "app/App.h"
#include "core/AppPaths.h"

// Estética y controles modernos: se enlaza el manifest de Common Controls v6 sin .rc.
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

void EnablePerMonitorDpiAwareness() {
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        using SetDpiContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto setContext = reinterpret_cast<SetDpiContextFn>(
            ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setContext != nullptr && setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return;
        }
        using SetDpiAwareFn = BOOL(WINAPI*)();
        auto setAware = reinterpret_cast<SetDpiAwareFn>(::GetProcAddress(user32, "SetProcessDPIAware"));
        if (setAware != nullptr && setAware()) return;
    }

    // Reserva para sistemas donde user32 no expone las funciones anteriores.
    HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
    if (shcore != nullptr) {
        using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(int);
        auto setAwareness = reinterpret_cast<SetProcessDpiAwarenessFn>(
            ::GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (setAwareness != nullptr) setAwareness(2);   // PROCESS_PER_MONITOR_DPI_AWARE
        ::FreeLibrary(shcore);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    EnablePerMonitorDpiAwareness();
    ::SetThreadDescription(::GetCurrentThread(), L"EdgeDock.Main");

    const HRESULT oleStatus = ::OleInitialize(nullptr);
    if (FAILED(oleStatus)) {
        edgedock::paths::AppendLog(L"main: OleInitialize falló; el panel sigue sin drag & drop");
    }

    int exitCode = 0;
    {
        edgedock::App app;
        if (app.Initialize(instance)) {
            exitCode = app.Run();
        }
        app.Shutdown();
    }

    if (SUCCEEDED(oleStatus)) ::OleUninitialize();
    return exitCode;
}
