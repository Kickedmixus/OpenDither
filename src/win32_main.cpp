#include <windows.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core.hpp"

namespace {

constexpr int kPanelWidth = 340;
constexpr int kMargin = 16;
constexpr int kSliderWidth = 290;
constexpr int kSliderHeight = 18;
constexpr int kButtonHeight = 28;
constexpr int kFooterHeight = 28;
constexpr int kSwatchSize = 24;
constexpr UINT kImportDoneMessage = WM_APP + 1;

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool contains(int px, int py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

struct Slider {
    std::string label;
    double* value = nullptr;
    double min = 0.0;
    double max = 1.0;
    Rect rect;
};

struct Button {
    std::string label;
    Formula formula = Formula::Threshold;
    Rect rect;
};

struct PreviewLayout {
    int width = 0;
    int height = 0;
    int x = 0;
    int y = 0;
};

struct Palette {
    COLORREF background = 0;
    COLORREF panel = 0;
    COLORREF text = 0;
    COLORREF mutedText = 0;
    COLORREF track = 0;
    COLORREF fill = 0;
    COLORREF knob = 0;
    COLORREF button = 0;
    COLORREF buttonText = 0;
    COLORREF buttonActive = 0;
    COLORREF buttonActiveText = 0;
    COLORREF black = 0;
    COLORREF white = 0;
    COLORREF border = 0;
};

struct AppState {
    HWND hwnd = nullptr;
    HDC backDC = nullptr;
    HBITMAP backBitmap = nullptr;
    void* backBits = nullptr;
    HFONT font = nullptr;
    int windowWidth = 920;
    int windowHeight = 720;
    Settings settings;
    PreviewMode previewMode = PreviewMode::Dithered;
    Image source = makeDemoImage();
    Image workingSource;
    std::vector<std::uint8_t> dithered;
    Palette palette{};
    bool dirty = true;
    bool running = true;
    int activeSlider = -1;
    bool panning = false;
    int panStartX = 0;
    int panStartY = 0;
    double panOriginX = 0.0;
    double panOriginY = 0.0;
    double viewPanX = 0.0;
    double viewPanY = 0.0;
    std::mutex importMutex;
    std::atomic<bool> importCompleted{false};
    bool importPending = false;
    std::optional<std::string> finishedImportPath;
    std::optional<Image> finishedImportImage;
    std::thread importThread;
};

AppState* g_app = nullptr;

double clampDouble(double value, double minValue, double maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

COLORREF rgbColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return RGB(r, g, b);
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string output(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), output.data(), required, nullptr, nullptr);
    return output;
}

std::string colorToHex(const Pixel& color) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", color.r, color.g, color.b);
    return buffer;
}

Palette makePalette(Theme theme) {
    if (theme == Theme::Dark) {
        return Palette{
            rgbColor(22, 24, 29),
            rgbColor(31, 34, 40),
            rgbColor(236, 238, 243),
            rgbColor(146, 153, 168),
            rgbColor(54, 58, 68),
            rgbColor(92, 138, 255),
            rgbColor(10, 12, 16),
            rgbColor(40, 44, 52),
            rgbColor(240, 242, 245),
            rgbColor(72, 78, 90),
            rgbColor(255, 255, 255),
            rgbColor(0, 0, 0),
            rgbColor(255, 255, 255),
            rgbColor(53, 57, 67),
        };
    }

    return Palette{
        rgbColor(245, 246, 248),
        rgbColor(231, 234, 239),
        rgbColor(25, 27, 31),
        rgbColor(95, 101, 114),
        rgbColor(204, 208, 214),
        rgbColor(51, 116, 228),
        rgbColor(244, 245, 247),
        rgbColor(223, 225, 229),
        rgbColor(30, 33, 38),
        rgbColor(39, 44, 53),
        rgbColor(255, 255, 255),
        rgbColor(0, 0, 0),
        rgbColor(255, 255, 255),
        rgbColor(198, 202, 210),
    };
}

std::optional<std::string> chooseImagePath() {
    wchar_t buffer[MAX_PATH * 4] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_app ? g_app->hwnd : nullptr;
    ofn.lpstrFilter =
        L"Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.ppm;*.pnm;*.pbm;*.pgm;*.tga;*.tif;*.tiff\0"
        L"All Files\0*.*\0\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buffer));
    ofn.lpstrTitle = L"Import image";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetOpenFileNameW(&ofn)) {
        return std::nullopt;
    }

    std::wstring selected(buffer);
    if (selected.empty()) {
        return std::nullopt;
    }
    return wideToUtf8(selected);
}

std::optional<std::string> chooseExportPath() {
    wchar_t buffer[MAX_PATH * 4] = L"opendither.png";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_app ? g_app->hwnd : nullptr;
    ofn.lpstrFilter =
        L"PNG image\0*.png\0"
        L"JPEG image\0*.jpg;*.jpeg\0"
        L"BMP image\0*.bmp\0"
        L"TGA image\0*.tga\0"
        L"PPM image\0*.ppm;*.pnm\0"
        L"All Files\0*.*\0\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buffer));
    ofn.lpstrTitle = L"Save export";
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetSaveFileNameW(&ofn)) {
        return std::nullopt;
    }

    return wideToUtf8(buffer);
}

