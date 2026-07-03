CC      = g++
RC      = windres
STRIP   = strip
TARGET  = UFunPlayer.exe
SRC     = UFunPlayer.cpp
RESOBJ  = resource.o

CFLAGS  = -m32 -std=c++14 -O2 -Wall -Wextra \
          -DUNICODE -D_UNICODE \
          -DWINVER=0x0600 -D_WIN32_WINNT=0x0600 -D_WIN32_IE=0x0700

# -municode selects the wide-entry CRT startup (wWinMainCRTStartup), which
# is what calls our wWinMain instead of an ANSI WinMain. Without this flag
# the linker will complain about a missing WinMain / duplicate entry point.
LDFLAGS = -mwindows -municode -m32 -static

LIBS    = -lole32 -loleaut32 -luuid \
          -lshell32 -lshlwapi -lcomctl32 -lwininet -lcomdlg32

all: $(TARGET)

$(TARGET): $(SRC) $(RESOBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)
	$(STRIP) $@
	@echo "Build OK: $(TARGET)"

$(RESOBJ): UFunPlayer.rc resource.h
	$(RC) -F pe-i386 UFunPlayer.rc -o $@

clean:
	rm -f $(TARGET) $(RESOBJ)

.PHONY: all clean
