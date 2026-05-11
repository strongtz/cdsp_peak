// SPDX-License-Identifier: BSD-3-Clause-Clear

#include <stddef.h>

typedef void init_func_t(int, char **, char **);
typedef void fini_func_t(void);

typedef struct {
   init_func_t **preinit_array;
   init_func_t **init_array;
   fini_func_t **fini_array;
   size_t preinit_array_count;
   size_t init_array_count;
   size_t fini_array_count;
} structors_array_t;

extern int main(int argc, char **argv, char **envp);
__attribute__((noreturn)) void __libc_init(void *raw_args,
                                           void (*onexit)(void),
                                           int (*slingshot)(int, char **,
                                                           char **),
                                           const structors_array_t *structors);

__attribute__((used)) static void _start_main(void *raw_args)
{
   static const structors_array_t structors;

   __libc_init(raw_args, NULL, main, &structors);
}

__asm__(".text\n"
        ".global _start\n"
        ".type _start,%function\n"
        "_start:\n"
        "mov x29,#0\n"
        "mov x30,#0\n"
        "mov x0,sp\n"
        "b _start_main\n"
        ".size _start, .-_start\n");

#if __ANDROID_API__ >= 29
__asm__(".section .tdata,\"awT\",@progbits\n"
        ".p2align 6\n"
        ".text\n"
        ".reloc 0, R_AARCH64_NONE, .tdata\n");
#endif

void *__dso_handle = &__dso_handle;
