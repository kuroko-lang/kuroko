#pragma once

#include "kuroko.h"

#define GROW_CAPACITY(c) ((c) < 8 ? 8 : (c) * 2)
#define GROW_ARRAY(t,p,o,n) (t*)krk_reallocate(p,sizeof(t)*o,sizeof(t)*n)

#define FREE_ARRAY(t,a,c) krk_reallocate(a,sizeof(t) * c, 0)
#define FREE(t,p) krk_reallocate(p,sizeof(t),0)

#define ALLOCATE(type, count) (type*)krk_reallocate(NULL,0,sizeof(type)*(count))

extern void * krk_reallocate(void *, size_t, size_t);
extern void krk_freeObjects(void);
