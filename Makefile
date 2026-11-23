# =============================================================================
#  FLOPOS -- a bootable 1.44MB psychedelic live environment
#
#    make            build dist/flopos.img
#    make run        boot it in QEMU with a window
#    make shot       boot headless and grab PNG frames into build/shots/
#    make write DEV=/dev/sdX    write the image to a real floppy
#    make wire       talk to the laptop over serial (logs/serial.log)
# =============================================================================

CC      := gcc
AS      := as
LD      := ld
PYTHON  := python3

CFLAGS  := -m32 -std=gnu99 -ffreestanding -nostdlib -nostdinc \
           -fno-pie -fno-pic -fno-builtin -fno-common \
           -O2 -march=i386 -mtune=pentium \
           -mno-mmx -mno-sse -mno-sse2 -mno-80387 -msoft-float \
           -fno-asynchronous-unwind-tables -fno-unwind-tables \
           -fstack-protector-strong -mstack-protector-guard=global -fomit-frame-pointer \
           -fno-tree-loop-distribute-patterns \
           -Wall -Wextra -Wno-unused-parameter -Wno-array-bounds -Isrc/kernel \
           $(EXTRA_CFLAGS)
# -Wno-array-bounds: the boot info block genuinely lives at fixed low
# addresses, and gcc reads any absolute dereference as an array at zero.

ASFLAGS := --32

# Everything compiled goes in build/ (the tests' too, one folder per test);
# dist/ holds only the finished floppy image.
BUILD   := build
DIST    := dist
OBJDIR  := $(BUILD)/obj

# The source is under src/: boot/, src/kernel/, src/room/ and src/apps/.  What a tool
# makes (tools/build) is in a gen/ folder beside the code it belongs to.
CSRC    := $(wildcard src/kernel/*.c)
GSRC    := $(wildcard src/kernel/gen/*.c)
ASRC    := src/kernel/start.S src/kernel/isr.S src/kernel/bios.S
COBJ    := $(patsubst src/kernel/%.c,$(OBJDIR)/%.o,$(CSRC)) $(patsubst src/kernel/gen/%.c,$(OBJDIR)/%.o,$(GSRC))
AOBJ    := $(patsubst src/kernel/%.S,$(OBJDIR)/%.o,$(ASRC))
KHDRS   := $(wildcard src/kernel/*.h src/kernel/gen/*.h)

# start.o must come first: it owns physical 0x10000.
KOBJ    := $(OBJDIR)/start.o $(OBJDIR)/isr.o $(OBJDIR)/bios.o $(COBJ)

IMG     := $(DIST)/flopos.img

# Apps: every folder under src/apps/ is one (but sdk/, what every app is
# built with, and lib/, code they share), built into its own file and put
# on the floppy beside the kernel.  They share the kernel's compiler flags
# but none of its code -- only src/apps/sdk/app.h.
SDK       := src/apps/sdk
APPNAMES  := $(filter-out lib sdk,$(patsubst src/apps/%/,%,$(wildcard src/apps/*/)))
APPFILES  := $(patsubst %,$(BUILD)/apps/%.app,$(APPNAMES))
APPCFLAGS := $(filter-out -Isrc/kernel,$(CFLAGS)) -I$(SDK)
# browzer plays songs and mp3s, which want the Pentium's FPU; nothing in the
# kernel uses it, so an app has it to itself.
APPFLAGS_browzer := -m80387 -mhard-float

# Libraries apps share: src/apps/lib/<name>/, built into each app that lists
# it.  audio turns songs into samples, for browzer and whatever plays sound
# next.
APPLIBS_browzer := audio bits dsp
APPLIBS_maelcum := tape

.PHONY: all clean run shot write wire size release
all: $(IMG)

$(OBJDIR):
	@mkdir -p $(OBJDIR)

# Every object is rebuilt when a header changes: a table such as the apps'
# (src/apps/sdk/app.h) is laid out in more than one file, and a stale one misleads.
$(OBJDIR)/%.o: src/kernel/%.c $(KHDRS) $(SDK)/app.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJDIR)/%.o: src/kernel/gen/%.c $(KHDRS) | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# The wizard's talk is sums in floating point: the one kernel file that
# uses the FPU (see src/kernel/wizsay.c).
$(OBJDIR)/wizsay.o: CFLAGS += -m80387 -mhard-float

