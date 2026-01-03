CC	?= gcc
CSTD	?= c11
SRC 	 = tinyfiledialogs.c
TARGET   = tinyfiledialogs.o
CFLAGS 	 = -I../../../include/libs $(LTO_FLAGS) -O3 -fvisibility=default -fPIC -std=$(CSTD)

all: $(TARGET)

$(TARGET):
	$(CC) $(SRC) $(CFLAGS) -c -o ../../../$(BUILDDIR)/$@

clean:
	rm -rf ../../../$(BUILDDIR)/$(TARGET)

.PHONY: $(TARGET) clean all
