
VERSION         = 0.7
PKG             = vx-scheme-$(VERSION)
OBJ		= cell.o ctx.o subr.o io.o symtab.o
# Three flavors
INTERP_OBJ 	= $(OBJ) interp.o 
BOOTSTRAP_OBJ	= $(OBJ) interp.o vm.o
VM_OBJ          = $(OBJ) vm.o lib.o _library.o
VM_COMP_OBJ     = $(VM_OBJ) _compiler.o 
#
UNIX_OBJ        = main.o
PROGRAM		= vx-scheme
DEFVER          = -DVERSION=$(VERSION)
CFLAGS		= -std=c++20 -O2 -fno-rtti -Wall $(DEFVER)
CC		= gcc	
TC              = ../testcases


all:		vxs-interp vx-scheme

vxs-interp:	$(INTERP_OBJ) $(UNIX_OBJ)
		$(CC) $(CFLAGS) -o $@ $^ -lstdc++ -lc -lm

vxs-bootstrap:	$(BOOTSTRAP_OBJ) $(UNIX_OBJ)
		$(CC) $(CFLAGS) -o $@ $^ -lstdc++ -lc -lm

vx-scheme:	$(VM_COMP_OBJ) $(UNIX_OBJ)
		$(CC) $(CFLAGS) -o $@ $^ -lstdc++ -lc -lm

_library.cpp _compiler.cpp: compiler.scm bootstrap.scm library.scm vxs-bootstrap
		./vxs-bootstrap . < bootstrap.scm

# Build the standalone scheme compiler.  This runs the compile-file.scm 
# script on itself, producing c code, which is then compiled and linked.

scheme-compiler: $(VM_COMP_OBJ) $(UNIX_OBJ) compile-file.scm vx-scheme
		./vx-scheme ./compile-file.scm < compile-file.scm \
		  > _compile-file.cpp
		$(CC) $(CFLAGS) -c _compile-file.cpp
		$(CC) $(CFLAGS) -o $@ $(VM_COMP_OBJ) $(UNIX_OBJ) \
		  _compile-file.o -lstdc++ -lm 

# Precompiled objects!  Run the scheme-compiler to produce bytecode in
# C++ format, compile and link.  The result is a standalone executable
# with no compiler, just the raw bytecode and the VM.  This rule will
# build a standalone executable for anything in the testcases
# directory. Example: "make pi".

%:		../testcases/%.scm scheme-compiler
		./scheme-compiler $< > _$(basename $(notdir $<)).cpp
		$(CC) $(CFLAGS) -c _$(basename $(notdir $<)).cpp
		$(CC) $(CFLAGS) -o $@ $(VM_OBJ) $(UNIX_OBJ) \
			_$(basename $(notdir $<)).o -lstdc++ -lm

# Standard object compilation rule
%.o:		%.cpp
		$(CC) $(CFLAGS) -c $<

# warning: this clean is pretty thorough!

