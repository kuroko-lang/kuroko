#pragma once
/**
 * @file compiler.h
 * @brief Exported methods for the source compiler.
 */
#include "object.h"

/**
 * @brief Compile a string to a code object.
 *
 * Compiles the source string 'src' into a code object.
 *
 * @param src      Source code string to compile.
 * @param newScope Whether the compiler should treat the source as a new module.
 * @param fileName Path name of the source file or a representative string like "<stdin>"
 * @return The code object resulting from the compilation, or NULL if compilation failed.
 */
extern KrkFunction * krk_compile(const char * src, int newScope, char * fileName);

/**
 * @brief Mark objects owned by the compiler as in use.
 */
extern void krk_markCompilerRoots(void);
