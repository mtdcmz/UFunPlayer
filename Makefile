CC      = g++
RC      = windres
STRIP   = strip
TARGET  = UFunPlayer.exe
SRC     = UFunPlayer.cpp
RESOBJ  = resource.o

# MinHook static library (inline hooks for webplayer_win.dll runtime).
MINHOOK_DIR = MinHook
MINHOOK_LIB = $(MINHOOK_DIR)/libMinHook.a

CFLAGS  = -m32 -std=c++14 -O2 -Wall -Wextra \
          -DUNICODE -D_UNICODE \
          -DWINVER=0x0600 -D_WIN32_WINNT=0x0600 -D_WIN32_IE=0x0700 \
          -I$(MINHOOK_DIR)/include

# -municode selects the wide-entry CRT startup (wWinMainCRTStartup), which
# is what calls our wWinMain instead of an ANSI WinMain. Without this flag
# the linker will complain about a missing WinMain / duplicate entry point.
# --large-address-aware sets the IMAGE_FILE_LARGE_ADDRESS_AWARE bit in the
# PE header so the 32-bit process gets a 4 GB/3 GB user VA (instead of 2 GB)
# on 64-bit / 32-bit Windows respectively. Needed by some games whose map
# deserialization would otherwise exceed the 2 GB limit and crash.
LDFLAGS = -mwindows -municode -m32 -static -Wl,--large-address-aware

LIBS    = -lole32 -loleaut32 -luuid \
          -lshell32 -lshlwapi -lcomctl32 -lwininet -lurlmon -lcomdlg32 -lws2_32 \
          -lversion

all: $(TARGET)

$(TARGET): $(SRC) $(RESOBJ) $(MINHOOK_LIB)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(RESOBJ) $(LDFLAGS) $(LIBS) $(MINHOOK_LIB)
	$(STRIP) $@
	@echo "Build OK: $(TARGET)"

$(RESOBJ): UFunPlayer.rc resource.h
	$(RC) -F pe-i386 UFunPlayer.rc -o $@

# Build MinHook static lib from source using its bundled MinGW makefile.
# Forces -m32 to match the main target.
$(MINHOOK_LIB):
	$(MAKE) -C $(MINHOOK_DIR) -f build/MinGW/Makefile libMinHook.a \
	  CC=gcc CFLAGS="-masm=intel -Wall -std=c11 -m32"

clean:
	rm -f $(TARGET) $(RESOBJ)

.PHONY: all clean
