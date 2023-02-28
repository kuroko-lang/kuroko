list_of_modules = ['math', 'random', 'socket', 'timeit', 'wcwidth']

# The list of modules above could easily be augmented if we wanted modules with more than a single
# file

foreach module : list_of_modules
    deps = linkdeps
    if module == 'math'
        deps += cc.find_library('m', required : true)
    endif
    shared_module(module, 'module_' + module + '.c', include_directories : srcinc, install : true,
                  name_prefix : '', install_dir : 'lib/kuroko', dependencies : deps)
endforeach
