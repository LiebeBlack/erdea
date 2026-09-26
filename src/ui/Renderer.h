#pragma once
// EdgeDock Studio :: ui/Renderer.h
// Envoltura mínima sobre Direct2D 1.0 + DirectWrite: brochas de color y formatos de texto
// cacheados, recorte por pila y primitivas geométricas. Cero asignaciones por frame salvo
// la primera vez que aparece un color o un estilo.

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ui/Theme.h"

namespace edgedock::ui {

using Microsoft::WRL::ComPtr;

struct Rectf {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    float Width() const { return right > left ? right - left : 0.0f; }
    float Height() const { return bottom > top ? bottom - top : 0.0f; }
    float CenterX() const { return (left + right) * 0.5f; }
    float CenterY() const { return (top + bottom) * 0.5f; }
    bool Contains(float x, float y) const { return x >= left && x < right && y >= top && y < bottom; }
    bool IsEmpty() const { return Width() <= 0.0f || Height() <= 0.0f; }
    Rectf Inset(float dx, float dy) const { return Rectf{left + dx, top + dy, right - dx, bottom - dy}; }
    Rectf Offset(float dx, float dy) const { return Rectf{left + dx, top + dy, right + dx, bottom + dy}; }
    Rectf Shifted(float dy) const { return Offset(0.0f, dy); }
    Rectf ClampedTo(const Rectf& bounds) const;
    D2D1_RECT_F ToD2D() const { return D2D1::RectF(left, top, right, bottom); }
    static Rectf FromSize(float width, float height) { return Rectf{0.0f, 0.0f, width, height}; }
    static Rectf Slice(const Rectf& source, float y, float height);
};

enum class FontRole { Ui, Mono };
enum class TextAlign { Left, Center, Right };
enum class VAlign { Top, Middle, Bottom };

struct TextStyle {
    float size = 13.0f;
    int weight = 400;                 // 300 light, 400 normal, 600 semibold, 700 bold
    FontRole font = FontRole::Ui;
    bool wrap = false;
    bool ellipsis = true;
    TextAlign align = TextAlign::Left;
    VAlign valign = VAlign::Top;
    bool letterSpacing = false;       // tracking amplio para títulos en mayúsculas
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Create(HWND hwnd);
    void Destroy();
    bool Ready() const { return target_ != nullptr; }
    void SetPalette(const theme::Palette& palette) { palette_ = palette; }
    const theme::Palette& Palette() const { return palette_; }

    bool Resize(UINT widthPx, UINT heightPx);
    bool BeginFrame();
    void EndFrame();

    // --- Primitivas -------------------------------------------------------------
    void Clear(theme::Rgba color);
    void FillRect(const Rectf& rect, theme::Rgba color, float radius = 0.0f);
    void StrokeRect(const Rectf& rect, theme::Rgba color, float radius = 0.0f, float strokeWidth = 1.0f);
    void DrawLine(float x1, float y1, float x2, float y2, theme::Rgba color, float strokeWidth = 1.0f);
    void FillCircle(float centerX, float centerY, float radius, theme::Rgba color);
    void DrawText(std::wstring_view text, const Rectf& rect, const TextStyle& style, theme::Rgba color);
    float MeasureHeight(std::wstring_view text, float width, const TextStyle& style) const;
    bool HasMonospace() const { return monoAvailable_; }

    // --- Recorte ----------------------------------------------------------------
    void PushClip(const Rectf& rect);
    void PopClip();
    size_t ClipDepth() const { return clipDepth_; }

private:
    ID2D1SolidColorBrush* BrushFor(theme::Rgba color);
    IDWriteTextFormat* FormatFor(const TextStyle& style) const;
    void ReleaseDeviceResources();
    bool CreateDeviceResources();

    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> write_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    theme::Palette palette_{};

    HWND hwnd_ = nullptr;
    mutable std::unordered_map<uint64_t, ComPtr<IDWriteTextFormat>> formats_;
    std::unordered_map<uint64_t, ComPtr<ID2D1SolidColorBrush>> brushes_;
    size_t clipDepth_ = 0;
    bool monoAvailable_ = false;
    bool comInitialized_ = false;
};

} // namespace edgedock::ui
