#include <stdio.h>

#include "kuroko.h"
#include "chunk.h"
#include "debug.h"
#include "vm.h"

int main(int argc, char * argv[]) {
	krk_initVM();

	FILE * f = fopen("test.krk","r");
	if (!f) return 1;
	fseek(f, 0, SEEK_END);
	size_t size = ftell(f);
	fseek(f, 0, SEEK_SET);

	char * buf = malloc(size+1);
	fread(buf, 1, size, f);
	fclose(f);
	buf[size] = '\0';

	krk_interpret(buf);

	krk_freeVM();

	free(buf);
	return 0;
}
