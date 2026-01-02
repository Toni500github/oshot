# oshot

A simple tool for extracting and translating text from a screenshot (on the fly)

## Dependencies
Names can vary from package manager (or distro) thus try to search with base name (e.g libglfw3-dev -> glfw3)
* libx11-dev (linux-only)
* grim (linux-only, if using wayland)
* libglfw3-dev
* tesseract (install the eng model/version too)

## Building

### with `make`
```bash
$ git clone https://github.com/Toni500github/oshot/
$ cd oshot/
$ make
# You can move it in a custom directory in your $PATH (preferably in local/home)
$ ./build/release/oshot
```

### with `cmake`
```bash
$ git clone https://github.com/Toni500github/oshot/
$ cd oshot/
$ mkdir build && cd $_
$ cmake .. -DCMAKE_BUILD_TYPE=Release
$ make
# You can move it a custom directory in your $PATH (preferably in local/home)
$ ./oshot
```

## Usage
https://github.com/user-attachments/assets/ac505de6-0818-4d67-bb51-064d86f1f970