std::optional<Pixel> chooseColor(const Pixel& initial) {
    static COLORREF customColors[16]{};
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = g_app ? g_app->hwnd : nullptr;
    cc.rgbResult = RGB(initial.r, initial.g, initial.b);
    cc.lpCustColors = customColors;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) {
        return std::nullopt;
    }

    return Pixel{
        static_cast<std::uint8_t>(GetRValue(cc.rgbResult)),
        static_cast<std::uint8_t>(GetGValue(cc.rgbResult)),
        static_cast<std::uint8_t>(GetBValue(cc.rgbResult)),
    };
}

void fillRect(HDC hdc, const Rect& rect, COLORREF color) {
    RECT winRect{rect.x, rect.y, rect.x + rect.w, rect.y + rect.h};
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(hdc, &winRect, brush);
    DeleteObject(brush);
}

void drawText(HDC hdc, int x, int y, const std::string& text, COLORREF color) {
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    TextOutA(hdc, x, y, text.c_str(), static_cast<int>(text.size()));
}

std::vector<Slider> makeSliders(Settings& settings) {
    return {
        {"Threshold", &settings.threshold, 0.0, 255.0, {}},
        {"Contrast", &settings.contrast, 0.2, 3.0, {}},
        {"Brightness", &settings.brightness, -100.0, 100.0, {}},
        {"Noise", &settings.noise, 0.0, 120.0, {}},
    };
}

std::vector<Button> makeButtons() {
    return {
        {"Threshold", Formula::Threshold, {}},
        {"Bayer", Formula::OrderedBayer, {}},
        {"Floyd", Formula::FloydSteinberg, {}},
        {"Atkinson", Formula::Atkinson, {}},
        {"Jarvis", Formula::JarvisJudiceNinke, {}},
    };
}

void layoutControls(std::vector<Slider>& sliders, std::vector<Button>& buttons, int windowWidth, SideTab activeTab) {
    (void)activeTab;
    const int panelX = windowWidth - kPanelWidth + kMargin;
    int y = 226;
    for (Slider& slider : sliders) {
        slider.rect = Rect{panelX, y + 16, kSliderWidth, kSliderHeight};
        y += 58;
    }

    y += 10;
    for (Button& button : buttons) {
        button.rect = Rect{panelX, y, kSliderWidth, kButtonHeight};
        y += kButtonHeight + 8;
    }
}

void drawSlider(HDC hdc, const Palette& palette, const Slider& slider) {
    char label[128];
    std::snprintf(label, sizeof(label), "%s: %.2f", slider.label.c_str(), *slider.value);
    drawText(hdc, slider.rect.x, slider.rect.y - 7, label, palette.text);

    fillRect(hdc, slider.rect, palette.track);
    const double t = clampDouble((*slider.value - slider.min) / (slider.max - slider.min), 0.0, 1.0);
    const int filled = static_cast<int>(std::lround(t * slider.rect.w));
    fillRect(hdc, Rect{slider.rect.x, slider.rect.y, filled, slider.rect.h}, palette.fill);
    fillRect(hdc, Rect{slider.rect.x + filled - 4, slider.rect.y - 3, 8, slider.rect.h + 6}, palette.knob);
}

