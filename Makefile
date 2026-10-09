# Wolfenstein: Enemy Territory -> WebAssembly + WebGPU
#
#   make            release build into dist/
#   make DEBUG=1    unoptimised build with assertions
#   make serve      serve dist/ + data/ and the UDP relay on http://localhost:8080
#
# The game/cgame/ui modules are normally separate .so files. Here they are
# linked statically into one wasm binary. Each module is compiled twice: the
# first pass collects its global symbols, the second recompiles it with a
# generated header that prefixes them (cgame_*, ui_*, qagame_*) so the three
# copies of q_shared.c, bg_*.c etc. don't collide.

EMSDK   ?= $(HOME)/emsdk
CC      := emcc
CXX     := em++
NM      := $(EMSDK)/upstream/bin/llvm-nm

BUILD   := build
DIST    := dist

ifeq ($(DEBUG),1)
OPT     := -O0 -g -sASSERTIONS=2 -sSAFE_HEAP=0
else
OPT     := -O2 -DNDEBUG
LDOPT   := --profiling-funcs
endif

WARN := -Wno-everything -Wimplicit-function-declaration \
        -Wno-error=implicit-function-declaration \
        -Wincompatible-function-pointer-types -Wno-error=incompatible-function-pointer-types

# id's master is long gone; ET: Legacy's master also lists 2.60b servers
MASTER := -DMASTER_SERVER_NAME='"etmaster.etlegacy.com"'

CFLAGS_BASE := $(OPT) $(WARN) $(MASTER) -fno-strict-aliasing -std=gnu99 -fcommon \
               -Wno-error=int-conversion -Wno-error=incompatible-pointer-types \
               -Wno-error=implicit-int -Wno-error=return-type
CXXFLAGS_BASE := $(OPT) -Wno-everything -fno-strict-aliasing -std=gnu++98

WEBGPU_PORT := --use-port=emdawnwebgpu

# ---------------------------------------------------------------------------
# sources

S := src

JPEG := jcapimin jchuff jcinit jccoefct jccolor jfdctflt jcdctmgr jcphuff \
        jcmainct jcmarker jcmaster jcomapi jcparam jcprepct jcsample jdapimin \
        jdapistd jdatasrc jdcoefct jdcolor jddctmgr jdhuff jdinput jdmainct \
        jdmarker jdmaster jdpostct jdsample jdtrans jerror jidctflt jmemmgr \
        jmemnobs jutils
RENDERER := tr_animation_mdm tr_animation_mds tr_backend tr_bsp tr_cmds \
        tr_cmesh tr_curve tr_decals tr_flares tr_font tr_image tr_init \
        tr_light tr_main tr_marks tr_mesh tr_model tr_noise tr_scene tr_shade \
        tr_shade_calc tr_shader tr_shadows tr_sky tr_surface tr_world
SERVER := sv_bot sv_ccmds sv_client sv_game sv_init sv_main sv_net_chan \
        sv_snapshot sv_world
QCOMMON := cm_load cm_patch cm_polylib cm_test cm_trace cmd common cvar \
        files huffman md4 msg net_chan unzip vm vm_interpreted
CLIENT := cl_cgame cl_cin cl_console cl_input cl_keys cl_main cl_net_chan \
        cl_parse cl_scrn cl_ui snd_adpcm snd_dma snd_mem snd_mix snd_wavelet
BOTLIB := be_aas_bspq3 be_aas_cluster be_aas_debug be_aas_entity be_aas_file \
        be_aas_main be_aas_move be_aas_optimize be_aas_reach be_aas_route \
        be_aas_routealt be_aas_routetable be_aas_sample be_ai_char be_ai_chat \
        be_ai_gen be_ai_goal be_ai_move be_ai_weap be_ai_weight be_ea \
        be_interface l_crc l_libvar l_log l_memory l_precomp l_script l_struct
SPLINES := math_angles math_matrix math_quaternion math_vector q_parse \
        splines util_str
WEB := sys_web web_input web_snd web_net web_dl web_modules glimp_vanilla wgl_core wgl_state wgl_draw \
        wgl_texture

