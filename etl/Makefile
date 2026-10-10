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

CFLAGS_BASE := -MMD -MP $(OPT) $(WARN) -fno-strict-aliasing -std=gnu11 -fcommon \
	-Wno-error=int-conversion -Wno-error=incompatible-pointer-types \
	-Wno-error=implicit-int -Wno-error=return-type \
	-DSOURCE_PATH_SIZE=0 $(PORTS)

INC := -I$(W) -I$(SHARED) -Ithird_party/minizip -Ithird_party -I$(S)/renderer -I$(S)/qcommon/json

# engine features (see ../README.md for what's off and why)
ENGINE_DEFS := -DETL_CLIENT=1 -DFEATURE_PNG -DFEATURE_PAKISOLATION -DFEATURE_MULTIVIEW -DFEATURE_FREETYPE \
	-DMASTER_SERVER_NAME='"etmaster.etlegacy.com"'
MOD_DEFS := -DMODLIB=1 -DFEATURE_MULTIVIEW -DFEATURE_EDV
CGAME_DEFS  := -DCGAMEDLL=1 $(MOD_DEFS) -DFEATURE_RATING -DFEATURE_XPSAVE
UI_DEFS     := -DUIDLL=1 $(MOD_DEFS)
QAGAME_DEFS := -DGAMEDLL=1 $(MOD_DEFS) -DFEATURE_SERVERMDX
# as v2.83.2's official builds (CMake mod_libraries / cgame_libraries)
MOD283_DEFS := -DMODLIB=1 -DFEATURE_MULTIVIEW -DFEATURE_RATING -DFEATURE_PRESTIGE
CGAME283_DEFS := -DCGAMEDLL=1 $(MOD283_DEFS) -DFEATURE_EDV
UI283_DEFS := -DUIDLL=1 $(MOD283_DEFS)

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
SHARED_WGL := wgl_core wgl_state wgl_draw wgl_texture wgl_readback wgl_post web_js web_frame web_module_data

UI_SHARED := $(addprefix $(S)/ui/,ui_shared.c ui_parse.c ui_script.c ui_menu.c ui_menuitem.c)
MODCOMMON := $(addprefix $(S)/qcommon/,q_math.c q_shared.c version.c)

