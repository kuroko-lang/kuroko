# Show relative speeds of local, nonlocal, global, and built-in access.

class _A(object):
    def m(self):
        pass

v_global = 1

def read_local():
    v_local = 1
    v_local;    v_local;    v_local;    v_local;    v_local
    v_local;    v_local;    v_local;    v_local;    v_local
    v_local;    v_local;    v_local;    v_local;    v_local
    v_local;    v_local;    v_local;    v_local;    v_local
    v_local;    v_local;    v_local;    v_local;    v_local

def make_nonlocal_reader():
    v_nonlocal = 1
    def inner():
        v_nonlocal; v_nonlocal; v_nonlocal; v_nonlocal; v_nonlocal
        v_nonlocal; v_nonlocal; v_nonlocal; v_nonlocal; v_nonlocal
        v_nonlocal; v_nonlocal; v_nonlocal; v_nonlocal; v_nonlocal
        v_nonlocal; v_nonlocal; v_nonlocal; v_nonlocal; v_nonlocal
        v_nonlocal; v_nonlocal; v_nonlocal; v_nonlocal; v_nonlocal
    return inner

read_nonlocal = make_nonlocal_reader()

def read_global():
    v_global; v_global; v_global; v_global; v_global
    v_global; v_global; v_global; v_global; v_global
    v_global; v_global; v_global; v_global; v_global
    v_global; v_global; v_global; v_global; v_global
    v_global; v_global; v_global; v_global; v_global

def read_builtin():
    oct; oct; oct; oct; oct
    oct; oct; oct; oct; oct
    oct; oct; oct; oct; oct
    oct; oct; oct; oct; oct
    oct; oct; oct; oct; oct

_A.x = 1

def read_classvar():
    A = _A
    A.x;    A.x;    A.x;    A.x;    A.x
    A.x;    A.x;    A.x;    A.x;    A.x
    A.x;    A.x;    A.x;    A.x;    A.x
    A.x;    A.x;    A.x;    A.x;    A.x
    A.x;    A.x;    A.x;    A.x;    A.x

_a = _A()
_a.x = 1

def read_instancevar():
    a = _a
    a.x;    a.x;    a.x;    a.x;    a.x
    a.x;    a.x;    a.x;    a.x;    a.x
    a.x;    a.x;    a.x;    a.x;    a.x
    a.x;    a.x;    a.x;    a.x;    a.x
    a.x;    a.x;    a.x;    a.x;    a.x

def read_unboundmethod():
    A = _A
    A.m;    A.m;    A.m;    A.m;    A.m
    A.m;    A.m;    A.m;    A.m;    A.m
    A.m;    A.m;    A.m;    A.m;    A.m
    A.m;    A.m;    A.m;    A.m;    A.m
    A.m;    A.m;    A.m;    A.m;    A.m

def read_boundmethod():
    a = _a
    a.m;    a.m;    a.m;    a.m;    a.m
    a.m;    a.m;    a.m;    a.m;    a.m
    a.m;    a.m;    a.m;    a.m;    a.m
    a.m;    a.m;    a.m;    a.m;    a.m
    a.m;    a.m;    a.m;    a.m;    a.m

def write_local():
    v_local = 1
    v_local = 1; v_local = 1; v_local = 1; v_local = 1; v_local = 1
    v_local = 1; v_local = 1; v_local = 1; v_local = 1; v_local = 1
    v_local = 1; v_local = 1; v_local = 1; v_local = 1; v_local = 1
    v_local = 1; v_local = 1; v_local = 1; v_local = 1; v_local = 1
    v_local = 1; v_local = 1; v_local = 1; v_local = 1; v_local = 1

def make_nonlocal_writer():
    v_nonlocal = 1
    def inner():
        nonlocal v_nonlocal
        v_nonlocal = 1; v_nonlocal = 1; v_nonlocal = 1; v_nonlocal = 1; v_nonlocal = 1
        v_nonlocal = 1; v_nonlocal = 1; v_nonlocal = 1; v_nonlocal = 1; v_nonlocal = 1
        v_nonlocal = 1; v_nonlocal = 1; v_nonlocal = 1; v_nonlocal = 1; v_nonlocal = 1
        v_nonlocal = 1; v_nonlocal = 1; v_nonlocal = 1; v_nonlocal = 1; v_nonlocal = 1
        v_nonlocal = 1; v_nonlocal = 1; v_nonlocal = 1; v_nonlocal = 1; v_nonlocal = 1
    return inner

write_nonlocal = make_nonlocal_writer()

def write_global():
    global v_global
    v_global = 1; v_global = 1; v_global = 1; v_global = 1; v_global = 1
    v_global = 1; v_global = 1; v_global = 1; v_global = 1; v_global = 1
    v_global = 1; v_global = 1; v_global = 1; v_global = 1; v_global = 1
    v_global = 1; v_global = 1; v_global = 1; v_global = 1; v_global = 1
    v_global = 1; v_global = 1; v_global = 1; v_global = 1; v_global = 1

def write_classvar():
    A = _A
    A.x = 1;    A.x = 1;    A.x = 1;    A.x = 1;    A.x = 1
    A.x = 1;    A.x = 1;    A.x = 1;    A.x = 1;    A.x = 1
    A.x = 1;    A.x = 1;    A.x = 1;    A.x = 1;    A.x = 1
    A.x = 1;    A.x = 1;    A.x = 1;    A.x = 1;    A.x = 1
    A.x = 1;    A.x = 1;    A.x = 1;    A.x = 1;    A.x = 1

def write_instancevar():
    a = _a
    a.x = 1;    a.x = 1;    a.x = 1;    a.x = 1;    a.x = 1
    a.x = 1;    a.x = 1;    a.x = 1;    a.x = 1;    a.x = 1
    a.x = 1;    a.x = 1;    a.x = 1;    a.x = 1;    a.x = 1
    a.x = 1;    a.x = 1;    a.x = 1;    a.x = 1;    a.x = 1
    a.x = 1;    a.x = 1;    a.x = 1;    a.x = 1;    a.x = 1

if __name__=='__main__':
    from fasttimer import timeit
    for f in [read_local, read_nonlocal, read_global, read_builtin,
              read_classvar, read_instancevar, read_unboundmethod, read_boundmethod,
              write_local, write_nonlocal, write_global,
              write_classvar, write_instancevar]:
        print(timeit(f,number=1000000), f.__qualname__ if hasattr(f,'__qualname__') else f.__name__ if hasattr(f,'__name__') else '?')

