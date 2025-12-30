CXX		?= g++
CXXSTD		?= c++20
CXXFLAGS 	 = -I../../../include/libs/tiny-process-library -std=$(CXXSTD) -O3 -DNDEBUG $(LTO_FLAGS) -fvisibility=default -fvisibility-inlines-hidden -fPIC

UNAME_S := $(shell uname -s)

ifeq ($(findstring MINGW64_NT,$(UNAME_S)),MINGW64_NT)
    CXXFLAGS 	+= -D_CRT_SECURE_NO_WARNINGS
    SRC 	+= process_win.cpp
    # Check if we're in MSYS environment
    ifneq (,$(findstring MSYS,$(shell uname -o)))
        CXXFLAGS += -DMSYS_PROCESS_USE_SH
    endif
else
    CXXFLAGS 	+= -Wall -Wextra
    SRC 	+= process_unix.cpp
endif

LIBNAME 	= libtiny-process-library.a
SRC            += process.cpp
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
