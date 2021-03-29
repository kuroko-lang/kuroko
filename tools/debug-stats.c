#include <stdio.h>
#include <kuroko/kuroko.h>
#include <kuroko/vm.h>
 
int main(int argc, char *argv[]) {
    fprintf(stderr, "sizeof(KrkValue)  = %lld\n", (long long)sizeof(KrkValue));
    fprintf(stderr, "alignof(KrkValue) = %lld\n", (long long)__alignof__(KrkValue));
    fprintf(stderr, "sizeof(KrkObj)    = %lld\n", (long long)sizeof(KrkObj));
    fprintf(stderr, "alignof(KrkObj)   = %lld\n", (long long)__alignof__(KrkObj));
    return 0;
}


