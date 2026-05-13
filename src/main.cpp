#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <optional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "core.hpp"

namespace {

constexpr int kPanelWidth = 340;
constexpr int kMargin = 16;
constexpr int kSliderWidth = 290;
constexpr int kSliderHeight = 18;
constexpr int kButtonHeight = 28;
constexpr int kFooterHeight = 28;
constexpr int kSwatchSize = 24;

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

double clampDouble(double value, double minValue, double maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

std::uint8_t toByte(double value) {
    return static_cast<std::uint8_t>(std::lround(clampDouble(value, 0.0, 255.0)));
}

unsigned long pixelFromRgb(const Visual* visual, int r, int g, int b) {
    const auto pack = [](int value, unsigned long mask) -> unsigned long {
        if (mask == 0) {
            return 0;
        }

        int shift = 0;
        while ((mask & 1UL) == 0UL) {
            mask >>= 1;
            ++shift;
        }

        int bits = 0;
        while ((mask & 1UL) != 0UL) {
            ++bits;
            mask >>= 1;
        }

        const unsigned long maxValue = (1UL << bits) - 1UL;
        const unsigned long scaled = static_cast<unsigned long>(std::lround((value / 255.0) * maxValue));
        return scaled << shift;
    };

    return pack(r, visual->red_mask) | pack(g, visual->green_mask) | pack(b, visual->blue_mask);
}

struct Palette {
    unsigned long background = 0;
    unsigned long panel = 0;
    unsigned long text = 0;
    unsigned long mutedText = 0;
    unsigned long track = 0;
    unsigned long fill = 0;
    unsigned long knob = 0;
    unsigned long button = 0;
    unsigned long buttonText = 0;
    unsigned long buttonActive = 0;
    unsigned long buttonActiveText = 0;
    unsigned long black = 0;
    unsigned long white = 0;
    unsigned long border = 0;
};

Palette makePalette(Display* display, int screen, Theme theme) {
    const Visual* visual = DefaultVisual(display, screen);
    if (theme == Theme::Dark) {
        return Palette{
            pixelFromRgb(visual, 22, 24, 29),
            pixelFromRgb(visual, 31, 34, 40),
            pixelFromRgb(visual, 236, 238, 243),
            pixelFromRgb(visual, 146, 153, 168),
            pixelFromRgb(visual, 54, 58, 68),
            pixelFromRgb(visual, 92, 138, 255),
            pixelFromRgb(visual, 10, 12, 16),
            pixelFromRgb(visual, 40, 44, 52),
            pixelFromRgb(visual, 240, 242, 245),
            pixelFromRgb(visual, 72, 78, 90),
            pixelFromRgb(visual, 255, 255, 255),
            pixelFromRgb(visual, 0, 0, 0),
            pixelFromRgb(visual, 255, 255, 255),
            pixelFromRgb(visual, 53, 57, 67),
        };
    }
    return Palette{
        pixelFromRgb(visual, 245, 246, 248),
        pixelFromRgb(visual, 231, 234, 239),
        pixelFromRgb(visual, 25, 27, 31),
        pixelFromRgb(visual, 95, 101, 114),
        pixelFromRgb(visual, 204, 208, 214),
        pixelFromRgb(visual, 51, 116, 228),
        pixelFromRgb(visual, 244, 245, 247),
        pixelFromRgb(visual, 223, 225, 229),
        pixelFromRgb(visual, 30, 33, 38),
        pixelFromRgb(visual, 39, 44, 53),
        pixelFromRgb(visual, 255, 255, 255),
        pixelFromRgb(visual, 0, 0, 0),
        pixelFromRgb(visual, 255, 255, 255),
        pixelFromRgb(visual, 198, 202, 210),
    };
}

std::optional<std::string> chooseImagePath() {
    const char* command =
        "zenity --file-selection "
        "--title='Import image' "
        "--filename=\"$HOME/\" "
        "--file-filter='Image files | *.png *.jpg *.jpeg *.bmp *.gif *.webp *.ppm *.pnm *.pbm *.pgm' "
        "--file-filter='All files | *'";
    FILE* pipe = popen(command, "r");
    if (!pipe) {
        return std::nullopt;
    }

    std::string path;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe)) {
        path += buffer;
    }

    const int status = pclose(pipe);
    if (status != 0) {
        return std::nullopt;
    }

    while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) {
        path.pop_back();
    }

    if (path.empty()) {
        return std::nullopt;
    }

    return path;
}

std::string colorToHex(const Pixel& color) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", color.r, color.g, color.b);
    return buffer;
}

