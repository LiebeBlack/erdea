#pragma once
// EdgeDock Studio :: ui/Widgets.h
// GUI en modo inmediato dibujada a mano sobre los pinceles de Direct2D: un único pase de
// dibujo resuelve hit-testing, hover animado, pulsación, foco y arrastre. Sin jerarquía de
// objetos, sin std::function, sin asignaciones de heap por frame (las etiquetas se pasan por
// std::wstring_view y los números se formatean en la arena del frame).

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/PmrPool.h"
#include "ui/Renderer.h"

namespace edgedock::ui {

enum class IconGlyph {
    None = 0,
    Pin,
    PinFilled,
    ChevronDown,
    ChevronRight,
    Search,
    Close,
    Plus,
    Clipboard,
    TextDoc,
    Gauge,
    Power,
    Folder,
    File,
    Link,
    Trash,
    Dot,
};

struct InputState {
    float x = -1.0f;
    float y = -1.0f;
    bool inside = false;
    bool down = false;
    bool pressed = false;      // transición de arriba a abajo en este frame
    bool released = false;     // transición de abajo a arriba en este frame
    bool doubleClick = false;
    int wheelDelta = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

enum class ButtonKind { Ghost, Filled, Accent };

struct ButtonStyle {
    ButtonKind kind = ButtonKind::Ghost;
    TextStyle text{};
    IconGlyph icon = IconGlyph::None;
    float radius = 5.0f;
    bool showText = true;
};

// Pintor de filas sin asignaciones: puntero a función + contexto opaco.
using RowPainter = void (*)(void* context, Renderer& renderer, std::size_t index, const Rectf& rect,
                            float hover, bool selected);

struct ListRowStyle {
    float rowHeight = 74.0f;
    float gap = 6.0f;
    float radius = 8.0f;
};

struct ListViewState {
    float offset = 0.0f;
    float target = 0.0f;
    float maximum = 0.0f;
    int hovered = -1;
    bool draggingThumb = false;
};

struct ListViewResult {
    bool clicked = false;
    std::size_t clickedIndex = 0;
    bool doubleClicked = false;
    std::size_t doubleClickedIndex = 0;
    int hoveredIndex = -1;
    float rowStride = 0.0f;
    bool thumbActive = false;
};

class UiState {
public:
    void Begin(float deltaSeconds, const InputState& input);
    void End();

    const InputState& Input() const { return input_; }
    float DeltaSeconds() const { return delta_; }

    // --- Interacción básica -------------------------------------------------------
    bool Hover(int id, const Rectf& rect);
    bool Clicked(int id, const Rectf& rect, bool enabled = true);
    bool Held(int id, const Rectf& rect, bool enabled = true);
    bool DoubleClicked(int id, const Rectf& rect, bool enabled = true);
    float HoverAmount(int id, bool hoveredValue, float speed = 14.0f);
    float HoverAmount(int id, const Rectf& rect, float speed = 14.0f);

    bool Focus(int id) const { return focused_ == id; }
    void SetFocus(int id) { focused_ = id; }
    void ClearFocus() { focused_ = 0; }
    bool HasFocus() const { return focused_ != 0; }
    bool HasHot() const { return hot_ != 0; }
    int ActiveId() const { return active_; }
    void ClearInteraction();

    // --- Widgets ------------------------------------------------------------------
    bool Button(Renderer& renderer, int id, const Rectf& rect, std::wstring_view label,
                const theme::Palette& palette, const ButtonStyle& style = ButtonStyle{}, bool enabled = true);
    bool IconButton(Renderer& renderer, int id, const Rectf& rect, IconGlyph glyph,
                    const theme::Palette& palette, theme::Rgba tint, bool active = false,
                    const wchar_t* tooltip = nullptr);
    bool ToggleChip(Renderer& renderer, int id, const Rectf& rect, std::wstring_view label, bool value,
                    const theme::Palette& palette);
    bool ToggleSwitch(Renderer& renderer, int id, const Rectf& rect, bool& value,
                      const theme::Palette& palette);
    void Label(Renderer& renderer, const Rectf& rect, std::wstring_view text, const TextStyle& style,
               theme::Rgba color);
    void SectionTitle(Renderer& renderer, const Rectf& rect, std::wstring_view text,
                      const theme::Palette& palette);
    void Separator(Renderer& renderer, const Rectf& rect, theme::Rgba color);
    void ProgressBar(Renderer& renderer, const Rectf& rect, double ratio, const theme::Palette& palette,
                     std::wstring_view caption = std::wstring_view());
    void Badge(Renderer& renderer, const Rectf& rect, std::wstring_view label, theme::Rgba color);

    // Lista virtualizada con filas de altura uniforme: sólo se pinta lo visible, el pintor
    // recibe el índice y el rectángulo ya desplazado. Consume la rueda del ratón.
    ListViewResult ScrollList(Renderer& renderer, int id, const Rectf& bounds, const ListRowStyle& style,
                              std::size_t itemCount, ListViewState& state, RowPainter painter,
                              void* context, int selectedIndex, const theme::Palette& palette);

    // --- Ayudas de presentación ---------------------------------------------------
    static std::wstring FormatCount(size_t value);
    static std::wstring FormatBytes(uint64_t bytes);
    static std::wstring FormatDuration(double seconds);
    static void DrawGlyph(Renderer& renderer, IconGlyph glyph, float centerX, float centerY, float size,
                          theme::Rgba color, bool filled = false);
    static void DrawNeonFrame(Renderer& renderer, const Rectf& rect, const theme::Palette& palette,
                              float highlight, float radius, bool filled = false);
    static void DrawChevron(Renderer& renderer, float centerX, float centerY, float size,
                            bool pointingDown, theme::Rgba color);
    static void DrawPin(Renderer& renderer, float centerX, float centerY, float size, theme::Rgba color,
                        bool filled);
    static void DrawKindBadge(Renderer& renderer, const Rectf& rect, std::wstring_view label,
                              theme::Rgba color);

private:
    InputState input_{};
    float delta_ = 0.0f;
    bool hotClaimed_ = false;
    int hot_ = 0;
    int active_ = 0;
    int focused_ = 0;
    int pressedId_ = 0;
    int releasedId_ = 0;
    std::unordered_map<int, float> hover_;
};

} // namespace edgedock::ui
