/*
 * RunShot: Live program state capture and playback tool
 *
 * Relocate the Linux startup stack and arguments into the replay address
 * range. This file is linked into the captured program, before libc
 * initialization.
 */
#include <stddef.h>
#include <stdint.h>

#ifndef AT_NULL
#define AT_NULL 0
#endif
#ifndef AT_PLATFORM
#define AT_PLATFORM 15
#endif
#ifndef AT_EXECFN
#define AT_EXECFN 31
#endif
#ifndef AT_RANDOM
#define AT_RANDOM 25
#endif
#ifndef AT_SYSINFO_EHDR
#define AT_SYSINFO_EHDR 33
#endif

/* Linux RISC-V syscall numbers. */
#define __NR_mmap 222
#define __NR_exit 93

/* mmap protection and mapping flags. */
#define PROT_READ           1
#define PROT_WRITE          2
#define MAP_PRIVATE         0x02
#define MAP_ANON            0x20
#define MAP_STACK           0x20000
#define MAP_FIXED           0x10
#define MAP_FIXED_NOREPLACE 0x100000

/* Keep startup data separate from the downward-growing stack. */
#define STACK_BASE 0x8F000000u
#define STACK_SIZE 0x01000000u /* 16 MiB */
#define STACK_SP   0x8FFFFFF0u
#define ARGS_BASE  0x8E000000u
#define ARGS_SIZE  0x00400000u /* 4 MiB */

typedef struct
{
    uintptr_t type;
    uintptr_t value;
} AuxEntry;

static inline long
sys_mmap(void *addr, size_t len, int prot, int flags, int fd, size_t off)
{
    register long a0 asm("a0") = (long)addr;
    register long a1 asm("a1") = (long)len;
    register long a2 asm("a2") = (long)prot;
    register long a3 asm("a3") = (long)flags;
    register long a4 asm("a4") = (long)fd;
    register long a5 asm("a5") = (long)off;
    register long a7 asm("a7") = __NR_mmap;
    asm volatile("ecall"
                 : "+r"(a0)
                 : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
                 : "memory");
    return a0;
}

__attribute__((noreturn)) static inline void
sys_exit(int code)
{
    register long a0 asm("a0") = code;
    register long a7 asm("a7") = __NR_exit;
    asm volatile("ecall" : : "r"(a0), "r"(a7) : "memory");
    __builtin_unreachable();
}

/* Preserve the fixed-address fallback used by the capture environment. */
static void
map_region(uintptr_t address, size_t size, int flags)
{
    int base_flags = MAP_PRIVATE | MAP_ANON | flags;
    long result = sys_mmap((void *)address, size, PROT_READ | PROT_WRITE,
                           base_flags | MAP_FIXED_NOREPLACE, -1, 0);
    if (result < 0) {
        result = sys_mmap((void *)address, size, PROT_READ | PROT_WRITE,
                          base_flags | MAP_FIXED, -1, 0);
        if (result < 0)
            sys_exit(127);
    }
}

static inline uintptr_t
align_up_16(uintptr_t x)
{
    return (x + 15u) & ~((uintptr_t)15u);
}

static inline size_t
string_length(const char *s)
{
    size_t n = 0;
    while (s && s[n])
        n++;
    return n;
}

static inline void
copy_bytes(void *dst, const void *src, size_t n)
{
    volatile unsigned char *d = (volatile unsigned char *)dst;
    const volatile unsigned char *p = (const volatile unsigned char *)src;
    for (size_t i = 0; i < n; i++)
        d[i] = p[i];
}

static inline char *
copy_string(char *out, const char *in)
{
    size_t n = string_length(in);
    copy_bytes(out, in, n + 1);
    return out + (n + 1);
}

/* Use local byte copies: libc has not been initialized yet. */
static char *
copy_strings(char **destination, char **source, int count, char *cursor)
{
    for (int i = 0; i < count; i++) {
        destination[i] = cursor;
        cursor = copy_string(cursor, source[i]);
    }
    destination[count] = NULL;
    return cursor;
}

/* The linker resolves this symbol when --wrap=__libc_start_main is used. */
extern int __real___libc_start_main(int (*main)(int, char **, char **), int,
                                    char **, void (*init)(void),
                                    void (*fini)(void),
                                    void (*rtld_fini)(void), void *stack_end);

__attribute__((noreturn)) int
__wrap___libc_start_main(int (*mainfn)(int, char **, char **), int argc,
                         char **argv, void (*init)(void), void (*fini)(void),
                         void (*rtld_fini)(void), void *stack_end)
{
    (void)stack_end;

    map_region(STACK_BASE, STACK_SIZE, MAP_STACK);
    map_region(ARGS_BASE, ARGS_SIZE, 0);

    /* argv, envp and auxv are contiguous in the Linux startup layout. */
    char **envp = argv + argc + 1;
    int envc = 0;
    while (envp[envc])
        envc++;

    AuxEntry *auxv = (AuxEntry *)(envp + envc + 1);
    int auxc = 0;
    while (auxv[auxc].type != AT_NULL)
        auxc++;
    auxc++; /* Include the AT_NULL terminator. */

    /* Reserve argc, pointer arrays and auxv before copying their payloads. */
    uintptr_t p = align_up_16(ARGS_BASE);

    uintptr_t *argc_ptr = (uintptr_t *)p;
    *argc_ptr = (uintptr_t)argc;
    p += sizeof(uintptr_t);

    char **new_argv = (char **)p;
    p += (size_t)(argc + 1) * sizeof(char *);
    char **new_envp = (char **)p;
    p += (size_t)(envc + 1) * sizeof(char *);
    AuxEntry *new_auxv = (AuxEntry *)p;
    p += (size_t)auxc * sizeof(AuxEntry);

    char *strcur = (char *)align_up_16(p);

    strcur = copy_strings(new_argv, argv, argc, strcur);
    strcur = copy_strings(new_envp, envp, envc, strcur);

    /* Relocate stack-backed auxv payloads and disable the high-address vDSO.
     */
    for (int i = 0; i < auxc; i++) {
        uintptr_t t = auxv[i].type;
        uintptr_t v = auxv[i].value;

        if (t == AT_SYSINFO_EHDR) {
            v = 0;
        } else if (t == AT_RANDOM) {
            /* AT_RANDOM points to 16 random bytes on the original stack. */
            char *rnd = (char *)align_up_16((uintptr_t)strcur);
            copy_bytes(rnd, (const void *)v, 16);
            v = (uintptr_t)rnd;
            strcur = rnd + 16;
        } else if (t == AT_PLATFORM || t == AT_EXECFN) {
            /* These strings may also point into the original stack. */
            char *s = strcur;
            strcur = copy_string(strcur, (const char *)v);
            v = (uintptr_t)s;
        }

        new_auxv[i].type = t;
        new_auxv[i].value = v;
    }

    /* Switch to the replay stack before entering the real libc startup. */
    asm volatile("mv sp, %0" : : "r"(STACK_SP) : "memory");

    __real___libc_start_main(mainfn, argc, new_argv, init, fini, rtld_fini,
                             (void *)argc_ptr);
    __builtin_unreachable();
}
