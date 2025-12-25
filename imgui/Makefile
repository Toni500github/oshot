CXX		?= g++
CXXSTD		?= c++20
CXXFLAGS 	 = -O3 -std=$(CXXSTD) -DNDEBUG $(LTO_FLAGS) -fvisibility=hidden -fvisibility-inlines-hidden -fPIC

SRC		:= $(wildcard *.cpp)
OBJ		:= $(SRC:.cpp=.o)
LIBNAME		:= libimgui.a

all: $(LIBNAME)

$(LIBNAME): $(OBJ)
	ar rcs $@ $^
	ranlib $(LIBNAME)
	mv -f $(LIBNAME) ../$(BUILDDIR)/$(LIBNAME)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(LIBNAME)

.PHONY: all clean