std::optional<Pixel> parseColorString(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }

    if (value.empty()) {
        return std::nullopt;
    }

    if (value[0] == '#' && value.size() >= 7) {
        const auto hexByte = [](char hi, char lo) -> std::uint8_t {
            const auto digit = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return 0;
            };
            return static_cast<std::uint8_t>((digit(hi) << 4) | digit(lo));
        };

        return Pixel{
            hexByte(value[1], value[2]),
            hexByte(value[3], value[4]),
            hexByte(value[5], value[6]),
        };
    }

    if (value.rfind("rgb(", 0) == 0 && value.back() == ')') {
        int r = 0;
        int g = 0;
        int b = 0;
        if (std::sscanf(value.c_str(), "rgb(%d,%d,%d)", &r, &g, &b) == 3) {
            return Pixel{toByte(r), toByte(g), toByte(b)};
        }
    }

    return std::nullopt;
}

std::optional<Pixel> chooseColor(const Pixel& initial) {
    const std::string command = "zenity --color-selection --title='Choose color' --show-palette --color=" + colorToHex(initial);
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return std::nullopt;
    }

    std::string output;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }
    const int status = pclose(pipe);
    if (status != 0) {
        return std::nullopt;
    }

    return parseColorString(output);
}

void fillRect(Display* display, Window window, GC gc, const Rect& rect, unsigned long color) {
    XSetForeground(display, gc, color);
    XFillRectangle(display, window, gc, rect.x, rect.y, static_cast<unsigned int>(rect.w), static_cast<unsigned int>(rect.h));
}

