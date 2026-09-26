#pragma once
// EdgeDock Studio :: modules/DragDrop.h
// OLE nativo sin atajos: IDataObject propio (para arrastrar clips hacia fuera) e IDropTarget
// propio (para aceptar texto/archivos soltados sobre el panel). Sin std::function y sin
// asignaciones de heap: los callbacks son punteros a función con contexto opaco y el texto
// soltado vive en una cadena PMR del pool global.

#include <windows.h>

#include <objidl.h>

#include <atomic>
#include <string>
#include <vector>

namespace edgedock::modules {

// --- Origen: arrastrar un clip fuera del panel ----------------------------------
class TextDataObject final : public IDataObject {
public:
    TextDataObject(std::wstring text, std::vector<std::wstring> files = {});

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDataObject
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override;
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC* format, STGMEDIUM* medium) override;
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override;
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* format, FORMATETC* out) override;
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override;
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override;
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC* format, DWORD flags, IAdviseSink* sink, DWORD* connection) override;
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD connection) override;
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA** out) override;

private:
    ~TextDataObject();
    std::atomic<long> refCount_{1};
    std::wstring text_;
    std::vector<std::wstring> files_;
};

// Lanza el arrastre (bloquea hasta que el usuario suelte) devolviendo el efecto aplicado.
DWORD StartTextDrag(HWND owner, const std::wstring& text, const std::vector<std::wstring>& files,
                    DWORD allowedEffects = DROPEFFECT_COPY | DROPEFFECT_MOVE);

// --- Destino: aceptar lo que suelten sobre el panel -----------------------------
struct DropCallbacks {
    void (*onText)(void* context, const wchar_t* text, size_t length, DWORD effect) = nullptr;
    void (*onFiles)(void* context, const wchar_t* const* paths, size_t count, DWORD effect) = nullptr;
    void (*onDragState)(void* context, bool active, DWORD effect) = nullptr;
    void* context = nullptr;
};

enum class DropPayload { None, Text, Files };

// Estado de la última inspección de un IDataObject, reutilizable desde el panel para pintar
// el resaltado del destino.
struct DropInspection {
    DropPayload payload = DropPayload::None;
    DWORD effect = DROPEFFECT_NONE;
    size_t estimatedSize = 0;
};

class PanelDropTarget final : public IDropTarget {
public:
    explicit PanelDropTarget(DropCallbacks callbacks);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keyState, POINTL point, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect) override;

    DropInspection Inspect(IDataObject* dataObject) const;
    bool IsDraggingOver() const { return dragging_; }
    DWORD LastEffect() const { return lastEffect_; }

private:
    ~PanelDropTarget();
    void ApplyEffect(DWORD keyState, DWORD* effect) const;

    std::atomic<long> refCount_{1};
    IDataObject* current_ = nullptr;
    DropCallbacks callbacks_{};
    DropInspection inspection_{};
    bool dragging_ = false;
    DWORD lastEffect_ = DROPEFFECT_NONE;
};

// Registro sobre un HWND (requiere OleInitialize en el hilo).
HRESULT RegisterPanelDropTarget(HWND hwnd, PanelDropTarget* target);
void RevokePanelDropTarget(HWND hwnd);

// Formatos registrados que interesan al panel.
UINT FormatHtml();
UINT FormatRtf();
UINT FormatPng();

} // namespace edgedock::modules
