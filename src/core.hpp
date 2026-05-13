#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

enum class ChannelMode {
    Solid,
    Gradient,
};

enum class GradientType {
    Horizontal,
    Vertical,
    Radial,
};

enum class SideTab {
    Colors,
    Dither,
};

enum class Theme {
    Dark,
    Light,
};

struct Settings {
    double threshold = 128.0;
    double contrast = 1.0;
    double brightness = 0.0;
    double noise = 0.0;
    int scale = 1;
    int pixelSize = 1;
    Formula formula = Formula::FloydSteinberg;
    SideTab activeTab = SideTab::Dither;
    Theme theme = Theme::Dark;
    int channelCount = 2;
    int selectedChannel = 0;
    std::vector<Pixel> colors;
    std::vector<Pixel> gradientColors;
    std::vector<ChannelMode> channelModes;
    std::vector<GradientType> gradientTypes;
    std::vector<double> gradientSpread;

    Settings();
};

Image makeDemoImage();
Image prepareImage(const Image& input, int pixelSize);
std::optional<Image> loadPpm(const std::string& path);
std::optional<Image> loadImageAny(const std::string& path);
std::vector<std::uint8_t> ditherImage(const Image& input, const Settings& settings);
bool savePpm(const std::string& path, const Image& image, const std::vector<std::uint8_t>& dithered, int levels);
void ensureChannelCount(Settings& settings);
double gradientMixForType(GradientType type, const Image& source, int sourceX, int sourceY, double spread);
Pixel lerpColor(const Pixel& a, const Pixel& b, double t);
Pixel gradientColor(const Pixel& a, const Pixel& b, double t);
Pixel channelColor(const Image& source, const Settings& settings, int channelIndex, int sourceX, int sourceY);
