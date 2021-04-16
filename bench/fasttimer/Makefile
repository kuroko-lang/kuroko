all: _fasttimer.so _mpytimer.so

_fasttimer.so: py_fasttimer.c
	python3 setup.py build
	cp build/*/*.so ./$@

_mpytimer.so: mpy_timer.c
	$(CC) -shared -o $@ $<
