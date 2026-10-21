#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal ELF64 (little-endian) structs */
typedef struct
{
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct
{
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf64_Shdr;

typedef struct
{
    uint32_t st_name;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

#define SHT_SYMTAB 2
#define SHT_DYNSYM 11

static int
read_at(FILE *f, uint64_t off, void *buf, size_t n)
{
    if (fseek(f, (long)off, SEEK_SET) != 0)
        return -1;
    return fread(buf, 1, n, f) == n ? 0 : -1;
}

static uint64_t
find_in_one_symsec(FILE *f, const Elf64_Shdr *shdrs, int shnum, int symsec_idx,
                   const char *want)
{
    const Elf64_Shdr *symsec = &shdrs[symsec_idx];
    if (symsec->sh_entsize == 0 || symsec->sh_size == 0)
        return 0;
    if (symsec->sh_link >= (uint32_t)shnum)
        return 0;

    const Elf64_Shdr *strsec = &shdrs[symsec->sh_link];
    char *strs = (char *)malloc((size_t)strsec->sh_size);
    if (!strs)
        return 0;
    if (read_at(f, strsec->sh_offset, strs, (size_t)strsec->sh_size) != 0) {
        free(strs);
        return 0;
    }

    size_t nsyms = (size_t)(symsec->sh_size / symsec->sh_entsize);
    for (size_t i = 0; i < nsyms; i++) {
        Elf64_Sym sym;
        if (read_at(f, symsec->sh_offset + i * symsec->sh_entsize, &sym,
                    sizeof(sym))
            != 0) {
            break;
        }
        if (sym.st_name >= strsec->sh_size)
            continue;
        const char *name = strs + sym.st_name;
        if (strcmp(name, want) == 0) {
            uint64_t v = sym.st_value;
            free(strs);
            return v;
        }
    }

    free(strs);
    return 0;
}

uint64_t
elf_find_start_addr(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;

    Elf64_Ehdr eh;
    if (read_at(f, 0, &eh, sizeof(eh)) != 0) {
        fclose(f);
        return 0;
    }

    if (!(eh.e_ident[0] == 0x7f && eh.e_ident[1] == 'E' && eh.e_ident[2] == 'L'
          && eh.e_ident[3] == 'F')) {
        fclose(f);
        return 0;
    }
    if (eh.e_ident[4] != 2 /*ELFCLASS64*/ || eh.e_ident[5] != 1 /*little*/) {
        fclose(f);
        return 0;
    }

    uint64_t entry = eh.e_entry;

    /* section headers (needed for symtab/dynsym) */
    if (eh.e_shoff == 0 || eh.e_shnum == 0 || eh.e_shentsize == 0) {
        fclose(f);
        return entry;
    }

    size_t shbytes = (size_t)eh.e_shentsize * eh.e_shnum;
    Elf64_Shdr *shdrs = (Elf64_Shdr *)malloc(shbytes);
    if (!shdrs) {
        fclose(f);
        return entry;
    }

    if (read_at(f, eh.e_shoff, shdrs, shbytes) != 0) {
        free(shdrs);
        fclose(f);
        return entry;
    }

    const char *cands[] = {
        "__libc_start_main",
        // "__wrap___libc_start_main",
        "main",
    };

    uint64_t v = 0;

    /* pass 0: symtab, pass 1: dynsym */
    for (int pass = 0; pass < 2 && v == 0; pass++) {
        uint32_t want_type = (pass == 0) ? SHT_SYMTAB : SHT_DYNSYM;
        for (int i = 0; i < (int)eh.e_shnum && v == 0; i++) {
            if (shdrs[i].sh_type != want_type)
                continue;
            for (size_t k = 0; k < sizeof(cands) / sizeof(cands[0]) && v == 0;
                 k++) {
                v = find_in_one_symsec(f, shdrs, (int)eh.e_shnum, i, cands[k]);
            }
        }
    }

    free(shdrs);
    fclose(f);

    return v ? v : entry;
}
