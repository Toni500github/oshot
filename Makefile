CXX       	?= g++
TAR		?= bsdtar
PREFIX	  	?= /usr
VARS  	  	?=
CXXSTD		?= c++20

DEBUG 		?= 0
WINDOWS_CMD	?= 0

COMPILER := $(shell $(CXX) --version | head -n1)

ifeq ($(findstring g++,$(COMPILER)),g++)
        export LTO_FLAGS = -flto=auto -ffat-lto-objects
else ifeq ($(findstring clang,$(COMPILER)),clang)
        export LTO_FLAGS = -flto=thin
else
    $(warning Unknown compiler: $(COMPILER). No LTO flags applied.)
endif

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
        LDLIBS += -lGL -lX11
	CXXFLAGS += `pkg-config --static --cflags gio-2.0 appindicator3-0.1 libpng` -DCLIP_ENABLE_IMAGE=1 -DCLIP_X11_WITH_PNG=1 -DHAVE_PNG_H=1
	LDLIBS   += `pkg-config --static --libs gio-2.0 appindicator3-0.1 libpng`

else ifeq ($(UNAME_S),Darwin) #APPLE
        LDFLAGS += -L/usr/local/lib -L/opt/local/lib -L/opt/homebrew/lib
        LDLIBS += -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
        #LDLIBS += -lglfw3
        #LDLIBS += -lglfw

        CXXFLAGS += -I/usr/local/include -I/opt/local/include -I/opt/homebrew/include

else ifeq ($(findstring _NT,$(UNAME_S)),_NT)
        LDLIBS += -ld3d11 -ldxgi -lgdi32 -lopengl32 -limm32 -lole32 -lcomdlg32 -luuid -lshcore -lshlwapi -lwindowscodecs
	ifneq ($(WINDOWS_CMD),1)
		CXXFLAGS += -mwindows
		LDFLAGS += -mwindows
	else
		CXXFLAGS += -DWINDOWS_CMD
	endif
endif

# https://stackoverflow.com/a/1079861
# WAY easier way to build debug and release builds
ifeq ($(DEBUG), 1)
        BUILDDIR  := build/debug
	LTO_FLAGS := -fno-lto
	SAN_FLAGS ?= -fsanitize=address -fsanitize=undefined
        CXXFLAGS  := -ggdb3 -Wall -Wextra -pedantic -Wno-unused-parameter \
			-DDEBUG=1 -fno-omit-frame-pointer $(DEBUG_CXXFLAGS) $(CXXFLAGS)
        LDFLAGS	  += -Wl,-rpath,$(BUILDDIR)
else
	# Check if an optimization flag is not already set
	ifneq ($(filter -O%,$(CXXFLAGS)),)
    		$(info Keeping the existing optimization flag in CXXFLAGS)
	else
    		CXXFLAGS := -O3 $(CXXFLAGS)
	endif
        BUILDDIR  := build/release
endif

NAME		 = oshot
TARGET		?= $(NAME)
OLDVERSION	 = 0.2.2
VERSION    	 = 0.2.3
SRC	 	 = $(wildcard src/*.cpp)
OBJ	 	 = $(SRC:.cpp=.o)
LDFLAGS   	+= -L$(BUILDDIR) $(LTO_FLAGS)
LDLIBS		+= $(wildcard $(BUILDDIR)/*.a) `pkg-config --static --libs glfw3 tesseract libcurl zbar`
CXXFLAGS        += $(LTO_FLAGS) -fvisibility-inlines-hidden -fvisibility=hidden -Iinclude -Iinclude/libs -Iinclude/libs/trayapp -std=$(CXXSTD) $(VARS) -DVERSION=\"$(VERSION)\"

all: imgui fmt tfd tpl clip trayapp getopt-port toml $(TARGET)

imgui:
ifeq ($(wildcard $(BUILDDIR)/libimgui.a),)
	mkdir -p $(BUILDDIR)
	$(MAKE) -C src/libs/imgui BUILDDIR=$(BUILDDIR) CXXSTD=$(CXXSTD) DEBUG=$(DEBUG)
endif

fmt:
ifeq ($(wildcard $(BUILDDIR)/libfmt.a),)
	mkdir -p $(BUILDDIR)
	$(MAKE) -C src/libs/fmt BUILDDIR=$(BUILDDIR) CXXSTD=$(CXXSTD) DEBUG=$(DEBUG)
endif

toml:
ifeq ($(wildcard $(BUILDDIR)/toml.o),)
	$(MAKE) -C src/libs/toml++ BUILDDIR=$(BUILDDIR) CXXSTD=$(CXXSTD) DEBUG=$(DEBUG)
endif

clip:
ifeq ($(wildcard $(BUILDDIR)/libclip.a),)
	$(MAKE) -C src/libs/clip BUILDDIR=$(BUILDDIR) CXXSTD=$(CXXSTD) DEBUG=$(DEBUG)
endif

trayapp:
ifeq ($(wildcard $(BUILDDIR)/libtrayapp.a),)
	$(MAKE) -C src/libs/trayapp BUILDDIR=$(BUILDDIR) CXXSTD=$(CXXSTD) DEBUG=$(DEBUG)
endif

tfd:
ifeq ($(wildcard $(BUILDDIR)/tinyfiledialogs.o),)
	$(MAKE) -C src/libs/tinyfiledialogs BUILDDIR=$(BUILDDIR)
endif

tpl:
ifeq ($(wildcard $(BUILDDIR)/libtiny-process-library.a),)
	$(MAKE) -C src/libs/tiny-process-library BUILDDIR=$(BUILDDIR) CXXSTD=$(CXXSTD) DEBUG=$(DEBUG)
endif

getopt-port:
ifeq ($(wildcard $(BUILDDIR)/getopt.o),)
	$(MAKE) -C src/libs/getopt_port BUILDDIR=$(BUILDDIR)
endif

genver: ./scripts/generateVersion.sh
	./scripts/generateVersion.sh

$(TARGET): genver fmt toml tfd tpl clip trayapp getopt-port $(OBJ)
	mkdir -p $(BUILDDIR)
	$(CXX) -o $(BUILDDIR)/$(TARGET) $(OBJ) $(BUILDDIR)/*.o $(LDFLAGS) $(LDLIBS)

dist: $(TARGET)
	zip -j $(NAME)-v$(VERSION).zip LICENSE README.md $(BUILDDIR)/$(TARGET)

clean:
	rm -rf $(BUILDDIR)/$(TARGET) $(OBJ)

distclean:
	rm -rf $(BUILDDIR) $(OBJ)
	find . -type f -name "*.tar.gz" -delete
	find . -type f -name "*.o" -delete
	find . -type f -name "*.a" -delete

updatever:
	sed -i "s#$(OLDVERSION)#$(VERSION)#g" $(wildcard .github/workflows/*.yml) CMakeLists.txt compile_flags.txt

.PHONY: $(TARGET) updatever distclean clean imgui fmt tpl toml getopt-port clip trayapp dist all
