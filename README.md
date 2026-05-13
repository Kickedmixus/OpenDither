# OpenDither

An early C++ black-and-white dithering playground inspired by Dither Boy.

## Build

```sh
cmake -S . -B build
cmake --build build
./build/opendither
```

You can optionally pass a PPM image (`P3` or `P6`) as the first argument:

```sh
./build/opendither image.ppm
```

## Current Controls

- Sliders: threshold, contrast, brightness, noise, and preview scale.
- Formula buttons: threshold, ordered Bayer, Floyd-Steinberg, Atkinson, and Jarvis-Judice-Ninke.
- `R`: reset settings.
- `S`: save the current dithered preview to `opendither-output.ppm`.
- `Esc`: quit.