clean:
		rm -f *.o *.a *.exe vxs-interp vxs-bootstrap vx-scheme 
		rm -f scheme-compiler
		rm -f _compile-file.cpp  _*.cpp
		find .. -name '*~' -print | xargs rm -f 
		find .. -name '#*#' -print | xargs rm -f 
		find .. -name '.#*' -print | xargs rm -f 
		rm -rf $(TC)/*.out $(TC)/tmp[123]
		rm -rf *.core *.stackdump $(TC)/*.core $(TC)/*.stackdump
		rm -f ../lib/slib_* ../lib/slibcat
		rm -f core core.*

# Clean before rolling a distribution tarball/zip.

distro:		clean 
		(cd ../..; tar czhf $(PKG).tgz $(PKG)/)
		(cd ../..; zip -q -r $(PKG).zip $(PKG)/)

# Testing: run the test suite.   If SLIB is installed, smoke-test it.

test:		test-interp test-compile

TESTARENA = 	SLABSIZE=1000000

test-interp:	vxs-interp
		@echo '========== TESTING INTERPRETER =========='
		@if [ -d /usr/share/slib ]; then \
		    (cd ../lib; ../src/vxs-interp < vx-slib-test.scm); \
		fi
		@(cd ../testcases; $(TESTARENA) ../src/vxs-interp < vx-test.scm)

test-compile:	vx-scheme
		@echo '========== TESTING COMPILER =========='
		@if [ -d /usr/share/slib ]; then \
		    (cd ../lib; ../src/vx-scheme < vx-slib-test.scm); \
		fi
		@(cd ../testcases; $(TESTARENA) ../src/vx-scheme < vx-test.scm)


#----------------------------------------------------------------------
#
# OS Adaptations (Don't try these, you don't have the right compiler.
# For VxWorks architectures, create new builds in the Tornado project.
#

OUT		= scheme.out
VX_CFLAGS	= $(CFLAGS) -DVXWORKS -I/w/gnu-t3/target/h

$(ARCH)-OBJ	= $(OBJ:%=obj-$(ARCH)/%) obj-$(ARCH)/vx-main.o
$(ARCH)-ALLOBJ	= $($(ARCH)-OBJ) obj-$(ARCH)/_ctdt.o

obj-$(ARCH):		
		mkdir $@
obj-$(ARCH)/%.o:  %.cpp
		$($(ARCH)-GC++) $(VX_CFLAGS) -c $^ -o $@ 
obj-$(ARCH)/_ctdt.o: obj-$(ARCH)/_ctdt.c
		$($(ARCH)-GCC) -o $@ -c $^
obj-$(ARCH)/_ctdt.c: $($(ARCH)-OBJ) 
		$($(ARCH)-NM) $($(ARCH)-OBJ) | $(MUNCH) > $@
obj-$(ARCH)/$(OUT): obj-$(ARCH) $($(ARCH)-ALLOBJ)
		$($(ARCH)-LD) -r -o $@ $($(ARCH)-ALLOBJ)
obj-$(ARCH)/size.out: obj-$(ARCH)/$(OUT) $($(ARCH)-ALLOBJ)
		$($(ARCH)-SIZE) $^ | tee $@

$(ARCH)-GC++	= $($(ARCH)-GC)-c++
$(ARCH)-GCC	= $($(ARCH)-GC)-gcc
$(ARCH)-LD	= $($(ARCH)-GC)-ld
$(ARCH)-NM	= $($(ARCH)-GC)-nm
$(ARCH)-SIZE	= $($(ARCH)-GC)-size
MUNCH		= tclsh8.3 /w/gnu-t3/host/src/hutils/munch.tcl

GCC_BASE	= /w/gnu-t3/host/x86-freebsd/bin

#-----------------------------------------------------------------------
#
# Architecture-specific material
#

simpc-GC	= $(GCC_BASE)/i386-pc-mingw32
simpc:
		$(MAKE) ARCH=simpc obj-simpc/$(OUT)
arm-GC		= $(GCC_BASE)/arm-wrs-vxworks
arm:
		$(MAKE) ARCH=arm obj-arm/$(OUT)
ppc-GC		= $(GCC_BASE)/powerpc-wrs-vxworks
ppc:
		$(MAKE) ARCH=ppc obj-ppc/$(OUT)
m68k-GC		= $(GCC_BASE)/m68k-wrs-vxworks
m68k:
		$(MAKE) ARCH=m68k obj-m68k/$(OUT)

#------------------------------------------------------------------------
#
# Dependencies
#

cell.o:		cell.cpp vx-scheme.h
vm.o:		vm.cpp vx-scheme.h
subr.o:		subr.cpp vx-scheme.h
io.o:		io.cpp 	vx-scheme.h
lib.o: 		lib.cpp vx-scheme.h
interp.o:	interp.cpp vx-scheme.h
symtab.o:       symtab.cpp vx-scheme.h
u-main.o:	u-main.cpp vx-scheme.h
vx-main.o:	vx-main.cpp vx-scheme.h
_compiler.o:	_compiler.cpp

#------------------------------------------------------------------------



