PKG_FLAGS	:= `pkg-config --cflags appindicator3-0.1`

CXX		?= g++
CXXSTD		?= c++20
CXXFLAGS 	 = $(PKG_FLAGS) -std=$(CXXSTD) $(LTO_FLAGS) -I../../../include/libs/trayapp -fvisibility=hidden -fvisibility-inlines-hidden -fPIC

SRC		:= $(wildcard ./*/*.cpp ./*/*/*.cpp)
OBJ		:= $(SRC:.cpp=.o)
LIBNAME		:= libtrayapp.a

ifeq ($(DEBUG),1)
	CXXFLAGS  := -ggdb3 -Wall -Wextra -pedantic -Wno-unused-parameter \
                        -DDEBUG=1 -fno-omit-frame-pointer $(DEBUG_CXXFLAGS) $(CXXFLAGS)
else
	CXXFLAGS += -O3 -DNDEBUG
endif

all: $(LIBNAME)

$(LIBNAME): $(OBJ)
	ar rcs $@ $^
	ranlib $(LIBNAME)
	mv -f $(LIBNAME) ../../../$(BUILDDIR)/$(LIBNAME)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(LIBNAME)

.PHONY: all clean
