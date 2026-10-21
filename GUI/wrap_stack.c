// wrap_stack.c
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

// Linux riscv64 syscalls
#define __NR_mmap 222
#define __NR_exit 93

// mmap prot/flags
#define PROT_READ           1
#define PROT_WRITE          2
#define MAP_PRIVATE         0x02
#define MAP_ANON            0x20
#define MAP_STACK           0x20000
#define MAP_FIXED           0x10
#define MAP_FIXED_NOREPLACE 0x100000

typedef struct
{
    uintptr_t a_type;
    uintptr_t a_val;
} auxv_t;

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

static inline uintptr_t
align16(uintptr_t x)
{
    return (x + 15u) & ~((uintptr_t)15u);
}

static inline size_t
cstrlen(const char *s)
{
    size_t n = 0;
    while (s && s[n])
        n++;
    return n;
}

static inline void
cpy_bytes(void *dst, const void *src, size_t n)
{
    volatile unsigned char *d = (volatile unsigned char *)dst;
    const volatile unsigned char *p = (const volatile unsigned char *)src;
    for (size_t i = 0; i < n; i++)
        d[i] = p[i];
}

static inline char *
cpy_cstr(char *out, const char *in)
{
    size_t n = cstrlen(in);
    cpy_bytes(out, in, n + 1);
    return out + (n + 1);
}

/* linker provides this when using --wrap */
extern int __real___libc_start_main(int (*main)(int, char **, char **), int,
                                    char **, void (*init)(void),
                                    void (*fini)(void),
                                    void (*rtld_fini)(void), void *stack_end);

__attribute__((noreturn)) int
__wrap___libc_start_main(int (*mainfn)(int, char **, char **), int argc,
                         char **argv, void (*init)(void), void (*fini)(void),
                         void (*rtld_fini)(void), void *stack_end)
{
    (void)stack_end; // 我们会传新的 stack_end

    // 你要求的范围
    const uintptr_t STACK_BASE = 0x8F000000u;
    const size_t STACK_SIZE = 0x01000000u; // 16MB
    const uintptr_t STACK_SP = 0x8FFFFFF0u;

    // 给“argv/envp/auxv+字符串+AT_RANDOM”等单独开一块 args
    // 区，避免被栈增长覆盖
    const uintptr_t ARGS_BASE = 0x8E000000u;
    const size_t ARGS_SIZE = 0x00400000u; // 4MB（够用可再调大）

    // 1) mmap 固定栈区
    long r = sys_mmap((void *)STACK_BASE, STACK_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON | MAP_STACK | MAP_FIXED_NOREPLACE,
                      -1, 0);
    if (r < 0) {
        r = sys_mmap((void *)STACK_BASE, STACK_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON | MAP_STACK | MAP_FIXED, -1, 0);
        if (r < 0)
            sys_exit(127);
    }

    // 2) mmap 固定 args 区
    r = sys_mmap((void *)ARGS_BASE, ARGS_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0);
    if (r < 0) {
        r = sys_mmap((void *)ARGS_BASE, ARGS_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (r < 0)
            sys_exit(127);
    }

    // 3) 从 argv 推出 envp/auxv（这比读 sp 更稳）
    char **envp = argv + argc + 1;
    int envc = 0;
    while (envp[envc])
        envc++;

    auxv_t *auxv = (auxv_t *)(envp + envc + 1);
    int auxc = 0;
    while (auxv[auxc].a_type != AT_NULL)
        auxc++;
    auxc++; // include AT_NULL

    // 4) 在 ARGS 区构造“新初始栈布局”：argc/argv/envp/auxv（全在 0x8E....）
    uintptr_t p = align16(ARGS_BASE);

    uintptr_t *argc_ptr = (uintptr_t *)p;
    *argc_ptr = (uintptr_t)argc;
    p += sizeof(uintptr_t);

    char **new_argv = (char **)p;
    p += (size_t)(argc + 1) * sizeof(char *);
    char **new_envp = (char **)p;
    p += (size_t)(envc + 1) * sizeof(char *);
    auxv_t *new_auxv = (auxv_t *)p;
    p += (size_t)auxc * sizeof(auxv_t);

    uintptr_t strp = align16(p);
    char *strcur = (char *)strp;

    // copy argv strings
    for (int i = 0; i < argc; i++) {
        new_argv[i] = strcur;
        strcur = cpy_cstr(strcur, argv[i]);
    }
    new_argv[argc] = NULL;

    // copy envp strings
    for (int i = 0; i < envc; i++) {
        new_envp[i] = strcur;
        strcur = cpy_cstr(strcur, envp[i]);
    }
    new_envp[envc] = NULL;

    // copy auxv, but rewrite stack-pointing entries + disable vdso
    for (int i = 0; i < auxc; i++) {
        uintptr_t t = auxv[i].a_type;
        uintptr_t v = auxv[i].a_val;

        if (t == AT_SYSINFO_EHDR) {
            v = 0; // 禁用 vDSO，避免高地址访问
        } else if (t == AT_RANDOM) {
            // AT_RANDOM 指向 16 bytes（原本在初始高地址栈上）
            char *rnd = (char *)align16((uintptr_t)strcur);
            cpy_bytes(rnd, (const void *)v, 16);
            v = (uintptr_t)rnd;
            strcur = rnd + 16;
        } else if (t == AT_PLATFORM || t == AT_EXECFN) {
            // 这些字符串通常也在初始高地址栈上
            char *s = strcur;
            strcur = cpy_cstr(strcur, (const char *)v);
            v = (uintptr_t)s;
        }

        new_auxv[i].a_type = t;
        new_auxv[i].a_val = v;
    }

    // 5) 切 sp 到 0x8FFFFFF0，然后进入真正的 glibc 启动
    asm volatile("mv sp, %0" : : "r"(STACK_SP) : "memory");

    __real___libc_start_main(mainfn, argc, new_argv, init, fini, rtld_fini,
                             (void *)argc_ptr);
    __builtin_unreachable();
}
