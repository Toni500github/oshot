# oshot

A simple tool for extracting and translating text from a screenshot (on the fly)

## Dependencies

### Linux
Package names may vary by distribution and package manager.  
If a package is not found, try searching by its base name (e.g. `libglfw3-dev` → `glfw`).

- `libx11-dev`
- `grim` (Wayland only)
- `libglfw3-dev`
- `tesseract` (including some language models)
- `python` and `pip`
  - Python packages: `pip install pillow pystray pyclip`

### Windows
1. Install **Tesseract OCR** from  
   https://github.com/UB-Mannheim/tesseract/wiki
2. Download the required language data from  
   https://github.com/tesseract-ocr/tessdata
3. Extract the language files to a directory of your choice
4. Configure the Tesseract and language data paths in `config.toml`  
   (see *Windows Configuration* section)
5. Install **Python** from the Microsoft Store or https://www.python.org/downloads/release/python-3142/ \
   NOTE: CHECK THE "Add python.exe to $PATH" OPTION
6. After installing Python, open the CMD and type: \
   `curl https://bootstrap.pypa.io/get-pip.py -o get-pip.py` \
   `python get-pip.py`
7. Installed pip, run `pip install pillow pystray pyclip`

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
$ cmake .. -G Ninja -DDCMAKE_BUILD_TYPE=Release
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

## Usage
https://github.com/user-attachments/assets/ac505de6-0818-4d67-bb51-064d86f1f970
