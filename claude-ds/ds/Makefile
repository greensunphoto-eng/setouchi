# Claude DS - Nintendo DS app. Build with BlocksDS (https://blocksds.skylyrac.net):
#
#   make                      # -> claude-ds.nds
#
# or with Docker:
#
#   docker run --rm -v "$PWD":/work skylyrac/blocksds:slim-latest make

BLOCKSDS	?= /opt/blocksds/core

NAME		:= claude-ds

GAME_TITLE	:= Claude DS
GAME_SUBTITLE	:= Claude Code on your DS
GAME_AUTHOR	:= claude-ds
GAME_ICON	:= icon.gif

SOURCEDIRS	:= source
INCLUDEDIRS	:= source

# ARM7 core with Wi-Fi support (also handles the PSG sound used for chimes).
ARM7ELF		:= $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi.elf

LIBS		:= -ldswifi9 -lnds9
LIBDIRS		:= $(BLOCKSDS)/libs/dswifi \
		   $(BLOCKSDS)/libs/libnds

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
