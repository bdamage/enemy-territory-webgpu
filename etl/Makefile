# ET: Legacy v2.86.1 -> WebAssembly + WebGPU
#
#   make -C etl            release build into dist/legacy/
#   make -C etl DEBUG=1    unoptimised build with assertions
#
# Same approach as the 2.60b build (../Makefile): the legacy mod's cgame, ui
# and qagame are linked statically into one wasm binary, each compiled twice
# so tools/gen_prefix.sh can prefix its global symbols. The renderer is
# ET: Legacy's vanilla renderer ("renderer", GL1 path) running on the shared
# WebGPU GL emulation in ../web/wgl_*.c through a generated GLEW stand-in
# (web/GL/glew.h, tools/gen_etl_glew.py).

EMSDK   ?= $(HOME)/emsdk
CC      := emcc
CXX     := em++
NM      := $(EMSDK)/upstream/bin/llvm-nm

TOP     := ..
S       := src
W       := web
SHARED  := $(TOP)/web
BUILD   := $(TOP)/build/etl
DIST    := $(TOP)/dist/legacy

ifeq ($(DEBUG),1)
OPT     := -O0 -g -sASSERTIONS=2
LDOPT   :=
else
OPT     := -O2 -DNDEBUG
LDOPT   := --profiling-funcs
endif

PORTS := -sUSE_ZLIB=1 -sUSE_LIBJPEG=1 -sUSE_SDL=2 -sUSE_FREETYPE=1
WEBGPU_PORT := --use-port=emdawnwebgpu

WARN := -Wno-everything -Wimplicit-function-declaration \
        -Wno-error=implicit-function-declaration \
        -Wincompatible-function-pointer-types -Wno-error=incompatible-function-pointer-types

CFLAGS_BASE := $(OPT) $(WARN) -fno-strict-aliasing -std=gnu11 -fcommon \
	-Wno-error=int-conversion -Wno-error=incompatible-pointer-types \
	-Wno-error=implicit-int -Wno-error=return-type \
	-DSOURCE_PATH_SIZE=0 $(PORTS)

INC := -I$(W) -I$(SHARED) -Ithird_party/minizip -Ithird_party -I$(S)/renderer -I$(S)/qcommon/json

# engine features (see ../README.md for what's off and why)
ENGINE_DEFS := -DETL_CLIENT=1 -DFEATURE_PAKISOLATION -DFEATURE_MULTIVIEW -DFEATURE_FREETYPE \
	-DMASTER_SERVER_NAME='"etmaster.etlegacy.com"'
MOD_DEFS := -DMODLIB=1 -DFEATURE_MULTIVIEW -DFEATURE_EDV
CGAME_DEFS  := -DCGAMEDLL=1 $(MOD_DEFS) -DFEATURE_RATING -DFEATURE_XPSAVE
UI_DEFS     := -DUIDLL=1 $(MOD_DEFS)
QAGAME_DEFS := -DGAMEDLL=1 $(MOD_DEFS) -DFEATURE_SERVERMDX

# ---------------------------------------------------------------------------
# sources

