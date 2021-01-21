#pragma once

#include "object.h"

extern KrkFunction * krk_compile(const char * src, int newScope, char * fileName);
extern void krk_markCompilerRoots(void);
