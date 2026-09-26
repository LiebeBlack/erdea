// EdgeDock Studio :: modules/DragDrop.cpp
#include "modules/DragDrop.h"

#include "core/PmrPool.h"
#include "core/TextConv.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "ole32.lib")

namespace edgedock::modules {
namespace {

DWORD SanitizeEffect(DWORD effect) {
    return effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
}

HGLOBAL AllocateGlobalBytes(std::size_t bytes) {
    HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    return handle;
}

// --- Enumerador de formatos sobre un array fijo (sin heap) ----------------------
class FormatEnumerator final : public IEnumFORMATETC {
public:
    FormatEnumerator(const FORMATETC* formats, std::size_t count) : formats_(formats), count_(count) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_IEnumFORMATETC) {
            *object = static_cast<IEnumFORMATETC*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG now = --refCount_;
        if (now == 0) delete this;
        return now;
    }

    HRESULT STDMETHODCALLTYPE Next(ULONG count, FORMATETC* out, ULONG* fetched) override {
        ULONG produced = 0;
        while (produced < count && index_ < count_) {
            if (out != nullptr) out[produced] = formats_[index_];
            ++index_;
            ++produced;
        }
        if (fetched != nullptr) *fetched = produced;
        return produced == count ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        index_ = std::min(count_, index_ + count);
        return index_ < count_ ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Reset() override {
        index_ = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** out) override {
        if (out == nullptr) return E_POINTER;
        auto* clone = new FormatEnumerator(formats_, count_);
        clone->index_ = index_;
        *out = clone;
        return S_OK;
    }

private:
    ~FormatEnumerator() = default;
    std::atomic<long> refCount_{1};
    const FORMATETC* formats_ = nullptr;
    std::size_t count_ = 0;
    std::size_t index_ = 0;
};

// --- Origen de arrastre: comportamiento estándar del shell ----------------------
class PanelDropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *object = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG now = --refCount_;
        if (now == 0) delete this;
        return now;
    }

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override {
        if (escapePressed) return DRAGDROP_S_CANCEL;
        const bool leftDown = (keyState & MK_LBUTTON) != 0;
        if (!leftDown) return DRAGDROP_S_DROP;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    ~PanelDropSource() = default;
    std::atomic<long> refCount_{1};
};

std::vector<std::wstring> ReadFileList(IDataObject* dataObject) {
    std::vector<std::wstring> paths;
    FORMATETC format{};
    format.cfFormat = CF_HDROP;
    format.ptd = nullptr;
    format.dwAspect = DVASPECT_CONTENT;
    format.lindex = -1;
    format.tymed = TYMED_HGLOBAL;

    STGMEDIUM medium{};
    if (dataObject->GetData(&format, &medium) != S_OK) return paths;

    auto* drop = static_cast<HDROP>(medium.hGlobal);
    const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    paths.reserve(count);
    for (UINT index = 0; index < count && index < 256; ++index) {
        const UINT length = ::DragQueryFileW(drop, index, nullptr, 0);
        if (length == 0) continue;
        std::wstring path(static_cast<size_t>(length) + 1, L'\0');
        const UINT copied = ::DragQueryFileW(drop, index, path.data(), length + 1);
        path.resize(copied);
        if (!path.empty()) paths.push_back(std::move(path));
    }
    ::ReleaseStgMedium(&medium);
    return paths;
}

} // namespace

// -----------------------------------------------------------------------------
// TextDataObject
// -----------------------------------------------------------------------------

TextDataObject::TextDataObject(std::wstring text, std::vector<std::wstring> files)
    : text_(std::move(text)), files_(std::move(files)) {}

TextDataObject::~TextDataObject() = default;

