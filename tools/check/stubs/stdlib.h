#ifndef __STUB_STDLIB_H__
#define __STUB_STDLIB_H__
#include <stddef.h>
void *malloc(size_t n);
void free(void *p);
int atoi(const char *s);
long strtol(const char *s, char **e, int b);
int abs(int x);
void exit(int code);
#endif
