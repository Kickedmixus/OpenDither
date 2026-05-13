#include "core.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image.h"
#include "../third_party/stb/stb_image_write.h"

namespace {

constexpr double clampDouble(double value, double minValue, double maxValue) {
    return value < minValue ? minValue : (value > maxValue ? maxValue : value);
}

std::uint8_t toByte(double value) {
    return static_cast<std::uint8_t>(std::lround(clampDouble(value, 0.0, 255.0)));
}

#ifdef _WIN32
std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), output.data(), required);
    return output;
}

#endif

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

Pixel lerpColorImpl(const Pixel& a, const Pixel& b, double t) {
    return Pixel{
        toByte(a.r + (b.r - a.r) * t),
        toByte(a.g + (b.g - a.g) * t),
        toByte(a.b + (b.b - a.b) * t),
    };
}

} // namespace

Settings::Settings()
    : colors{
        Pixel{0, 0, 0},
        Pixel{255, 255, 255},
        Pixel{255, 255, 255},
        Pixel{255, 255, 255},
    }
    , gradientColors{
        Pixel{255, 255, 255},
        Pixel{255, 255, 255},
        Pixel{255, 255, 255},
        Pixel{255, 255, 255},
    }
    , channelModes{
        ChannelMode::Solid,
        ChannelMode::Solid,
        ChannelMode::Solid,
        ChannelMode::Solid,
    }
    , gradientTypes{
        GradientType::Horizontal,
        GradientType::Horizontal,
        GradientType::Horizontal,
        GradientType::Horizontal,
    }
    , gradientSpread{
        1.0,
        1.0,
        1.0,
        1.0,
    } {}

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

Image prepareImage(const Image& input, int pixelSize) {
    const int block = std::max(1, pixelSize);
    if (block == 1 || input.width <= 0 || input.height <= 0 || input.pixels.empty()) {
        return input;
    }

    Image output;
    output.width = std::max(1, (input.width + block - 1) / block);
    output.height = std::max(1, (input.height + block - 1) / block);
    output.pixels.resize(static_cast<std::size_t>(output.width * output.height));

    for (int y = 0; y < output.height; ++y) {
        for (int x = 0; x < output.width; ++x) {
            const int startX = x * block;
            const int startY = y * block;
            const int endX = std::min(input.width, startX + block);
            const int endY = std::min(input.height, startY + block);
            int count = 0;
            int sumR = 0;
            int sumG = 0;
            int sumB = 0;

            for (int sy = startY; sy < endY; ++sy) {
                for (int sx = startX; sx < endX; ++sx) {
                    const Pixel& src = input.pixels[static_cast<std::size_t>(sy * input.width + sx)];
                    sumR += src.r;
                    sumG += src.g;
                    sumB += src.b;
                    ++count;
                }
            }

            if (count == 0) {
                output.pixels[static_cast<std::size_t>(y * output.width + x)] = Pixel{};
            } else {
                output.pixels[static_cast<std::size_t>(y * output.width + x)] = Pixel{
                    toByte(static_cast<double>(sumR) / count),
                    toByte(static_cast<double>(sumG) / count),
                    toByte(static_cast<double>(sumB) / count),
                };
            }
        }
    }

    return output;
}

