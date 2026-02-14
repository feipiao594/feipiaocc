#ifndef __STDARG_H
#define __STDARG_H

// This header is a small stand-in for <stdarg.h> for use with chibicc-like
// compilers. Keep it also compilable by GCC/Clang in GNU mode.
//
// Notes:
// - We avoid invalid operations on void* by using char* and uintptr_t.
// - va_arg uses a GNU statement expression; it won't compile under -std=c11.

#ifdef __UINTPTR_TYPE__
typedef __UINTPTR_TYPE__ _fp_uintptr_t;
#else
typedef unsigned long _fp_uintptr_t;
#endif

typedef struct {
  unsigned int gp_offset;
  unsigned int fp_offset;
  char *overflow_arg_area;
  char *reg_save_area;
} __va_elem;

typedef __va_elem va_list[1];

#define va_start(ap, last) \
  do { *(ap) = *(__va_elem *)__va_area__; } while (0)

#define va_end(ap)

static void *__va_arg_mem(__va_elem *ap, int sz, int align) {
  char *p = ap->overflow_arg_area;
  if (align > 8) {
    _fp_uintptr_t x = (_fp_uintptr_t)p;
    x = (x + 15) / 16 * 16;
    p = (char *)x;
  }
  ap->overflow_arg_area = (char *)(((_fp_uintptr_t)p + (unsigned)sz + 7) / 8 * 8);
  return p;
}

static void *__va_arg_gp(__va_elem *ap, int sz, int align) {
  if (ap->gp_offset >= 48)
    return __va_arg_mem(ap, sz, align);

  void *r = ap->reg_save_area + ap->gp_offset;
  ap->gp_offset += 8;
  return r;
}

static void *__va_arg_fp(__va_elem *ap, int sz, int align) {
  if (ap->fp_offset >= 112)
    return __va_arg_mem(ap, sz, align);

  void *r = ap->reg_save_area + ap->fp_offset;
  ap->fp_offset += 8;
  return r;
}

#define va_arg(ap, ty)                                                  \
  ({                                                                    \
    int klass = __builtin_reg_class(ty);                                \
    *(ty *)(klass == 0 ? __va_arg_gp(ap, sizeof(ty), _Alignof(ty)) :    \
            klass == 1 ? __va_arg_fp(ap, sizeof(ty), _Alignof(ty)) :    \
            __va_arg_mem(ap, sizeof(ty), _Alignof(ty)));                \
  })

#define va_copy(dest, src) ((dest)[0] = (src)[0])

#define __GNUC_VA_LIST 1
typedef va_list __gnuc_va_list;

#endif
