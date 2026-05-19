CC      ?= gcc
PKGCONF ?= pkg-config

CFLAGS  := -std=c11 -Wall -Wextra -Wconversion -Winline -Werror=format -Werror=format-security -Werror=write-strings -Wno-sign-conversion
CFLAGS  += -I../../../include/libs/nvdialog $(LTO_FLAGS) -Iimpl -Ibackend -O3 -fvisibility=default -DNVDIALOG_MAXBUF=4096 -DNVD_EXPORT_SYMBOLS -DNVD_STATIC_LINKAGE=1
CFLAGS  += $(shell $(PKGCONF) --cflags gtk+-3.0 dbus-1)
LDFLAGS := $(shell $(PKGCONF) --libs gtk+-3.0 dbus-1)

LIBNAME    := libnvdialog.a
COMMON_SRC := $(wildcard src/*.c)
SRC        := $(wildcard *.c backend/gtk/*.c)
OBJ        := $(SRC:%.c=%.o)
OBJ        := $(OBJ:%.c=%.o)

LIBNAME := libnvdialog.a

all: $(LIBNAME)

$(LIBNAME): $(OBJ)
	ar rcs $@ $^
	ranlib $@
	mv -f $@ ../../../$(BUILDDIR)/$@

# universal compile rule
$(OBJDIR)/%.o: src/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(LIBNAME)

.PHONY: all clean