Image buildExportImage(const Image& input, const std::vector<std::uint8_t>& dithered, const Settings& settings, PreviewMode mode, int exportScale) {
    const int scale = std::max(1, exportScale);
    if (input.width <= 0 || input.height <= 0 || input.pixels.empty()) {
        return {};
    }

    Image output;
    output.width = input.width * scale;
    output.height = input.height * scale;
    output.pixels.resize(static_cast<std::size_t>(output.width * output.height));

    for (int y = 0; y < output.height; ++y) {
        const int sourceY = std::min(input.height - 1, y / scale);
        for (int x = 0; x < output.width; ++x) {
            const int sourceX = std::min(input.width - 1, x / scale);
            const std::size_t index = static_cast<std::size_t>(sourceY * input.width + sourceX);
            Pixel outputPixel = input.pixels[index];
            if (mode == PreviewMode::Dithered) {
                const int level = std::min<int>(dithered[index], settings.channelCount - 1);
                outputPixel = channelColor(input, settings, level, sourceX, sourceY);
            }
            output.pixels[static_cast<std::size_t>(y * output.width + x)] = outputPixel;
        }
    }

    return output;
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

std::optional<Image> loadImageAny(const std::string& path) {
    if (auto ppm = loadPpm(path)) {
        return ppm;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* data = nullptr;

#ifdef _WIN32
    const std::wstring widePath = utf8ToWide(path);
    if (widePath.empty()) {
        return std::nullopt;
    }

    FILE* file = _wfopen(widePath.c_str(), L"rb");
    if (!file) {
        return std::nullopt;
    }
    data = stbi_load_from_file(file, &width, &height, &channels, 3);
    std::fclose(file);
#else
    data = stbi_load(path.c_str(), &width, &height, &channels, 3);
#endif

    if (!data || width <= 0 || height <= 0) {
        stbi_image_free(data);
        return std::nullopt;
    }

    Image image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(width * height));
    for (std::size_t i = 0; i < image.pixels.size(); ++i) {
        image.pixels[i] = Pixel{
            data[i * 3 + 0],
            data[i * 3 + 1],
            data[i * 3 + 2],
        };
    }
    stbi_image_free(data);
    return image;
}

std::vector<std::uint8_t> ditherImage(const Image& input, const Settings& settings) {
    const int width = input.width;
    const int height = input.height;
    const int levels = std::clamp(settings.channelCount, 2, 4);
    const double step = 255.0 / static_cast<double>(levels - 1);
    std::vector<double> gray(static_cast<std::size_t>(width * height), 0.0);
    std::vector<std::uint8_t> output(static_cast<std::size_t>(width * height), 0);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            gray[static_cast<std::size_t>(y * width + x)] = luminance(input.pixels[static_cast<std::size_t>(y * width + x)], settings, x, y);
        }
    }

    const auto quantize = [&](double value) -> std::uint8_t {
        const int q = std::clamp(static_cast<int>(std::lround(value / step)), 0, levels - 1);
        return static_cast<std::uint8_t>(q);
    };

    const auto writeThreshold = [&](int x, int y, double thresholdBias) {
        const std::size_t index = static_cast<std::size_t>(y * width + x);
        output[index] = quantize(clampDouble(gray[index] + thresholdBias, 0.0, 255.0));
    };

    if (settings.formula == Formula::Threshold) {
        const double thresholdBias = settings.threshold - 128.0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                writeThreshold(x, y, thresholdBias);
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
                const double offset = (static_cast<double>(bayer[static_cast<std::size_t>(y % 4)][static_cast<std::size_t>(x % 4)]) - 7.5) * (step / 8.0);
                writeThreshold(x, y, offset);
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
            const double newValue = std::lround(oldValue / step) * step;
            const double error = oldValue - newValue;
            output[index] = quantize(newValue);

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

bool savePpm(const std::string& path, const Image& image, const std::vector<std::uint8_t>& dithered, int levels) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    file << "P6\n" << image.width << " " << image.height << "\n255\n";
    const double step = 255.0 / static_cast<double>(levels - 1);
    for (std::uint8_t value : dithered) {
        const std::uint8_t shade = toByte(static_cast<double>(value) * step);
        const char byte = static_cast<char>(shade);
        file.write(&byte, 1);
        file.write(&byte, 1);
        file.write(&byte, 1);
    }
    return true;
}

bool exportImage(const std::string& path, const Image& input, const std::vector<std::uint8_t>& dithered, const Settings& settings, PreviewMode mode, int exportScale) {
    const Image exportImageData = buildExportImage(input, dithered, settings, mode, exportScale);
    if (exportImageData.width <= 0 || exportImageData.height <= 0 || exportImageData.pixels.empty()) {
        return false;
    }

    const std::filesystem::path outputPath(path);
    if (outputPath.extension() == ".ppm" || outputPath.extension() == ".pnm") {
        std::ofstream file(path, std::ios::binary);
        if (!file) {
            return false;
        }

        file << "P6\n" << exportImageData.width << " " << exportImageData.height << "\n255\n";
        for (const Pixel& pixel : exportImageData.pixels) {
            const char r = static_cast<char>(pixel.r);
            const char g = static_cast<char>(pixel.g);
            const char b = static_cast<char>(pixel.b);
            file.write(&r, 1);
            file.write(&g, 1);
            file.write(&b, 1);
        }
        return true;
    }

    auto writeFunc = [](void* context, void* data, int size) {
        auto* file = static_cast<std::FILE*>(context);
        std::fwrite(data, 1, static_cast<std::size_t>(size), file);
    };

    const int stride = exportImageData.width * 3;
    std::vector<std::uint8_t> packed(static_cast<std::size_t>(stride * exportImageData.height), 0);
    for (int y = 0; y < exportImageData.height; ++y) {
        for (int x = 0; x < exportImageData.width; ++x) {
            const Pixel& pixel = exportImageData.pixels[static_cast<std::size_t>(y * exportImageData.width + x)];
            const std::size_t index = static_cast<std::size_t>(y * stride + x * 3);
            packed[index + 0] = pixel.r;
            packed[index + 1] = pixel.g;
            packed[index + 2] = pixel.b;
        }
    }

#ifdef _WIN32
    const std::wstring widePath = utf8ToWide(path);
    if (widePath.empty()) {
        return false;
    }
    FILE* file = _wfopen(widePath.c_str(), L"wb");
#else
    FILE* file = std::fopen(path.c_str(), "wb");
#endif

    if (!file) {
        return false;
    }

    const std::string ext = outputPath.extension().string();
    bool ok = false;
    if (ext == ".png") {
        ok = stbi_write_png_to_func(writeFunc, file, exportImageData.width, exportImageData.height, 3, packed.data(), stride) != 0;
    } else if (ext == ".jpg" || ext == ".jpeg") {
        ok = stbi_write_jpg_to_func(writeFunc, file, exportImageData.width, exportImageData.height, 3, packed.data(), 90) != 0;
    } else if (ext == ".bmp") {
        ok = stbi_write_bmp_to_func(writeFunc, file, exportImageData.width, exportImageData.height, 3, packed.data()) != 0;
    } else if (ext == ".tga") {
        ok = stbi_write_tga_to_func(writeFunc, file, exportImageData.width, exportImageData.height, 3, packed.data()) != 0;
    } else {
        ok = stbi_write_png_to_func(writeFunc, file, exportImageData.width, exportImageData.height, 3, packed.data(), stride) != 0;
    }

    std::fclose(file);
    return ok;
}

