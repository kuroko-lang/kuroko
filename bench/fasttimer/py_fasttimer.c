#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <time.h>
#include <sys/time.h>

static PyObject *
fasttimer_timeit(PyObject * self, PyObject * args, PyObject* kwargs)
{
	PyObject * callable;
	int times = 1000000;

	static char * keywords[] = {"callable","number",NULL};

	if (!PyArg_ParseTupleAndKeywords(args,kwargs,"O|i:timeit",
		keywords, &callable, &times)) {
		PyErr_SetString(PyExc_TypeError, "bad arguments?");
		return NULL;
	}

	if (!PyCallable_Check(callable)) {
		PyErr_SetString(PyExc_TypeError, "expected callable");
		return NULL;
	}


	Py_XINCREF(callable);
	struct timeval tv_before, tv_after;
	gettimeofday(&tv_before,NULL);
	for (int t = 0; t < times; ++t) {
		/* Call it here */
		PyObject_CallObject(callable, NULL);
	}
	gettimeofday(&tv_after,NULL);
	Py_XDECREF(callable);

	double before = (double)tv_before.tv_sec + (double)tv_before.tv_usec / 1000000.0;
	double after = (double)tv_after.tv_sec + (double)tv_after.tv_usec / 1000000.0;

	return PyFloat_FromDouble(after-before);
}

static PyMethodDef FasttimerMethods[] = {
	{"timeit", (PyCFunction)(void(*)(void))fasttimer_timeit, METH_VARARGS | METH_KEYWORDS, "Call a callable in a tight loop."},
	{NULL,NULL,0,NULL},
};

static struct PyModuleDef fasttimermodule = {
	PyModuleDef_HEAD_INIT,
	"fasttimer",
	NULL,
	-1,
	FasttimerMethods
};

PyMODINIT_FUNC
PyInit__fasttimer(void)
{
	return PyModule_Create(&fasttimermodule);
}
