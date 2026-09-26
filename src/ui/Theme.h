#pragma once
// EdgeDock Studio :: ui/Theme.h
// Paleta OLED (negro puro 0,0,0), acentos neón cian (0,255,255) y púrpura (160,32,240).

#include <string>

namespace edgedock::theme {

struct Rgba {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

inline constexpr Rgba Rgba8(int r, int g, int b, float a = 1.0f) {
    return Rgba{static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                static_cast<float>(b) / 255.0f, a};
}

inline constexpr Rgba WithAlpha(Rgba color, float alpha) {
    return Rgba{color.r, color.g, color.b, alpha};
}

inline constexpr Rgba Mix(Rgba a, Rgba b, float t) {
    return Rgba{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                a.a + (b.a - a.a) * t};
}

// --- OLED puro -----------------------------------------------------------------
inline constexpr Rgba kBlack = Rgba8(0, 0, 0, 1.0f);
inline constexpr Rgba kBackdrop = Rgba8(0, 0, 0, 1.0f);
inline constexpr Rgba kPanel = Rgba8(2, 3, 4, 0.985f);
inline constexpr Rgba kPanelAlt = Rgba8(6, 8, 10, 1.0f);
inline constexpr Rgba kSurface = Rgba8(10, 12, 14, 1.0f);
inline constexpr Rgba kSurfaceHot = Rgba8(16, 20, 24, 1.0f);

// --- Acentos -------------------------------------------------------------------
inline constexpr Rgba kCyan = Rgba8(0, 255, 255, 1.0f);
inline constexpr Rgba kPurple = Rgba8(160, 32, 240, 1.0f);
inline constexpr Rgba kAmber = Rgba8(255, 176, 0, 1.0f);

// --- Texto ---------------------------------------------------------------------
inline constexpr Rgba kText = Rgba8(236, 242, 246, 1.0f);
inline constexpr Rgba kTextSoft = Rgba8(168, 178, 190, 1.0f);
inline constexpr Rgba kMuted = Rgba8(104, 114, 128, 1.0f);
inline constexpr Rgba kWhite = Rgba8(255, 255, 255, 1.0f);
inline constexpr Rgba kWarn = Rgba8(255, 96, 96, 1.0f);
inline constexpr Rgba kOk = Rgba8(90, 240, 170, 1.0f);

// --- Líneas y superficies translúcidas -----------------------------------------
inline constexpr Rgba kHairline = Rgba8(255, 255, 255, 0.055f);
inline constexpr Rgba kRowHover = Rgba8(255, 255, 255, 0.045f);
inline constexpr Rgba kRowSelected = Rgba8(255, 255, 255, 0.085f);
inline constexpr Rgba kScrim = Rgba8(0, 0, 0, 0.55f);

// --- Métricas (DIP) ------------------------------------------------------------
inline constexpr float kPad = 14.0f;
inline constexpr float kPadSmall = 8.0f;
inline constexpr float kRowHeight = 74.0f;
inline constexpr float kRowGap = 6.0f;
inline constexpr float kHeaderHeight = 54.0f;
inline constexpr float kTabsHeight = 38.0f;
inline constexpr float kSearchHeight = 34.0f;
inline constexpr float kStatusHeight = 30.0f;
inline constexpr float kCorner = 12.0f;
inline constexpr float kCornerSmall = 6.0f;
inline constexpr float kScrollBarWidth = 4.0f;

// --- Tipografía ----------------------------------------------------------------
inline constexpr wchar_t kFontUi[] = L"Segoe UI";
inline constexpr wchar_t kFontMono[] = L"Cascadia Mono";
inline constexpr wchar_t kFontMonoFallback[] = L"Consolas";

struct Palette {
    Rgba accent = kCyan;        // Acento principal
    Rgba accentSoft = Mix(kCyan, kBlack, 0.55f);
    Rgba accentGhost = WithAlpha(kCyan, 0.18f);
    Rgba secondary = kPurple;   // Acento secundario
    Rgba secondaryGhost = WithAlpha(kPurple, 0.20f);
};

inline Palette PaletteFor(const std::wstring& name) {
    Palette palette;
    if (name == L"purple") {
        palette.accent = kPurple;
        palette.secondary = kCyan;
    } else if (name == L"amber") {
        palette.accent = kAmber;
        palette.secondary = kCyan;
    }
    palette.accentSoft = Mix(palette.accent, kBlack, 0.55f);
    palette.accentGhost = WithAlpha(palette.accent, 0.18f);
    palette.secondaryGhost = WithAlpha(palette.secondary, 0.20f);
    return palette;
}

// Escala según DPI: mantiene el panel idéntico en píxeles físicos en pantallas 1x-2x.
inline constexpr float Dip(float dpi) { return dpi / 96.0f; }

} // namespace edgedock::theme