void ensureChannelCount(Settings& settings) {
    const int target = std::clamp(settings.channelCount, 2, 4);
    settings.channelCount = target;
    settings.selectedChannel = std::clamp(settings.selectedChannel, 0, target - 1);

    if (static_cast<int>(settings.colors.size()) < target) {
        settings.colors.resize(static_cast<std::size_t>(target), Pixel{255, 255, 255});
        settings.gradientColors.resize(static_cast<std::size_t>(target), Pixel{255, 255, 255});
        settings.channelModes.resize(static_cast<std::size_t>(target), ChannelMode::Solid);
        settings.gradientTypes.resize(static_cast<std::size_t>(target), GradientType::Horizontal);
        settings.gradientSpread.resize(static_cast<std::size_t>(target), 1.0);
    } else {
        settings.colors.resize(static_cast<std::size_t>(target));
        settings.gradientColors.resize(static_cast<std::size_t>(target));
        settings.channelModes.resize(static_cast<std::size_t>(target));
        settings.gradientTypes.resize(static_cast<std::size_t>(target));
        settings.gradientSpread.resize(static_cast<std::size_t>(target));
    }
}

double gradientMixForType(GradientType type, const Image& source, int sourceX, int sourceY, double spread) {
    double t = 0.0;
    switch (type) {
        case GradientType::Horizontal:
            t = source.width <= 1 ? 0.0 : static_cast<double>(sourceX) / (source.width - 1);
            break;
        case GradientType::Vertical:
            t = source.height <= 1 ? 0.0 : static_cast<double>(sourceY) / (source.height - 1);
            break;
        case GradientType::Radial: {
            const double nx = source.width <= 1 ? 0.0 : (static_cast<double>(sourceX) / (source.width - 1)) - 0.5;
            const double ny = source.height <= 1 ? 0.0 : (static_cast<double>(sourceY) / (source.height - 1)) - 0.5;
            t = clampDouble(std::hypot(nx, ny) * 2.0, 0.0, 1.0);
            break;
        }
    }

    const double clampedSpread = std::max(0.05, spread);
    const double start = 0.5 - clampedSpread * 0.5;
    const double end = 0.5 + clampedSpread * 0.5;
    return clampDouble((t - start) / std::max(0.0001, end - start), 0.0, 1.0);
}

Pixel lerpColor(const Pixel& a, const Pixel& b, double t) {
    return lerpColorImpl(a, b, clampDouble(t, 0.0, 1.0));
}

Pixel gradientColor(const Pixel& a, const Pixel& b, double t) {
    return lerpColor(a, b, t);
}

Pixel channelColor(const Image& source, const Settings& settings, int channelIndex, int sourceX, int sourceY) {
    const std::size_t idx = static_cast<std::size_t>(channelIndex);
    const Pixel color = settings.colors[idx];

    if (settings.channelModes[idx] == ChannelMode::Solid) {
        return color;
    }

    const Pixel colorB = settings.gradientColors[idx];
    const double t = gradientMixForType(settings.gradientTypes[idx], source, sourceX, sourceY, settings.gradientSpread[idx]);
    return gradientColor(color, colorB, t);
}
