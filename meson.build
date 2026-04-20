project('kuroko', 'c', version : '1.4.0', license : 'MIT')

cc = meson.get_compiler('c')
dl_dep = cc.find_library('dl', required : true)
rt_dep = cc.find_library('rt', required : true)

linkdeps = [dl_dep, rt_dep]

if meson.is_cross_build()
    cc_host = meson.get_compiler('c', native : true)
    dl_dep = cc.find_library('dl', required : true)
    rt_dep = cc.find_library('rt', required : true)

    linkdeps_native = [dl_dep, rt_dep]
else
    linkdeps_native = linkdeps
endif

# If __builtin_floor is not implemented by the compiler, link with libm
if cc.links('''
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char * argv[]) { return printf("%f\n", __builtin_floor(strtod(argv[1], 0))); }
''') == false
    message('Compiler does not implement a __builtin_floor, linking with libm')
    linkdeps += cc.find_library('m', required : true)
endif

subdir('src')

# Having src as an include directory is required so <kuroko/> include paths work fine.
srcinc = include_directories('src')

# Kuroko exports all symbols
executable('kuroko', 'src/kuroko.c', export_dynamic : true,
           include_directories : srcinc, link_with : libkuroko.get_static_lib(),
           install : true, dependencies : linkdeps)

native_krk = executable('nativekuroko', 'src/kuroko.c', export_dynamic : true,
           include_directories : srcinc, link_with : libkuroko_native.get_static_lib(),
           dependencies : linkdeps_native, native: true)

codecs_mod = ['sbencs.krk', 'dbdata.krk']

foreach mod : codecs_mod
    gen_script = meson.source_root() + '/tools/codectools/gen_' + mod
    gen_src = custom_target('gen-' + mod,
                        input : ['tools/codectools/gen_sbencs.krk', 'tools/codectools/encodings.json', 'tools/codectools/indexes.json'],
                        output : [mod],
                        env: { 'KUROKOPATH' : meson.source_root() + '/modules/' },
                        command : [native_krk, gen_script],
                        install : true,
                        install_dir : 'lib/kuroko/')
endforeach

install_subdir('modules', install_dir : 'lib/kuroko/', strip_directory : true)

# Generate a trivial pkgconfig file
pkg = import('pkgconfig')
pkg.generate(libkuroko, name : 'libkuroko', description : 'Kuroko language interpreter')
