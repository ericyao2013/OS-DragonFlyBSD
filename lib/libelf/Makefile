# ELF library is a component of ELF Tool Chain (replacement for binutils)
# For now, do not expose this library.  It is needed for execinfo library.


LIB=			elf
INTERNALLIB=		yes
INTERNALLIBPROF=	yes
INSTALL_PIC_ARCHIVE=	yes
TOOLCHAINDIR=		${.CURDIR}/../../contrib/elftoolchain
SRCDIR=			${TOOLCHAINDIR}/libelf
COMMON=			${TOOLCHAINDIR}/common
CFLAGS+=		-I. -I${SRCDIR} -I${COMMON}

.PATH:			${SRCDIR}

# verbatim from vendor sourcefile
SRCS=	elf.c							\
	elf_begin.c						\
	elf_cntl.c						\
	elf_end.c elf_errmsg.c elf_errno.c			\
	elf_data.c						\
	elf_fill.c						\
	elf_flag.c						\
	elf_getarhdr.c						\
	elf_getarsym.c						\
	elf_getbase.c						\
	elf_getident.c						\
	elf_hash.c						\
	elf_kind.c						\
	elf_memory.c						\
	elf_next.c						\
	elf_open.c						\
	elf_rand.c						\
	elf_rawfile.c						\
	elf_phnum.c						\
	elf_shnum.c						\
	elf_shstrndx.c						\
	elf_scn.c						\
	elf_strptr.c						\
	elf_update.c						\
	elf_version.c						\
	gelf_cap.c						\
	gelf_checksum.c						\
	gelf_dyn.c						\
	gelf_ehdr.c						\
	gelf_getclass.c						\
	gelf_fsize.c						\
	gelf_move.c						\
	gelf_phdr.c						\
	gelf_rel.c						\
	gelf_rela.c						\
	gelf_shdr.c						\
	gelf_sym.c						\
	gelf_syminfo.c						\
	gelf_symshndx.c						\
	gelf_xlate.c						\
	libelf_align.c						\
	libelf_allocate.c					\
	libelf_ar.c						\
	libelf_ar_util.c					\
	libelf_checksum.c					\
	libelf_data.c						\
	libelf_ehdr.c						\
	libelf_extended.c					\
	libelf_memory.c						\
	libelf_open.c						\
	libelf_phdr.c						\
	libelf_shdr.c						\
	libelf_xlate.c						\
	${GENSRCS}

GENSRCS=	libelf_fsize.c libelf_msize.c libelf_convert.c

libelf_convert.c:	elf_types.m4 libelf_convert.m4
libelf_fsize.c:		elf_types.m4 libelf_fsize.m4
libelf_msize.c:		elf_types.m4 libelf_msize.m4

# Copy correct headers over to object directory

ELFHEAD=	sys/elf32.h sys/elf64.h sys/elf_common.h
SRCS+=		${ELFHEAD}

${.OBJDIR}/sys:
	mkdir -p ${.TARGET}

${ELFHEAD}: ${.OBJDIR}/sys ${.CURDIR}/../../sys/${.TARGET}
	ln -sf ${.CURDIR}/../../sys/${.TARGET} ${.TARGET}

CLEANFILES=	${GENSRCS}
CLEANDIRS=	sys

.include <bsd.lib.mk>

# Keep the .SUFFIXES line after the include of <bsd.lib.mk>
.SUFFIXES:	.m4 .c
.m4.c:
	m4 -D SRCDIR=${SRCDIR} ${M4FLAGS} ${.IMPSRC} > ${.TARGET}
