![logo](https://i.imgur.com/BcMysWw.png)

Dithering playground inspired by Dither Boy. Its open source and free!

## Build

```sh
cmake -S . -B build
cmake --build build
./build/opendither
```
## Future/Goals

1  - UI port to windows
2  - add post processing and glow effects (and much more)
3  - video support
4  - keyframe support for dithering settings of videos and images


## Current Controls

- Sliders: threshold, contrast, brightness, noise, and pixel scale.
- Color channels and setting allow for diffrent gradient types and channels to be set as the dithered pixels
- Formula buttons: threshold, ordered Bayer, Floyd-Steinberg, Atkinson, and Jarvis-Judice-Ninke.
- Import and save files in many types, you can multiply each pixel for higher file resolution too!
