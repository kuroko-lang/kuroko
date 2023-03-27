#!/bin/sh
($1 -o /dev/null -x c - 2>/dev/null && echo "yes" || echo "no") <<END
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char * argv[]) {
	return printf("%f", __builtin_floor(strtod(argv[1],NULL)));
}
END
