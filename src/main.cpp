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

namespace {

constexpr int kPanelWidth = 260;
constexpr int kMargin = 16;
constexpr int kSliderWidth = 190;
constexpr int kSliderHeight = 18;
constexpr int kButtonHeight = 28;
constexpr int kFooterHeight = 28;
constexpr int kTabHeight = 28;
constexpr int kSwatchSize = 28;

struct Pixel {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

struct Image {
    int width = 0;
    int height = 0;
    std::vector<Pixel> pixels;
};

enum class Formula {
    Threshold,
    OrderedBayer,
    FloydSteinberg,
    Atkinson,
    JarvisJudiceNinke,
};

enum class PreviewMode {
    Original,
    Dithered,
};

enum class ColorMode {
    Monochrome,
    Palette,
    HorizontalGradient,
    VerticalGradient,
    RadialGradient,
};

enum class SideTab {
    Colors,
    Dither,
};

struct Settings {
    double threshold = 128.0;
    double contrast = 1.0;
    double brightness = 0.0;
    double noise = 0.0;
    int scale = 2;
    Formula formula = Formula::FloydSteinberg;
    ColorMode colorMode = ColorMode::Monochrome;
    SideTab activeTab = SideTab::Dither;
    int selectedColorIndex = 0;
    std::vector<Pixel> colors{
        Pixel{0, 0, 0},
        Pixel{255, 255, 255},
    };
};

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

Palette makePalette(Display* display, int screen) {
    const Visual* visual = DefaultVisual(display, screen);
    return Palette{
        pixelFromRgb(visual, 242, 240, 235),
        pixelFromRgb(visual, 232, 229, 222),
        pixelFromRgb(visual, 28, 28, 28),
        pixelFromRgb(visual, 84, 84, 84),
        pixelFromRgb(visual, 196, 196, 196),
        pixelFromRgb(visual, 42, 95, 180),
        pixelFromRgb(visual, 24, 24, 24),
        pixelFromRgb(visual, 225, 225, 225),
        pixelFromRgb(visual, 30, 30, 30),
        pixelFromRgb(visual, 34, 34, 34),
        pixelFromRgb(visual, 255, 255, 255),
        pixelFromRgb(visual, 0, 0, 0),
        pixelFromRgb(visual, 255, 255, 255),
        pixelFromRgb(visual, 209, 206, 198),
    };
}

Image makeDemoImage() {
    Image image;
    image.width = 256;
    image.height = 192;
    image.pixels.resize(static_cast<std::size_t>(image.width * image.height));

    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const double nx = static_cast<double>(x) / (image.width - 1);
            const double ny = static_cast<double>(y) / (image.height - 1);
            const double wave = (std::sin(nx * 18.0) + std::cos(ny * 14.0)) * 0.5;
            const double vignette = 1.0 - std::hypot(nx - 0.5, ny - 0.5);
            const int value = static_cast<int>(255.0 * clampDouble(nx * 0.55 + ny * 0.25 + wave * 0.12 + vignette * 0.22, 0.0, 1.0));
            image.pixels[static_cast<std::size_t>(y * image.width + x)] = Pixel{toByte(value), toByte(value * 0.92 + x % 32), toByte(value * 0.85 + y % 48)};
        }
    }

    return image;
}

std::string readToken(std::istream& input) {
    std::string token;
    while (input >> token) {
        if (!token.empty() && token[0] == '#') {
            std::string rest;
            std::getline(input, rest);
            continue;
        }
        return token;
    }
    return {};
}

