CC      = g++
RC      = windres
STRIP   = strip
TARGET  = UFunPlayer.exe
SRC     = UFunPlayer.cpp
RESOBJ  = resource.o

CFLAGS  = -m32 -std=c++14 -O2 -Wall -Wextra \
          -DWINVER=0x0600 -D_WIN32_WINNT=0x0600 -D_WIN32_IE=0x0700

LDFLAGS = -mwindows -m32 -static

# urlmon removed — GetMoniker reverted to E_NOTIMPL, no longer needed
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
