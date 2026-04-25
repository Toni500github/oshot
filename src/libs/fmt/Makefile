# Variables taken from {fmt} cmake build files
# I don't wanna use cmake
# cmd: cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF

CXX		?= g++
CXXSTD		?= c++20
# CXX_DEFINES 	= -DFMT_LIB_EXPORT -Dfmt_EXPORTS
CXXFLAGS 	= -I../../../include/libs -std=$(CXXSTD) $(LTO_FLAGS) -fvisibility=hidden -fvisibility-inlines-hidden -fPIC

OBJ = format.cc.o os.cc.o

ifeq ($(DEBUG),1)
	CXXFLAGS  := -ggdb3 -Wall -Wextra -pedantic -Wno-unused-parameter \
                        -DDEBUG=1 -fno-omit-frame-pointer $(DEBUG_CXXFLAGS) $(CXXFLAGS)
else
	CXXFLAGS += -O3 -DNDEBUG
endif

all: fmt

fmt: $(OBJ)
	ar qc libfmt.a $(OBJ)
	ranlib libfmt.a
	mkdir -p ../../../$(BUILDDIR)
	mv -f libfmt.a ../../../$(BUILDDIR)/libfmt.a

%.cc.o: %.cc
	$(CXX) $(CXX_DEFINES) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f *.o *.so *.a ../../../$(BUILDDIR)/fmt/.*a

.PHONY: clean all fmt
