LIBKIND    := static
LIBNAME    := libnvdialog
VISIBILITY := default

CFLAGS += -I../../../include/libs/nvdialog -Iimpl -Ibackend -O3 \
          -Wall -Wextra -Wconversion -Winline -Wno-sign-conversion \
          -Werror=format -Werror=format-security -Werror=write-strings \
          -DNVDIALOG_MAXBUF=4096 -DNVD_EXPORT_SYMBOLS -DNVD_STATIC_LINKAGE=1

PKGCONF ?= pkg-config

# Sources common to every platform.
SRC := $(wildcard *.c) $(wildcard src/*.c)

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    SRC    += $(wildcard backend/cocoa/*.m)
    CFLAGS += -DNVD_USE_COCOA=1 -fno-objc-arc
    # Whatever links libnvdialog.a must additionally add:
    #   -framework AppKit -framework Cocoa -framework Foundation -framework UserNotifications
else ifeq ($(findstring NT,$(UNAME_S)),NT)
    SRC += $(wildcard backend/win32/*.c)
    # Whatever links libnvdialog.a must additionally add:
    #   -lcomdlg32 -lshell32 -luser32 -lgdi32 -lole32
else
    SRC    += $(wildcard backend/gtk/*.c)
    CFLAGS += `$(PKGCONF) --cflags gtk+-3.0 dbus-1`
    # Whatever links libnvdialog.a must additionally add:
    #   `pkg-config --libs gtk+-3.0 dbus-1`
endif

include ../../../mk/common.mk
