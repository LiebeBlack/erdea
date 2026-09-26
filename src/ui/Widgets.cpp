// EdgeDock Studio :: ui/Widgets.cpp
#include "ui/Widgets.h"

#include "core/TextConv.h"

#include <algorithm>
#include <cmath>

namespace edgedock::ui {

// -----------------------------------------------------------------------------
// Estado de interacción
// -----------------------------------------------------------------------------

void UiState::Begin(float deltaSeconds, const InputState& input) {
    delta_ = deltaSeconds < 0.0f ? 0.0f : (deltaSeconds > 0.25f ? 0.25f : deltaSeconds);
    input_ = input;
    hotClaimed_ = false;
    hot_ = 0;
    if (!input_.down) {
        // Sin botón pulsado no puede haber widget activo: evita arrastres fantasma cuando la
        // liberación ocurre fuera de la ventana.
        active_ = 0;
    }
    if (input_.pressed) releasedId_ = 0;
}

void UiState::End() {
    if (hover_.size() > 256) {
        for (auto it = hover_.begin(); it != hover_.end();) {
            if (it->second < 0.001f) {
                it = hover_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

bool UiState::Hover(int id, const Rectf& rect) {
    if (hotClaimed_ || !input_.inside) return false;
    if (!rect.Contains(input_.x, input_.y)) return false;
    hot_ = id;
    hotClaimed_ = true;
    return true;
}

bool UiState::Clicked(int id, const Rectf& rect, bool enabled) {
    if (!enabled) return false;
    const bool hovered = input_.inside && rect.Contains(input_.x, input_.y);
    if (input_.pressed && hovered) active_ = id;
    if (input_.released) {
        const bool wasActive = active_ == id;
        if (wasActive) active_ = 0;
        if (wasActive && hovered) {
            releasedId_ = id;
            return true;
        }
    }
    return false;
}

bool UiState::Held(int id, const Rectf& rect, bool enabled) {
    if (!enabled) return false;
    if (input_.down && input_.inside && rect.Contains(input_.x, input_.y)) {
        active_ = id;
        return true;
    }
    return false;
}

bool UiState::DoubleClicked(int id, const Rectf& rect, bool enabled) {
    if (!enabled || !input_.doubleClick) return false;
    return input_.inside && rect.Contains(input_.x, input_.y);
}

float UiState::HoverAmount(int id, bool hoveredValue, float speed) {
    auto it = hover_.find(id);
    if (it == hover_.end()) {
        it = hover_.emplace(id, hoveredValue ? 1.0f : 0.0f).first;
        return it->second;
    }
    const float target = hoveredValue ? 1.0f : 0.0f;
    if (delta_ <= 0.0f) {
        it->second = target;
        return it->second;
    }
    const float factor = 1.0f - std::exp(-speed * delta_);
    it->second += (target - it->second) * factor;
    if (it->second < 0.0005f) it->second = 0.0f;
    if (it->second > 0.9995f) it->second = 1.0f;
    return it->second;
}

float UiState::HoverAmount(int id, const Rectf& rect, float speed) {
    return HoverAmount(id, Hover(id, rect), speed);
}

void UiState::ClearInteraction() {
    active_ = 0;
    hotClaimed_ = false;
    hover_.clear();
}

// -----------------------------------------------------------------------------
// Widgets
// -----------------------------------------------------------------------------

bool UiState::Button(Renderer& renderer, int id, const Rectf& rect, std::wstring_view label,
                     const theme::Palette& palette, const ButtonStyle& style, bool enabled) {
    if (!enabled) {
        renderer.FillRect(rect, theme::WithAlpha(theme::kWhite, 0.02f), style.radius);
        if (style.showText) {
            renderer.DrawText(label, rect, style.text, theme::WithAlpha(theme::kMuted, 0.65f));
        }
        return false;
    }

    const bool clicked = Clicked(id, rect, true);
    const float highlight = HoverAmount(id, rect, 20.0f);
    const bool pressed = active_ == id;

    switch (style.kind) {
        case ButtonKind::Accent: {
            const theme::Rgba fill = theme::Mix(theme::WithAlpha(palette.accent, 0.14f),
                                                theme::WithAlpha(palette.accent, (pressed ? 0.42f : 0.30f)),
                                                highlight);
            renderer.FillRect(rect, fill, style.radius);
            renderer.StrokeRect(rect, theme::Mix(theme::WithAlpha(palette.accent, 0.55f), palette.accent, highlight),
                                style.radius, 1.0f);
            break;
        }
        case ButtonKind::Filled: {
            renderer.FillRect(rect, theme::Mix(theme::kSurface, theme::kSurfaceHot, highlight), style.radius);
            renderer.StrokeRect(rect,
                                theme::Mix(theme::kHairline, theme::WithAlpha(palette.accent, 0.7f), highlight),
                                style.radius, 1.0f);
            break;
        }
        case ButtonKind::Ghost:
        default: {
            if (highlight > 0.01f) {
                renderer.FillRect(rect, theme::WithAlpha(palette.accent, 0.08f * highlight), style.radius);
            }
            renderer.StrokeRect(rect,
                                theme::Mix(theme::kHairline, theme::WithAlpha(palette.accent, 0.55f), highlight),
                                style.radius, 1.0f);
            break;
        }
    }

    const Rectf content = pressed ? rect.Offset(0.0f, 1.0f) : rect;
    const theme::Rgba textColor = style.kind == ButtonKind::Accent
                                      ? theme::Mix(palette.accent, theme::kWhite, 0.22f * highlight)
                                      : theme::Mix(theme::kTextSoft, palette.accent, highlight);

    if (style.icon != IconGlyph::None) {
        DrawGlyph(renderer, style.icon, content.left + 15.0f, content.CenterY(), 13.0f, textColor,
                  style.kind == ButtonKind::Accent);
        if (style.showText) {
            renderer.DrawText(label,
                              Rectf{content.left + 26.0f, content.top, content.right - 6.0f, content.bottom},
                              style.text, textColor);
        }
    } else if (style.showText) {
        renderer.DrawText(label, content, style.text, textColor);
    }
    return clicked;
}

bool UiState::IconButton(Renderer& renderer, int id, const Rectf& rect, IconGlyph glyph,
                         const theme::Palette& palette, theme::Rgba tint, bool active,
                         const wchar_t* tooltip) {
    (void)tooltip;
    const bool clicked = Clicked(id, rect);
    const float highlight = HoverAmount(id, rect, 24.0f);
    const bool pressed = this->active_ == id;

    if (active || highlight > 0.01f) {
        const theme::Rgba fill = active ? theme::WithAlpha(palette.accent, 0.18f)
                                        : theme::WithAlpha(theme::kWhite, 0.03f + 0.07f * highlight);
        renderer.FillRect(rect, fill, 4.0f);
    }
    const theme::Rgba color = theme::Mix(tint, palette.accent, active ? 1.0f : highlight);
    const float size = std::min(rect.Width(), rect.Height()) * 0.58f;
    DrawGlyph(renderer, glyph, rect.CenterX(), rect.CenterY() + (pressed ? 1.0f : 0.0f), size, color, active);
    return clicked;
}

bool UiState::ToggleChip(Renderer& renderer, int id, const Rectf& rect, std::wstring_view label, bool value,
                         const theme::Palette& palette) {
    ButtonStyle style;
    style.kind = value ? ButtonKind::Accent : ButtonKind::Ghost;
    style.text = TextStyle{};
    style.text.size = 10.0f;
    style.text.weight = 600;
    style.text.align = TextAlign::Center;
    style.text.valign = VAlign::Middle;
    style.radius = 4.0f;
    return Button(renderer, id, rect, label, palette, style, true);
}

bool UiState::ToggleSwitch(Renderer& renderer, int id, const Rectf& rect, bool& value,
                           const theme::Palette& palette) {
    const bool clicked = Clicked(id, rect);
    if (clicked) value = !value;

    const float position = HoverAmount(id, value, 14.0f);
    const float radius = rect.Height() * 0.5f;
    renderer.FillRect(rect,
                      theme::Mix(theme::WithAlpha(theme::kWhite, 0.08f),
                                 theme::WithAlpha(palette.accent, 0.32f), position),
                      radius);
    renderer.StrokeRect(rect, theme::Mix(theme::kHairline, palette.accent, position), radius, 1.0f);

    const float knobRadius = radius - 3.0f;
    const float travel = rect.Width() - rect.Height();
    const float knobX = rect.left + radius + travel * position;
    renderer.FillCircle(knobX, rect.CenterY(), knobRadius, theme::Mix(theme::kMuted, palette.accent, position));
    return clicked;
}

void UiState::Label(Renderer& renderer, const Rectf& rect, std::wstring_view text, const TextStyle& style,
                    theme::Rgba color) {
    renderer.DrawText(text, rect, style, color);
}

void UiState::SectionTitle(Renderer& renderer, const Rectf& rect, std::wstring_view text,
                           const theme::Palette& palette) {
    TextStyle style;
    style.size = 9.5f;
    style.weight = 700;
    style.letterSpacing = true;
    renderer.DrawText(text, rect, style, palette.accent);
}

void UiState::Separator(Renderer& renderer, const Rectf& rect, theme::Rgba color) {
    renderer.FillRect(rect, color);
}

void UiState::ProgressBar(Renderer& renderer, const Rectf& rect, double ratio, const theme::Palette& palette,
                          std::wstring_view caption) {
    renderer.FillRect(rect, theme::WithAlpha(theme::kWhite, 0.06f), 2.0f);
    const float clamped = static_cast<float>(std::clamp(ratio, 0.0, 1.0));
    const float filled = clamped * rect.Width();
    if (filled > 1.0f) {
        renderer.FillRect(Rectf{rect.left, rect.top, rect.left + filled, rect.bottom}, palette.accent, 2.0f);
    }
    if (!caption.empty()) {
        TextStyle style;
        style.size = 9.5f;
        style.font = FontRole::Mono;
        style.align = TextAlign::Right;
        style.valign = VAlign::Middle;
        renderer.DrawText(caption, Rectf{rect.left, rect.top - 14.0f, rect.right, rect.top - 1.0f}, style,
                          theme::kMuted);
    }
}

void UiState::Badge(Renderer& renderer, const Rectf& rect, std::wstring_view label, theme::Rgba color) {
    DrawKindBadge(renderer, rect, label, color);
}

// -----------------------------------------------------------------------------
// Formatos (usan la arena del frame: cero heap en el camino de dibujo)
// -----------------------------------------------------------------------------

std::wstring UiState::FormatCount(size_t value) {
    if (value < 1000) return text::Format(L"%llu", static_cast<unsigned long long>(value));
    if (value < 1000000) return text::Format(L"%.1fk", static_cast<double>(value) / 1000.0);
    return text::Format(L"%.1fM", static_cast<double>(value) / 1000000.0);
}

std::wstring UiState::FormatBytes(uint64_t bytes) {
    constexpr double kUnit = 1024.0;
    if (bytes < static_cast<uint64_t>(kUnit)) return text::Format(L"%llu B", static_cast<unsigned long long>(bytes));
    const double kib = static_cast<double>(bytes) / kUnit;
    if (kib < kUnit) return text::Format(L"%.1f KB", kib);
    const double mib = kib / kUnit;
    if (mib < kUnit) return text::Format(L"%.1f MB", mib);
    return text::Format(L"%.2f GB", mib / kUnit);
}

std::wstring UiState::FormatDuration(double seconds) {
    if (seconds < 1.0) return L"<1 s";
    if (seconds < 60.0) return text::Format(L"%.0f s", seconds);
    if (seconds < 3600.0) return text::Format(L"%d min", static_cast<int>(seconds / 60.0));
    const int hours = static_cast<int>(seconds / 3600.0);
    const int minutes = static_cast<int>((seconds - hours * 3600.0) / 60.0);
    if (hours < 24) return text::Format(L"%d h %d min", hours, minutes);
    return text::Format(L"%.1f d", seconds / 86400.0);
}

// -----------------------------------------------------------------------------
// Lista virtualizada (historial del portapapeles, notas, procesos)
// -----------------------------------------------------------------------------

ListViewResult UiState::ScrollList(Renderer& renderer, int id, const Rectf& bounds, const ListRowStyle& style,
                                   std::size_t itemCount, ListViewState& state, RowPainter painter,
                                   void* context, int selectedIndex, const theme::Palette& palette) {
    ListViewResult result;
    result.rowStride = style.rowHeight + style.gap;
    if (bounds.IsEmpty()) return result;

    const float total = static_cast<float>(itemCount) * result.rowStride;
    state.maximum = std::max(0.0f, total - bounds.Height() + style.gap);

    const bool insideArea = input_.inside && bounds.Contains(input_.x, input_.y);
    if (insideArea && input_.wheelDelta != 0) {
        state.target -= (static_cast<float>(input_.wheelDelta) / 120.0f) * 84.0f;
    }
    state.target = std::clamp(state.target, 0.0f, state.maximum);

    // Suavizado exponencial independiente del frame-rate.
    const float factor = 1.0f - std::exp(-20.0f * std::max(0.001f, delta_));
    state.offset += (state.target - state.offset) * factor;
    if (std::fabs(state.target - state.offset) < 0.5f) state.offset = state.target;
    state.offset = std::clamp(state.offset, 0.0f, state.maximum);

    state.hovered = -1;
    const int firstVisible = static_cast<int>(std::max(0.0f, std::floor(state.offset / result.rowStride)));
    const int visibleRows = static_cast<int>(bounds.Height() / result.rowStride) + 2;
    const int lastVisible = std::min(static_cast<int>(itemCount), firstVisible + visibleRows);
    const int rowIdBase = id * 4096;

    // Sólo se recorren las filas visibles: con 600 clips el coste es constante.
    renderer.PushClip(bounds);
    for (int index = firstVisible; index < lastVisible; ++index) {
        const float top = bounds.top - state.offset + static_cast<float>(index) * result.rowStride;
        const Rectf row{bounds.left, top, bounds.right, top + style.rowHeight};
        if (row.bottom < bounds.top || row.top > bounds.bottom) continue;

        const int rowId = rowIdBase + index;
        const bool hovered = Hover(rowId, row);
        if (hovered) {
            state.hovered = index;
            result.hoveredIndex = index;
        }
        const float highlight = HoverAmount(rowId, hovered, 20.0f);

        if (Clicked(rowId, row)) {
            result.clicked = true;
            result.clickedIndex = static_cast<std::size_t>(index);
        }
        if (DoubleClicked(rowId + 1, row)) {
            result.doubleClicked = true;
            result.doubleClickedIndex = static_cast<std::size_t>(index);
        }

        if (painter != nullptr) {
            painter(context, renderer, static_cast<std::size_t>(index), row, highlight,
                    static_cast<int>(index) == selectedIndex);
        }
    }
    renderer.PopClip();

    if (state.maximum > 1.0f) {
        const float trackHeight = bounds.Height();
        const float thumbHeight =
            std::max(28.0f, trackHeight * (trackHeight / (trackHeight + state.maximum)));
        const float thumbTop = bounds.top + (trackHeight - thumbHeight) * (state.offset / state.maximum);

        renderer.FillRect(Rectf{bounds.right - 4.0f, bounds.top, bounds.right - 2.0f, bounds.bottom},
                          theme::WithAlpha(theme::kWhite, 0.05f));

        const Rectf thumb{bounds.right - 5.0f, thumbTop, bounds.right - 1.0f, thumbTop + thumbHeight};
        if (Held(rowIdBase + 100000, thumb)) state.draggingThumb = true;
        if (state.draggingThumb) {
            const float relative = (input_.y - bounds.top) / std::max(1.0f, trackHeight - thumbHeight);
            state.offset = state.target = std::clamp(relative, 0.0f, 1.0f) * state.maximum;
        }
        if (!input_.down) state.draggingThumb = false;
        result.thumbActive = state.draggingThumb;

        renderer.FillRect(thumb, theme::WithAlpha(palette.accent, state.draggingThumb ? 0.95f : 0.6f), 1.5f);
    }
    return result;
}

// -----------------------------------------------------------------------------
// Glifos vectoriales y estética neón
// -----------------------------------------------------------------------------

void UiState::DrawGlyph(Renderer& renderer, IconGlyph glyph, float cx, float cy, float size,
                        theme::Rgba color, bool filled) {
    const float half = size * 0.5f;
    switch (glyph) {
        case IconGlyph::None:
            break;
        case IconGlyph::Pin:
        case IconGlyph::PinFilled:
            DrawPin(renderer, cx, cy, size, color, filled || glyph == IconGlyph::PinFilled);
            break;
        case IconGlyph::ChevronDown:
            DrawChevron(renderer, cx, cy, size, true, color);
            break;
        case IconGlyph::ChevronRight:
            DrawChevron(renderer, cx, cy, size, false, color);
            break;
        case IconGlyph::Search:
            renderer.StrokeRect(Rectf{cx - half, cy - half, cx + half * 0.45f, cy + half * 0.45f}, color,
                                half, 1.4f);
            renderer.DrawLine(cx + half * 0.35f, cy + half * 0.35f, cx + half, cy + half, color, 1.4f);
            break;
        case IconGlyph::Close:
            renderer.DrawLine(cx - half, cy - half, cx + half, cy + half, color, 1.5f);
            renderer.DrawLine(cx + half, cy - half, cx - half, cy + half, color, 1.5f);
            break;
        case IconGlyph::Plus:
            renderer.DrawLine(cx - half, cy, cx + half, cy, color, 1.6f);
            renderer.DrawLine(cx, cy - half, cx, cy + half, color, 1.6f);
            break;
        case IconGlyph::Clipboard:
            renderer.StrokeRect(Rectf{cx - half, cy - half * 0.7f, cx + half, cy + half}, color, 2.0f, 1.3f);
            renderer.FillRect(Rectf{cx - half * 0.45f, cy - half, cx + half * 0.45f, cy - half * 0.55f}, color,
                              1.5f);
            break;
        case IconGlyph::TextDoc:
        case IconGlyph::File:
            renderer.StrokeRect(Rectf{cx - half * 0.75f, cy - half, cx + half * 0.75f, cy + half}, color, 1.5f,
                                1.3f);
            renderer.DrawLine(cx - half * 0.35f, cy - half * 0.25f, cx + half * 0.35f, cy - half * 0.25f, color, 1.1f);
            renderer.DrawLine(cx - half * 0.35f, cy + half * 0.25f, cx + half * 0.35f, cy + half * 0.25f, color, 1.1f);
            break;
        case IconGlyph::Gauge:
            renderer.DrawLine(cx - half, cy + half * 0.5f, cx - half * 0.5f, cy - half * 0.4f, color, 1.3f);
            renderer.DrawLine(cx - half * 0.5f, cy - half * 0.4f, cx + half * 0.5f, cy - half * 0.4f, color, 1.3f);
            renderer.DrawLine(cx + half * 0.5f, cy - half * 0.4f, cx + half, cy + half * 0.5f, color, 1.3f);
            renderer.DrawLine(cx, cy + half * 0.5f, cx + half * 0.35f, cy - half * 0.15f, color, 1.5f);
            break;
        case IconGlyph::Power:
            renderer.StrokeRect(Rectf{cx - half, cy - half * 0.6f, cx + half, cy + half}, color, half, 1.4f);
            renderer.DrawLine(cx, cy - half, cx, cy + half * 0.15f, color, 1.5f);
            break;
        case IconGlyph::Folder:
            renderer.FillRect(Rectf{cx - half, cy - half * 0.5f, cx + half * 0.2f, cy - half * 0.15f}, color, 1.5f);
            renderer.StrokeRect(Rectf{cx - half, cy - half * 0.35f, cx + half, cy + half * 0.7f}, color, 1.5f, 1.3f);
            break;
        case IconGlyph::Link:
            renderer.StrokeRect(Rectf{cx - half, cy - half * 0.45f, cx, cy + half * 0.45f}, color, half * 0.45f, 1.3f);
            renderer.StrokeRect(Rectf{cx, cy - half * 0.45f, cx + half, cy + half * 0.45f}, color, half * 0.45f, 1.3f);
            renderer.DrawLine(cx - half * 0.2f, cy, cx + half * 0.2f, cy, color, 1.3f);
            break;
        case IconGlyph::Trash:
            renderer.FillRect(Rectf{cx - half, cy - half * 0.6f, cx + half, cy - half * 0.45f}, color, 1.0f);
            renderer.StrokeRect(Rectf{cx - half * 0.7f, cy - half * 0.45f, cx + half * 0.7f, cy + half}, color,
                                1.5f, 1.3f);
            break;
        case IconGlyph::Dot:
            renderer.FillCircle(cx, cy, half * 0.55f, color);
            break;
    }
}

void UiState::DrawNeonFrame(Renderer& renderer, const Rectf& rect, const theme::Palette& palette,
                            float highlight, float radius, bool filled) {
    const float glow = std::clamp(highlight, 0.0f, 1.0f);
    if (filled || glow > 0.0f) {
        const theme::Rgba base = filled ? theme::kSurface : theme::kSurfaceHot;
        renderer.FillRect(rect, theme::Mix(base, palette.accentGhost, glow), radius);
    }
    renderer.StrokeRect(rect, theme::Mix(theme::kHairline, theme::WithAlpha(palette.accent, 0.85f), glow),
                        radius, 1.0f);
}

void UiState::DrawChevron(Renderer& renderer, float centerX, float centerY, float size, bool pointingDown,
                          theme::Rgba color) {
    const float half = size * 0.5f;
    const float dy = pointingDown ? half * 0.5f : -half * 0.5f;
    renderer.DrawLine(centerX - half, centerY - dy, centerX, centerY + dy, color, 1.4f);
    renderer.DrawLine(centerX, centerY + dy, centerX + half, centerY - dy, color, 1.4f);
}

void UiState::DrawPin(Renderer& renderer, float centerX, float centerY, float size, theme::Rgba color,
                      bool filled) {
    const float half = size * 0.5f;
    if (filled) {
        renderer.FillCircle(centerX, centerY - half * 0.35f, half * 0.62f, color);
    } else {
        renderer.FillCircle(centerX, centerY - half * 0.35f, half * 0.62f, theme::WithAlpha(color, 0.25f));
        renderer.StrokeRect(Rectf{centerX - half * 0.62f, centerY - half * 0.97f, centerX + half * 0.62f,
                                 centerY + half * 0.27f},
                            color, half * 0.62f, 1.0f);
    }
    renderer.DrawLine(centerX, centerY, centerX, centerY + half, color, 1.4f);
}

void UiState::DrawKindBadge(Renderer& renderer, const Rectf& rect, std::wstring_view label,
                            theme::Rgba color) {
    renderer.FillRect(rect, theme::WithAlpha(color, 0.10f), 3.0f);
    TextStyle style;
    style.size = 9.0f;
    style.weight = 600;
    style.align = TextAlign::Center;
    style.valign = VAlign::Middle;
    style.letterSpacing = true;
    renderer.DrawText(label, rect, style, color);
}

} // namespace edgedock::ui
