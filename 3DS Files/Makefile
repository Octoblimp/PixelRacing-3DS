#---------------------------------------------------------------------------------
# 3D Pixel Racing -- Nintendo 3DS port (functional decompilation of the iOS 1.1 build)
#
#   make            PixelRacing-3DS.3dsx (+ .smdh, .elf)
#   make cia        PixelRacing-3DS.cia (banner + icon from meta/, meta/app.rsf)
#   make romfs      rebuild romfs/ from the user's own IPA (../tools/build_romfs.py)
#   make banner     meta/pixelracing/banner.cgfx/.wav + meta/icon.png from the Wii banner
#   make MODELVIEW=1  the model viewer test app (../tools/modelview) instead of the game
#
# Needs devkitARM (libctru, citro3d), python 3 and ../tools/3ds-bin/{makerom,bannertool}.
# romfs/ is generated locally from the user's game files and never committed.
#
# GNU Make's $(notdir ...) and friends split on whitespace, so like every devkitPro template
# this Makefile cannot run from a path with a space in it ("3DS Files" has one). Build with
# ../tools/build.ps1, which substs a space-free drive letter around the build.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET     output name
# BUILD      object directory
# SOURCES    source directories
# INCLUDES   header directories
# ROMFS      RomFS directory (used when it exists)
# APP_*      SMDH text (the HOME Menu bubble shows APP_DESCRIPTION over APP_AUTHOR)
#---------------------------------------------------------------------------------
# The repository's tools/ folder (build.ps1 passes a space-free path to it).
TOOLS		?=	$(CURDIR)/../tools
PYTHON		?=	python

TARGET		:=	PixelRacing-3DS
BUILD		:=	build
ifeq ($(MODELVIEW),1)
TARGET		:=	ModelView
BUILD		:=	build_mv
SOURCES		:=	source/platform
EXTRA_SRC	:=	$(TOOLS)/modelview
else
SOURCES		:=	source/platform source/game
endif
DATA		:=
INCLUDES	:=	include
ROMFS		:=	romfs

APP_TITLE		:=	3D Pixel Racing
APP_DESCRIPTION	:=	3D Pixel Racing iOS Game Port to Nintendo 3DS
APP_AUTHOR		:=	Octoblimp
ICON			:=	meta/icon.png

# The HOME Menu banner: the Wii channel banner rebuilt as an animated CGFX with its jingle
# (../tools/banner/make_banner.py, committed in meta/pixelracing).
ifneq ($(wildcard meta/pixelracing/banner.cgfx),)
BANNER_ART	:=	-ci meta/pixelracing/banner.cgfx -a meta/pixelracing/banner.wav
else
BANNER_ART	:=	-i meta/icon.png
endif

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations -ffunction-sections -fno-strict-aliasing \
			-Wno-unused-variable -Wno-unused-but-set-variable \
			$(ARCH)
CFLAGS	+=	$(INCLUDE) -D__3DS__ -std=gnu11
# make DEBUG=1: sys_log writes sdmc:/3ds/PixelRacing3D/log.txt (and the sound trace).
ifeq ($(DEBUG),1)
CFLAGS	+=	-DDEBUG_LOG
endif
CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11
ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lcitro3d -lctru -lm
LIBDIRS	:= $(CTRULIB)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) $(EXTRA_SRC)
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES) $(EXTRA_SRC),$(notdir $(wildcard $(dir)/*.c)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))

export LD	:=	$(CC)
export OFILES_SOURCES	:=	$(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES_BIN	:=	$(PICAFILES:.v.pica=.shbin.o)
export OFILES		:=	$(OFILES_BIN) $(OFILES_SOURCES)
export HFILES		:=	$(PICAFILES:.v.pica=_shbin.h)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON		:=	$(TOPDIR)/$(ICON)
export _3DSXDEPS	:=	$(OUTPUT).smdh
export _3DSXFLAGS	+=	--smdh=$(OUTPUT).smdh
ifneq ($(wildcard $(ROMFS)),)
export _3DSXFLAGS	+=	--romfs=$(CURDIR)/$(ROMFS)
endif

MAKEROM		:=	$(TOOLS)/3ds-bin/makerom.exe
BANNERTOOL	:=	$(TOOLS)/3ds-bin/bannertool.exe

.PHONY: all clean cia romfs banner protos

all: protos $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# include/protos.h: one prototype per game function, from ../decomp (symbols, signatures,
# per-module fixes in ../decomp/protos/*.tsv).
protos:
	@$(PYTHON) $(TOOLS)/gen_protos.py include/protos.h > /dev/null

$(BUILD):
	@mkdir -p $@

cia: all
	@"$(BANNERTOOL)" makebanner $(BANNER_ART) -o meta/banner.bnr
	@"$(BANNERTOOL)" makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" \
		-i $(ICON) -o meta/icon.icn
	@"$(MAKEROM)" -f cia -o "$(TARGET).cia" -DAPP_ENCRYPTED=false -target t -exefslogo \
		-elf "$(OUTPUT).elf" -icon meta/icon.icn -banner meta/banner.bnr \
		-rsf meta/app.rsf -DAPP_ROMFS="$(ROMFS)"
	@echo built ... $(TARGET).cia

romfs:
	$(PYTHON) $(TOOLS)/build_romfs.py --out $(ROMFS) --tex3ds $(DEVKITPRO)/tools/bin/tex3ds

banner:
	$(PYTHON) $(TOOLS)/banner/make_banner.py --out meta

clean:
	@echo clean ...
	@rm -fr build build_mv "$(TARGET).3dsx" "$(TARGET).smdh" "$(TARGET).elf" "$(TARGET).cia" \
		ModelView.3dsx ModelView.smdh ModelView.elf

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)
$(OFILES_SOURCES) : $(HFILES)
$(OUTPUT).elf	:	$(OFILES)

%.shbin.o %_shbin.h : %.shbin
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