$(OBJDIR)/%.o: src/kernel/%.S | $(OBJDIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/boot.bin: src/boot/boot.S | $(OBJDIR)
	$(AS) $(ASFLAGS) $< -o $(OBJDIR)/boot.o
	$(LD) -m elf_i386 --section-start=.boot=0x7c00 --oformat binary \
	      $(OBJDIR)/boot.o -o $@
	@test $$(stat -c%s $@) -eq 512 || (echo "boot sector is not 512 bytes"; exit 1)

$(BUILD)/kernel.bin: $(KOBJ) src/kernel/linker.ld
	$(LD) -m elf_i386 -T src/kernel/linker.ld -o $@ $(KOBJ) --no-warn-rwx-segments
	@echo "   kernel.bin: $$(stat -c%s $@) bytes"

.SECONDEXPANSION:
$(BUILD)/apps/%.app: $$(wildcard src/apps/%/*.c) $$(wildcard src/apps/%/*.h) \
                     $$(foreach l,$$(APPLIBS_$$*),$$(wildcard src/apps/lib/$$(l)/*.[ch] src/apps/lib/$$(l)/gen/*.h)) \
                     $(SDK)/crt.c $(SDK)/app.h $(SDK)/gen/app_glyphs.h $(SDK)/app.ld
	@mkdir -p $(BUILD)/apps
	$(CC) $(APPCFLAGS) $(APPFLAGS_$*) $(foreach l,$(APPLIBS_$*),-Isrc/apps/lib/$(l)) \
	      -DAPP_NAME='"$*"' -static -Wl,-T,$(SDK)/app.ld -Wl,--no-warn-rwx-segments -o $@ \
	      $(SDK)/crt.c $(wildcard src/apps/$*/*.c) $(foreach l,$(APPLIBS_$*),$(wildcard src/apps/lib/$(l)/*.c))
	@echo "   $*.app: $$(stat -c%s $@) bytes"

# The script room's engine (src/room/): its own program, linked to run in
# the room's memory, never an app (there is no word that starts it).
# Built for the floppy's room: no padding to align code (it buys a Pentium
# next to nothing) except in vm.c, the interpreter's loop; and the compiler,
# run once a script, inlines nothing on its own (12 KB less, 3% slower).
ROOMSRC := $(wildcard src/room/*.c)
ROOMOBJ := $(patsubst src/room/%.c,$(BUILD)/room/%.o,$(ROOMSRC))
ROOM_NOALIGN := -fno-align-functions -fno-align-jumps -fno-align-loops -fno-align-labels
ROOMFLAGS_compile := $(ROOM_NOALIGN) -fno-inline-functions -fno-inline-small-functions
ROOMFLAGS_vm :=
$(BUILD)/room/%.o: src/room/%.c $(wildcard src/room/*.h)
	@mkdir -p $(BUILD)/room
	$(CC) $(APPCFLAGS) -m80387 -mhard-float -Isrc/room $(if $(filter undefined,$(origin ROOMFLAGS_$*)),$(ROOM_NOALIGN),$(ROOMFLAGS_$*)) -c -o $@ $<
$(BUILD)/room.bin: $(ROOMOBJ) src/room/room.ld
	$(CC) $(APPCFLAGS) -static -Wl,-T,src/room/room.ld -Wl,--no-warn-rwx-segments -o $@ $(ROOMOBJ)
	@echo "   room.bin: $$(stat -c%s $@) bytes"

# Test apps (test/walltest): built like any app, never put on the floppy.
$(BUILD)/walltest/%.app: test/walltest/%.c $(SDK)/crt.c $(SDK)/app.h $(SDK)/app.ld
	@mkdir -p $(@D)
	$(CC) $(APPCFLAGS) -DAPP_NAME='"$*"' -static -Wl,-T,$(SDK)/app.ld -Wl,--no-warn-rwx-segments -o $@ $(SDK)/crt.c $<

# The script room's tests (test/roomtest): an app that drives it, and a
# room that breaks the rules on purpose.  Never put on the floppy.
$(BUILD)/roomtest/roomt.app: test/roomtest/roomt.c $(SDK)/crt.c $(SDK)/app.h $(SDK)/app.ld src/room/room.h
	@mkdir -p $(@D)
	$(CC) $(APPCFLAGS) -DAPP_NAME='"roomt"' -static -Wl,-T,$(SDK)/app.ld -Wl,--no-warn-rwx-segments -o $@ $(SDK)/crt.c $<
$(BUILD)/roomtest/rogueroom.bin: test/roomtest/rogueroom.c src/room/room.h src/room/room.ld
	@mkdir -p $(@D)
	$(CC) $(APPCFLAGS) -static -Wl,-T,src/room/room.ld -Wl,--no-warn-rwx-segments -o $@ $<

$(IMG): $(BUILD)/boot.bin $(BUILD)/kernel.bin $(APPFILES) $(BUILD)/room.bin data/saffran.pix data/ozzy.pix data/roots.dat data/wizard.wiz data/help.txt data/gibson.raw data/hudon.raw data/hudoff.raw tools/build/mkimage.py README.md
	@mkdir -p $(DIST)
	$(PYTHON) tools/build/mkimage.py $(BUILD)/boot.bin $(BUILD)/kernel.bin $@ $(APPFILES)

size: $(BUILD)/kernel.bin
	@$(LD) -m elf_i386 -T src/kernel/linker.ld -o /dev/null $(KOBJ) --no-warn-rwx-segments -M 2>/dev/null \
	 | grep -E "^\.(text|rodata|data|bss)|_bss|_image_end" | head -20

run: $(IMG)
	qemu-system-i386 -machine pc -cpu pentium -m 16 \
	  -drive file=$(IMG),if=floppy,format=raw -boot a \
	  -vga std -audiodev pa,id=sp -machine pcspk-audiodev=sp

shot: $(IMG)
	$(PYTHON) test/shots.py $(IMG)

write: $(IMG)
	tools/floppy/writefloppy.sh $(DEV)

wire:
	@$(PYTHON) tools/laptop/wire.py

# Every image handed over gets its own number, its own archived copy, and
# its own notes.  Nothing is bumped or built unless CHANGELOG.md says what
# changed; the notes are stamped with the version only once the image exists.
release:
	@$(PYTHON) tools/release/changelog.py check && \
	 v=$$($(PYTHON) tools/release/release.py $(PART)) && \
	 $(MAKE) --no-print-directory clean >/dev/null && \
	 $(MAKE) --no-print-directory all && \
	 mkdir -p releases && \
	 cp $(IMG) releases/flopos-$$v.img && \
	 { cp -f $(IMG).used releases/flopos-$$v.img.used 2>/dev/null || true; } && \
	 $(PYTHON) tools/release/changelog.py stamp $$v releases/flopos-$$v.txt && \
	 echo "   released flopos-$$v.img, notes in releases/flopos-$$v.txt"

clean:
	rm -rf $(OBJDIR) $(BUILD)/apps $(BUILD)/*.bin $(IMG) $(IMG).used
