#include "core.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

constexpr double clampDouble(double value, double minValue, double maxValue) {
    return value < minValue ? minValue : (value > maxValue ? maxValue : value);
}

std::uint8_t toByte(double value) {
    return static_cast<std::uint8_t>(std::lround(clampDouble(value, 0.0, 255.0)));
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
