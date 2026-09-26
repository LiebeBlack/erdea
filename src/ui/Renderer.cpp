// EdgeDock Studio :: ui/Renderer.cpp
#include "ui/Renderer.h"

#include "core/AppPaths.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <dwrite_1.h>   // IDWriteTextLayout1 (SetCharacterSpacing)

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace edgedock::ui {
namespace {

uint32_t QuantizeChannel(float value) {
    if (value <= 0.0f) return 0;
    if (value >= 1.0f) return 255;
    return static_cast<uint32_t>(value * 255.0f + 0.5f);
}

uint64_t ColorKey(theme::Rgba color) {
    const uint64_t r = QuantizeChannel(color.r);
    const uint64_t g = QuantizeChannel(color.g);
    const uint64_t b = QuantizeChannel(color.b);
    const uint64_t a = QuantizeChannel(color.a);
    return (r << 24) | (g << 16) | (b << 8) | a;
}

uint64_t StyleKey(const TextStyle& style) {
    const uint64_t size = static_cast<uint64_t>(style.size * 4.0f + 0.5f);
    uint64_t key = size & 0xFFFF;
    key |= static_cast<uint64_t>(style.weight & 0xFF) << 16;
    key |= static_cast<uint64_t>(style.font == FontRole::Mono ? 1 : 0) << 24;
    key |= static_cast<uint64_t>(style.wrap ? 1 : 0) << 25;
    key |= static_cast<uint64_t>(style.ellipsis ? 1 : 0) << 26;
    key |= static_cast<uint64_t>(style.align == TextAlign::Center ? 1 : style.align == TextAlign::Right ? 2 : 0) << 27;
    key |= static_cast<uint64_t>(style.valign == VAlign::Middle ? 1 : style.valign == VAlign::Bottom ? 2 : 0) << 29;
    key |= static_cast<uint64_t>(style.letterSpacing ? 1 : 0) << 31;
    return key;
}

} // namespace

Rectf Rectf::ClampedTo(const Rectf& bounds) const {
    Rectf out;
    out.left = std::max(left, bounds.left);
    out.top = std::max(top, bounds.top);
    out.right = std::min(right, bounds.right);
    out.bottom = std::min(bottom, bounds.bottom);
    if (out.right < out.left) out.right = out.left;
    if (out.bottom < out.top) out.bottom = out.top;
    return out;
}

Rectf Rectf::Slice(const Rectf& source, float y, float height) {
    return Rectf{source.left, y, source.right, y + height};
}

Renderer::~Renderer() {
    Destroy();
}

void Renderer::ReleaseDeviceResources() {
    brushes_.clear();
    target_.Reset();
    clipDepth_ = 0;
}

void Renderer::Destroy() {
    ReleaseDeviceResources();
    formats_.clear();
    write_.Reset();
    factory_.Reset();
}

bool Renderer::Create(HWND hwnd) {
    hwnd_ = hwnd;
    if (factory_ && write_) return true;

    if (!factory_) {
        D2D1_FACTORY_OPTIONS options{};
#if defined(_DEBUG)
        options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
        if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
                                       &options, reinterpret_cast<void**>(factory_.GetAddressOf())))) {
            paths::AppendLog(L"Renderer: D2D1CreateFactory falló");
            return false;
        }
    }
    if (!write_) {
        if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(write_.GetAddressOf())))) {
            paths::AppendLog(L"Renderer: DWriteCreateFactory falló");
            return false;
        }
    }

    // ¿Existe una familia monoespaciada instalada? Se decide una sola vez por proceso.
    ComPtr<IDWriteFontCollection> collection;
    if (SUCCEEDED(write_->GetSystemFontCollection(collection.GetAddressOf(), FALSE))) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(collection->FindFamilyName(theme::kFontMono, &index, &exists)) && exists) {
            monoAvailable_ = true;
        } else if (SUCCEEDED(collection->FindFamilyName(theme::kFontMonoFallback, &index, &exists)) && exists) {
            monoAvailable_ = true;
        }
    }
    return CreateDeviceResources();
}

