libkuroko_sources = ['builtins.c',
                     'chunk.c',
                     'compiler.c',
                     'debug.c',
                     'exceptions.c',
                     'fileio.c',
                     'kuroko.c',
                     'memory.c',
                     'obj_base.c',
                     'obj_bytes.c',
                     'obj_dict.c',
                     'object.c',
                     'obj_function.c',
                     'obj_gen.c',
                     'obj_list.c',
                     'obj_long.c',
                     'obj_numeric.c',
                     'obj_range.c',
                     'obj_set.c',
                     'obj_slice.c',
                     'obj_str.c',
                     'obj_tuple.c',
                     'obj_typing.c',
                     'os.c',
                     'parseargs.c',
                     'scanner.c',
                     'sys.c',
                     'table.c',
                     'threads.c',
                     'time.c',
                     'value.c',
                     'vm.c',
                     'vendor/rline.c'
                     ]

inc = include_directories('kuroko')

srcinc = include_directories('.')

subdir('kuroko')

# We need libkuroko for the host too
libkuroko = both_libraries('kuroko', libkuroko_sources, include_directories : inc, install : true, dependencies : linkdeps)

if meson.is_cross_build()
    libkuroko_native = both_libraries('kuroko_native', libkuroko_sources, include_directories : inc,
                                      dependencies : linkdeps_native, native : true)
else
    libkuroko_native = libkuroko
endif

subdir('modules')