std::optional<Image> loadPpm(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }

    const std::string magic = readToken(file);
    if (magic != "P3" && magic != "P6") {
        return std::nullopt;
    }

    Image image;
    image.width = std::stoi(readToken(file));
    image.height = std::stoi(readToken(file));
    const int maxValue = std::stoi(readToken(file));
    if (image.width <= 0 || image.height <= 0 || maxValue <= 0) {
        return std::nullopt;
    }

    image.pixels.resize(static_cast<std::size_t>(image.width * image.height));
    const auto scaleValue = [maxValue](int value) {
        return toByte(static_cast<double>(value) * 255.0 / maxValue);
    };

    if (magic == "P3") {
        for (Pixel& pixel : image.pixels) {
            pixel.r = scaleValue(std::stoi(readToken(file)));
            pixel.g = scaleValue(std::stoi(readToken(file)));
            pixel.b = scaleValue(std::stoi(readToken(file)));
        }
        return image;
    }

    file.get();
    for (Pixel& pixel : image.pixels) {
        unsigned char rgbBytes[3]{};
        file.read(reinterpret_cast<char*>(rgbBytes), 3);
        if (!file) {
            return std::nullopt;
        }
        pixel.r = scaleValue(rgbBytes[0]);
        pixel.g = scaleValue(rgbBytes[1]);
        pixel.b = scaleValue(rgbBytes[2]);
    }
    return image;
}

std::string shellQuote(const std::string& value) {
    std::string quoted = "'";
    for (char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
    }
    quoted += "'";
    return quoted;
}

std::optional<std::filesystem::path> makeTempPpmPath() {
    std::filesystem::path templatePath = std::filesystem::temp_directory_path() / "opendither-import-XXXXXX.ppm";
    std::string temp = templatePath.string();
    std::vector<char> buffer(temp.begin(), temp.end());
    buffer.push_back('\0');

    int fd = mkstemps(buffer.data(), 4);
    if (fd == -1) {
        return std::nullopt;
    }
    close(fd);
    return std::filesystem::path(buffer.data());
}

std::optional<Image> loadImageAny(const std::string& path) {
    if (auto ppm = loadPpm(path)) {
        return ppm;
    }

    const auto tempPath = makeTempPpmPath();
    if (!tempPath) {
        return std::nullopt;
    }

    const std::string command = "ffmpeg -y -hide_banner -loglevel error -i " + shellQuote(path) + " -frames:v 1 -f image2 -vcodec ppm " + shellQuote(tempPath->string());
    if (std::system(command.c_str()) != 0) {
        std::filesystem::remove(*tempPath);
        return std::nullopt;
    }

    std::optional<Image> image = loadPpm(tempPath->string());
    std::filesystem::remove(*tempPath);
    return image;
}

double luminance(const Pixel& pixel, const Settings& settings, int x, int y) {
    double value = pixel.r * 0.299 + pixel.g * 0.587 + pixel.b * 0.114;
    value = (value - 128.0) * settings.contrast + 128.0 + settings.brightness;
    if (settings.noise > 0.0) {
        const unsigned int hash = static_cast<unsigned int>((x * 73856093) ^ (y * 19349663));
        const double noise = (static_cast<double>(hash % 1024) / 1023.0 - 0.5) * settings.noise;
        value += noise;
    }
    return clampDouble(value, 0.0, 255.0);
}

