#ifndef __STUB_STDIO_H__
#define __STUB_STDIO_H__
#include <stddef.h>
typedef struct __stub_file { int handle; } FILE;
extern FILE *stdout;
extern FILE *stderr;
int printf(const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int puts(const char *s);
int fputc(int c, FILE *f);
int putchar(int c);
#endif
