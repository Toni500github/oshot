CXX		?= g++
CXXSTD		?= c++20
CXXFLAGS 	 = -I../../../include/libs/clip -std=$(CXXSTD) $(LTO_FLAGS) -fvisibility=default -fvisibility-inlines-hidden -fPIC -DCLIP_ENABLE_IMAGE=1 -DCLIP_X11_WITH_PNG=1 -DHAVE_PNG_H=1

ifeq ($(DEBUG),1)
	CXXFLAGS  := -ggdb3 -Wall -Wextra -pedantic -Wno-unused-parameter \
                        -DDEBUG=1 -fno-omit-frame-pointer $(DEBUG_CXXFLAGS) $(CXXFLAGS)
else
	CXXFLAGS += -O3 -DNDEBUG
endif

UNAME_S := $(shell uname -s)

ifeq ($(findstring NT,$(UNAME_S)),NT)
    CXXFLAGS 	+= -D_CRT_SECURE_NO_WARNINGS
    SRC 	+= $(wildcard clip_win*.cpp)
    # Check if we're in MSYS environment
    ifneq (,$(findstring MSYS,$(shell uname -o)))
        CXXFLAGS += -DMSYS_PROCESS_USE_SH
    endif
else
    CXXFLAGS 	+= -Wall -Wextra
    SRC 	+= clip_x11.cpp
endif

LIBNAME 	= libclip.a
SRC            += clip.cpp image.cpp
OBJ		= $(SRC:.cpp=.o)

all: $(LIBNAME)

$(LIBNAME): $(OBJ)
	ar rcs $@ $^
	mv -f $(LIBNAME) ../../../$(BUILDDIR)/$(LIBNAME)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(LIBNAME)

.PHONY: all clean