CORE_OBJS := \
	$(addprefix $(BUILD)/core/jpeg-6/,$(addsuffix .o,$(JPEG))) \
	$(addprefix $(BUILD)/core/renderer/,$(addsuffix .o,$(RENDERER))) \
	$(addprefix $(BUILD)/core/server/,$(addsuffix .o,$(SERVER))) \
	$(addprefix $(BUILD)/core/qcommon/,$(addsuffix .o,$(QCOMMON))) \
	$(addprefix $(BUILD)/core/client/,$(addsuffix .o,$(CLIENT))) \
	$(addprefix $(BUILD)/core/botlib/,$(addsuffix .o,$(BOTLIB))) \
	$(addprefix $(BUILD)/core/splines/,$(addsuffix .o,$(SPLINES))) \
	$(BUILD)/core/game/q_shared.o $(BUILD)/core/game/q_math.o \
	$(addprefix $(BUILD)/web/,$(addsuffix .o,$(WEB)))

QAGAME_SRC := $(addprefix botai/,ai_cmd ai_dmgoal_mp ai_dmnet_mp ai_dmq3 \
        ai_main ai_script ai_script_actions ai_team) \
	$(addprefix game/,bg_animation bg_animgroup bg_campaign bg_character \
	bg_classes bg_misc bg_pmove bg_slidemove bg_sscript bg_stats bg_tracemap \
	g_active g_alarm g_antilag g_bot g_buddy_list g_character g_client g_cmds \
	g_cmds_ext g_combat g_config g_fireteams g_items g_main g_match g_mem \
	g_misc g_missile g_mover g_multiview g_props g_referee g_save g_script \
	g_script_actions g_session g_spawn g_stats g_sv_entities g_svcmds \
	g_syscalls g_systemmsg g_target g_team g_teammapdata g_trigger g_utils \
	g_vote g_weapon q_math q_shared)

CGAME_SRC := $(addprefix cgame/,cg_atmospheric cg_character cg_commandmap \
	cg_consolecmds cg_debriefing cg_draw cg_drawtools cg_effects cg_ents \
	cg_event cg_fireteamoverlay cg_fireteams cg_flamethrower cg_info \
	cg_limbopanel cg_loadpanel cg_localents cg_main cg_marks \
	cg_missionbriefing cg_multiview cg_newDraw cg_particles cg_players \
	cg_playerstate cg_polybus cg_popupmessages cg_predict cg_scoreboard \
	cg_servercmds cg_snapshot cg_sound cg_spawn cg_statsranksmedals \
	cg_syscalls cg_trails cg_view cg_weapons cg_window) \
	$(addprefix game/,bg_animation bg_animgroup bg_character bg_classes \
	bg_misc bg_pmove bg_slidemove bg_sscript bg_stats bg_tracemap q_math \
	q_shared) ui/ui_shared

UI_SRC := $(addprefix ui/,ui_atoms ui_gameinfo ui_loadpanel ui_main \
	ui_players ui_shared ui_syscalls ui_util) \
	$(addprefix game/,bg_campaign bg_classes bg_misc q_math q_shared)

QAGAME_DEFS := -DGAMEDLL
CGAME_DEFS  := -DCGAMEDLL
UI_DEFS     := -DUIDLL

define module_objs
$(addprefix $(BUILD)/$(1)/,$(addsuffix .o,$(2)))
endef

QAGAME_RAW := $(call module_objs,qagame_raw,$(QAGAME_SRC))
QAGAME_OBJS := $(call module_objs,qagame,$(QAGAME_SRC))
CGAME_RAW := $(call module_objs,cgame_raw,$(CGAME_SRC))
CGAME_OBJS := $(call module_objs,cgame,$(CGAME_SRC))
UI_RAW := $(call module_objs,ui_raw,$(UI_SRC))
UI_OBJS := $(call module_objs,ui,$(UI_SRC))

ALL_OBJS := $(CORE_OBJS) $(QAGAME_OBJS) $(CGAME_OBJS) $(UI_OBJS)

