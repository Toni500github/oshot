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
        LDLIBS += -lGL
	CXXFLAGS += `pkg-config --static --cflags gio-2.0 appindicator3-0.1 x11` -DCLIP_ENABLE_IMAGE=1
	LDLIBS   += `pkg-config --static --libs gio-2.0 appindicator3-0.1 x11 xrandr`

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
    		CXXFLAGS := -O3 -march=native $(CXXFLAGS)
	endif
        BUILDDIR  := build/release
endif

NAME		 = oshot
TARGET		?= $(NAME)
OLDVERSION	 = 0.4.2
VERSION    	 = 0.4.3
SRC		 = $(wildcard src/*.cpp)
OBJ 		 = $(patsubst src/%.cpp,$(BUILDDIR)/%.o,$(SRC))
LDFLAGS   	+= -L$(BUILDDIR) $(LTO_FLAGS)
LDLIBS		+= $(LIBS) `pkg-config --static --libs glfw3 tesseract zbar`
CXXFLAGS        += $(LTO_FLAGS) -fvisibility-inlines-hidden -fvisibility=hidden -Iinclude -Iinclude/libs -std=$(CXXSTD) $(VARS) -DVERSION=\"$(VERSION)\"

LIBS := \
  $(BUILDDIR)/libimgui.a \
  $(BUILDDIR)/libfmt.a \
  $(BUILDDIR)/libclip.a \
  $(BUILDDIR)/libtray.a \
  $(BUILDDIR)/libtiny-process-library.a

EXTRA_OBJ := \
  $(BUILDDIR)/toml.o \
  $(BUILDDIR)/tinyfiledialogs.o \
  $(BUILDDIR)/getopt.o

all: $(BUILDDIR)/$(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/libimgui.a: | $(BUILDDIR)
	$(MAKE) -C src/libs/imgui BUILDDIR=$(BUILDDIR) CXXSTD=$(CXXSTD) DEBUG=$(DEBUG)

$(BUILDDIR)/libfmt.a: | $(BUILDDIR)
	$(MAKE) -C src/libs/fmt BUILDDIR=$(BUILDDIR) CXXSTD=$(CXXSTD) DEBUG=$(DEBUG)

$(BUILDDIR)/toml.o:
	$(MAKE) -C src/libs/toml++ BUILDDIR=$(BUILDDIR) CXXSTD=$(CXXSTD) DEBUG=$(DEBUG)

$(BUILDDIR)/libclip.a:
	$(MAKE) -C src/libs/clip BUILDDIR=$(BUILDDIR) CXXSTD=$(CXXSTD) DEBUG=$(DEBUG)

$(BUILDDIR)/libtray.a:
	$(MAKE) -C src/libs/tray BUILDDIR=$(BUILDDIR) CXXSTD=$(CXXSTD) DEBUG=$(DEBUG)

$(BUILDDIR)/tinyfiledialogs.o:
	$(MAKE) -C src/libs/tinyfiledialogs BUILDDIR=$(BUILDDIR)

$(BUILDDIR)/libtiny-process-library.a:
	$(MAKE) -C src/libs/tiny-process-library BUILDDIR=$(BUILDDIR) CXXSTD=$(CXXSTD) DEBUG=$(DEBUG)

$(BUILDDIR)/getopt.o:
	$(MAKE) -C src/libs/getopt_port BUILDDIR=$(BUILDDIR)

genver:
	./scripts/generateVersion.sh

$(BUILDDIR)/$(TARGET): genver $(OBJ) $(EXTRA_OBJ) $(LIBS)
	$(CXX) -o $@ $(OBJ) $(EXTRA_OBJ) $(LDFLAGS) $(LDLIBS)

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
	sed -i "s#$(OLDVERSION)#$(VERSION)#g" $(wildcard .github/workflows/*.yml scripts/*) CMakeLists.txt compile_flags.txt

.PHONY: $(TARGET) updatever distclean clean imgui fmt tpl toml getopt-port clip tray dist all
