#include <stdio.h>
#include <kuroko/kuroko.h>
#include <kuroko/vm.h>
 
int main(int argc, char *argv[]) {
    krk_initVM(0);
    krk_startModule("__main__");
    krk_interpret("import kuroko\nprint('Kuroko',kuroko.version)\n", "<stdin>");
    krk_freeVM();
    return 0;
}