# ---------------------------------------------------------------------------
# link

LDFLAGS := $(OPT) $(LDOPT) $(WEBGPU_PORT) \
	-sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=256MB -sMAXIMUM_MEMORY=2GB \
	-sSTACK_SIZE=4MB -sEXIT_RUNTIME=0 \
	-sENVIRONMENT=web -sFORCE_FILESYSTEM=1 -lidbfs.js \
	-sEXPORTED_RUNTIME_METHODS=FS,ccall,IDBFS,HEAPU8,HEAPF32,HEAP16 \
	-sEXPORTED_FUNCTIONS=_main,_malloc,_free,_WEB_ConsoleCommand \
	--pre-js web/pre.js

all: $(DIST)/et.js $(DIST)/index.html

$(DIST)/et.js: $(ALL_OBJS) web/pre.js
	@mkdir -p $(DIST)
	@echo "  LINK $@"
	@$(CXX) $(ALL_OBJS) $(LDFLAGS) -o $@

$(DIST)/index.html: web/index.html
	@mkdir -p $(DIST)
	cp $< $@

# ---------------------------------------------------------------------------
# compile

CORE_INC := -I$(S) -Iweb

$(BUILD)/core/botlib/%.o: $(S)/botlib/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS_BASE) $(CORE_INC) -DBOTLIB -c $< -o $@

$(BUILD)/core/splines/%.o: $(S)/splines/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX $<"
	@$(CXX) $(CXXFLAGS_BASE) $(CORE_INC) -c $< -o $@

$(BUILD)/core/splines/q_shared.o: $(S)/splines/q_shared.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS_BASE) $(CORE_INC) -c $< -o $@

$(BUILD)/core/%.o: $(S)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS_BASE) $(CORE_INC) -c $< -o $@

$(BUILD)/web/%.o: web/%.c web/*.h
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS_BASE) $(WEBGPU_PORT) $(CORE_INC) -Wall -Wno-unused-function -c $< -o $@

# --- statically linked game modules (two-pass symbol prefixing) -----------

$(BUILD)/qagame_raw/%.o: $(S)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS_BASE) $(QAGAME_DEFS) -O0 -flto -c $< -o $@
$(BUILD)/cgame_raw/%.o: $(S)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS_BASE) $(CGAME_DEFS) -O0 -flto -c $< -o $@
$(BUILD)/ui_raw/%.o: $(S)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS_BASE) $(UI_DEFS) -O0 -flto -c $< -o $@

$(BUILD)/prefix/qagame.h: $(QAGAME_RAW) tools/gen_prefix.sh
	@mkdir -p $(dir $@)
	sh tools/gen_prefix.sh $(NM) qagame $(QAGAME_RAW) > $@
$(BUILD)/prefix/cgame.h: $(CGAME_RAW) tools/gen_prefix.sh
	@mkdir -p $(dir $@)
	sh tools/gen_prefix.sh $(NM) cgame $(CGAME_RAW) > $@
$(BUILD)/prefix/ui.h: $(UI_RAW) tools/gen_prefix.sh
	@mkdir -p $(dir $@)
	sh tools/gen_prefix.sh $(NM) ui $(UI_RAW) > $@

$(BUILD)/qagame/%.o: $(S)/%.c $(BUILD)/prefix/qagame.h
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS_BASE) $(QAGAME_DEFS) -include $(BUILD)/prefix/qagame.h -c $< -o $@
$(BUILD)/cgame/%.o: $(S)/%.c $(BUILD)/prefix/cgame.h
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS_BASE) $(CGAME_DEFS) -include $(BUILD)/prefix/cgame.h -c $< -o $@
$(BUILD)/ui/%.o: $(S)/%.c $(BUILD)/prefix/ui.h
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS_BASE) $(UI_DEFS) -include $(BUILD)/prefix/ui.h -c $< -o $@

# ---------------------------------------------------------------------------

serve: all
	node tools/server.mjs

clean:
	rm -rf $(BUILD) $(DIST)

.PHONY: all serve clean