void drawButton(HDC hdc, const Palette& palette, const Button& button, Formula selected) {
    const bool active = button.formula == selected;
    fillRect(hdc, button.rect, active ? palette.buttonActive : palette.button);
    RECT rc{button.rect.x, button.rect.y, button.rect.x + button.rect.w, button.rect.y + button.rect.h};
    SetTextColor(hdc, active ? palette.buttonActiveText : palette.buttonText);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, button.label.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void drawToggle(HDC hdc, const Palette& palette, const Rect& rect, const std::string& label, bool active = false) {
    fillRect(hdc, rect, active ? palette.buttonActive : palette.button);
    RECT rc{rect.x, rect.y, rect.x + rect.w, rect.y + rect.h};
    SetTextColor(hdc, active ? palette.buttonActiveText : palette.buttonText);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, label.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void drawFooter(HDC hdc, const Palette& palette, int windowWidth, int windowHeight, PreviewMode previewMode) {
    const Rect footer{0, windowHeight - kFooterHeight, windowWidth, kFooterHeight};
    fillRect(hdc, footer, palette.panel);
    drawText(hdc, kMargin, windowHeight - 9, "Drag to pan  Wheel to zoom  I import  Space toggle  R reset  S save  Esc quit", palette.text);
    drawText(hdc, windowWidth - 170, windowHeight - 9, previewMode == PreviewMode::Dithered ? "View: dithered" : "View: original", palette.mutedText);
}

void drawStepControl(HDC hdc, const Palette& palette, int x, int topY, const std::string& label, int value, const std::string& suffix) {
    drawText(hdc, x, topY, label, palette.text);

    const Rect minus{x, topY + 14, 38, 26};
    const Rect plus{x + 52, topY + 14, 38, 26};
    fillRect(hdc, minus, palette.button);
    fillRect(hdc, plus, palette.button);

    RECT minusRc{minus.x, minus.y, minus.x + minus.w, minus.y + minus.h};
    RECT plusRc{plus.x, plus.y, plus.x + plus.w, plus.y + plus.h};
    SetTextColor(hdc, palette.buttonText);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, "-", -1, &minusRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawTextA(hdc, "+", -1, &plusRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    drawText(hdc, plus.x + 52, plus.y + 18, std::to_string(value) + suffix, palette.text);
}

void drawTab(HDC hdc, const Palette& palette, const Rect& rect, const std::string& label, bool active) {
    fillRect(hdc, rect, active ? palette.buttonActive : palette.button);
    RECT rc{rect.x, rect.y, rect.x + rect.w, rect.y + rect.h};
    SetTextColor(hdc, active ? palette.buttonActiveText : palette.buttonText);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, label.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void drawSwatch(HDC hdc, const Palette& palette, const Rect& rect, const Pixel& color, bool selected) {
    fillRect(hdc, rect, RGB(color.r, color.g, color.b));
    if (selected) {
        fillRect(hdc, Rect{rect.x - 2, rect.y - 2, rect.w + 4, 2}, palette.text);
        fillRect(hdc, Rect{rect.x - 2, rect.y + rect.h, rect.w + 4, 2}, palette.text);
        fillRect(hdc, Rect{rect.x - 2, rect.y - 2, 2, rect.h + 4}, palette.text);
        fillRect(hdc, Rect{rect.x + rect.w, rect.y - 2, 2, rect.h + 4}, palette.text);
    }
}

void drawMiniSlider(HDC hdc, const Palette& palette, const Rect& rect, const std::string& label, double value, double minValue, double maxValue) {
    drawText(hdc, rect.x, rect.y - 7, label, palette.text);
    fillRect(hdc, rect, palette.track);
    const double t = clampDouble((value - minValue) / (maxValue - minValue), 0.0, 1.0);
    const int filled = static_cast<int>(std::lround(t * rect.w));
    fillRect(hdc, Rect{rect.x, rect.y, filled, rect.h}, palette.fill);
    fillRect(hdc, Rect{rect.x + filled - 4, rect.y - 3, 8, rect.h + 6}, palette.knob);
}

void drawModeButton(HDC hdc, const Palette& palette, const Rect& rect, const std::string& label, bool active) {
    fillRect(hdc, rect, active ? palette.buttonActive : palette.button);
    RECT rc{rect.x, rect.y, rect.x + rect.w, rect.y + rect.h};
    SetTextColor(hdc, active ? palette.buttonActiveText : palette.buttonText);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, label.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void updateSliderFromMouse(const Slider& slider, int mouseX) {
    const double t = clampDouble(static_cast<double>(mouseX - slider.rect.x) / slider.rect.w, 0.0, 1.0);
    *slider.value = slider.min + t * (slider.max - slider.min);
}

PreviewLayout computePreviewLayout(int windowWidth, int windowHeight) {
    const int previewLeft = kMargin;
    const int previewTop = 44;
    const int previewRight = windowWidth - kPanelWidth - kMargin;
    const int previewBottom = windowHeight - kFooterHeight - 104;
    const int maxPreviewWidth = std::max(1, previewRight - previewLeft);
    const int maxPreviewHeight = std::max(1, previewBottom - previewTop);
    return PreviewLayout{maxPreviewWidth, maxPreviewHeight, previewLeft, previewTop};
}

double previewScaleForLayout(const Image& source, const PreviewLayout& layout, int zoom) {
    const double fitScale = std::min(
        static_cast<double>(layout.width) / std::max(1, source.width),
        static_cast<double>(layout.height) / std::max(1, source.height)
    );
    return fitScale * std::max(1, zoom);
}

void drawPreview(HDC hdc, const Image& source, const std::vector<std::uint8_t>& dithered, const Settings& settings, PreviewMode mode, const PreviewLayout& layout, double panX, double panY) {
    if (layout.width <= 0 || layout.height <= 0) {
        return;
    }

    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(layout.width * layout.height), 0);
    const double zoomScale = previewScaleForLayout(source, layout, settings.scale);
    const double centerX = static_cast<double>(source.width) * 0.5 + panX;
    const double centerY = static_cast<double>(source.height) * 0.5 + panY;

    for (int y = 0; y < layout.height; ++y) {
        const double sampleY = (static_cast<double>(y) - layout.height * 0.5) / zoomScale + centerY;
        for (int x = 0; x < layout.width; ++x) {
            const double sampleX = (static_cast<double>(x) - layout.width * 0.5) / zoomScale + centerX;
            Pixel output{20, 22, 28};
            if (sampleX >= 0.0 && sampleY >= 0.0 && sampleX < source.width && sampleY < source.height) {
                const int sourceX = static_cast<int>(sampleX);
                const int sourceY = static_cast<int>(sampleY);
                const std::size_t index = static_cast<std::size_t>(sourceY * source.width + sourceX);
                output = mode == PreviewMode::Original
                    ? source.pixels[index]
                    : channelColor(source, settings, std::min<int>(dithered[index], settings.channelCount - 1), sourceX, sourceY);
            }
            pixels[static_cast<std::size_t>(y * layout.width + x)] = static_cast<std::uint32_t>(output.r) << 16 | static_cast<std::uint32_t>(output.g) << 8 | static_cast<std::uint32_t>(output.b);
        }
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = layout.width;
    bmi.bmiHeader.biHeight = -layout.height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(
        hdc,
        layout.x,
        layout.y,
        layout.width,
        layout.height,
        0,
        0,
        layout.width,
        layout.height,
        pixels.data(),
        &bmi,
        DIB_RGB_COLORS,
        SRCCOPY
    );
}

void destroyBackBuffer(AppState& app) {
    if (app.backBitmap) {
        DeleteObject(app.backBitmap);
        app.backBitmap = nullptr;
    }
    if (app.backDC) {
        DeleteDC(app.backDC);
        app.backDC = nullptr;
    }
    app.backBits = nullptr;
}

void ensureBackBuffer(AppState& app) {
    if (app.windowWidth <= 0 || app.windowHeight <= 0) {
        return;
    }

    if (app.backDC && app.backBitmap) {
        return;
    }

    app.backDC = CreateCompatibleDC(nullptr);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = app.windowWidth;
    bmi.bmiHeader.biHeight = -app.windowHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    app.backBitmap = CreateDIBSection(app.backDC, &bmi, DIB_RGB_COLORS, &app.backBits, nullptr, 0);
    SelectObject(app.backDC, app.backBitmap);
}

void refreshWorkingImage(AppState& app) {
    if (app.dirty) {
        app.workingSource = prepareImage(app.source, app.settings.pixelSize);
        app.dithered = ditherImage(app.workingSource, app.settings);
        app.dirty = false;
    }
}

void applyImportedImage(AppState& app, Image loaded) {
    app.source = std::move(loaded);
    app.workingSource = prepareImage(app.source, app.settings.pixelSize);
    app.dithered = ditherImage(app.workingSource, app.settings);
    ensureChannelCount(app.settings);
    app.viewPanX = 0.0;
    app.viewPanY = 0.0;
    app.previewMode = PreviewMode::Original;
    app.dirty = false;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void performExport(AppState& app) {
    refreshWorkingImage(app);
    if (auto path = chooseExportPath()) {
        if (!exportImage(*path, app.workingSource, app.dithered, app.settings, app.previewMode, app.settings.exportScale)) {
            MessageBoxA(app.hwnd, "Could not export image.", "OpenDither", MB_OK | MB_ICONERROR);
        }
    }
}

void startImport(AppState& app) {
    if (app.importPending) {
        return;
    }

    app.importPending = true;
    app.importCompleted = false;
    {
        std::lock_guard<std::mutex> lock(app.importMutex);
        app.finishedImportPath.reset();
        app.finishedImportImage.reset();
    }

    app.importThread = std::thread([&app]() {
        std::optional<std::string> selected = chooseImagePath();
        std::optional<Image> loaded;
        if (selected.has_value()) {
            loaded = loadImageAny(*selected);
        }
        {
            std::lock_guard<std::mutex> lock(app.importMutex);
            app.finishedImportPath = std::move(selected);
            app.finishedImportImage = std::move(loaded);
        }
        app.importCompleted = true;
        if (app.hwnd) {
            PostMessageA(app.hwnd, kImportDoneMessage, 0, 0);
        }
    });
}

void rebuildTheme(AppState& app) {
    app.palette = makePalette(app.settings.theme);
}

void redraw(AppState& app) {
    ensureBackBuffer(app);
    refreshWorkingImage(app);

    if (!app.backDC) {
        return;
    }

    SelectObject(app.backDC, app.font);
    SetBkMode(app.backDC, TRANSPARENT);

    fillRect(app.backDC, Rect{0, 0, app.windowWidth, app.windowHeight}, app.palette.background);
    fillRect(app.backDC, Rect{app.windowWidth - kPanelWidth, 0, kPanelWidth, app.windowHeight}, app.palette.panel);
    fillRect(app.backDC, Rect{app.windowWidth - kPanelWidth - 1, 0, 1, app.windowHeight}, app.palette.border);

    drawText(app.backDC, kMargin, 24, "OpenDither", app.palette.text);
    const Rect themeToggle{app.windowWidth - kPanelWidth + kMargin, 20, 86, 28};
    drawToggle(app.backDC, app.palette, themeToggle, app.settings.theme == Theme::Dark ? "Dark" : "Light", app.settings.theme == Theme::Dark);
    drawText(app.backDC, themeToggle.x + 96, 39, "Theme", app.palette.mutedText);

    const PreviewLayout previewLayout = computePreviewLayout(app.windowWidth, app.windowHeight);
    fillRect(app.backDC, Rect{previewLayout.x - 1, previewLayout.y - 1, previewLayout.width + 2, previewLayout.height + 2}, app.palette.border);
    fillRect(app.backDC, Rect{previewLayout.x, previewLayout.y, previewLayout.width, previewLayout.height}, app.palette.panel);
    drawPreview(app.backDC, app.workingSource, app.dithered, app.settings, app.previewMode, previewLayout, app.viewPanX, app.viewPanY);

    const int actionRowY = previewLayout.y + previewLayout.height + 8;
    const int actionRowX = previewLayout.x;
    const Rect saveButton{actionRowX, actionRowY, 92, 28};
    const Rect importButton{actionRowX + 104, actionRowY, 92, 28};
    const Rect flipButton{actionRowX + 208, actionRowY, 156, 28};
    drawToggle(app.backDC, app.palette, saveButton, "Save");
    drawToggle(app.backDC, app.palette, importButton, "Import");
    drawToggle(app.backDC, app.palette, flipButton, app.previewMode == PreviewMode::Dithered ? "Dithered" : "Original");
    drawStepControl(app.backDC, app.palette, saveButton.x, previewLayout.y + previewLayout.height + 64, "Export Mult.", app.settings.exportScale, "x");

    const int panelX = app.windowWidth - kPanelWidth + kMargin;
    drawStepControl(app.backDC, app.palette, panelX, 64, "Zoom", app.settings.scale, "x");
    drawStepControl(app.backDC, app.palette, panelX, 116, "Pixel Size", app.settings.pixelSize, "px");

    const Rect colorsTab{panelX, 170, 122, 28};
    const Rect ditherTab{panelX + 130, 170, 122, 28};
    drawTab(app.backDC, app.palette, colorsTab, "Colors", app.settings.activeTab == SideTab::Colors);
    drawTab(app.backDC, app.palette, ditherTab, "Dither", app.settings.activeTab == SideTab::Dither);

    if (app.settings.activeTab == SideTab::Colors) {
        drawText(app.backDC, panelX, 226, "Channels", app.palette.text);
        const std::array<int, 3> channelCounts{2, 3, 4};
        for (std::size_t i = 0; i < channelCounts.size(); ++i) {
            const Rect channelRect{panelX + static_cast<int>(i) * 74, 242, 66, 28};
            drawModeButton(app.backDC, app.palette, channelRect, std::to_string(channelCounts[i]), app.settings.channelCount == channelCounts[i]);
        }

        const int stripY = 286;
        const int cardGap = 8;
        const int cardWidth = (kPanelWidth - 2 * kMargin - static_cast<int>(cardGap * 3)) / std::max(1, app.settings.channelCount);
        for (int i = 0; i < app.settings.channelCount; ++i) {
            const int cardX = panelX + i * (cardWidth + cardGap);
            const Rect card{cardX, stripY, cardWidth, 56};
            fillRect(app.backDC, card, i == app.settings.selectedChannel ? app.palette.buttonActive : app.palette.button);
            drawText(app.backDC, card.x + 8, card.y + 18, "C" + std::to_string(i + 1), i == app.settings.selectedChannel ? app.palette.buttonActiveText : app.palette.buttonText);
            drawSwatch(app.backDC, app.palette, Rect{card.x + 8, card.y + 26, 22, 22}, app.settings.colors[static_cast<std::size_t>(i)], false);
            drawText(app.backDC, card.x + 34, card.y + 43, app.settings.channelModes[static_cast<std::size_t>(i)] == ChannelMode::Solid ? "S" : "G", app.palette.mutedText);
        }

        const std::size_t idx = static_cast<std::size_t>(app.settings.selectedChannel);
        const int detailY = 356;
        drawText(app.backDC, panelX, detailY, "Selected Channel", app.palette.text);
        const Rect solidButton{panelX, detailY + 18, 88, 28};
        const Rect gradientButton{panelX + 96, detailY + 18, 96, 28};
        drawModeButton(app.backDC, app.palette, solidButton, "Solid", app.settings.channelModes[idx] == ChannelMode::Solid);
        drawModeButton(app.backDC, app.palette, gradientButton, "Grad", app.settings.channelModes[idx] == ChannelMode::Gradient);

        if (app.settings.channelModes[idx] == ChannelMode::Solid) {
            const Rect colorA{panelX, detailY + 68, kSwatchSize, kSwatchSize};
            const Rect colorAButton{panelX + 36, detailY + 70, 150, 24};
            drawSwatch(app.backDC, app.palette, colorA, app.settings.colors[idx], true);
            drawToggle(app.backDC, app.palette, colorAButton, colorToHex(app.settings.colors[idx]));
        } else {
            const Rect colorA{panelX, detailY + 68, kSwatchSize, kSwatchSize};
            const Rect colorAButton{panelX + 36, detailY + 70, 150, 24};
            const Rect colorB{panelX, detailY + 104, kSwatchSize, kSwatchSize};
            const Rect colorBButton{panelX + 36, detailY + 106, 150, 24};
            drawSwatch(app.backDC, app.palette, colorA, app.settings.colors[idx], true);
            drawToggle(app.backDC, app.palette, colorAButton, colorToHex(app.settings.colors[idx]));
            drawSwatch(app.backDC, app.palette, colorB, app.settings.gradientColors[idx], true);
            drawToggle(app.backDC, app.palette, colorBButton, colorToHex(app.settings.gradientColors[idx]));

            const Rect horiz{panelX, detailY + 144, 60, 28};
            const Rect vert{panelX + 68, detailY + 144, 60, 28};
            const Rect rad{panelX + 136, detailY + 144, 60, 28};
            drawModeButton(app.backDC, app.palette, horiz, "H", app.settings.gradientTypes[idx] == GradientType::Horizontal);
            drawModeButton(app.backDC, app.palette, vert, "V", app.settings.gradientTypes[idx] == GradientType::Vertical);
            drawModeButton(app.backDC, app.palette, rad, "R", app.settings.gradientTypes[idx] == GradientType::Radial);

            const Rect gradientSlider{panelX, detailY + 196, 290, 18};
            drawMiniSlider(app.backDC, app.palette, gradientSlider, "Blend Width", app.settings.gradientSpread[idx], 0.05, 1.0);
        }
    } else {
        for (const Slider& slider : makeSliders(app.settings)) {
            // This temporary vector is only for layout helpers; the actual buttons use shared state.
            (void)slider;
        }
        auto sliders = makeSliders(app.settings);
        auto buttons = makeButtons();
        layoutControls(sliders, buttons, app.windowWidth, app.settings.activeTab);
        for (const Slider& slider : sliders) {
            drawSlider(app.backDC, app.palette, slider);
        }
        for (const Button& button : buttons) {
            drawButton(app.backDC, app.palette, button, app.settings.formula);
        }
    }

    drawFooter(app.backDC, app.palette, app.windowWidth, app.windowHeight, app.previewMode);
    if (app.importPending) {
        drawText(app.backDC, app.windowWidth - 170, app.windowHeight - 25, "Importing...", app.palette.mutedText);
    }
}

bool pointInPreview(const PreviewLayout& previewLayout, int x, int y) {
    return x >= previewLayout.x && x < previewLayout.x + previewLayout.width && y >= previewLayout.y && y < previewLayout.y + previewLayout.height;
}

} // namespace

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* app = reinterpret_cast<AppState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* created = new AppState();
            created->hwnd = hwnd;
            created->palette = makePalette(created->settings.theme);
            created->workingSource = prepareImage(created->source, created->settings.pixelSize);
            created->dithered = ditherImage(created->workingSource, created->settings);
            ensureChannelCount(created->settings);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
            g_app = created;
            created->font = CreateFontA(
                -16,
                0,
                0,
                0,
                FW_NORMAL,
                FALSE,
                FALSE,
                FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE,
                "Segoe UI"
            );
            SetTimer(hwnd, 1, 16, nullptr);
            return 0;
        }
        case WM_DESTROY: {
            if (app) {
                if (app->importThread.joinable()) {
                    app->importThread.join();
                }
                destroyBackBuffer(*app);
                if (app->font) {
                    DeleteObject(app->font);
                }
                delete app;
                g_app = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        }
        case WM_SIZE: {
            if (app) {
                app->windowWidth = LOWORD(lParam);
                app->windowHeight = HIWORD(lParam);
                destroyBackBuffer(*app);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER: {
            if (app && app->importPending && app->importCompleted) {
                std::optional<std::string> importedPath;
                std::optional<Image> importedImage;
                {
                    std::lock_guard<std::mutex> lock(app->importMutex);
                    if (app->finishedImportPath.has_value()) {
                        importedPath = std::move(app->finishedImportPath);
                        app->finishedImportPath.reset();
                    }
                    if (app->finishedImportImage.has_value()) {
                        importedImage = std::move(app->finishedImportImage);
                        app->finishedImportImage.reset();
                    }
                }
                if (app->importThread.joinable()) {
                    app->importThread.join();
                }
                app->importPending = false;
                if (importedImage.has_value()) {
                    applyImportedImage(*app, std::move(*importedImage));
                } else if (importedPath.has_value()) {
                    MessageBoxA(hwnd, ("Could not load image: " + *importedPath).c_str(), "OpenDither", MB_OK | MB_ICONERROR);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        case kImportDoneMessage: {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (app) {
                const short delta = GET_WHEEL_DELTA_WPARAM(wParam);
                if (delta > 0) {
                    app->settings.scale = std::min(16, app->settings.scale + 1);
                } else {
                    app->settings.scale = std::max(1, app->settings.scale - 1);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!app) {
                return 0;
            }
            SetCapture(hwnd);
            const int mouseX = GET_X_LPARAM(lParam);
            const int mouseY = GET_Y_LPARAM(lParam);
            const int panelX = app->windowWidth - kPanelWidth + kMargin;
            const Rect themeToggle{panelX, 20, 86, 28};
            const Rect zoomMinus{panelX, 78, 38, 26};
            const Rect zoomPlus{panelX + 52, 78, 38, 26};
            const Rect pixelMinus{panelX, 130, 38, 26};
            const Rect pixelPlus{panelX + 52, 130, 38, 26};
            const Rect colorsTab{panelX, 170, 122, 28};
            const Rect ditherTab{panelX + 130, 170, 122, 28};
            const PreviewLayout previewLayout = computePreviewLayout(app->windowWidth, app->windowHeight);
            const int actionRowY = previewLayout.y + previewLayout.height + 8;
            const int actionRowX = previewLayout.x;
            const Rect saveButton{actionRowX, actionRowY, 92, 28};
            const Rect importButton{actionRowX + 104, actionRowY, 92, 28};
            const Rect flipButton{actionRowX + 208, actionRowY, 156, 28};
            const Rect exportMinus{saveButton.x, previewLayout.y + previewLayout.height + 76, 38, 26};
            const Rect exportPlus{saveButton.x + 52, previewLayout.y + previewLayout.height + 76, 38, 26};

            if (themeToggle.contains(mouseX, mouseY)) {
                app->settings.theme = app->settings.theme == Theme::Dark ? Theme::Light : Theme::Dark;
                rebuildTheme(*app);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (colorsTab.contains(mouseX, mouseY)) {
                app->settings.activeTab = SideTab::Colors;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (ditherTab.contains(mouseX, mouseY)) {
                app->settings.activeTab = SideTab::Dither;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (zoomMinus.contains(mouseX, mouseY)) {
                app->settings.scale = std::max(1, app->settings.scale - 1);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (zoomPlus.contains(mouseX, mouseY)) {
                app->settings.scale = std::min(16, app->settings.scale + 1);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (pixelMinus.contains(mouseX, mouseY)) {
                app->settings.pixelSize = std::max(1, app->settings.pixelSize - 1);
                app->dirty = true;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (pixelPlus.contains(mouseX, mouseY)) {
                app->settings.pixelSize = std::min(8, app->settings.pixelSize + 1);
                app->dirty = true;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (importButton.contains(mouseX, mouseY)) {
                startImport(*app);
                return 0;
            }
            if (flipButton.contains(mouseX, mouseY)) {
                app->previewMode = app->previewMode == PreviewMode::Dithered ? PreviewMode::Original : PreviewMode::Dithered;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (saveButton.contains(mouseX, mouseY)) {
                performExport(*app);
                return 0;
            }
            if (exportMinus.contains(mouseX, mouseY)) {
                app->settings.exportScale = std::max(1, app->settings.exportScale - 1);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (exportPlus.contains(mouseX, mouseY)) {
                app->settings.exportScale = std::min(8, app->settings.exportScale + 1);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (pointInPreview(previewLayout, mouseX, mouseY) &&
                !importButton.contains(mouseX, mouseY) &&
                !flipButton.contains(mouseX, mouseY)) {
                app->panning = true;
                app->panStartX = mouseX;
                app->panStartY = mouseY;
                app->panOriginX = app->viewPanX;
                app->panOriginY = app->viewPanY;
                return 0;
            }

            if (app->settings.activeTab == SideTab::Dither) {
                auto sliders = makeSliders(app->settings);
                auto buttons = makeButtons();
                layoutControls(sliders, buttons, app->windowWidth, app->settings.activeTab);
                for (std::size_t i = 0; i < sliders.size(); ++i) {
                    if (sliders[i].rect.contains(mouseX, mouseY)) {
                        app->activeSlider = static_cast<int>(i);
                        updateSliderFromMouse(sliders[i], mouseX);
                        app->dirty = true;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }

                for (const Button& button : buttons) {
                    if (button.rect.contains(mouseX, mouseY)) {
                        app->settings.formula = button.formula;
                        app->dirty = true;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }
            } else {
                const std::array<int, 3> channelCounts{2, 3, 4};
                for (std::size_t i = 0; i < channelCounts.size(); ++i) {
                    const Rect channelRect{panelX + static_cast<int>(i) * 74, 242, 66, 28};
                    if (channelRect.contains(mouseX, mouseY)) {
                        app->settings.channelCount = channelCounts[i];
                        ensureChannelCount(app->settings);
                        app->dirty = true;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }

                const int stripY = 286;
                const int cardGap = 8;
                const int cardWidth = (kPanelWidth - 2 * kMargin - static_cast<int>(cardGap * 3)) / std::max(1, app->settings.channelCount);
                for (int i = 0; i < app->settings.channelCount; ++i) {
                    const int cardX = panelX + i * (cardWidth + cardGap);
                    const Rect card{cardX, stripY, cardWidth, 56};
                    if (card.contains(mouseX, mouseY)) {
                        app->settings.selectedChannel = i;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }

                const std::size_t idx = static_cast<std::size_t>(app->settings.selectedChannel);
                const int detailY = 356;
                const Rect solidButton{panelX, detailY + 18, 88, 28};
                const Rect gradientButton{panelX + 96, detailY + 18, 96, 28};
                if (solidButton.contains(mouseX, mouseY)) {
                    app->settings.channelModes[idx] = ChannelMode::Solid;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (gradientButton.contains(mouseX, mouseY)) {
                    app->settings.channelModes[idx] = ChannelMode::Gradient;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }

                const Rect colorA{panelX, detailY + 68, kSwatchSize, kSwatchSize};
                const Rect colorAButton{panelX + 36, detailY + 70, 150, 24};
                if (colorA.contains(mouseX, mouseY) || colorAButton.contains(mouseX, mouseY)) {
                    if (auto picked = chooseColor(app->settings.colors[idx])) {
                        app->settings.colors[idx] = *picked;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }

                if (app->settings.channelModes[idx] == ChannelMode::Gradient) {
                    const Rect colorB{panelX, detailY + 104, kSwatchSize, kSwatchSize};
                    const Rect colorBButton{panelX + 36, detailY + 106, 150, 24};
                    if (colorB.contains(mouseX, mouseY) || colorBButton.contains(mouseX, mouseY)) {
                        if (auto picked = chooseColor(app->settings.gradientColors[idx])) {
                            app->settings.gradientColors[idx] = *picked;
                            InvalidateRect(hwnd, nullptr, FALSE);
                        }
                        return 0;
                    }

                    const Rect horiz{panelX, detailY + 144, 60, 28};
                    const Rect vert{panelX + 68, detailY + 144, 60, 28};
                    const Rect rad{panelX + 136, detailY + 144, 60, 28};
                    if (horiz.contains(mouseX, mouseY)) {
                        app->settings.gradientTypes[idx] = GradientType::Horizontal;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (vert.contains(mouseX, mouseY)) {
                        app->settings.gradientTypes[idx] = GradientType::Vertical;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (rad.contains(mouseX, mouseY)) {
                        app->settings.gradientTypes[idx] = GradientType::Radial;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }

                    const Rect gradientSlider{panelX, detailY + 196, 290, 18};
                    if (gradientSlider.contains(mouseX, mouseY)) {
                        const double t = clampDouble(static_cast<double>(mouseX - gradientSlider.x) / gradientSlider.w, 0.0, 1.0);
                        app->settings.gradientSpread[idx] = 0.05 + t * (1.0 - 0.05);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (app) {
                app->activeSlider = -1;
                app->panning = false;
            }
            ReleaseCapture();
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!app) {
                return 0;
            }
            const int mouseX = GET_X_LPARAM(lParam);
            const int mouseY = GET_Y_LPARAM(lParam);
            if (app->activeSlider >= 0 && (wParam & MK_LBUTTON)) {
                auto sliders = makeSliders(app->settings);
                auto buttons = makeButtons();
                layoutControls(sliders, buttons, app->windowWidth, app->settings.activeTab);
                const double before = *sliders[static_cast<std::size_t>(app->activeSlider)].value;
                updateSliderFromMouse(sliders[static_cast<std::size_t>(app->activeSlider)], mouseX);
                if (*sliders[static_cast<std::size_t>(app->activeSlider)].value != before) {
                    app->dirty = true;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (app->panning && (wParam & MK_LBUTTON)) {
                const PreviewLayout previewLayout = computePreviewLayout(app->windowWidth, app->windowHeight);
                const double zoomScale = previewScaleForLayout(app->workingSource, previewLayout, app->settings.scale);
                app->viewPanX = app->panOriginX - static_cast<double>(mouseX - app->panStartX) / std::max(0.0001, zoomScale);
                app->viewPanY = app->panOriginY - static_cast<double>(mouseY - app->panStartY) / std::max(0.0001, zoomScale);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (app->settings.activeTab == SideTab::Colors && (wParam & MK_LBUTTON)) {
                const int panelX = app->windowWidth - kPanelWidth + kMargin;
                const int detailY = 356;
                const std::size_t idx = static_cast<std::size_t>(app->settings.selectedChannel);
                if (app->settings.channelModes[idx] == ChannelMode::Gradient) {
                    const Rect gradientSlider{panelX, detailY + 196, 290, 18};
                    if (gradientSlider.contains(mouseX, mouseY)) {
                        const double t = clampDouble(static_cast<double>(mouseX - gradientSlider.x) / gradientSlider.w, 0.0, 1.0);
                        app->settings.gradientSpread[idx] = 0.05 + t * (1.0 - 0.05);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }
            }
            return 0;
        }
        case WM_KEYDOWN: {
            if (!app) {
                return 0;
            }
            switch (wParam) {
                case VK_ESCAPE:
                    DestroyWindow(hwnd);
                    break;
                case 'R':
                    app->settings = Settings{};
                    ensureChannelCount(app->settings);
                    app->palette = makePalette(app->settings.theme);
                    app->viewPanX = 0.0;
                    app->viewPanY = 0.0;
                    app->dirty = true;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                case VK_SPACE:
                    app->previewMode = app->previewMode == PreviewMode::Dithered ? PreviewMode::Original : PreviewMode::Dithered;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                case 'I':
                    startImport(*app);
                    break;
                case 'S':
                    performExport(*app);
                    break;
                default:
                    break;
            }
            return 0;
        }
        case WM_COMMAND:
            return 0;
        case WM_PAINT: {
            if (!app) {
                break;
            }
            PAINTSTRUCT ps{};
            HDC paintDC = BeginPaint(hwnd, &ps);
            redraw(*app);
            if (app->backDC) {
                BitBlt(paintDC, 0, 0, app->windowWidth, app->windowHeight, app->backDC, 0, 0, SRCCOPY);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            break;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCmd) {
    WNDCLASSA wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = "OpenDitherWin32";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(
        0,
        wc.lpszClassName,
        "OpenDither",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        920,
        720,
        nullptr,
        nullptr,
        instance,
        nullptr
    );

    if (!hwnd) {
        return 1;
    }

    ShowWindow(hwnd, showCmd);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageA(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return static_cast<int>(msg.wParam);
}
