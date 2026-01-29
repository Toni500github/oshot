# oshot

A simple and lightweight tool for extracting and translating text from a screenshot/image (on the fly)

## Dependencies
### Linux
Package names may vary by distribution and package manager.
If a package is not found, try searching by its base name (e.g., `libglfw3-dev` → `glfw`).

- `libx11-dev`
- `libxcb-dev`
- `libpng-dev`
- `libglfw3-dev`
- `libtesseract` (including necessary language models, e.g `tesseract-ocr-eng`)
- `libzbar-dev`
- `libappindicator3-dev`
- `grim` (Wayland only)
- `wl-clipboard` (Wayland only)

## Building
### Make
```bash
$ git clone https://github.com/Toni500github/oshot/
$ cd oshot/
$ make
# You can move it in a custom directory in your $PATH (preferably in the home)
$ ./build/release/oshot
```
### CMake (ninja)
```bash
$ git clone https://github.com/Toni500github/oshot/
$ cd oshot/
$ mkdir build2 && cd build2
$ cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
$ ninja
# You can move it in a custom directory in your $PATH (preferably in home)
$ ./oshot
```

## Downloading additional language models
Tesseract uses separate language model files (`.traineddata`) for each language.  
You can store these files anywhere you like, as long as the path is configured correctly.

1. Download the required language model(s) from the official Tesseract repository:  
   https://github.com/tesseract-ocr/tessdata

2. Place the downloaded `.traineddata` files in one of the following locations:
   - The `models/` directory next to the `oshot` binary (recommended)
   - Or any other directory of your choice (configure the path in the config file)

3. Configure the language data path in `config.toml`:
   - Windows: `%APPDATA%/oshot/config.toml`
   - Linux: `~/.config/oshot/config.toml`

   Set the `ocr-path` variable to the directory containing the `.traineddata` files.
   Example:
   ```toml
   # Works on windows too
   ocr-path = "~/Downloads/oshot/models"
   ```

## Troubleshooting
### Windows
If when starting oshot, it starts to flick a screen black (or it won't launch), try the following steps:
1. Download [MesaForWindows-x64-20.1.8.7z](https://downloads.fdossena.com/geth.php?r=mesa64-latest)
2. Extract the `opengl32.dll` file into the directory where `oshot.exe` is located
3. Try to launch it again

### Linux
- If oshot gives linking library errors, when trying to run it, then try to use the AppImage release instead.
- If you try to copy the text into the clipboard and doesn't work, try to launch `oshot --tray` and then from the system tray you launch oshot

If still errors, please open an [Issue](https://github.com/Toni500github/oshot/issues) and take a screenshot/paste the text of the error appearing in the console when executing oshot
## Usage
https://github.com/user-attachments/assets/ac505de6-0818-4d67-bb51-064d86f1f970