std::vector<std::uint8_t> ditherImage(const Image& input, const Settings& settings) {
    const int width = input.width;
    const int height = input.height;
    std::vector<double> gray(static_cast<std::size_t>(width * height), 0.0);
    std::vector<std::uint8_t> output(static_cast<std::size_t>(width * height), 0);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            gray[static_cast<std::size_t>(y * width + x)] = luminance(input.pixels[static_cast<std::size_t>(y * width + x)], settings, x, y);
        }
    }

    const auto writeThreshold = [&](int x, int y, double threshold) {
        const std::size_t index = static_cast<std::size_t>(y * width + x);
        output[index] = gray[index] >= threshold ? 255 : 0;
    };

    if (settings.formula == Formula::Threshold) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                writeThreshold(x, y, settings.threshold);
            }
        }
        return output;
    }

    if (settings.formula == Formula::OrderedBayer) {
        constexpr std::array<std::array<int, 4>, 4> bayer{{
            {{0, 8, 2, 10}},
            {{12, 4, 14, 6}},
            {{3, 11, 1, 9}},
            {{15, 7, 13, 5}},
        }};
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const double offset = (static_cast<double>(bayer[static_cast<std::size_t>(y % 4)][static_cast<std::size_t>(x % 4)]) - 7.5) * 10.0;
                writeThreshold(x, y, settings.threshold + offset);
            }
        }
        return output;
    }

    const auto diffuse = [&](int x, int y, double error, double factor) {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return;
        }
        const std::size_t index = static_cast<std::size_t>(y * width + x);
        gray[index] = clampDouble(gray[index] + error * factor, 0.0, 255.0);
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y * width + x);
            const double oldValue = gray[index];
            const double newValue = oldValue >= settings.threshold ? 255.0 : 0.0;
            const double error = oldValue - newValue;
            output[index] = toByte(newValue);

            if (settings.formula == Formula::FloydSteinberg) {
                diffuse(x + 1, y, error, 7.0 / 16.0);
                diffuse(x - 1, y + 1, error, 3.0 / 16.0);
                diffuse(x, y + 1, error, 5.0 / 16.0);
                diffuse(x + 1, y + 1, error, 1.0 / 16.0);
            } else if (settings.formula == Formula::Atkinson) {
                diffuse(x + 1, y, error, 1.0 / 8.0);
                diffuse(x + 2, y, error, 1.0 / 8.0);
                diffuse(x - 1, y + 1, error, 1.0 / 8.0);
                diffuse(x, y + 1, error, 1.0 / 8.0);
                diffuse(x + 1, y + 1, error, 1.0 / 8.0);
                diffuse(x, y + 2, error, 1.0 / 8.0);
            } else if (settings.formula == Formula::JarvisJudiceNinke) {
                diffuse(x + 1, y, error, 7.0 / 48.0);
                diffuse(x + 2, y, error, 5.0 / 48.0);
                diffuse(x - 2, y + 1, error, 3.0 / 48.0);
                diffuse(x - 1, y + 1, error, 5.0 / 48.0);
                diffuse(x, y + 1, error, 7.0 / 48.0);
                diffuse(x + 1, y + 1, error, 5.0 / 48.0);
                diffuse(x + 2, y + 1, error, 3.0 / 48.0);
                diffuse(x - 2, y + 2, error, 1.0 / 48.0);
                diffuse(x - 1, y + 2, error, 3.0 / 48.0);
                diffuse(x, y + 2, error, 5.0 / 48.0);
                diffuse(x + 1, y + 2, error, 3.0 / 48.0);
                diffuse(x + 2, y + 2, error, 1.0 / 48.0);
            }
        }
    }

    return output;
}

bool savePpm(const std::string& path, const Image& image, const std::vector<std::uint8_t>& dithered) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    file << "P6\n" << image.width << " " << image.height << "\n255\n";
    for (std::uint8_t value : dithered) {
        const char byte = static_cast<char>(value);
        file.write(&byte, 1);
        file.write(&byte, 1);
        file.write(&byte, 1);
    }
    return true;
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

