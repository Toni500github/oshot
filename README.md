# oshot

A simple and lightweight tool for extracting and translating text from a screenshot/image (on the fly)

## Dependencies
#### Linux
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

#### Windows
1. Download the required language data for Tesseract from [tessdata](https://github.com/tesseract-ocr/tessdata).
2. Extract the language files to a directory of your choice.
3. Configure the Tesseract and language data paths in the `config.toml` file (see *Windows Configuration* section for more details).

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

## Windows Configuration
After installing Tesseract:
1. Open the `%APPDATA%` directory (press Win+R and type `%APPDATA%`)
2. Navigate to the `oshot` directory
3. Edit `config.toml` and set the `ocr-path` variable to your Tesseract data directory
   - Use double backslashes in the path: `C:\\Users\\Name\\tessdata`
Example:
```toml
ocr-path = "C:\\Program Files\\Tesseract-OCR\\tessdata"
```

## Troubleshooting
### Windows
If when starting oshot, it starts to flick a screen black (or it won't launch), try the following steps:
1. Download [MesaForWindows-x64-20.1.8.7z](https://downloads.fdossena.com/geth.php?r=mesa64-latest)
2. Extract the `opengl32.dll` file into the directory where `oshot.exe` is located
3. Try to launch it again

### Linux
- If oshot gives linking library errors, when trying to run it, then try to use the AppImage release instead.
- If `oshot` doesn't take the screenshot, then you need to take the screenshot from an external tool and either pipe into stdin or use the `-f /path/to/image` flag.

If still errors, please open an [Issue](https://github.com/Toni500github/oshot/issues) and take a screenshot/paste the text of the error appearing in the console when executing oshot
## Usage
https://github.com/user-attachments/assets/ac505de6-0818-4d67-bb51-064d86f1f970