bool Renderer::CreateDeviceResources() {
    if (hwnd_ == nullptr || !factory_ || !write_) return false;

    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const UINT width = static_cast<UINT>(std::max<LONG>(0, client.right - client.left));
    const UINT height = static_cast<UINT>(std::max<LONG>(0, client.bottom - client.top));
    if (width == 0 || height == 0) return false;

    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        0.0f, 0.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProperties = D2D1::HwndRenderTargetProperties(
        hwnd_, D2D1::SizeU(width, height), D2D1_PRESENT_OPTIONS_NONE);

    if (FAILED(factory_->CreateHwndRenderTarget(properties, hwndProperties, target_.GetAddressOf()))) {
        paths::AppendLog(L"Renderer: CreateHwndRenderTarget falló");
        target_.Reset();
        return false;
    }

    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    brushes_.clear();
    clipDepth_ = 0;
    return true;
}

bool Renderer::Resize(UINT widthPx, UINT heightPx) {
    if (!target_) return CreateDeviceResources();
    if (widthPx == 0 || heightPx == 0) return true;
    // Solo se re-crean los recursos si el tamaño cambió de verdad: redimensionar en cada
    // frame de la animación de despliegue costaría milisegundos innecesarios.
    const D2D1_SIZE_U current = target_->GetPixelSize();
    if (current.width == widthPx && current.height == heightPx) return true;
    const HRESULT hr = target_->Resize(D2D1::SizeU(widthPx, heightPx));
    if (FAILED(hr)) {
        ReleaseDeviceResources();
        return CreateDeviceResources();
    }
    return true;
}

bool Renderer::BeginFrame() {
    if (!target_) {
        if (!CreateDeviceResources()) return false;
    }
    const D2D1_SIZE_U size = target_->GetPixelSize();
    if (size.width == 0 || size.height == 0) return false;

    // Estado del dispositivo: tras perder el contexto (bloqueo de sesión, cambio de GPU)
    // D2D devuelve D2DERR_RECREATE_TARGET y hay que reconstruir el render target.
    const float dpi = static_cast<float>(::GetDpiForWindow(hwnd_));
    const float scale = theme::Dip(dpi > 0.0f ? dpi : 96.0f);
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    clipDepth_ = 0;
    return true;
}

void Renderer::EndFrame() {
    if (!target_) return;
    while (clipDepth_ > 0) {
        target_->PopAxisAlignedClip();
        --clipDepth_;
    }
    const HRESULT hr = target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        // La GPU o la sesión cambiaron bajo nuestros pies: se reconstruye en el próximo frame.
        ReleaseDeviceResources();
    }
}

void Renderer::Clear(theme::Rgba color) {
    if (target_) target_->Clear(D2D1::ColorF(color.r, color.g, color.b, color.a));
}

ID2D1SolidColorBrush* Renderer::BrushFor(theme::Rgba color) {
    if (!target_) return nullptr;
    const uint64_t key = ColorKey(color);
    const auto it = brushes_.find(key);
    if (it != brushes_.end()) return it->second.Get();

    ComPtr<ID2D1SolidColorBrush> brush;
    const D2D1_COLOR_F d2dColor = D2D1::ColorF(color.r, color.g, color.b, color.a);
    if (FAILED(target_->CreateSolidColorBrush(d2dColor, brush.GetAddressOf()))) return nullptr;
    ID2D1SolidColorBrush* raw = brush.Get();
    brushes_.emplace(key, std::move(brush));
    return raw;
}

IDWriteTextFormat* Renderer::FormatFor(const TextStyle& style) const {
    if (!write_) return nullptr;
    const uint64_t key = StyleKey(style);
    const auto it = formats_.find(key);
    if (it != formats_.end()) return it->second.Get();

    const wchar_t* family = style.font == FontRole::Mono
                                ? (monoAvailable_ ? theme::kFontMono : theme::kFontMonoFallback)
                                : theme::kFontUi;

    ComPtr<IDWriteTextFormat> format;
    if (FAILED(write_->CreateTextFormat(family, nullptr,
                                        static_cast<DWRITE_FONT_WEIGHT>(style.weight),
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        style.size, L"es-ES", format.GetAddressOf()))) {
        return nullptr;
    }

    format->SetWordWrapping(style.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    switch (style.align) {
        case TextAlign::Left: format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING); break;
        case TextAlign::Center: format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); break;
        case TextAlign::Right: format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING); break;
    }
    switch (style.valign) {
        case VAlign::Top: format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR); break;
        case VAlign::Middle: format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); break;
        case VAlign::Bottom: format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_FAR); break;
    }

    if (style.ellipsis && !style.wrap) {
        DWRITE_TRIMMING trimming{};
        trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        trimming.delimiter = 0;
        trimming.delimiterCount = 0;
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(write_->CreateEllipsisTrimmingSign(format.Get(), sign.GetAddressOf()))) {
            format->SetTrimming(&trimming, sign.Get());
        }
    }

    IDWriteTextFormat* raw = format.Get();
    formats_.emplace(key, std::move(format));
    return raw;
}

