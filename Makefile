# Makefile for the DLNA libretro core.
#
#   make                 build dlna_libretro.so (.dylib / .dll)
#   make DEBUG=1         unoptimised build with debug symbols
#   make harness         build tools/dlna_harness, a headless test frontend
#   make clean
#
# Libraries are found with pkg-config.  If deps/prefix exists (created by
# scripts/build-deps.sh from the git submodules) it is used in preference
# to the system copies, and the libraries are linked statically.
#
# Variables you can override on the command line:
#   LIBRETRO_COMM_DIR   where libretro-common lives (deps/libretro-common)
#   DEPS_PREFIX         where build-deps.sh installed things (deps/prefix)
#   PKG_CONFIG          the pkg-config program to use
#   platform            unix, osx or win (detected automatically)

TARGET_NAME       := dlna
CORE_DIR          := .
LIBRETRO_COMM_DIR ?= $(CORE_DIR)/deps/libretro-common
DEPS_PREFIX       ?= $(CORE_DIR)/deps/prefix
PKG_CONFIG        ?= pkg-config
OBJDIR            := build

# ---------------------------------------------------------------------
# Platform

ifeq ($(platform),)
   platform = unix
   UNAME := $(shell uname -s)
   ifneq ($(findstring MINGW,$(UNAME)),)
      platform = win
   else ifneq ($(findstring MSYS,$(UNAME)),)
      platform = win
   else ifneq ($(findstring Darwin,$(UNAME)),)
      platform = osx
   endif
endif

ifeq ($(platform),osx)
   TARGET  := $(TARGET_NAME)_libretro.dylib
   SHARED  := -dynamiclib
   FPIC    := -fPIC
   DL_LIBS :=
else ifeq ($(platform),win)
   TARGET  := $(TARGET_NAME)_libretro.dll
   SHARED  := -shared -static-libgcc -Wl,--version-script=link.T
   FPIC    :=
   DL_LIBS :=
else
   TARGET  := $(TARGET_NAME)_libretro.so
   SHARED  := -shared -Wl,--version-script=link.T -Wl,--no-undefined
   FPIC    := -fPIC
   DL_LIBS := -ldl
endif

# ---------------------------------------------------------------------
# Dependencies

ifneq ($(wildcard $(DEPS_PREFIX)/lib/pkgconfig),)
   export PKG_CONFIG_PATH := $(abspath $(DEPS_PREFIX))/lib/pkgconfig:$(abspath $(DEPS_PREFIX))/lib64/pkgconfig
   PKG_STATIC := --static
   STATIC_DEPS := 1
endif

PKGS := libavformat libavcodec libswscale libswresample libavutil libupnp freetype2

# SFTP support is optional: used when libssh is available.
ifneq ($(shell $(PKG_CONFIG) --exists libssh && echo yes),)
   PKGS    += libssh
   CFLAGS  += -DHAVE_LIBSSH
   # A static libssh's pkg-config file does not mention the crypto library
   # it was built with, so name mbedTLS (from build-deps.sh) explicitly.
   ifeq ($(STATIC_DEPS),1)
      ifneq ($(shell $(PKG_CONFIG) --exists mbedtls && echo yes),)
         PKGS += mbedtls mbedx509 mbedcrypto
      endif
   endif
endif

# SMB support is optional: used when libsmb2 is available.
ifneq ($(shell $(PKG_CONFIG) --exists libsmb2 && echo yes),)
   PKGS    += libsmb2
   CFLAGS  += -DHAVE_LIBSMB2
   # Some distributions build libsmb2 with Kerberos support but without
   # linking the Kerberos library into it, so it fails to load unless the
   # program links it.  Do so when it is installed.  --no-as-needed stops
   # the linker from dropping it, as the core itself never calls it.
   # (Not needed for the static libsmb2 from build-deps.sh, which is built
   # without Kerberos.)
   ifneq ($(shell $(PKG_CONFIG) --exists krb5-gssapi && echo yes),)
      ifneq ($(platform),osx)
         ifneq ($(STATIC_DEPS),1)
            SMB_EXTRA_LIBS := -Wl,--no-as-needed $(shell $(PKG_CONFIG) --libs krb5-gssapi) -Wl,--as-needed
         endif
      endif
   endif
endif

PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS))
PKG_LIBS   := $(shell $(PKG_CONFIG) $(PKG_STATIC) --libs $(PKGS))
ifeq ($(PKG_LIBS),)
   $(error pkg-config could not find: $(PKGS).  See README.md)
endif

# Library headers are included as "system" headers so the strict warnings
# below apply only to our own code (FFmpeg's headers use C99 features).
SYS_INCLUDES := $(patsubst -I%,-isystem %,$(PKG_CFLAGS))

# ---------------------------------------------------------------------
# Flags

ifeq ($(DEBUG),1)
   OPTFLAGS := -O0 -g
else
   OPTFLAGS := -O2 -DNDEBUG
   # Strip symbols from release builds (a static build shrinks from about
   # 90 MB to about 20 MB).  macOS's linker spells this differently.
   ifneq ($(platform),osx)
      LDFLAGS += -s
   endif
endif

GIT_VERSION := $(shell git rev-parse --short HEAD 2>/dev/null)
ifneq ($(GIT_VERSION),)
   VERSION_DEFINE := -DGIT_VERSION=\"\ $(GIT_VERSION)\"
endif

# Our own code: C90 rules (no // comments, declarations first, no VLAs,
# ...).  gnu89 rather than c89 only because the library headers we include
# need 'inline' and 'long long'; see docs/C90_NOTES.md.
STD_FLAGS := -std=gnu89 -pedantic -Wall -Wextra -Wno-unused-parameter \
             -Wdeclaration-after-statement -Wno-overlength-strings

CFLAGS  += $(OPTFLAGS) $(FPIC) $(VERSION_DEFINE)
INCLUDES := -I$(LIBRETRO_COMM_DIR)/include -Isrc $(SYS_INCLUDES)
# With static libraries the link order matters (a library must come after
# everything that uses it).  A linker group makes GNU ld search the whole
# group repeatedly, so the order pkg-config produces no longer matters.
ifeq ($(STATIC_DEPS)$(platform),1unix)
   LIBS := -Wl,--start-group $(PKG_LIBS) -Wl,--end-group $(SMB_EXTRA_LIBS) -lpthread -lm
else
   LIBS := $(PKG_LIBS) $(SMB_EXTRA_LIBS) -lpthread -lm
endif

# ---------------------------------------------------------------------
# Sources

SOURCES := $(wildcard src/*.c)
LRC_SOURCES := $(LIBRETRO_COMM_DIR)/rthreads/rthreads.c

OBJECTS := $(addprefix $(OBJDIR)/,$(notdir $(SOURCES:.c=.o))) \
           $(addprefix $(OBJDIR)/lrc_,$(notdir $(LRC_SOURCES:.c=.o)))

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $(SHARED) -o $@ $(OBJECTS) $(LIBS)

$(OBJDIR)/%.o: src/%.c src/*.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(STD_FLAGS) $(INCLUDES) -c -o $@ $<

# libretro-common is compiled with its own (relaxed) settings.
$(OBJDIR)/lrc_%.o: $(LIBRETRO_COMM_DIR)/rthreads/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -I$(LIBRETRO_COMM_DIR)/include -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

# ---------------------------------------------------------------------
# Headless test frontend (see tools/harness.c)

harness: tools/dlna_harness

tools/dlna_harness: tools/harness.c
	$(CC) $(OPTFLAGS) $(STD_FLAGS) -I$(LIBRETRO_COMM_DIR)/include -o $@ $< $(DL_LIBS) -lm

clean:
	rm -rf $(OBJDIR) $(TARGET) tools/dlna_harness

.PHONY: all clean harness

# ---------------------------------------------------------------------
# Strict C90 check of our own sources.  FFmpeg's headers use the C99
# 'inline' keyword, so it is mapped to GCC's __inline__ for this check
# only; everything in src/ must otherwise compile as pure ISO C89/C90.

check-c90:
	@for f in $(SOURCES) tools/harness.c; do \
	   echo "  C90 $$f"; \
	   $(CC) -std=c89 -pedantic-errors -Wall -Wextra -Wno-unused-parameter \
	      -Wdeclaration-after-statement -Dinline=__inline__ \
	      $(INCLUDES) -fsyntax-only $$f || exit 1; \
	done
	@echo "All sources are C90 clean."

.PHONY: check-c90