CGAME_SRC := $(wildcard $(S)/cgame/*.c) $(MODCOMMON) $(S)/qcommon/q_unicode.c $(S)/qcommon/i18n.c \
	$(wildcard $(S)/game/bg_*.c) $(UI_SHARED)
UI_SRC := $(wildcard $(S)/ui/*.c) $(wildcard $(S)/ui/lib/*.c) $(MODCOMMON) $(S)/qcommon/q_unicode.c $(S)/qcommon/i18n.c \
	$(S)/game/bg_classes.c $(S)/game/bg_misc.c
QAGAME_SRC := $(wildcard $(S)/game/*.c) vendor/sha-1/sha1.c $(MODCOMMON)

# v2.83.2 client code, for servers still running that mod (mod/v2.83.2/UPSTREAM.txt)
M283 := mod/v2.83.2
M283_COMMON := $(addprefix $(M283)/src/qcommon/,q_math.c q_shared.c q_unicode.c)
CGAME283_SRC := $(wildcard $(M283)/src/cgame/*.c) $(M283_COMMON) $(M283)/src/qcommon/json/json.c $(wildcard $(M283)/src/game/bg_*.c) \
	$(addprefix $(M283)/src/ui/,ui_shared.c ui_parse.c ui_script.c ui_menu.c ui_menuitem.c)
UI283_SRC := $(wildcard $(M283)/src/ui/*.c) $(M283_COMMON) $(addprefix $(M283)/src/game/,bg_classes.c bg_misc.c)

obj = $(patsubst %.c,$(BUILD)/$(1)/%.o,$(2))

ENGINE_OBJS := $(call obj,engine,$(ENGINE_SRC) $(WEB_SRC)) \
	$(addprefix $(BUILD)/shared/,$(addsuffix .o,$(SHARED_WGL)))
# game modules: sources and compile flags
MODULES := cgame ui qagame cgame283 ui283
cgame_SRC := $(CGAME_SRC)
cgame_FLAGS := $(INC) $(CGAME_DEFS)
ui_SRC := $(UI_SRC)
ui_FLAGS := $(INC) $(UI_DEFS)
qagame_SRC := $(QAGAME_SRC)
qagame_FLAGS := $(INC) $(QAGAME_DEFS)
cgame283_SRC := $(CGAME283_SRC)
cgame283_FLAGS := -I$(M283)/web -I$(M283)/src/qcommon/json -Ithird_party -I$(SHARED) $(CGAME283_DEFS)
ui283_SRC := $(UI283_SRC)
ui283_FLAGS := -I$(M283)/web -I$(SHARED) $(UI283_DEFS)

$(foreach m,$(MODULES),$(eval $(m)_RAW := $(call obj,$(m)_raw,$($(m)_SRC))))
$(foreach m,$(MODULES),$(eval $(m)_OBJS := $(call obj,$(m),$($(m)_SRC))))

# each module's objects between markers that bound its data (../web/module_bounds.c)
bounded = $(BUILD)/bounds/$(1)_begin.o $($(1)_OBJS) $(BUILD)/bounds/$(1)_end.o
ALL_OBJS := $(ENGINE_OBJS) $(foreach m,$(MODULES),$(call bounded,$(m)))

# ---------------------------------------------------------------------------
# link

# loading screens suspend the engine mid-load (../web/web_frame.c)
ASYNCIFY := -sASYNCIFY=1 -sASYNCIFY_ONLY=@$(W)/asyncify.txt -sASYNCIFY_STACK_SIZE=65536 \
	--pre-js $(BUILD)/asyncify.js

LDFLAGS := $(OPT) $(LDOPT) $(PORTS) $(WEBGPU_PORT) \
	-sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=384MB -sMAXIMUM_MEMORY=2GB \
	-sSTACK_SIZE=8MB -sEXIT_RUNTIME=0 \
	-sENVIRONMENT=web -sFORCE_FILESYSTEM=1 -lidbfs.js \
	-sEXPORTED_RUNTIME_METHODS=FS,ccall,IDBFS,HEAPU8,HEAPF32,HEAP16 \
	-sEXPORTED_FUNCTIONS=_main,_malloc,_free,_WEB_ConsoleCommand \
	--pre-js $(SHARED)/pre.js -Wl,--error-limit=0 $(ASYNCIFY)


all: $(DIST)/etl.js $(DIST)/index.html

$(BUILD)/asyncify.js: $(W)/asyncify.txt $(TOP)/tools/gen_asyncify_js.sh
	@mkdir -p $(dir $@)
	sh $(TOP)/tools/gen_asyncify_js.sh $< > $@

$(DIST)/etl.js: $(ALL_OBJS) $(SHARED)/pre.js $(BUILD)/asyncify.js
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

$(BUILD)/bounds/%_begin.o: $(SHARED)/module_bounds.c
	@mkdir -p $(dir $@)
	@$(CC) $(OPT) -fno-common -DMODULE=$* -DBOUND=begin -c $< -o $@
$(BUILD)/bounds/%_end.o: $(SHARED)/module_bounds.c
	@mkdir -p $(dir $@)
	@$(CC) $(OPT) -fno-common -DMODULE=$* -DBOUND=end -c $< -o $@

# Modules are compiled twice: the first pass (bitcode) collects the global
# symbols, the second recompiles with a header that prefixes them (cgame__X,
# ui283__X, ...) so several modules' copies of q_shared.c etc. can coexist.
define module_rules
$(BUILD)/$(1)_raw/%.o: %.c
	@mkdir -p $$(dir $$@)
	@$$(CC) $$(CFLAGS_BASE) $$($(1)_FLAGS) -O0 -flto -c $$< -o $$@

$(BUILD)/prefix/$(1).h: $$($(1)_RAW) $(TOP)/tools/gen_prefix.sh
	@mkdir -p $$(dir $$@)
	sh $(TOP)/tools/gen_prefix.sh $$(NM) $(1) $$($(1)_RAW) > $$@

$(BUILD)/$(1)/%.o: %.c $(BUILD)/prefix/$(1).h
	@mkdir -p $$(dir $$@)
	@echo "  CC  $$< ($(1))"
	@$$(CC) $$(CFLAGS_BASE) -fno-common $$($(1)_FLAGS) -include $(BUILD)/prefix/$(1).h -c $$< -o $$@
endef
$(foreach m,$(MODULES),$(eval $(call module_rules,$(m))))

clean:
	rm -rf $(BUILD) $(DIST)

.PHONY: all clean

# header dependencies (-MMD)
-include $(ALL_OBJS:.o=.d)