void drawText(Display* display, Window window, GC gc, int x, int y, const std::string& text, unsigned long color) {
    XSetForeground(display, gc, color);
    XDrawString(display, window, gc, x, y, text.c_str(), static_cast<int>(text.size()));
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
    const int panelX = windowWidth - kPanelWidth + kMargin;
    int y = activeTab == SideTab::Dither ? 226 : 226;
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

void drawSlider(Display* display, Window window, GC gc, const Palette& palette, const Slider& slider) {
    char label[128];
    std::snprintf(label, sizeof(label), "%s: %.2f", slider.label.c_str(), *slider.value);
    drawText(display, window, gc, slider.rect.x, slider.rect.y - 7, label, palette.text);

    fillRect(display, window, gc, slider.rect, palette.track);
    const double t = clampDouble((*slider.value - slider.min) / (slider.max - slider.min), 0.0, 1.0);
    const int filled = static_cast<int>(std::lround(t * slider.rect.w));
    fillRect(display, window, gc, Rect{slider.rect.x, slider.rect.y, filled, slider.rect.h}, palette.fill);
    fillRect(display, window, gc, Rect{slider.rect.x + filled - 4, slider.rect.y - 3, 8, slider.rect.h + 6}, palette.knob);
}

void drawButton(Display* display, Window window, GC gc, const Palette& palette, const Button& button, Formula selected) {
    const bool active = button.formula == selected;
    const unsigned long bg = active ? palette.buttonActive : palette.button;
    const unsigned long fg = active ? palette.buttonActiveText : palette.buttonText;
    fillRect(display, window, gc, button.rect, bg);
    drawText(display, window, gc, button.rect.x + 10, button.rect.y + 18, button.label, fg);
}

void drawToggle(Display* display, Window window, GC gc, const Palette& palette, const Rect& rect, const std::string& label, bool active = false) {
    fillRect(display, window, gc, rect, active ? palette.buttonActive : palette.button);
    drawText(display, window, gc, rect.x + 10, rect.y + 18, label, active ? palette.buttonActiveText : palette.buttonText);
}

void drawFooter(Display* display, Drawable drawable, GC gc, const Palette& palette, int windowWidth, int windowHeight, PreviewMode previewMode) {
    const Rect footer{0, windowHeight - kFooterHeight, windowWidth, kFooterHeight};
    fillRect(display, drawable, gc, footer, palette.panel);
    drawText(display, drawable, gc, kMargin, windowHeight - 9, "Drag to pan  Wheel to zoom  I import  Space toggle  R reset  S save  Esc quit", palette.text);
    drawText(display, drawable, gc, windowWidth - 170, windowHeight - 9, previewMode == PreviewMode::Dithered ? "View: dithered" : "View: original", palette.mutedText);
}

void drawStepControl(Display* display, Window window, GC gc, const Palette& palette, int windowWidth, int topY, const std::string& label, int value, const std::string& suffix) {
    const int panelX = windowWidth - kPanelWidth + kMargin;
    drawText(display, window, gc, panelX, topY, label, palette.text);

    const Rect minus{panelX, topY + 14, 38, 26};
    const Rect plus{panelX + 52, topY + 14, 38, 26};
    fillRect(display, window, gc, minus, palette.button);
    fillRect(display, window, gc, plus, palette.button);
    drawText(display, window, gc, minus.x + 14, minus.y + 18, "-", palette.buttonText);
    drawText(display, window, gc, plus.x + 14, plus.y + 18, "+", palette.buttonText);
    drawText(display, window, gc, plus.x + 52, plus.y + 18, std::to_string(value) + suffix, palette.text);
}

void drawTab(Display* display, Window window, GC gc, const Palette& palette, const Rect& rect, const std::string& label, bool active) {
    fillRect(display, window, gc, rect, active ? palette.buttonActive : palette.button);
    drawText(display, window, gc, rect.x + 10, rect.y + 18, label, active ? palette.buttonActiveText : palette.buttonText);
}

void drawSwatch(Display* display, Window window, GC gc, const Palette& palette, const Rect& rect, const Pixel& color, bool selected) {
    fillRect(display, window, gc, rect, pixelFromRgb(DefaultVisual(display, DefaultScreen(display)), color.r, color.g, color.b));
    if (selected) {
        fillRect(display, window, gc, Rect{rect.x - 2, rect.y - 2, rect.w + 4, 2}, palette.text);
        fillRect(display, window, gc, Rect{rect.x - 2, rect.y + rect.h, rect.w + 4, 2}, palette.text);
        fillRect(display, window, gc, Rect{rect.x - 2, rect.y - 2, 2, rect.h + 4}, palette.text);
        fillRect(display, window, gc, Rect{rect.x + rect.w, rect.y - 2, 2, rect.h + 4}, palette.text);
    }
}

void drawMiniSlider(Display* display, Window window, GC gc, const Palette& palette, const Rect& rect, const std::string& label, double value, double minValue, double maxValue) {
    drawText(display, window, gc, rect.x, rect.y - 7, label, palette.text);
    fillRect(display, window, gc, rect, palette.track);
    const double t = clampDouble((value - minValue) / (maxValue - minValue), 0.0, 1.0);
    const int filled = static_cast<int>(std::lround(t * rect.w));
    fillRect(display, window, gc, Rect{rect.x, rect.y, filled, rect.h}, palette.fill);
    fillRect(display, window, gc, Rect{rect.x + filled - 4, rect.y - 3, 8, rect.h + 6}, palette.knob);
}

void drawModeButton(Display* display, Window window, GC gc, const Palette& palette, const Rect& rect, const std::string& label, bool active) {
    fillRect(display, window, gc, rect, active ? palette.buttonActive : palette.button);
    drawText(display, window, gc, rect.x + 10, rect.y + 18, label, active ? palette.buttonActiveText : palette.buttonText);
}

void updateSliderFromMouse(const Slider& slider, int mouseX) {
    const double t = clampDouble(static_cast<double>(mouseX - slider.rect.x) / slider.rect.w, 0.0, 1.0);
    *slider.value = slider.min + t * (slider.max - slider.min);
}

PreviewLayout computePreviewLayout(int windowWidth, int windowHeight) {
    const int previewLeft = kMargin;
    const int previewTop = 44;
    const int previewRight = windowWidth - kPanelWidth - kMargin;
    const int previewBottom = windowHeight - kFooterHeight - 60;
    const int maxPreviewWidth = std::max(1, previewRight - previewLeft);
    const int maxPreviewHeight = std::max(1, previewBottom - previewTop);
    return PreviewLayout{
        maxPreviewWidth,
        maxPreviewHeight,
        previewLeft,
        previewTop,
    };
}

double previewScaleForLayout(const Image& source, const PreviewLayout& layout, int zoom) {
    const double fitScale = std::min(
        static_cast<double>(layout.width) / std::max(1, source.width),
        static_cast<double>(layout.height) / std::max(1, source.height)
    );
    return fitScale * std::max(1, zoom);
}

void drawPreview(Display* display, Drawable drawable, GC gc, const Visual* visual, const Image& source, const std::vector<std::uint8_t>& dithered, const Settings& settings, PreviewMode mode, const PreviewLayout& layout, double panX, double panY) {
    XImage* image = XCreateImage(
        display,
        DefaultVisual(display, DefaultScreen(display)),
        DefaultDepth(display, DefaultScreen(display)),
        ZPixmap,
        0,
        nullptr,
        static_cast<unsigned int>(layout.width),
        static_cast<unsigned int>(layout.height),
        32,
        0
    );

    if (!image) {
        return;
    }

    image->data = static_cast<char*>(std::calloc(static_cast<std::size_t>(image->bytes_per_line), static_cast<std::size_t>(image->height)));
    if (!image->data) {
        image->data = nullptr;
        XDestroyImage(image);
        return;
    }

    const double zoomScale = previewScaleForLayout(source, layout, settings.scale);
    const double centerX = static_cast<double>(source.width) * 0.5 + panX;
    const double centerY = static_cast<double>(source.height) * 0.5 + panY;

    for (int y = 0; y < layout.height; ++y) {
        const double sampleY = (static_cast<double>(y) - layout.height * 0.5) / zoomScale + centerY;
        for (int x = 0; x < layout.width; ++x) {
            const double sampleX = (static_cast<double>(x) - layout.width * 0.5) / zoomScale + centerX;
            const bool inside = sampleX >= 0.0 && sampleY >= 0.0 && sampleX < source.width && sampleY < source.height;
            Pixel output = Pixel{};
            if (inside) {
                const int sourceX = static_cast<int>(sampleX);
                const int sourceY = static_cast<int>(sampleY);
                const std::size_t index = static_cast<std::size_t>(sourceY * source.width + sourceX);
                output = mode == PreviewMode::Original
                    ? source.pixels[index]
                    : channelColor(source, settings, std::min<int>(dithered[index], settings.channelCount - 1), sourceX, sourceY);
            } else {
                output = Pixel{20, 22, 28};
            }
            const unsigned long pixel = pixelFromRgb(visual, output.r, output.g, output.b);
            XPutPixel(image, x, y, pixel);
        }
    }

    XPutImage(display, drawable, gc, image, 0, 0, layout.x, layout.y, static_cast<unsigned int>(layout.width), static_cast<unsigned int>(layout.height));
    XDestroyImage(image);
}

void importImageFromPath(const std::string& path, Image& source, Settings& settings, std::vector<std::uint8_t>& dithered, bool& dirty, bool& needsRedraw) {
    if (auto loaded = loadImageAny(path)) {
        source = *loaded;
        dithered = ditherImage(source, settings);
        dirty = false;
        needsRedraw = true;
        std::cout << "Loaded image: " << path << "\n";
    } else {
        std::cerr << "Could not load image: " << path << "\n";
    }
}

Pixmap createBackBuffer(Display* display, Drawable drawable, int width, int height, int depth) {
    return XCreatePixmap(display, drawable, static_cast<unsigned int>(width), static_cast<unsigned int>(height), static_cast<unsigned int>(depth));
}

} // namespace

