#include "Python.h"

static int initialized = 0;

void
Py_Initialize(void)
{
	printf("> Py_Initialize\n");

	PyInterpreterState *interp;
	PyThreadState *tstate;

	if (initialized)
		return;
	initialized = 1;

	interp = PyInterpreterState_New();
	if (interp == NULL)
		Py_FatalError("Py_Initialize: can't make first interpreter");

	tstate = PyThreadState_New(interp);
	if (tstate == NULL)
		Py_FatalError("Py_Initialize: can't make first thread");

	(void) PyThreadState_Swap(tstate);

	/* @@@ ready types? */

	if (!_PyFrame_Init())
		Py_FatalError("Py_Initialize: can't init frames");

	if (!_PyInt_Init())
		Py_FatalError("Py_Initialize: can't init ints");

	interp->modules = PyDict_New();
	if (interp->modules == NULL)
	  Py_FatalError("Py_Initialize: can't make modules dictionary");

	printf("< Py_Initialize\n");
}

void
Py_Finalize(void)
{
	printf("> Py_Finalize\n");
	PyInterpreterState *interp;
	PyThreadState *tstate;

	initialized = 0;

	tstate = PyThreadState_Get();
	interp = tstate->interp;

	PyInterpreterState_Clear(interp);

	PyThreadState_Swap(NULL);
	PyInterpreterState_Delete(interp);

	PyFrame_Fini();
	PyInt_Fini();
	printf("< Py_Finalize\n");
}

void
Py_FatalError(const char *msg)
{
	printf("FATAL ERROR: ");
	printf(msg);
	printf("\n");
	while (1) {}
}