QCOMMON := $(filter-out %/dl_main_curl.c %/dl_main_android.c %/dl_main_stubs.c %/net_ip.c \
	%/auth.c, $(wildcard $(S)/qcommon/*.c)) \
	$(S)/qcommon/json/json.c
ENGINE_SRC := $(QCOMMON) \
	$(wildcard $(S)/server/*.c) \
	$(filter-out %/snd_openal.c %/qal.c, $(wildcard $(S)/client/*.c)) \
	$(wildcard $(S)/botlib/be*.c) $(wildcard $(S)/botlib/l_*.c) \
	$(wildcard $(S)/renderer/*.c) \
	$(filter-out %/tr_common_vulkan.c %/tr_image_svg.c, $(wildcard $(S)/renderercommon/*.c)) \
	$(S)/sys/sys_unix.c $(S)/sys/con_log.c $(S)/sys/con_passive.c \
	vendor/sha-1/sha1.c \
	third_party/minizip/unzip.c third_party/minizip/ioapi.c \
	third_party/cjson/cJSON.c

WEB_SRC := $(wildcard $(W)/*.c)
SHARED_WGL := wgl_core wgl_state wgl_draw wgl_texture web_js

UI_SHARED := $(addprefix $(S)/ui/,ui_shared.c ui_parse.c ui_script.c ui_menu.c ui_menuitem.c)
MODCOMMON := $(addprefix $(S)/qcommon/,q_math.c q_shared.c version.c)

CGAME_SRC := $(wildcard $(S)/cgame/*.c) $(MODCOMMON) $(S)/qcommon/q_unicode.c $(S)/qcommon/i18n.c \
	$(wildcard $(S)/game/bg_*.c) $(UI_SHARED)
UI_SRC := $(wildcard $(S)/ui/*.c) $(wildcard $(S)/ui/lib/*.c) $(MODCOMMON) $(S)/qcommon/q_unicode.c $(S)/qcommon/i18n.c \
	$(S)/game/bg_classes.c $(S)/game/bg_misc.c
QAGAME_SRC := $(wildcard $(S)/game/*.c) vendor/sha-1/sha1.c $(MODCOMMON)

obj = $(patsubst %.c,$(BUILD)/$(1)/%.o,$(2))

ENGINE_OBJS := $(call obj,engine,$(ENGINE_SRC) $(WEB_SRC)) \
	$(addprefix $(BUILD)/shared/,$(addsuffix .o,$(SHARED_WGL)))
CGAME_RAW := $(call obj,cgame_raw,$(CGAME_SRC))
CGAME_OBJS := $(call obj,cgame,$(CGAME_SRC))
UI_RAW := $(call obj,ui_raw,$(UI_SRC))
UI_OBJS := $(call obj,ui,$(UI_SRC))
QAGAME_RAW := $(call obj,qagame_raw,$(QAGAME_SRC))
QAGAME_OBJS := $(call obj,qagame,$(QAGAME_SRC))

ALL_OBJS := $(ENGINE_OBJS) $(CGAME_OBJS) $(UI_OBJS) $(QAGAME_OBJS)

# ---------------------------------------------------------------------------
# link

LDFLAGS := $(OPT) $(LDOPT) $(PORTS) $(WEBGPU_PORT) \
	-sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=384MB -sMAXIMUM_MEMORY=2GB \
	-sSTACK_SIZE=8MB -sEXIT_RUNTIME=0 \
	-sENVIRONMENT=web -sFORCE_FILESYSTEM=1 -lidbfs.js \
	-sEXPORTED_RUNTIME_METHODS=FS,ccall,IDBFS,HEAPU8,HEAPF32,HEAP16 \
	-sEXPORTED_FUNCTIONS=_main,_malloc,_free,_WEB_ConsoleCommand \
	--pre-js $(SHARED)/pre.js -Wl,--error-limit=0

all: $(DIST)/etl.js $(DIST)/index.html

$(DIST)/etl.js: $(ALL_OBJS) $(SHARED)/pre.js
	@mkdir -p $(DIST)
	@echo "  LINK $@"
	@$(CXX) $(ALL_OBJS) $(LDFLAGS) -o $@

# the shared page (../web/index.html) with this build's configuration
$(DIST)/index.html: $(SHARED)/index.html $(W)/config.js
	@mkdir -p $(DIST)
	@awk 'FNR==NR { cfg = cfg $$0 "\n"; next } /^\(function \(\) \{/ && !done { printf "%s", cfg; done = 1 } { print }' \
		$(W)/config.js $(SHARED)/index.html > $@

# ---------------------------------------------------------------------------
# compile

$(BUILD)/engine/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS_BASE) $(WEBGPU_PORT) $(INC) $(ENGINE_DEFS) -c $< -o $@

$(BUILD)/shared/%.o: $(SHARED)/%.c $(SHARED)/*.h
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS_BASE) $(WEBGPU_PORT) -I$(SHARED) -c $< -o $@

# modules, first pass: bitcode for symbol collection
$(BUILD)/cgame_raw/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_BASE) $(INC) $(CGAME_DEFS) -O0 -flto -c $< -o $@
$(BUILD)/ui_raw/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_BASE) $(INC) $(UI_DEFS) -O0 -flto -c $< -o $@
$(BUILD)/qagame_raw/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_BASE) $(INC) $(QAGAME_DEFS) -O0 -flto -c $< -o $@

$(BUILD)/prefix/cgame.h: $(CGAME_RAW) $(TOP)/tools/gen_prefix.sh
	@mkdir -p $(dir $@)
	sh $(TOP)/tools/gen_prefix.sh $(NM) cgame $(CGAME_RAW) > $@
$(BUILD)/prefix/ui.h: $(UI_RAW) $(TOP)/tools/gen_prefix.sh
	@mkdir -p $(dir $@)
	sh $(TOP)/tools/gen_prefix.sh $(NM) ui $(UI_RAW) > $@
$(BUILD)/prefix/qagame.h: $(QAGAME_RAW) $(TOP)/tools/gen_prefix.sh
	@mkdir -p $(dir $@)
	sh $(TOP)/tools/gen_prefix.sh $(NM) qagame $(QAGAME_RAW) > $@

# modules, second pass: prefixed objects
$(BUILD)/cgame/%.o: %.c $(BUILD)/prefix/cgame.h
	@mkdir -p $(dir $@)
	@echo "  CC  $< (cgame)"
	@$(CC) $(CFLAGS_BASE) $(INC) $(CGAME_DEFS) -include $(BUILD)/prefix/cgame.h -c $< -o $@
$(BUILD)/ui/%.o: %.c $(BUILD)/prefix/ui.h
	@mkdir -p $(dir $@)
	@echo "  CC  $< (ui)"
	@$(CC) $(CFLAGS_BASE) $(INC) $(UI_DEFS) -include $(BUILD)/prefix/ui.h -c $< -o $@
$(BUILD)/qagame/%.o: %.c $(BUILD)/prefix/qagame.h
	@mkdir -p $(dir $@)
	@echo "  CC  $< (qagame)"
	@$(CC) $(CFLAGS_BASE) $(INC) $(QAGAME_DEFS) -include $(BUILD)/prefix/qagame.h -c $< -o $@

clean:
	rm -rf $(BUILD) $(DIST)

.PHONY: all clean