int main(int argc, char** argv) {
    Image source = makeDemoImage();
    if (argc > 1) {
        if (auto loaded = loadPpm(argv[1])) {
            source = *loaded;
        } else {
            std::cerr << "Could not load PPM image: " << argv[1] << "\n";
        }
    }

    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        std::cerr << "Could not open X display.\n";
        return 1;
    }

    const int screen = DefaultScreen(display);
    const Visual* visual = DefaultVisual(display, screen);
    const int depth = DefaultDepth(display, screen);
    Settings settings;
    ensureChannelCount(settings);
    PreviewMode previewMode = PreviewMode::Dithered;
    auto sliders = makeSliders(settings);
    auto buttons = makeButtons();
    Palette palette = makePalette(display, screen, settings.theme);

    int windowWidth = 920;
    int windowHeight = 720;

    Window window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        80,
        80,
        static_cast<unsigned int>(windowWidth),
        static_cast<unsigned int>(windowHeight),
        1,
        BlackPixel(display, screen),
        palette.background
    );
    XStoreName(display, window, "OpenDither");
    XSelectInput(display, window, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);
    XMapWindow(display, window);

    GC gc = XCreateGC(display, window, 0, nullptr);
    bool running = true;
    bool dirty = true;
    bool needsRedraw = true;
    bool importPending = false;
    std::atomic<bool> importCompleted{false};
    int activeSlider = -1;
    double viewPanX = 0.0;
    double viewPanY = 0.0;
    bool panning = false;
    int panStartX = 0;
    int panStartY = 0;
    double panOriginX = 0.0;
    double panOriginY = 0.0;
    Image workingSource = prepareImage(source, settings.pixelSize);
    std::vector<std::uint8_t> dithered = ditherImage(workingSource, settings);
    Pixmap backBuffer = createBackBuffer(display, window, windowWidth, windowHeight, depth);
    std::mutex importMutex;
    std::optional<std::string> finishedImportPath;
    std::thread importThread;

    const auto startImport = [&]() {
        if (importPending) {
            return;
        }

        importPending = true;
        importCompleted = false;
        {
            std::lock_guard<std::mutex> lock(importMutex);
            finishedImportPath.reset();
        }
        importThread = std::thread([&]() {
            std::optional<std::string> selected = chooseImagePath();
            {
                std::lock_guard<std::mutex> lock(importMutex);
                finishedImportPath = std::move(selected);
            }
            importCompleted = true;
        });
    };

    const auto redraw = [&]() {
        palette = makePalette(display, screen, settings.theme);
        if (dirty) {
            workingSource = prepareImage(source, settings.pixelSize);
            dithered = ditherImage(workingSource, settings);
            dirty = false;
        }

        if (backBuffer != None) {
            XFreePixmap(display, backBuffer);
        }
        backBuffer = createBackBuffer(display, window, windowWidth, windowHeight, depth);

        layoutControls(sliders, buttons, windowWidth, settings.activeTab);

        fillRect(display, backBuffer, gc, Rect{0, 0, windowWidth, windowHeight}, palette.background);
        fillRect(display, backBuffer, gc, Rect{windowWidth - kPanelWidth, 0, kPanelWidth, windowHeight}, palette.panel);
        fillRect(display, backBuffer, gc, Rect{windowWidth - kPanelWidth - 1, 0, 1, windowHeight}, palette.border);

        drawText(display, backBuffer, gc, kMargin, 24, "OpenDither", palette.text);
        const Rect themeToggle{windowWidth - kPanelWidth + kMargin, 20, 86, 28};
        drawToggle(display, backBuffer, gc, palette, themeToggle, settings.theme == Theme::Dark ? "Dark" : "Light", settings.theme == Theme::Dark);
        drawText(display, backBuffer, gc, themeToggle.x + 96, 39, "Theme", palette.mutedText);

        const PreviewLayout previewLayout = computePreviewLayout(windowWidth, windowHeight);
        fillRect(display, backBuffer, gc, Rect{previewLayout.x - 1, previewLayout.y - 1, previewLayout.width + 2, previewLayout.height + 2}, palette.border);
        fillRect(display, backBuffer, gc, Rect{previewLayout.x, previewLayout.y, previewLayout.width, previewLayout.height}, palette.panel);
        drawPreview(display, backBuffer, gc, visual, workingSource, dithered, settings, previewMode, previewLayout, viewPanX, viewPanY);

        const Rect importButton{previewLayout.x, previewLayout.y + previewLayout.height + 8, 92, 28};
        const Rect flipButton{previewLayout.x + 100, previewLayout.y + previewLayout.height + 8, 156, 28};
        drawToggle(display, backBuffer, gc, palette, importButton, "Import");
        drawToggle(display, backBuffer, gc, palette, flipButton, previewMode == PreviewMode::Dithered ? "Dithered" : "Original");

        drawStepControl(display, backBuffer, gc, palette, windowWidth, 64, "Zoom", settings.scale, "x");
        drawStepControl(display, backBuffer, gc, palette, windowWidth, 116, "Pixel Size", settings.pixelSize, "px");

        const int panelX = windowWidth - kPanelWidth + kMargin;
        const Rect colorsTab{panelX, 170, 122, 28};
        const Rect ditherTab{panelX + 130, 170, 122, 28};
        drawTab(display, backBuffer, gc, palette, colorsTab, "Colors", settings.activeTab == SideTab::Colors);
        drawTab(display, backBuffer, gc, palette, ditherTab, "Dither", settings.activeTab == SideTab::Dither);

        if (settings.activeTab == SideTab::Colors) {
            drawText(display, backBuffer, gc, panelX, 226, "Channels", palette.text);
            const std::array<int, 3> channelCounts{2, 3, 4};
            for (std::size_t i = 0; i < channelCounts.size(); ++i) {
                const Rect channelRect{panelX + static_cast<int>(i) * 74, 242, 66, 28};
                drawModeButton(display, backBuffer, gc, palette, channelRect, std::to_string(channelCounts[i]), settings.channelCount == channelCounts[i]);
            }

            const int stripY = 286;
            const int cardGap = 8;
            const int cardWidth = (kPanelWidth - 2 * kMargin - static_cast<int>(cardGap * 3)) / std::max(1, settings.channelCount);
            for (int i = 0; i < settings.channelCount; ++i) {
                const int cardX = panelX + i * (cardWidth + cardGap);
                const Rect card{cardX, stripY, cardWidth, 56};
                fillRect(display, backBuffer, gc, card, i == settings.selectedChannel ? palette.buttonActive : palette.button);
                drawText(display, backBuffer, gc, card.x + 8, card.y + 18, "C" + std::to_string(i + 1), i == settings.selectedChannel ? palette.buttonActiveText : palette.buttonText);
                drawSwatch(display, backBuffer, gc, palette, Rect{card.x + 8, card.y + 26, 22, 22}, settings.colors[static_cast<std::size_t>(i)], false);
                drawText(display, backBuffer, gc, card.x + 34, card.y + 43, settings.channelModes[static_cast<std::size_t>(i)] == ChannelMode::Solid ? "S" : "G", palette.mutedText);
            }

            const std::size_t idx = static_cast<std::size_t>(settings.selectedChannel);
            const int detailY = 356;
            drawText(display, backBuffer, gc, panelX, detailY, "Selected Channel", palette.text);
            const Rect solidButton{panelX, detailY + 18, 88, 28};
            const Rect gradientButton{panelX + 96, detailY + 18, 96, 28};
            drawModeButton(display, backBuffer, gc, palette, solidButton, "Solid", settings.channelModes[idx] == ChannelMode::Solid);
            drawModeButton(display, backBuffer, gc, palette, gradientButton, "Grad", settings.channelModes[idx] == ChannelMode::Gradient);

            if (settings.channelModes[idx] == ChannelMode::Solid) {
                const Rect colorA{panelX, detailY + 68, kSwatchSize, kSwatchSize};
                const Rect colorAButton{panelX + 36, detailY + 70, 150, 24};
                drawSwatch(display, backBuffer, gc, palette, colorA, settings.colors[idx], true);
                drawToggle(display, backBuffer, gc, palette, colorAButton, colorToHex(settings.colors[idx]));
            } else {
                const Rect colorA{panelX, detailY + 68, kSwatchSize, kSwatchSize};
                const Rect colorAButton{panelX + 36, detailY + 70, 150, 24};
                const Rect colorB{panelX, detailY + 104, kSwatchSize, kSwatchSize};
                const Rect colorBButton{panelX + 36, detailY + 106, 150, 24};
                drawSwatch(display, backBuffer, gc, palette, colorA, settings.colors[idx], true);
                drawToggle(display, backBuffer, gc, palette, colorAButton, colorToHex(settings.colors[idx]));
                drawSwatch(display, backBuffer, gc, palette, colorB, settings.gradientColors[idx], true);
                drawToggle(display, backBuffer, gc, palette, colorBButton, colorToHex(settings.gradientColors[idx]));

                const Rect horiz{panelX, detailY + 144, 60, 28};
                const Rect vert{panelX + 68, detailY + 144, 60, 28};
                const Rect rad{panelX + 136, detailY + 144, 60, 28};
                drawModeButton(display, backBuffer, gc, palette, horiz, "H", settings.gradientTypes[idx] == GradientType::Horizontal);
                drawModeButton(display, backBuffer, gc, palette, vert, "V", settings.gradientTypes[idx] == GradientType::Vertical);
                drawModeButton(display, backBuffer, gc, palette, rad, "R", settings.gradientTypes[idx] == GradientType::Radial);

                const Rect gradientSlider{panelX, detailY + 196, 290, 18};
                drawMiniSlider(display, backBuffer, gc, palette, gradientSlider, "Blend Width", settings.gradientSpread[idx], 0.05, 1.0);
            }
        } else {
            for (const Slider& slider : sliders) {
                drawSlider(display, backBuffer, gc, palette, slider);
            }

            for (const Button& button : buttons) {
                drawButton(display, backBuffer, gc, palette, button, settings.formula);
            }
        }

        drawFooter(display, backBuffer, gc, palette, windowWidth, windowHeight, previewMode);
        if (importPending) {
            drawText(display, backBuffer, gc, windowWidth - 170, windowHeight - 25, "Importing...", palette.mutedText);
        }
        XCopyArea(display, backBuffer, window, gc, 0, 0, static_cast<unsigned int>(windowWidth), static_cast<unsigned int>(windowHeight), 0, 0);
        XFlush(display);
    };

    while (running) {
        if (importPending && importCompleted) {
            std::optional<std::string> importedPath;
            {
                std::lock_guard<std::mutex> lock(importMutex);
                if (finishedImportPath.has_value()) {
                    importedPath = std::move(finishedImportPath);
                    finishedImportPath.reset();
                }
            }

            if (importThread.joinable()) {
                importThread.join();
            }
            importPending = false;

            if (importedPath.has_value()) {
                importImageFromPath(*importedPath, source, settings, dithered, dirty, needsRedraw);
                ensureChannelCount(settings);
                sliders = makeSliders(settings);
                palette = makePalette(display, screen, settings.theme);
                viewPanX = 0.0;
                viewPanY = 0.0;
                previewMode = PreviewMode::Original;
            }
        }

        while (XPending(display) > 0) {
            XEvent event{};
            XNextEvent(display, &event);

            if (event.type == Expose) {
                needsRedraw = true;
            } else if (event.type == ConfigureNotify) {
                windowWidth = event.xconfigure.width;
                windowHeight = event.xconfigure.height;
                needsRedraw = true;
            } else if (event.type == ButtonPress) {
                if (event.xbutton.button == Button4) {
                    settings.scale = std::min(16, settings.scale + 1);
                    needsRedraw = true;
                    continue;
                }
                if (event.xbutton.button == Button5) {
                    settings.scale = std::max(1, settings.scale - 1);
                    needsRedraw = true;
                    continue;
                }

                layoutControls(sliders, buttons, windowWidth, settings.activeTab);
                activeSlider = -1;
                const int panelX = windowWidth - kPanelWidth + kMargin;
                const Rect themeToggle{panelX, 20, 86, 28};
                const Rect zoomMinus{panelX, 78, 38, 26};
                const Rect zoomPlus{panelX + 52, 78, 38, 26};
                const Rect pixelMinus{panelX, 130, 38, 26};
                const Rect pixelPlus{panelX + 52, 130, 38, 26};
                const Rect colorsTab{panelX, 170, 122, 28};
                const Rect ditherTab{panelX + 130, 170, 122, 28};
                const PreviewLayout previewLayout = computePreviewLayout(windowWidth, windowHeight);
                const Rect previewRect{previewLayout.x, previewLayout.y, previewLayout.width, previewLayout.height};
                const Rect importButton{previewLayout.x, previewLayout.y + previewLayout.height + 8, 92, 28};
                const Rect flipButton{previewLayout.x + 100, previewLayout.y + previewLayout.height + 8, 156, 28};

                if (themeToggle.contains(event.xbutton.x, event.xbutton.y)) {
                    settings.theme = settings.theme == Theme::Dark ? Theme::Light : Theme::Dark;
                    needsRedraw = true;
                }
                if (colorsTab.contains(event.xbutton.x, event.xbutton.y)) {
                    settings.activeTab = SideTab::Colors;
                    needsRedraw = true;
                }
                if (ditherTab.contains(event.xbutton.x, event.xbutton.y)) {
                    settings.activeTab = SideTab::Dither;
                    needsRedraw = true;
                }

                if (zoomMinus.contains(event.xbutton.x, event.xbutton.y)) {
                    settings.scale = std::max(1, settings.scale - 1);
                    needsRedraw = true;
                }
                if (zoomPlus.contains(event.xbutton.x, event.xbutton.y)) {
                    settings.scale = std::min(16, settings.scale + 1);
                    needsRedraw = true;
                }

                if (pixelMinus.contains(event.xbutton.x, event.xbutton.y)) {
                    settings.pixelSize = std::max(1, settings.pixelSize - 1);
                    dirty = true;
                    needsRedraw = true;
                }
                if (pixelPlus.contains(event.xbutton.x, event.xbutton.y)) {
                    settings.pixelSize = std::min(8, settings.pixelSize + 1);
                    dirty = true;
                    needsRedraw = true;
                }

                if (importButton.contains(event.xbutton.x, event.xbutton.y)) {
                    startImport();
                }
                if (flipButton.contains(event.xbutton.x, event.xbutton.y)) {
                    previewMode = previewMode == PreviewMode::Dithered ? PreviewMode::Original : PreviewMode::Dithered;
                    needsRedraw = true;
                }

                if (previewRect.contains(event.xbutton.x, event.xbutton.y) &&
                    !importButton.contains(event.xbutton.x, event.xbutton.y) &&
                    !flipButton.contains(event.xbutton.x, event.xbutton.y)) {
                    panning = true;
                    panStartX = event.xbutton.x;
                    panStartY = event.xbutton.y;
                    panOriginX = viewPanX;
                    panOriginY = viewPanY;
                }

                if (settings.activeTab == SideTab::Dither) {
                    for (std::size_t i = 0; i < sliders.size(); ++i) {
                        if (sliders[i].rect.contains(event.xbutton.x, event.xbutton.y)) {
                            activeSlider = static_cast<int>(i);
                            updateSliderFromMouse(sliders[i], event.xbutton.x);
                            dirty = true;
                            needsRedraw = true;
                        }
                    }

                    for (const Button& button : buttons) {
                        if (button.rect.contains(event.xbutton.x, event.xbutton.y)) {
                            settings.formula = button.formula;
                            dirty = true;
                            needsRedraw = true;
                        }
                    }
                } else {
                    const std::array<int, 3> channelCounts{2, 3, 4};
                    for (std::size_t i = 0; i < channelCounts.size(); ++i) {
                        const Rect channelRect{panelX + static_cast<int>(i) * 74, 242, 66, 28};
                        if (channelRect.contains(event.xbutton.x, event.xbutton.y)) {
                            settings.channelCount = channelCounts[i];
                            ensureChannelCount(settings);
                            dirty = true;
                            needsRedraw = true;
                        }
                    }

                    const int stripY = 286;
                    const int cardGap = 8;
                    const int cardWidth = (kPanelWidth - 2 * kMargin - static_cast<int>(cardGap * 3)) / std::max(1, settings.channelCount);
                    for (int i = 0; i < settings.channelCount; ++i) {
                        const int cardX = panelX + i * (cardWidth + cardGap);
                        const Rect card{cardX, stripY, cardWidth, 56};
                        if (card.contains(event.xbutton.x, event.xbutton.y)) {
                            settings.selectedChannel = i;
                            needsRedraw = true;
                        }
                    }

                    const std::size_t idx = static_cast<std::size_t>(settings.selectedChannel);
                    const int detailY = 356;
                    const Rect solidButton{panelX, detailY + 18, 88, 28};
                    const Rect gradientButton{panelX + 96, detailY + 18, 96, 28};
                    if (solidButton.contains(event.xbutton.x, event.xbutton.y)) {
                        settings.channelModes[idx] = ChannelMode::Solid;
                        needsRedraw = true;
                    }
                    if (gradientButton.contains(event.xbutton.x, event.xbutton.y)) {
                        settings.channelModes[idx] = ChannelMode::Gradient;
                        needsRedraw = true;
                    }

                    const Rect colorA{panelX, detailY + 68, kSwatchSize, kSwatchSize};
                    const Rect colorAButton{panelX + 36, detailY + 70, 150, 24};
                    if (colorA.contains(event.xbutton.x, event.xbutton.y) || colorAButton.contains(event.xbutton.x, event.xbutton.y)) {
                        if (auto picked = chooseColor(settings.colors[idx])) {
                            settings.colors[idx] = *picked;
                            needsRedraw = true;
                        }
                    }

                    if (settings.channelModes[idx] == ChannelMode::Gradient) {
                        const Rect colorB{panelX, detailY + 104, kSwatchSize, kSwatchSize};
                        const Rect colorBButton{panelX + 36, detailY + 106, 150, 24};
                        if (colorB.contains(event.xbutton.x, event.xbutton.y) || colorBButton.contains(event.xbutton.x, event.xbutton.y)) {
                            if (auto picked = chooseColor(settings.gradientColors[idx])) {
                                settings.gradientColors[idx] = *picked;
                                needsRedraw = true;
                            }
                        }

                        const Rect horiz{panelX, detailY + 144, 60, 28};
                        const Rect vert{panelX + 68, detailY + 144, 60, 28};
                        const Rect rad{panelX + 136, detailY + 144, 60, 28};
                        if (horiz.contains(event.xbutton.x, event.xbutton.y)) {
                            settings.gradientTypes[idx] = GradientType::Horizontal;
                            needsRedraw = true;
                        }
                        if (vert.contains(event.xbutton.x, event.xbutton.y)) {
                            settings.gradientTypes[idx] = GradientType::Vertical;
                            needsRedraw = true;
                        }
                        if (rad.contains(event.xbutton.x, event.xbutton.y)) {
                            settings.gradientTypes[idx] = GradientType::Radial;
                            needsRedraw = true;
                        }

                        const Rect gradientSlider{panelX, detailY + 196, 290, 18};
                        if (gradientSlider.contains(event.xbutton.x, event.xbutton.y)) {
                            const double t = clampDouble(static_cast<double>(event.xbutton.x - gradientSlider.x) / gradientSlider.w, 0.0, 1.0);
                            settings.gradientSpread[idx] = 0.05 + t * (1.0 - 0.05);
                            needsRedraw = true;
                        }
                    }
                }
            } else if (event.type == MotionNotify && activeSlider >= 0) {
                const double before = *sliders[static_cast<std::size_t>(activeSlider)].value;
                updateSliderFromMouse(sliders[static_cast<std::size_t>(activeSlider)], event.xmotion.x);
                if (*sliders[static_cast<std::size_t>(activeSlider)].value != before) {
                    dirty = true;
                    needsRedraw = true;
                }
            } else if (event.type == MotionNotify && panning && (event.xmotion.state & Button1Mask)) {
                const PreviewLayout previewLayout = computePreviewLayout(windowWidth, windowHeight);
                const double zoomScale = previewScaleForLayout(workingSource, previewLayout, settings.scale);
                viewPanX = panOriginX - static_cast<double>(event.xmotion.x - panStartX) / std::max(0.0001, zoomScale);
                viewPanY = panOriginY - static_cast<double>(event.xmotion.y - panStartY) / std::max(0.0001, zoomScale);
                needsRedraw = true;
            } else if (event.type == MotionNotify && settings.activeTab == SideTab::Colors && (event.xmotion.state & Button1Mask)) {
                const int panelX = windowWidth - kPanelWidth + kMargin;
                const int detailY = 356;
                const std::size_t idx = static_cast<std::size_t>(settings.selectedChannel);
                if (settings.channelModes[idx] == ChannelMode::Gradient) {
                    const Rect gradientSlider{panelX, detailY + 196, 290, 18};
                    if (gradientSlider.contains(event.xmotion.x, event.xmotion.y)) {
                        const double t = clampDouble(static_cast<double>(event.xmotion.x - gradientSlider.x) / gradientSlider.w, 0.0, 1.0);
                        settings.gradientSpread[idx] = 0.05 + t * (1.0 - 0.05);
                        needsRedraw = true;
                    }
                }
            } else if (event.type == ButtonRelease) {
                activeSlider = -1;
                panning = false;
            } else if (event.type == KeyPress) {
                const KeySym key = XLookupKeysym(&event.xkey, 0);
                if (key == XK_Escape) {
                    running = false;
                } else if (key == XK_r || key == XK_R) {
                    settings = Settings{};
                    ensureChannelCount(settings);
                    sliders = makeSliders(settings);
                    palette = makePalette(display, screen, settings.theme);
                    viewPanX = 0.0;
                    viewPanY = 0.0;
                    dirty = true;
                    needsRedraw = true;
                } else if (key == XK_space) {
                    previewMode = previewMode == PreviewMode::Dithered ? PreviewMode::Original : PreviewMode::Dithered;
                    needsRedraw = true;
                } else if (key == XK_i || key == XK_I) {
                    startImport();
                } else if (key == XK_s || key == XK_S) {
                    if (dirty) {
                        workingSource = prepareImage(source, settings.pixelSize);
                        dithered = ditherImage(workingSource, settings);
                        dirty = false;
                    }
                    if (savePpm("opendither-output.ppm", workingSource, dithered, std::clamp(settings.channelCount, 2, 4))) {
                        std::cout << "Saved opendither-output.ppm\n";
                    }
                }
            }
        }

        if (needsRedraw || dirty) {
            redraw();
            needsRedraw = false;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    }

    if (importThread.joinable()) {
        importThread.join();
    }
    if (backBuffer != None) {
        XFreePixmap(display, backBuffer);
    }
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
