/*
 * Post-process a vdso elf image for inclusion into qemu.
 *
 * Copyright 2021 Linaro, Ltd.
 * 
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <endian.h>
#include <unistd.h>
#include "elf.h"


#define bswap_(p)  _Generic(*(p), \
                            uint16_t: __builtin_bswap16,       \
                            uint32_t: __builtin_bswap32,       \
                            uint64_t: __builtin_bswap64,       \
                            int16_t: __builtin_bswap16,        \
                            int32_t: __builtin_bswap32,        \
                            int64_t: __builtin_bswap64)
#define bswaps(p) (*(p) = bswap_(p)(*(p)))

static void output_reloc(FILE *outf, void *buf, void *loc)
{
    fprintf(outf, "    0x%08lx,\n", loc - buf);
}

static const char *sigreturn_sym;
static const char *rt_sigreturn_sym;

static unsigned sigreturn_addr;
static unsigned rt_sigreturn_addr;

#define N 32
#define elfN(x)  elf32_##x
#define ElfN(x)  Elf32_##x
#include "gen-vdso-elfn.c.inc"
#undef N
#undef elfN
#undef ElfN

#define N 64
#define elfN(x)  elf64_##x
#define ElfN(x)  Elf64_##x
#include "gen-vdso-elfn.c.inc"
#undef N
#undef elfN
#undef ElfN


int main(int argc, char **argv)
{
    FILE *inf, *outf;
    long total_len;
    const char *prefix = "vdso";
    const char *inf_name;
    const char *outf_name = NULL;
    unsigned char *buf;
    bool need_bswap;

    while (1) {
        int opt = getopt(argc, argv, "o:p:r:s:");
        if (opt < 0) {
            break;
        }
        switch (opt) {
        case 'o':
            outf_name = optarg;
            break;
        case 'p':
            prefix = optarg;
            break;
        case 'r':
            rt_sigreturn_sym = optarg;
            break;
        case 's':
            sigreturn_sym = optarg;
            break;
        default:
        usage:
            fprintf(stderr, "usage: [-p prefix] [-r rt-sigreturn-name] "
                    "[-s sigreturn-name] -o output-file input-file\n");
            return EXIT_FAILURE;
        }
    }

    if (optind >= argc || outf_name == NULL) {
        goto usage;
    }
    inf_name = argv[optind];

    /*
     * Open the input and output files.
     */
    inf = fopen(inf_name, "rb");
    if (inf == NULL) {
        goto perror_inf;
    }
    outf = fopen(outf_name, "w");
    if (outf == NULL) {
        goto perror_outf;
    }

    /*
     * Read the input file into a buffer.
     * We expect the vdso to be small, on the order of one page,
     * therefore we do not expect a partial read.
     */
    fseek(inf, 0, SEEK_END);
    total_len = ftell(inf);
    fseek(inf, 0, SEEK_SET);

    buf = malloc(total_len);
    if (buf == NULL) {
        goto perror_inf;
    }

    errno = 0;
    if (fread(buf, 1, total_len, inf) != total_len) {
        if (errno) {
            goto perror_inf;
        }
        fprintf(stderr, "%s: incomplete read\n", inf_name);
        return EXIT_FAILURE;
    }
    fclose(inf);

    /*
     * Write out the vdso image now, before we make local changes.
     */

    fprintf(outf,
            "/* Automatically generated from linux-user/gen-vdso.c. */\n"
            "\n"
            "static const uint8_t %s_image[] = {",
            prefix);
    for (long i = 0; i < total_len; ++i) {
        if (i % 12 == 0) {
            fputs("\n   ", outf);
        }
        fprintf(outf, " 0x%02x,", buf[i]);
    }
    fprintf(outf, "\n};\n\n");

    /*
     * Identify which elf flavor we're processing.
     * The first 16 bytes of the file are e_ident.
     */

    if (buf[EI_MAG0] != ELFMAG0 || buf[EI_MAG1] != ELFMAG1 ||
        buf[EI_MAG2] != ELFMAG2 || buf[EI_MAG3] != ELFMAG3) {
        fprintf(stderr, "%s: not an elf file\n", inf_name);
        return EXIT_FAILURE;
    }
    switch (buf[EI_DATA]) {
    case ELFDATA2LSB:
        need_bswap = BYTE_ORDER != LITTLE_ENDIAN;
        break;
    case ELFDATA2MSB:
        need_bswap = BYTE_ORDER != BIG_ENDIAN;
        break;
    default:
        fprintf(stderr, "%s: invalid elf EI_DATA (%u)\n",
                inf_name, buf[EI_DATA]);
        return EXIT_FAILURE;
    }

    /*
     * We need to relocate the VDSO image.  The one built into the kernel
     * is built for a fixed address.  The one we built for QEMU is not,
     * since that requires close control of the guest address space.
     *
     * Output relocation addresses as we go.
     */

    fprintf(outf, "static const unsigned %s_relocs[] = {\n", prefix);

    switch (buf[EI_CLASS]) {
    case ELFCLASS32:
        elf32_process(outf, buf, need_bswap);
        break;
    case ELFCLASS64:
        elf64_process(outf, buf, need_bswap);
        break;
    default:
        fprintf(stderr, "%s: invalid elf EI_CLASS (%u)\n",
                inf_name, buf[EI_CLASS]);
        return EXIT_FAILURE;
    }

    fprintf(outf, "};\n\n");   /* end vdso_relocs. */

    fprintf(outf, "static const VdsoImageInfo %s_image_info = {\n", prefix);
    fprintf(outf, "    .image = %s_image,\n", prefix);
    fprintf(outf, "    .relocs = %s_relocs,\n", prefix);
    fprintf(outf, "    .image_size = sizeof(%s_image),\n", prefix);
    fprintf(outf, "    .reloc_count = ARRAY_SIZE(%s_relocs),\n", prefix);
    fprintf(outf, "    .sigreturn_ofs = 0x%x,\n", sigreturn_addr);
    fprintf(outf, "    .rt_sigreturn_ofs = 0x%x,\n", rt_sigreturn_addr);
    fprintf(outf, "};\n");

    /*
     * Everything should have gone well.
     */
    if (fclose(outf)) {
        goto perror_outf;
    }
    return EXIT_SUCCESS;

 perror_inf:
    perror(inf_name);
    return EXIT_FAILURE;

 perror_outf:
    perror(outf_name);
    return EXIT_FAILURE;
}