void Renderer::FillRect(const Rectf& rect, theme::Rgba color, float radius) {
    if (!target_ || rect.IsEmpty()) return;
    ID2D1SolidColorBrush* brush = BrushFor(color);
    if (brush == nullptr) return;
    if (radius <= 0.25f) {
        target_->FillRectangle(rect.ToD2D(), brush);
    } else {
        target_->FillRoundedRectangle(D2D1::RoundedRect(rect.ToD2D(), radius, radius), brush);
    }
}

void Renderer::StrokeRect(const Rectf& rect, theme::Rgba color, float radius, float strokeWidth) {
    if (!target_ || rect.IsEmpty()) return;
    ID2D1SolidColorBrush* brush = BrushFor(color);
    if (brush == nullptr) return;
    const float inset = strokeWidth * 0.5f;
    const D2D1_RECT_F box = D2D1::RectF(rect.left + inset, rect.top + inset,
                                        rect.right - inset, rect.bottom - inset);
    if (radius <= 0.25f) {
        target_->DrawRectangle(box, brush, strokeWidth);
    } else {
        const float r = std::max(0.0f, radius - inset);
        target_->DrawRoundedRectangle(D2D1::RoundedRect(box, r, r), brush, strokeWidth);
    }
}

void Renderer::DrawLine(float x1, float y1, float x2, float y2, theme::Rgba color, float strokeWidth) {
    if (!target_) return;
    ID2D1SolidColorBrush* brush = BrushFor(color);
    if (brush == nullptr) return;
    target_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush, strokeWidth);
}

void Renderer::FillCircle(float centerX, float centerY, float radius, theme::Rgba color) {
    if (!target_ || radius <= 0.0f) return;
    ID2D1SolidColorBrush* brush = BrushFor(color);
    if (brush == nullptr) return;
    const D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(centerX, centerY), radius, radius);
    target_->FillEllipse(ellipse, brush);
}

void Renderer::DrawText(std::wstring_view text, const Rectf& rect, const TextStyle& style, theme::Rgba color) {
    if (!target_ || text.empty() || rect.IsEmpty()) return;
    IDWriteTextFormat* format = FormatFor(style);
    if (format == nullptr) return;
    if (color.a < 0.5f) return;   // texto casi invisible: se evita la llamada a DWrite

    ID2D1SolidColorBrush* brush = BrushFor(color);
    if (brush == nullptr) return;

    const bool simpleCase = !style.wrap && style.valign == VAlign::Top && !style.letterSpacing;
    if (simpleCase) {
        target_->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format, rect.ToD2D(), brush,
                           D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        return;
    }

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(write_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format,
                                        rect.Width(), rect.Height(), layout.GetAddressOf()))) {
        return;
    }
    layout->SetTextAlignment(format->GetTextAlignment());
    layout->SetParagraphAlignment(format->GetParagraphAlignment());
    if (style.letterSpacing) {
        ComPtr<IDWriteTextLayout1> layout1;
        if (SUCCEEDED(layout.As(&layout1))) {
            const DWRITE_TEXT_RANGE range{0, static_cast<UINT32>(text.size())};
            layout1->SetCharacterSpacing(1.2f, 1.2f, 0.0f, range);
        }
    }
    target_->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), brush,
                            D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Renderer::MeasureHeight(std::wstring_view text, float width, const TextStyle& style) const {
    if (!write_ || text.empty() || width <= 0.0f) return 0.0f;
    IDWriteTextFormat* format = FormatFor(style);
    if (format == nullptr) return 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(write_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, width,
                                        4096.0f, layout.GetAddressOf()))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return 0.0f;
    return metrics.height;
}

void Renderer::PushClip(const Rectf& rect) {
    if (!target_ || rect.IsEmpty()) return;
    target_->PushAxisAlignedClip(rect.ToD2D(), D2D1_ANTIALIAS_MODE_ALIASED);
    ++clipDepth_;
}

void Renderer::PopClip() {
    if (!target_ || clipDepth_ == 0) return;
    target_->PopAxisAlignedClip();
    --clipDepth_;
}

} // namespace edgedock::ui
