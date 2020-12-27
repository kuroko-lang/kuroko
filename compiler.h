#pragma once

#include "object.h"

extern KrkFunction * krk_compile(const char * src);
extern void krk_markCompilerRoots(void);
