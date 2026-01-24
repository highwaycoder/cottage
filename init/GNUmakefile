# Nuke built-in rules and variables.
override MAKEFLAGS += -rR

# This is the name that our final initramfs archive will have.
# Change as needed.
override INITRAMFS_ARCHIVE := initramfs.tar

# Convenience macro to reliably declare user overridable variables.
define DEFAULT_VAR =
    ifeq ($(origin $1),default)
        override $(1) := $(2)
    endif
    ifeq ($(origin $1),undefined)
        override $(1) := $(2)
    endif
endef

# It is suggested to use a custom built cross toolchain to build a kernel.
# We are using the standard "cc" here, it may work by using
# the host system's toolchain, but this is not guaranteed.
override DEFAULT_CC := clang
$(eval $(call DEFAULT_VAR,CC,$(DEFAULT_CC)))

# Use cc as the linker so that it can correctly call its own linker
override DEFAULT_LD := clang
$(eval $(call DEFAULT_VAR,LD,$(DEFAULT_LD)))

# User controllable C flags.
# -Wno-address-of-packed-member is added because GCC is a bit overzealous about emitting this warning
# and will emit it even when the structure does have good alignment on the target platform.
# I don't like turning warnings off, but GCC have kind of forced my hand here...
override DEFAULT_CFLAGS := -g -O2 -pipe -Werror 
$(eval $(call DEFAULT_VAR,CFLAGS,$(DEFAULT_CFLAGS)))

# User controllable C preprocessor flags. We set none by default.
override DEFAULT_CPPFLAGS :=
$(eval $(call DEFAULT_VAR,CPPFLAGS,$(DEFAULT_CPPFLAGS)))

# User controllable nasm flags.
override DEFAULT_NASMFLAGS := -F dwarf -g
$(eval $(call DEFAULT_VAR,NASMFLAGS,$(DEFAULT_NASMFLAGS)))

# User controllable linker flags. We set none by default.
override DEFAULT_LDFLAGS :=
$(eval $(call DEFAULT_VAR,LDFLAGS,$(DEFAULT_LDFLAGS)))

# Internal C flags that should not be changed by the user.
override CFLAGS += \
    -Wall \
    -Wextra \
    -ffreestanding \
    -std=c11 \
    -I../kernel/include/
    #-march=x86-64

# Internal C preprocessor flags that should not be changed by the user.
override CPPFLAGS := \
    $(CPPFLAGS)

# Internal linker flags that should not be changed by the user.
override LDFLAGS += \
    -Wl,"-melf_x86_64" \
		-nostdlib \
		-nodefaultlibs \
		-static \
		-e main \

# Internal nasm flags that should not be changed by the user.
override NASMFLAGS += \
    -Wall \
    -f elf64

# Use "find" to glob all *.c, *.S, and *.asm files in the tree and obtain the
# object and header dependency file names.
override CFILES := $(shell cd src && find -L . -type f -name '*.c')
override ASFILES := $(shell cd src && find -L . -type f -name '*.S')
override NASMFILES := $(shell cd src && find -L . -type f -name '*.asm')
override OBJ := $(addprefix obj/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o) $(NASMFILES:.asm=.asm.o))
override HEADER_DEPS := $(addprefix obj/,$(CFILES:.c=.c.d) $(ASFILES:.S=.S.d))

# Default target.
.PHONY: all
all: bin/$(INITRAMFS_ARCHIVE)

# Make the initramfs archive 
bin/$(INITRAMFS_ARCHIVE): sysroot/sbin/init
	mkdir -p "$$(dirname $@)"
	cd sysroot/ && tar cf ../bin/$(INITRAMFS_ARCHIVE) *

# Include header dependencies.
-include $(HEADER_DEPS)

sysroot/sbin/init: GNUmakefile $(OBJ) 
	mkdir -p "$$(dirname $@)"
	$(LD) $(OBJ) $(LDFLAGS) -o $@

# Compilation rules for *.c files.
obj/%.c.o: src/%.c GNUmakefile 
	mkdir -p "$$(dirname $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Compilation rules for *.S files.
obj/%.S.o: src/%.S GNUmakefile 
	mkdir -p "$$(dirname $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Compilation rules for *.asm (nasm) files.
obj/%.asm.o: src/%.asm GNUmakefile
	mkdir -p "$$(dirname $@)"
	nasm $(NASMFLAGS) $< -o $@

# Remove object files and the final executable.
.PHONY: clean
clean:
	rm -rf bin obj sysroot/* bin/$(INITRAMFS_ARCHIVE)
