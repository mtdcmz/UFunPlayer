CC      = g++
RC      = windres
STRIP   = strip

TARGET  = UFunPlayer.exe
SRC     = UFunPlayer.cpp
RES_SRC = UFunPlayer.rc
RES_OBJ = resource.o

# ─── Compiler flags ───────────────────────────────────────────────────────────
CFLAGS  = -m32 -std=c++14 -O2 -Wall -Wextra \
          -DWINVER=0x0600 -D_WIN32_WINNT=0x0600 -D_WIN32_IE=0x0700

# ─── Linker flags ─────────────────────────────────────────────────────────────
#  -mwindows          : GUI subsystem (no console window)
#  -static-libgcc     : embed GCC runtime (no libgcc_s_dw2-1.dll dependency)
#  -static-libstdc++  : embed C++ runtime (no libstdc++-6.dll dependency)
LDFLAGS = -mwindows -m32 -static

LIBS    = -lole32 -loleaut32 -luuid \
          -lshell32 -lshlwapi       \
          -lcomctl32 -lwininet      \
          -lcomdlg32

# =============================================================================

all: $(TARGET)

$(TARGET): $(SRC) $(RES_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)
	$(STRIP) $@
	@echo "Build complete: $(TARGET)"

$(RES_OBJ): $(RES_SRC) resource.h
	$(RC) -F pe-i386 $(RES_SRC) -o $@

clean:
	rm -f $(TARGET) $(RES_OBJ) *.o

.PHONY: all clean
