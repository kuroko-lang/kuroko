#include <stdio.h>

#include "kuroko.h"
#include "chunk.h"
#include "debug.h"
#include "vm.h"

int main(int argc, char * argv[]) {
	krk_initVM();

	krk_interpret("let breakfast = \"beignets\"\nlet beverage = \"café au lait\"\nbreakfast = \"beignets with \" + beverage\nprint breakfast");

	krk_freeVM();
	return 0;
}
