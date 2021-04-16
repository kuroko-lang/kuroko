from distutils.core import setup, Extension

fasttimer = Extension('_fasttimer',sources=['py_fasttimer.c'])
setup(name="_fasttimer",version='1.0',description='fast timer',ext_modules=[fasttimer])