const char* formulaName(Formula formula) {
    switch (formula) {
        case Formula::Threshold: return "Threshold";
        case Formula::OrderedBayer: return "Ordered Bayer";
        case Formula::FloydSteinberg: return "Floyd-Steinberg";
        case Formula::Atkinson: return "Atkinson";
        case Formula::JarvisJudiceNinke: return "Jarvis";
    }
    return "Unknown";
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

void layoutControls(std::vector<Slider>& sliders, std::vector<Button>& buttons, int windowWidth) {
    const int panelX = windowWidth - kPanelWidth + kMargin;
    int y = 138;
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

void drawToggle(Display* display, Window window, GC gc, const Palette& palette, const Rect& rect, const std::string& label) {
    fillRect(display, window, gc, rect, palette.button);
    drawText(display, window, gc, rect.x + 10, rect.y + 18, label, palette.buttonText);
}

void drawFooter(Display* display, Drawable drawable, GC gc, const Palette& palette, int windowWidth, int windowHeight, PreviewMode previewMode) {
    const Rect footer{0, windowHeight - kFooterHeight, windowWidth, kFooterHeight};
    fillRect(display, drawable, gc, footer, palette.panel);
    drawText(display, drawable, gc, kMargin, windowHeight - 9, "Space/Flip toggle  I/Import image  R reset  S save  Esc quit", palette.text);
    drawText(display, drawable, gc, windowWidth - 170, windowHeight - 9, previewMode == PreviewMode::Dithered ? "View: dithered" : "View: original", palette.mutedText);
}

void drawPixelSizeControl(Display* display, Window window, GC gc, const Palette& palette, int windowWidth, int scale) {
    const int panelX = windowWidth - kPanelWidth + kMargin;
    drawText(display, window, gc, panelX, 64, "Pixel Size", palette.text);

    const Rect minus{panelX, 78, 38, 26};
    const Rect plus{panelX + 52, 78, 38, 26};
    fillRect(display, window, gc, minus, palette.button);
    fillRect(display, window, gc, plus, palette.button);
    drawText(display, window, gc, minus.x + 14, minus.y + 18, "-", palette.buttonText);
    drawText(display, window, gc, plus.x + 14, plus.y + 18, "+", palette.buttonText);
    drawText(display, window, gc, plus.x + 52, plus.y + 18, std::to_string(scale) + "x", palette.text);
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

void drawColorModeButton(Display* display, Window window, GC gc, const Palette& palette, const Rect& rect, const std::string& label, bool active) {
    fillRect(display, window, gc, rect, active ? palette.buttonActive : palette.button);
    drawText(display, window, gc, rect.x + 10, rect.y + 18, label, active ? palette.buttonActiveText : palette.buttonText);
}

const char* colorModeName(ColorMode mode) {
    switch (mode) {
        case ColorMode::Monochrome: return "Mono";
        case ColorMode::Palette: return "Palette";
        case ColorMode::HorizontalGradient: return "H Grad";
        case ColorMode::VerticalGradient: return "V Grad";
        case ColorMode::RadialGradient: return "Radial";
    }
    return "Color";
}

void updateSliderFromMouse(const Slider& slider, int mouseX) {
    const double t = clampDouble(static_cast<double>(mouseX - slider.rect.x) / slider.rect.w, 0.0, 1.0);
    *slider.value = slider.min + t * (slider.max - slider.min);
}

Pixel lerpColor(const Pixel& a, const Pixel& b, double t) {
    return Pixel{
        toByte(a.r + (b.r - a.r) * t),
        toByte(a.g + (b.g - a.g) * t),
        toByte(a.b + (b.b - a.b) * t),
    };
}

Pixel sampleColors(const std::vector<Pixel>& colors, double t) {
    if (colors.empty()) {
        return Pixel{0, 0, 0};
    }
    if (colors.size() == 1) {
        return colors.front();
    }

    t = clampDouble(t, 0.0, 1.0);
    const double segment = t * static_cast<double>(colors.size() - 1);
    const std::size_t index = static_cast<std::size_t>(segment);
    const double localT = segment - static_cast<double>(index);
    if (index >= colors.size() - 1) {
        return colors.back();
    }
    return lerpColor(colors[index], colors[index + 1], localT);
}

PreviewLayout computePreviewLayout(const Image& source, int settingsScale, int windowWidth, int windowHeight) {
    const int previewLeft = kMargin;
    const int previewTop = 44;
    const int previewRight = windowWidth - kPanelWidth - kMargin;
    const int previewBottom = windowHeight - kFooterHeight - 130;
    const int maxPreviewWidth = std::max(1, previewRight - previewLeft);
    const int maxPreviewHeight = std::max(1, previewBottom - previewTop);
    const double desiredScale = static_cast<double>(std::max(1, settingsScale));
    const double fitScale = std::min(
        static_cast<double>(maxPreviewWidth) / std::max(1, source.width),
        static_cast<double>(maxPreviewHeight) / std::max(1, source.height)
    );
    const double chosenScale = std::min(desiredScale, fitScale);
    const int width = std::max(1, static_cast<int>(std::lround(source.width * chosenScale)));
    const int height = std::max(1, static_cast<int>(std::lround(source.height * chosenScale)));
    return PreviewLayout{
        width,
        height,
        previewLeft + (maxPreviewWidth - width) / 2,
        previewTop + std::max(0, (maxPreviewHeight - height) / 2),
    };
}

Pixel colorizePixel(const Image& source, const Settings& settings, int sourceX, int sourceY, std::uint8_t ditherValue) {
    const Pixel dark = settings.colors.empty() ? Pixel{0, 0, 0} : settings.colors.front();
    const Pixel light = settings.colors.size() > 1 ? settings.colors[1] : Pixel{255, 255, 255};

    switch (settings.colorMode) {
        case ColorMode::Monochrome:
            return ditherValue ? light : dark;
        case ColorMode::Palette:
            return sampleColors(settings.colors, ditherValue ? 1.0 : 0.0);
        case ColorMode::HorizontalGradient: {
            const double t = source.width <= 1 ? 0.0 : static_cast<double>(sourceX) / (source.width - 1);
            return sampleColors(settings.colors, t);
        }
        case ColorMode::VerticalGradient: {
            const double t = source.height <= 1 ? 0.0 : static_cast<double>(sourceY) / (source.height - 1);
            return sampleColors(settings.colors, t);
        }
        case ColorMode::RadialGradient: {
            const double nx = source.width <= 1 ? 0.0 : (static_cast<double>(sourceX) / (source.width - 1)) - 0.5;
            const double ny = source.height <= 1 ? 0.0 : (static_cast<double>(sourceY) / (source.height - 1)) - 0.5;
            const double t = clampDouble(std::hypot(nx, ny) * 2.0, 0.0, 1.0);
            return sampleColors(settings.colors, t);
        }
    }

    return light;
}

void drawPreview(Display* display, Drawable drawable, GC gc, const Visual* visual, const Image& source, const std::vector<std::uint8_t>& dithered, const Settings& settings, PreviewMode mode, int previewX, int previewY, int targetWidth, int targetHeight) {
    XImage* image = XCreateImage(
        display,
        DefaultVisual(display, DefaultScreen(display)),
        DefaultDepth(display, DefaultScreen(display)),
        ZPixmap,
        0,
        nullptr,
        static_cast<unsigned int>(targetWidth),
        static_cast<unsigned int>(targetHeight),
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

    for (int y = 0; y < targetHeight; ++y) {
        const int sourceY = std::min(source.height - 1, static_cast<int>((static_cast<long long>(y) * source.height) / std::max(1, targetHeight)));
        for (int x = 0; x < targetWidth; ++x) {
            const int sourceX = std::min(source.width - 1, static_cast<int>((static_cast<long long>(x) * source.width) / std::max(1, targetWidth)));
            const std::size_t index = static_cast<std::size_t>(sourceY * source.width + sourceX);
            const Pixel output = mode == PreviewMode::Original
                ? source.pixels[index]
                : colorizePixel(source, settings, sourceX, sourceY, dithered[index]);
            const unsigned long pixel = pixelFromRgb(visual, output.r, output.g, output.b);
            XPutPixel(image, x, y, pixel);
        }
    }

    XPutImage(display, drawable, gc, image, 0, 0, previewX, previewY, static_cast<unsigned int>(targetWidth), static_cast<unsigned int>(targetHeight));
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
    const Palette palette = makePalette(display, screen);
    Settings settings;
    PreviewMode previewMode = PreviewMode::Dithered;
    auto sliders = makeSliders(settings);
    auto buttons = makeButtons();

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
    std::vector<std::uint8_t> dithered = ditherImage(source, settings);
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
        if (dirty) {
            dithered = ditherImage(source, settings);
            dirty = false;
        }

        if (backBuffer != None) {
            XFreePixmap(display, backBuffer);
        }
        backBuffer = createBackBuffer(display, window, windowWidth, windowHeight, depth);

        layoutControls(sliders, buttons, windowWidth);

        fillRect(display, backBuffer, gc, Rect{0, 0, windowWidth, windowHeight}, palette.background);
        fillRect(display, backBuffer, gc, Rect{windowWidth - kPanelWidth, 0, kPanelWidth, windowHeight}, palette.panel);
        fillRect(display, backBuffer, gc, Rect{windowWidth - kPanelWidth - 1, 0, 1, windowHeight}, palette.border);

        drawText(display, backBuffer, gc, kMargin, 24, "OpenDither - black and white preview", palette.text);
        drawText(display, backBuffer, gc, windowWidth - kPanelWidth + kMargin, 24, settings.activeTab == SideTab::Colors ? "Color Settings" : "Dither Settings", palette.text);

        const int previewX = kMargin;
        const PreviewLayout previewLayout = computePreviewLayout(source, settings.scale, windowWidth, windowHeight);
        drawPreview(display, backBuffer, gc, visual, source, dithered, settings, previewMode, previewLayout.x, previewLayout.y, previewLayout.width, previewLayout.height);

        drawText(display, backBuffer, gc, previewX, previewLayout.y + previewLayout.height + 22, std::string("Formula: ") + formulaName(settings.formula), palette.fill);

        const Rect importButton{previewLayout.x, previewLayout.y + previewLayout.height + 8, 92, 28};
        const Rect flipButton{previewLayout.x + 100, previewLayout.y + previewLayout.height + 8, 156, 28};
        drawToggle(display, backBuffer, gc, palette, importButton, "Import");
        drawToggle(display, backBuffer, gc, palette, flipButton, previewMode == PreviewMode::Dithered ? "Showing: Dithered" : "Showing: Original");

        drawPixelSizeControl(display, backBuffer, gc, palette, windowWidth, settings.scale);

        const int panelX = windowWidth - kPanelWidth + kMargin;
        const Rect colorsTab{panelX, 104, 86, 28};
        const Rect ditherTab{panelX + 92, 104, 102, 28};
        drawTab(display, backBuffer, gc, palette, colorsTab, "Colors", settings.activeTab == SideTab::Colors);
        drawTab(display, backBuffer, gc, palette, ditherTab, "Dither", settings.activeTab == SideTab::Dither);

        if (settings.activeTab == SideTab::Colors) {
            drawText(display, backBuffer, gc, panelX, 150, "Color Mode", palette.text);
            const std::array<ColorMode, 5> modes{
                ColorMode::Monochrome,
                ColorMode::Palette,
                ColorMode::HorizontalGradient,
                ColorMode::VerticalGradient,
                ColorMode::RadialGradient,
            };
            for (std::size_t i = 0; i < modes.size(); ++i) {
                const Rect modeRect{panelX, 166 + static_cast<int>(i) * 34, 190, 28};
                drawColorModeButton(display, backBuffer, gc, palette, modeRect, colorModeName(modes[i]), settings.colorMode == modes[i]);
            }

            drawText(display, backBuffer, gc, panelX, 346, "Colors", palette.text);
            for (std::size_t i = 0; i < settings.colors.size(); ++i) {
                const int rowY = 362 + static_cast<int>(i) * 36;
                const Rect swatch{panelX, rowY, kSwatchSize, kSwatchSize};
                const Rect choose{panelX + 40, rowY + 2, 110, 24};
                drawSwatch(display, backBuffer, gc, palette, swatch, settings.colors[i], static_cast<int>(i) == settings.selectedColorIndex);
                drawToggle(display, backBuffer, gc, palette, choose, colorToHex(settings.colors[i]));
            }

            const int colorButtonsY = 362 + static_cast<int>(settings.colors.size()) * 36 + 10;
            const Rect addColor{panelX, colorButtonsY, 92, 28};
            const Rect removeColor{panelX + 100, colorButtonsY, 92, 28};
            drawToggle(display, backBuffer, gc, palette, addColor, "Add Color");
            drawToggle(display, backBuffer, gc, palette, removeColor, "Remove");
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
                sliders = makeSliders(settings);
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
                layoutControls(sliders, buttons, windowWidth);
                activeSlider = -1;
                const int panelX = windowWidth - kPanelWidth + kMargin;
                const Rect colorsTab{panelX, 104, 86, 28};
                const Rect ditherTab{panelX + 92, 104, 102, 28};
                if (colorsTab.contains(event.xbutton.x, event.xbutton.y)) {
                    settings.activeTab = SideTab::Colors;
                    needsRedraw = true;
                }
                if (ditherTab.contains(event.xbutton.x, event.xbutton.y)) {
                    settings.activeTab = SideTab::Dither;
                    needsRedraw = true;
                }

                const Rect minus{windowWidth - kPanelWidth + kMargin, 78, 38, 26};
                const Rect plus{windowWidth - kPanelWidth + kMargin + 52, 78, 38, 26};
                if (minus.contains(event.xbutton.x, event.xbutton.y)) {
                    settings.scale = std::max(1, settings.scale - 1);
                    needsRedraw = true;
                }
                if (plus.contains(event.xbutton.x, event.xbutton.y)) {
                    settings.scale = std::min(8, settings.scale + 1);
                    needsRedraw = true;
                }

                const PreviewLayout previewLayout = computePreviewLayout(source, settings.scale, windowWidth, windowHeight);
                const Rect importButton{previewLayout.x, previewLayout.y + previewLayout.height + 8, 92, 28};
                const Rect flipButton{previewLayout.x + 100, previewLayout.y + previewLayout.height + 8, 156, 28};
                if (importButton.contains(event.xbutton.x, event.xbutton.y)) {
                    startImport();
                }
                if (flipButton.contains(event.xbutton.x, event.xbutton.y)) {
                    previewMode = previewMode == PreviewMode::Dithered ? PreviewMode::Original : PreviewMode::Dithered;
                    needsRedraw = true;
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
                const std::array<ColorMode, 5> modes{
                    ColorMode::Monochrome,
                    ColorMode::Palette,
                    ColorMode::HorizontalGradient,
                    ColorMode::VerticalGradient,
                    ColorMode::RadialGradient,
                };
                for (std::size_t i = 0; i < modes.size(); ++i) {
                    const Rect modeRect{panelX, 166 + static_cast<int>(i) * 34, 190, 28};
                    if (modeRect.contains(event.xbutton.x, event.xbutton.y)) {
                        settings.colorMode = modes[i];
                        needsRedraw = true;
                    }
                }

                for (std::size_t i = 0; i < settings.colors.size(); ++i) {
                    const int rowY = 362 + static_cast<int>(i) * 36;
                    const Rect swatch{panelX, rowY, kSwatchSize, kSwatchSize};
                    const Rect choose{panelX + 40, rowY + 2, 110, 24};
                    if (swatch.contains(event.xbutton.x, event.xbutton.y) || choose.contains(event.xbutton.x, event.xbutton.y)) {
                        settings.selectedColorIndex = static_cast<int>(i);
                        if (auto picked = chooseColor(settings.colors[i])) {
                            settings.colors[i] = *picked;
                        }
                        needsRedraw = true;
                    }
                }

                    const int colorButtonsY = 362 + static_cast<int>(settings.colors.size()) * 36 + 10;
                    const Rect addColor{panelX, colorButtonsY, 92, 28};
                    const Rect removeColor{panelX + 100, colorButtonsY, 92, 28};
                    if (addColor.contains(event.xbutton.x, event.xbutton.y)) {
                        settings.colors.push_back(settings.colors.empty() ? Pixel{0, 0, 0} : settings.colors.back());
                        settings.selectedColorIndex = static_cast<int>(settings.colors.size()) - 1;
                        needsRedraw = true;
                    }
                    if (removeColor.contains(event.xbutton.x, event.xbutton.y) && settings.colors.size() > 2) {
                        settings.colors.pop_back();
                        settings.selectedColorIndex = std::min(settings.selectedColorIndex, static_cast<int>(settings.colors.size()) - 1);
                        needsRedraw = true;
                    }
                }
            } else if (event.type == MotionNotify && activeSlider >= 0) {
                const double before = *sliders[static_cast<std::size_t>(activeSlider)].value;
                updateSliderFromMouse(sliders[static_cast<std::size_t>(activeSlider)], event.xmotion.x);
                if (*sliders[static_cast<std::size_t>(activeSlider)].value != before) {
                    dirty = true;
                    needsRedraw = true;
                }
            } else if (event.type == ButtonRelease) {
                activeSlider = -1;
            } else if (event.type == KeyPress) {
                const KeySym key = XLookupKeysym(&event.xkey, 0);
                if (key == XK_Escape) {
                    running = false;
                } else if (key == XK_r || key == XK_R) {
                    settings = Settings{};
                    sliders = makeSliders(settings);
                    dirty = true;
                    needsRedraw = true;
                } else if (key == XK_space) {
                    previewMode = previewMode == PreviewMode::Dithered ? PreviewMode::Original : PreviewMode::Dithered;
                    needsRedraw = true;
                } else if (key == XK_i || key == XK_I) {
                    startImport();
                } else if (key == XK_s || key == XK_S) {
                    dithered = ditherImage(source, settings);
                    if (savePpm("opendither-output.ppm", source, dithered)) {
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
