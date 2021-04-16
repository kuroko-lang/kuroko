try:
    from ._fasttimer import timeit
except:
    import ffi
    path = __file__.split('/')[:-1]
    if not path: path = '.'
    else: path = '/'.join(path)
    lib = ffi.open(path + '/_mpytimer.so')
    _timeit = lib.func('d','timeit','Ci')
    def timeit(callback,number=1000000):
        cb = ffi.callback('v',callback,'')
        return _timeit(cb,number)

