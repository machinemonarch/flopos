# =============================================================================
#  FLOPOS -- a bootable 1.44MB psychedelic live environment
#
#    make            build dist/flopos.img
#    make run        boot it in QEMU with a window
#    make shot       boot headless and grab PNG frames into dist/shots/
#    make write DEV=/dev/sdX    write the image to a real floppy
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
           -fno-stack-protector -fomit-frame-pointer \
           -fno-tree-loop-distribute-patterns \
           -Wall -Wextra -Wno-unused-parameter -Wno-array-bounds -Ikernel \
           $(EXTRA_CFLAGS)
# -Wno-array-bounds: the boot info block genuinely lives at fixed low
# addresses, and gcc reads any absolute dereference as an array at zero.

ASFLAGS := --32

BUILD   := dist
OBJDIR  := $(BUILD)/obj

CSRC    := $(wildcard kernel/*.c)
ASRC    := kernel/start.S kernel/isr.S kernel/bios.S
COBJ    := $(patsubst kernel/%.c,$(OBJDIR)/%.o,$(CSRC))
AOBJ    := $(patsubst kernel/%.S,$(OBJDIR)/%.o,$(ASRC))

# start.o must come first: it owns physical 0x10000.
KOBJ    := $(OBJDIR)/start.o $(OBJDIR)/isr.o $(OBJDIR)/bios.o $(COBJ)

IMG     := $(BUILD)/flopos.img

# Apps: every folder under apps/ is one, built into its own file and put on
# the floppy beside the kernel.  They share the kernel's compiler flags but
# none of its code -- only apps/app.h.
APPNAMES  := $(patsubst apps/%/,%,$(wildcard apps/*/))
APPFILES  := $(patsubst %,$(BUILD)/apps/%.app,$(APPNAMES))
APPCFLAGS := $(filter-out -Ikernel,$(CFLAGS)) -Iapps

.PHONY: all clean run shot write size release
all: $(IMG)

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: kernel/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: kernel/%.S | $(OBJDIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/boot.bin: boot/boot.S | $(OBJDIR)
	$(AS) $(ASFLAGS) $< -o $(OBJDIR)/boot.o
	$(LD) -m elf_i386 --section-start=.boot=0x7c00 --oformat binary \
	      $(OBJDIR)/boot.o -o $@
	@test $$(stat -c%s $@) -eq 512 || (echo "boot sector is not 512 bytes"; exit 1)

$(BUILD)/kernel.bin: $(KOBJ) linker.ld
	$(LD) -m elf_i386 -T linker.ld -o $@ $(KOBJ) --no-warn-rwx-segments
	@echo "   kernel.bin: $$(stat -c%s $@) bytes"

.SECONDEXPANSION:
$(BUILD)/apps/%.app: $$(wildcard apps/%/*.c) $$(wildcard apps/%/*.h) apps/crt.c apps/app.h apps/app.ld
	@mkdir -p $(BUILD)/apps
	$(CC) $(APPCFLAGS) -DAPP_NAME='"$*"' -static -Wl,-T,apps/app.ld \
	      -Wl,--no-warn-rwx-segments -o $@ apps/crt.c $(wildcard apps/$*/*.c)
	@echo "   $*.app: $$(stat -c%s $@) bytes"

$(IMG): $(BUILD)/boot.bin $(BUILD)/kernel.bin $(APPFILES) boot/saffran.pix boot/roots.dat tools/mkimage.py README.md
	$(PYTHON) tools/mkimage.py $(BUILD)/boot.bin $(BUILD)/kernel.bin $@ $(APPFILES)

size: $(BUILD)/kernel.bin
	@$(LD) -m elf_i386 -T linker.ld -o /dev/null $(KOBJ) --no-warn-rwx-segments -M 2>/dev/null \
	 | grep -E "^\.(text|rodata|data|bss)|_bss|_image_end" | head -20

run: $(IMG)
	qemu-system-i386 -machine pc -cpu pentium -m 16 \
	  -drive file=$(IMG),if=floppy,format=raw -boot a \
	  -vga std -audiodev pa,id=sp -machine pcspk-audiodev=sp

shot: $(IMG)
	$(PYTHON) tools/shots.py $(IMG)

write: $(IMG)
	tools/writefloppy.sh $(DEV)

# Every image handed over gets its own number, its own archived copy, and
# its own notes.  Nothing is bumped or built unless CHANGELOG.md says what
# changed; the notes are stamped with the version only once the image exists.
release:
	@$(PYTHON) tools/changelog.py check && \
	 v=$$($(PYTHON) tools/release.py $(PART)) && \
	 $(MAKE) --no-print-directory clean >/dev/null && \
	 $(MAKE) --no-print-directory all && \
	 mkdir -p releases && \
	 cp $(IMG) releases/flopos-$$v.img && \
	 { cp -f $(IMG).used releases/flopos-$$v.img.used 2>/dev/null || true; } && \
	 $(PYTHON) tools/changelog.py stamp $$v releases/flopos-$$v.txt && \
	 echo "   released flopos-$$v.img, notes in releases/flopos-$$v.txt"

clean:
	rm -rf $(OBJDIR) $(BUILD)/apps $(BUILD)/*.bin $(BUILD)/*.img
