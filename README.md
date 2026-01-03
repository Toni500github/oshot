# oshot

A simple tool for extracting and translating text from a screenshot (on the fly)

## Dependencies
### Linux
Names can vary from package manager (or distro) thus try to search with base name (e.g libglfw3-dev -> glfw3)
* libx11-dev
* grim (if using wayland)
* libglfw3-dev
* tesseract (with its model too)
### Windows
1. Install Tesseract OCR from [UB-Mannheim/tesseract](https://github.com/UB-Mannheim/tesseract/wiki)
2. Download language data from [tesseract-ocr/tessdata](https://github.com/tesseract-ocr/tessdata)
3. Extract language data to your preferred directory
4. Configure the path in `config.toml` (see Windows Configuration section)

## Building
### with `make`
```bash
$ git clone https://github.com/Toni500github/oshot/
$ cd oshot/
$ make
# You can move it in a custom directory in your $PATH (preferably in local/home)
$ ./build/release/oshot
```

### Windows Configuration
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
