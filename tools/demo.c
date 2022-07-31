#include <stdio.h>
#include <kuroko/kuroko.h>
#include <kuroko/vm.h>
 
int main(int argc, char *argv[]) {
    KrkThreadState * _thread = krk_initVM(0);
    krk_startModule("__main__");
    krk_interpret(_thread, "import kuroko\nprint('Kuroko',kuroko.version)\n", "<stdin>");
    krk_freeVM(_thread);
    return 0;
}