HRESULT STDMETHODCALLTYPE TextDataObject::QueryInterface(REFIID riid, void** object) {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
        *object = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE TextDataObject::AddRef() {
    return static_cast<ULONG>(++refCount_);
}

ULONG STDMETHODCALLTYPE TextDataObject::Release() {
    const long now = --refCount_;
    if (now == 0) delete this;
    return static_cast<ULONG>(now);
}

HRESULT STDMETHODCALLTYPE TextDataObject::GetData(FORMATETC* format, STGMEDIUM* medium) {
    if (format == nullptr || medium == nullptr) return E_POINTER;
    if (format->lindex != -1 || (format->tymed & TYMED_HGLOBAL) == 0) return DV_E_LINDEX;
    if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;

    if (format->cfFormat == CF_UNICODETEXT) {
        const std::size_t bytes = (text_.size() + 1) * sizeof(wchar_t);
        HGLOBAL handle = AllocateGlobalBytes(bytes);
        if (handle == nullptr) return STG_E_MEDIUMFULL;
        void* locked = ::GlobalLock(handle);
        if (locked == nullptr) {
            ::GlobalFree(handle);
            return STG_E_MEDIUMFULL;
        }
        std::memcpy(locked, text_.c_str(), bytes);
        ::GlobalUnlock(handle);

        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = handle;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    if (format->cfFormat == CF_HDROP && !files_.empty()) {
        const std::size_t headerSize = sizeof(DROPFILES);
        std::size_t payload = 0;
        for (const auto& path : files_) payload += (path.size() + 1) * sizeof(wchar_t);
        payload += sizeof(wchar_t);   // terminador doble obligatorio

        HGLOBAL handle = AllocateGlobalBytes(headerSize + payload);
        if (handle == nullptr) return STG_E_MEDIUMFULL;
        auto* locked = static_cast<std::byte*>(::GlobalLock(handle));
        if (locked == nullptr) {
            ::GlobalFree(handle);
            return STG_E_MEDIUMFULL;
        }

        auto* drop = reinterpret_cast<DROPFILES*>(locked);
        drop->pFiles = static_cast<DWORD>(headerSize);
        drop->fWide = TRUE;
        auto* cursor = locked + headerSize;
        for (const auto& path : files_) {
            const std::size_t bytes = (path.size() + 1) * sizeof(wchar_t);
            std::memcpy(cursor, path.c_str(), bytes);
            cursor += bytes;
        }
        *reinterpret_cast<wchar_t*>(cursor) = L'\0';
        ::GlobalUnlock(handle);

        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = handle;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    return DV_E_FORMATETC;
}

HRESULT STDMETHODCALLTYPE TextDataObject::GetDataHere(FORMATETC*, STGMEDIUM*) {
    return DATA_E_FORMATETC;
}

HRESULT STDMETHODCALLTYPE TextDataObject::QueryGetData(FORMATETC* format) {
    if (format == nullptr) return E_POINTER;
    if (format->lindex != -1) return DV_E_LINDEX;
    if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
    if ((format->tymed & TYMED_HGLOBAL) == 0) return DV_E_TYMED;
    if (format->cfFormat == CF_UNICODETEXT) return S_OK;
    if (format->cfFormat == CF_HDROP && !files_.empty()) return S_OK;
    return DV_E_FORMATETC;
}

HRESULT STDMETHODCALLTYPE TextDataObject::GetCanonicalFormatEtc(FORMATETC* format, FORMATETC* out) {
    if (format == nullptr || out == nullptr) return E_POINTER;
    *out = *format;
    out->ptd = nullptr;
    return DATA_S_SAMEFORMATETC;
}

HRESULT STDMETHODCALLTYPE TextDataObject::SetData(FORMATETC*, STGMEDIUM*, BOOL) {
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE TextDataObject::EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) {
    if (out == nullptr) return E_POINTER;
    if (direction != DATADIR_GET) return E_NOTIMPL;

    static const FORMATETC kFormats[2] = {
        {static_cast<CLIPFORMAT>(CF_UNICODETEXT), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        {static_cast<CLIPFORMAT>(CF_HDROP), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
    };
    *out = new FormatEnumerator(kFormats, 2);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TextDataObject::DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) {
    return OLE_E_ADVISENOTSUPPORTED;
}

HRESULT STDMETHODCALLTYPE TextDataObject::DUnadvise(DWORD) {
    return OLE_E_ADVISENOTSUPPORTED;
}

HRESULT STDMETHODCALLTYPE TextDataObject::EnumDAdvise(IEnumSTATDATA**) {
    return OLE_E_ADVISENOTSUPPORTED;
}

DWORD StartTextDrag(HWND owner, const std::wstring& text, const std::vector<std::wstring>& files,
                    DWORD allowedEffects) {
    if (text.empty() && files.empty()) return DROPEFFECT_NONE;

    // Una sola reserva por arrastre (fuera del camino por frame) porque COM exige contar
    // referencias; todo lo demás queda en el objeto.
    auto* dataObject = new TextDataObject(text, files);
    auto* dropSource = new PanelDropSource();

    DWORD effect = DROPEFFECT_NONE;
    const HRESULT status = ::DoDragDrop(dataObject, dropSource, SanitizeEffect(allowedEffects), &effect);
    dataObject->Release();
    dropSource->Release();

    if (status == DRAGDROP_S_CANCEL) return DROPEFFECT_NONE;
    (void)owner;
    return effect;
}

// -----------------------------------------------------------------------------
// PanelDropTarget
// -----------------------------------------------------------------------------

PanelDropTarget::PanelDropTarget(DropCallbacks callbacks) : callbacks_(callbacks) {}

PanelDropTarget::~PanelDropTarget() {
    if (current_ != nullptr) {
        current_->Release();
        current_ = nullptr;
    }
}

HRESULT STDMETHODCALLTYPE PanelDropTarget::QueryInterface(REFIID riid, void** object) {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
        *object = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE PanelDropTarget::AddRef() {
    return static_cast<ULONG>(++refCount_);
}

ULONG STDMETHODCALLTYPE PanelDropTarget::Release() {
    const long now = --refCount_;
    if (now == 0) delete this;
    return static_cast<ULONG>(now);
}

DropInspection PanelDropTarget::Inspect(IDataObject* dataObject) const {
    DropInspection inspection;
    if (dataObject == nullptr) return inspection;

    FORMATETC format{};
    format.ptd = nullptr;
    format.dwAspect = DVASPECT_CONTENT;
    format.lindex = -1;
    format.tymed = TYMED_HGLOBAL;

    format.cfFormat = static_cast<CLIPFORMAT>(CF_UNICODETEXT);
    if (dataObject->QueryGetData(&format) == S_OK) {
        inspection.payload = DropPayload::Text;
        inspection.effect = DROPEFFECT_COPY;
        STGMEDIUM medium{};
        if (dataObject->GetData(&format, &medium) == S_OK) {
            if (medium.hGlobal != nullptr) inspection.estimatedSize = static_cast<size_t>(::GlobalSize(medium.hGlobal));
            ::ReleaseStgMedium(&medium);
        }
        return inspection;
    }

    format.cfFormat = static_cast<CLIPFORMAT>(CF_HDROP);
    if (dataObject->QueryGetData(&format) == S_OK) {
        inspection.payload = DropPayload::Files;
        inspection.effect = DROPEFFECT_COPY;
    }
    return inspection;
}

void PanelDropTarget::ApplyEffect(DWORD keyState, DWORD* effect) const {
    if (effect == nullptr) return;
    if (inspection_.payload == DropPayload::None) {
        *effect = DROPEFFECT_NONE;
        return;
    }
    DWORD wanted = SanitizeEffect(*effect);
    if (wanted == DROPEFFECT_NONE) wanted = DROPEFFECT_COPY;
    if ((keyState & MK_CONTROL) != 0) wanted = DROPEFFECT_COPY;
    wanted &= SanitizeEffect(inspection_.effect);
    *effect = wanted != DROPEFFECT_NONE ? wanted : DROPEFFECT_COPY;
}

HRESULT STDMETHODCALLTYPE PanelDropTarget::DragEnter(IDataObject* dataObject, DWORD keyState, POINTL point,
                                                     DWORD* effect) {
    (void)point;
    if (dataObject == nullptr || effect == nullptr) return E_POINTER;

    if (current_ != nullptr) {
        current_->Release();
        current_ = nullptr;
    }
    current_ = dataObject;
    current_->AddRef();

    inspection_ = Inspect(dataObject);
    dragging_ = inspection_.payload != DropPayload::None;
    ApplyEffect(keyState, effect);
    lastEffect_ = *effect;
    if (callbacks_.onDragState != nullptr) {
        callbacks_.onDragState(callbacks_.context, dragging_, lastEffect_);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE PanelDropTarget::DragOver(DWORD keyState, POINTL point, DWORD* effect) {
    (void)point;
    if (effect == nullptr) return E_POINTER;
    ApplyEffect(keyState, effect);
    if (*effect != lastEffect_) {
        lastEffect_ = *effect;
        if (callbacks_.onDragState != nullptr) callbacks_.onDragState(callbacks_.context, true, lastEffect_);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE PanelDropTarget::DragLeave() {
    if (current_ != nullptr) {
        current_->Release();
        current_ = nullptr;
    }
    dragging_ = false;
    inspection_ = DropInspection{};
    lastEffect_ = DROPEFFECT_NONE;
    if (callbacks_.onDragState != nullptr) callbacks_.onDragState(callbacks_.context, false, DROPEFFECT_NONE);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE PanelDropTarget::Drop(IDataObject* dataObject, DWORD keyState, POINTL point,
                                                DWORD* effect) {
    (void)point;
    if (effect == nullptr) return E_POINTER;

    const DropInspection inspection = Inspect(dataObject);
    DWORD applied = inspection.payload != DropPayload::None ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if ((keyState & MK_CONTROL) != 0 || applied == DROPEFFECT_NONE) applied = DROPEFFECT_COPY;
    if (inspection.payload == DropPayload::None) applied = DROPEFFECT_NONE;
    *effect = applied;

    if (inspection.payload == DropPayload::Text && callbacks_.onText != nullptr) {
        FORMATETC format{};
        format.cfFormat = static_cast<CLIPFORMAT>(CF_UNICODETEXT);
        format.ptd = nullptr;
        format.dwAspect = DVASPECT_CONTENT;
        format.lindex = -1;
        format.tymed = TYMED_HGLOBAL;

        STGMEDIUM medium{};
        if (dataObject->GetData(&format, &medium) == S_OK) {
            const wchar_t* wide = nullptr;
            size_t length = 0;
            if (medium.hGlobal != nullptr) {
                wide = static_cast<const wchar_t*>(::GlobalLock(medium.hGlobal));
                if (wide != nullptr) {
                    length = static_cast<size_t>(::GlobalSize(medium.hGlobal)) / sizeof(wchar_t);
                    while (length > 0 && wide[length - 1] == L'\0') --length;
                }
            }
            if (wide != nullptr && length > 0) {
                // Copia en el pool sincronizado: el drop no toca el heap del CRT y el
                // callbacks recibe una vista válida mientras dure la llamada.
                std::pmr::polymorphic_allocator<wchar_t> allocator{mem::PoolResource::Global()};
                std::pmr::wstring buffer(allocator);
                buffer.assign(wide, length);
                ::GlobalUnlock(medium.hGlobal);
                callbacks_.onText(callbacks_.context, buffer.c_str(), buffer.size(), applied);
            } else if (wide != nullptr) {
                ::GlobalUnlock(medium.hGlobal);
            }
            ::ReleaseStgMedium(&medium);
        }
    } else if (inspection.payload == DropPayload::Files && callbacks_.onFiles != nullptr) {
        const std::vector<std::wstring> paths = ReadFileList(dataObject);
        if (!paths.empty()) {
            std::vector<const wchar_t*> rawPaths;
            rawPaths.reserve(paths.size());
            for (const auto& path : paths) rawPaths.push_back(path.c_str());
            callbacks_.onFiles(callbacks_.context, rawPaths.data(), rawPaths.size(), applied);
        }
    }

    if (current_ != nullptr) {
        current_->Release();
        current_ = nullptr;
    }
    dragging_ = false;
    inspection_ = DropInspection{};
    lastEffect_ = applied;
    if (callbacks_.onDragState != nullptr) callbacks_.onDragState(callbacks_.context, false, applied);
    return S_OK;
}

HRESULT RegisterPanelDropTarget(HWND hwnd, PanelDropTarget* target) {
    if (hwnd == nullptr || target == nullptr) return E_INVALIDARG;
    return ::RegisterDragDrop(hwnd, target);
}

void RevokePanelDropTarget(HWND hwnd) {
    if (hwnd != nullptr) ::RevokeDragDrop(hwnd);
}

UINT FormatHtml() {
    static const UINT id = ::RegisterClipboardFormatW(L"HTML Format");
    return id;
}

UINT FormatRtf() {
    static const UINT id = ::RegisterClipboardFormatW(L"Rich Text Format");
    return id;
}

UINT FormatPng() {
    static const UINT id = ::RegisterClipboardFormatW(L"PNG");
    return id;
}

} // namespace edgedock::modules
