#include "Python.h"

PyObject *
PyObject_Init(PyObject *op, PyTypeObject *tp)
{
	LOG("> PyObject_Init\n");
	if (op == NULL)
	  Py_FatalError("out of memory");
	/* Any changes should be reflected in PyObject_INIT (objimpl.h) */
	op->ob_type = tp;
	_Py_NewReference(op);
	LOG("< PyObject_Init\n");
	return op;
}

/* Helper to warn about deprecated tp_compare return values.  Return:
   -2 for an exception;
   -1 if v <  w;
    0 if v == w;
    1 if v  > w.
   (This function cannot return 2.)
*/
static int
adjust_tp_compare(int c)
{
	if (PyErr_Occurred()) {
		if (c != -1 && c != -2) {
			PyObject *t, *v, *tb;
			PyErr_Fetch(&t, &v, &tb);
			if (PyErr_Warn(PyExc_RuntimeWarning,
				       "tp_compare didn't return -1 or -2 "
				       "for exception") < 0) {
				Py_XDECREF(t);
				Py_XDECREF(v);
				Py_XDECREF(tb);
			}
			else
				PyErr_Restore(t, v, tb);
		}
		return -2;
	}
	else if (c < -1 || c > 1) {
		if (PyErr_Warn(PyExc_RuntimeWarning,
			       "tp_compare didn't return -1, 0 or 1") < 0)
			return -2;
		else
			return c < -1 ? -1 : 1;
	}
	else {
		assert(c >= -1 && c <= 1);
		return c;
	}
}

#define RICHCOMPARE(t) (PyType_HasFeature((t), Py_TPFLAGS_HAVE_RICHCOMPARE) \
                         ? (t)->tp_richcompare : NULL)

/* Map rich comparison operators to their swapped version, e.g. LT --> GT */
static int swapped_op[] = {Py_GT, Py_GE, Py_EQ, Py_NE, Py_LT, Py_LE};

/* Try a genuine rich comparison, returning an object.  Return:
   NULL for exception;
   NotImplemented if this particular rich comparison is not implemented or
     undefined;
   some object not equal to NotImplemented if it is implemented
     (this latter object may not be a Boolean).
*/
static PyObject *
try_rich_compare(PyObject *v, PyObject *w, int op)
{
	richcmpfunc f;
	PyObject *res;

	if (v->ob_type != w->ob_type &&
	    PyType_IsSubtype(w->ob_type, v->ob_type) &&
	    (f = RICHCOMPARE(w->ob_type)) != NULL) {
		res = (*f)(w, v, swapped_op[op]);
		if (res != Py_NotImplemented)
			return res;
		Py_DECREF(res);
	}
	if ((f = RICHCOMPARE(v->ob_type)) != NULL) {
		res = (*f)(v, w, op);
		if (res != Py_NotImplemented)
			return res;
		Py_DECREF(res);
	}
	if ((f = RICHCOMPARE(w->ob_type)) != NULL) {
		return (*f)(w, v, swapped_op[op]);
	}
	res = Py_NotImplemented;
	Py_INCREF(res);
	return res;
}

/* Try a genuine rich comparison, returning an int.  Return:
   -1 for exception (including the case where try_rich_compare() returns an
      object that's not a Boolean);
    0 if the outcome is false;
    1 if the outcome is true;
    2 if this particular rich comparison is not implemented or undefined.
*/
static int
try_rich_compare_bool(PyObject *v, PyObject *w, int op)
{
	PyObject *res;
	int ok;

	if (RICHCOMPARE(v->ob_type) == NULL && RICHCOMPARE(w->ob_type) == NULL)
		return 2; /* Shortcut, avoid INCREF+DECREF */
	res = try_rich_compare(v, w, op);
	if (res == NULL)
		return -1;
	if (res == Py_NotImplemented) {
		Py_DECREF(res);
		return 2;
	}
	ok = PyObject_IsTrue(res);
	Py_DECREF(res);
	return ok;
}

/* Try rich comparisons to determine a 3-way comparison.  Return:
   -2 for an exception;
   -1 if v  < w;
    0 if v == w;
    1 if v  > w;
    2 if this particular rich comparison is not implemented or undefined.
*/
static int
try_rich_to_3way_compare(PyObject *v, PyObject *w)
{
	static struct { int op; int outcome; } tries[3] = {
		/* Try this operator, and if it is true, use this outcome: */
		{Py_EQ, 0},
		{Py_LT, -1},
		{Py_GT, 1},
	};
	int i;

	if (RICHCOMPARE(v->ob_type) == NULL && RICHCOMPARE(w->ob_type) == NULL)
		return 2; /* Shortcut */

	for (i = 0; i < 3; i++) {
		switch (try_rich_compare_bool(v, w, tries[i].op)) {
		case -1:
			return -2;
		case 1:
			return tries[i].outcome;
		}
	}

	return 2;
}

/* Try a 3-way comparison, returning an int.  Return:
   -2 for an exception;
   -1 if v <  w;
    0 if v == w;
    1 if v  > w;
    2 if this particular 3-way comparison is not implemented or undefined.
*/
static int
try_3way_compare(PyObject *v, PyObject *w)
{
	int c;
	cmpfunc f;

	/* Comparisons involving instances are given to instance_compare,
	   which has the same return conventions as this function. */

	f = v->ob_type->tp_compare;
	if (PyInstance_Check(v))
		return (*f)(v, w);
	if (PyInstance_Check(w))
		return (*w->ob_type->tp_compare)(v, w);

	/* If both have the same (non-NULL) tp_compare, use it. */
	if (f != NULL && f == w->ob_type->tp_compare) {
		c = (*f)(v, w);
		return adjust_tp_compare(c);
	}

	/* If either tp_compare is _PyObject_SlotCompare, that's safe. */
	if (f == _PyObject_SlotCompare ||
	    w->ob_type->tp_compare == _PyObject_SlotCompare)
		return _PyObject_SlotCompare(v, w);

	/* Try coercion; if it fails, give up */
	c = PyNumber_CoerceEx(&v, &w);
	if (c < 0)
		return -2;
	if (c > 0)
		return 2;

	/* Try v's comparison, if defined */
	if ((f = v->ob_type->tp_compare) != NULL) {
		c = (*f)(v, w);
		Py_DECREF(v);
		Py_DECREF(w);
		return adjust_tp_compare(c);
	}

	/* Try w's comparison, if defined */
	if ((f = w->ob_type->tp_compare) != NULL) {
		c = (*f)(w, v); /* swapped! */
		Py_DECREF(v);
		Py_DECREF(w);
		c = adjust_tp_compare(c);
		if (c >= -1)
			return -c; /* Swapped! */
		else
			return c;
	}

	/* No comparison defined */
	Py_DECREF(v);
	Py_DECREF(w);
	return 2;
}

/* Final fallback 3-way comparison, returning an int.  Return:
   -2 if an error occurred;
   -1 if v <  w;
    0 if v == w;
    1 if v >  w.
*/
static int
default_3way_compare(PyObject *v, PyObject *w)
{
	int c;
	char *vname, *wname;

	if (v->ob_type == w->ob_type) {
		/* When comparing these pointers, they must be cast to
		 * integer types (i.e. Py_uintptr_t, our spelling of C9X's
		 * uintptr_t).  ANSI specifies that pointer compares other
		 * than == and != to non-related structures are undefined.
		 */
		Py_uintptr_t vv = (Py_uintptr_t)v;
		Py_uintptr_t ww = (Py_uintptr_t)w;
		return (vv < ww) ? -1 : (vv > ww) ? 1 : 0;
	}

	/* None is smaller than anything */
	if (v == Py_None)
		return -1;
	if (w == Py_None)
		return 1;

	/* different type: compare type names; numbers are smaller */
	if (PyNumber_Check(v))
		vname = "";
	else
		vname = v->ob_type->tp_name;
	if (PyNumber_Check(w))
		wname = "";
	else
		wname = w->ob_type->tp_name;
	c = strcmp(vname, wname);
	if (c < 0)
		return -1;
	if (c > 0)
		return 1;
	/* Same type name, or (more likely) incomparable numeric types */
	return ((Py_uintptr_t)(v->ob_type) < (
		Py_uintptr_t)(w->ob_type)) ? -1 : 1;
}

/* Do a 3-way comparison, by hook or by crook.  Return:
   -2 for an exception (but see below);
   -1 if v <  w;
    0 if v == w;
    1 if v >  w;
   BUT: if the object implements a tp_compare function, it returns
   whatever this function returns (whether with an exception or not).
*/
static int
do_cmp(PyObject *v, PyObject *w)
{
	int c;
	cmpfunc f;

	if (v->ob_type == w->ob_type
	    && (f = v->ob_type->tp_compare) != NULL) {
		c = (*f)(v, w);
		if (PyInstance_Check(v)) {
			/* Instance tp_compare has a different signature.
			   But if it returns undefined we fall through. */
			if (c != 2)
				return c;
			/* Else fall through to try_rich_to_3way_compare() */
		}
		else
			return adjust_tp_compare(c);
	}
	/* We only get here if one of the following is true:
	   a) v and w have different types
	   b) v and w have the same type, which doesn't have tp_compare
	   c) v and w are instances, and either __cmp__ is not defined or
	      __cmp__ returns NotImplemented
	*/
	c = try_rich_to_3way_compare(v, w);
	if (c < 2)
		return c;
	c = try_3way_compare(v, w);
	if (c < 2)
		return c;
	return default_3way_compare(v, w);
}

#define NESTING_LIMIT 20

PyObject *
_PyObject_New(PyTypeObject *tp)
{
	PyObject *op;
	op = (PyObject *) PyObject_MALLOC(_PyObject_SIZE(tp));
	if (op == NULL)
		return PyErr_NoMemory();
	return PyObject_INIT(op, tp);
}

static int
internal_print(PyObject *op)
{
	return (*op->ob_type->tp_print)(op);
}

int
PyObject_Print(PyObject *op)
{
	return internal_print(op);
}

long
PyObject_Hash(PyObject *v)
{
	PyTypeObject *tp = v->ob_type;
	if (tp->tp_hash != NULL)
		return (*tp->tp_hash)(v);
	/* TO DO */
	return -1;
}

PyObject *
PyObject_GetAttr(PyObject *v, PyObject *name)
{
	PyTypeObject *tp = v->ob_type;

	if (!PyString_Check(name)) {
		/* ERROR */
		printf("attribute name must be string\n");
		return NULL;
	}
	if (tp->tp_getattro != NULL)
		return (*tp->tp_getattro)(v, name);
	if (tp->tp_getattr != NULL)
		return (*tp->tp_getattr)(v, PyString_AS_STRING(name));
	/* ERROR */
	printf("'%s' object has no attribute '%s'", 
	       tp->tp_name, PyString_AS_STRING(name));
	return NULL;
}

int
PyObject_IsTrue(PyObject *v)
{
	int res;
	if (v == Py_True)
		return 1;
	if (v == Py_False)
		return 0;
	if (v == Py_None)
		return 0;
	else if (v->ob_type->tp_as_number != NULL &&
		 v->ob_type->tp_as_number->nb_nonzero != NULL)
		res = (*v->ob_type->tp_as_number->nb_nonzero)(v);
	else if (v->ob_type->tp_as_mapping != NULL &&
		 v->ob_type->tp_as_mapping->mp_length != NULL)
		res = (*v->ob_type->tp_as_mapping->mp_length)(v);
	else if (v->ob_type->tp_as_sequence != NULL &&
		 v->ob_type->tp_as_sequence->sq_length != NULL)
		res = (*v->ob_type->tp_as_sequence->sq_length)(v);
	else
		return 1;
	return (res > 0) ? 1 : res;
}

PyObject *
PyObject_Repr(PyObject *v)
{
	if (v == NULL)
		return PyString_FromString("<NULL>");
	else if (v->ob_type->tp_repr == NULL)
		return PyString_FromFormat("<%s object at %p>",
					   v->ob_type->tp_name, v);
	else {
		PyObject *res;
		res = (*v->ob_type->tp_repr)(v);
		if (res == NULL)
			return NULL;
		if (!PyString_Check(res)) {
			PyErr_Format(PyExc_TypeError,
				     "__repr__ returned non-string (type %.200s)",
				     res->ob_type->tp_name);
			Py_DECREF(res);
			return NULL;
		}
		return res;
	}
}

PyObject *
PyObject_Str(PyObject *v)
{
	PyObject *res;

	if (v == NULL)
		return PyString_FromString("<NULL>");
	if (PyString_CheckExact(v)) {
		Py_INCREF(v);
		return v;
	}
	Py_FatalError("PyObject_Str doesn't support non-strings yet.");
	if (v->ob_type->tp_str == NULL)
		; //return PyObject_Repr(v);

	res = (*v->ob_type->tp_str)(v);
	if (res == NULL)
		return NULL;
	if (!PyString_Check(res)) {
		/* ERROR */
		Py_DECREF(res);
		return NULL;
	}
	return res;
}


/* Coerce two numeric types to the "larger" one.
   Increment the reference count on each argument.
   Return value:
   -1 if an error occurred;
   0 if the coercion succeeded (and then the reference counts are increased);
   1 if no coercion is possible (and no error is raised).
*/
int
PyNumber_CoerceEx(PyObject **pv, PyObject **pw)
{
	register PyObject *v = *pv;
	register PyObject *w = *pw;
	int res;

	/* Shortcut only for old-style types */
	if (v->ob_type == w->ob_type &&
	    !PyType_HasFeature(v->ob_type, Py_TPFLAGS_CHECKTYPES))
	{
		Py_INCREF(v);
		Py_INCREF(w);
		return 0;
	}
	if (v->ob_type->tp_as_number && v->ob_type->tp_as_number->nb_coerce) {
		res = (*v->ob_type->tp_as_number->nb_coerce)(pv, pw);
		if (res <= 0)
			return res;
	}
	if (w->ob_type->tp_as_number && w->ob_type->tp_as_number->nb_coerce) {
		res = (*w->ob_type->tp_as_number->nb_coerce)(pw, pv);
		if (res <= 0)
			return res;
	}
	return 1;
}

/* compare_nesting is incremented before calling compare (for
   some types) and decremented on exit.  If the count exceeds the
   nesting limit, enable code to detect circular data structures.

   This is a tunable parameter that should only affect the performance
   of comparisons, nothing else.  Setting it high makes comparing deeply
   nested non-cyclical data structures faster, but makes comparing cyclical
   data structures slower.
*/
#define NESTING_LIMIT 20

static int compare_nesting = 0;

static PyObject*
get_inprogress_dict(void)
{
	static PyObject *key;
	PyObject *tstate_dict, *inprogress;

	if (key == NULL) {
		key = PyString_InternFromString("cmp_state");
		if (key == NULL)
			return NULL;
	}

	tstate_dict = PyThreadState_GetDict();
	if (tstate_dict == NULL) {
		PyErr_BadInternalCall();
		return NULL;
	}

	inprogress = PyDict_GetItem(tstate_dict, key);
	if (inprogress == NULL) {
		inprogress = PyDict_New();
		if (inprogress == NULL)
			return NULL;
		if (PyDict_SetItem(tstate_dict, key, inprogress) == -1) {
		    Py_DECREF(inprogress);
		    return NULL;
		}
		Py_DECREF(inprogress);
	}

	return inprogress;
}

/* We want a rich comparison but don't have one.  Try a 3-way cmp instead.
   Return
   NULL      if error
   Py_True   if v op w
   Py_False  if not (v op w)
*/
static PyObject *
try_3way_to_rich_compare(PyObject *v, PyObject *w, int op)
{
	int c;

	c = try_3way_compare(v, w);
	if (c >= 2)
		c = default_3way_compare(v, w);
	if (c <= -2)
		return NULL;
	return convert_3way_to_object(op, c);
}

/* Do rich comparison on v and w.  Return
   NULL      if error
   Else a new reference to an object other than Py_NotImplemented, usually(?):
   Py_True   if v op w
   Py_False  if not (v op w)
*/
static PyObject *
do_richcmp(PyObject *v, PyObject *w, int op)
{
	PyObject *res;

	res = try_rich_compare(v, w, op);
	if (res != Py_NotImplemented)
		return res;
	Py_DECREF(res);

	return try_3way_to_rich_compare(v, w, op);
}

/* Return:
   NULL for exception;
   some object not equal to NotImplemented if it is implemented
     (this latter object may not be a Boolean).
*/
PyObject *
PyObject_RichCompare(PyObject *v, PyObject *w, int op)
{
	PyObject *res;

	assert(Py_LT <= op && op <= Py_GE);

	compare_nesting++;
	if (compare_nesting > NESTING_LIMIT &&
	    (v->ob_type->tp_as_mapping || v->ob_type->tp_as_sequence) &&
	    !PyString_CheckExact(v) &&
	    !PyTuple_CheckExact(v)) {
		/* try to detect circular data structures */
		PyObject *token = check_recursion(v, w, op);
		if (token == NULL) {
			res = NULL;
			goto Done;
		}
		else if (token == Py_None) {
			/* already comparing these objects with this operator.
			   assume they're equal until shown otherwise */
			if (op == Py_EQ)
				res = Py_True;
			else if (op == Py_NE)
				res = Py_False;
			else {
				PyErr_SetString(PyExc_ValueError,
					"can't order recursive values");
				res = NULL;
			}
			Py_XINCREF(res);
		}
		else {
			res = do_richcmp(v, w, op);
			delete_token(token);
		}
		goto Done;
	}

	/* No nesting extremism.
	   If the types are equal, and not old-style instances, try to
	   get out cheap (don't bother with coercions etc.). */
	if (v->ob_type == w->ob_type && !PyInstance_Check(v)) {
		cmpfunc fcmp;
		richcmpfunc frich = RICHCOMPARE(v->ob_type);
		/* If the type has richcmp, try it first.  try_rich_compare
		   tries it two-sided, which is not needed since we've a
		   single type only. */
		if (frich != NULL) {
			res = (*frich)(v, w, op);
			if (res != Py_NotImplemented)
				goto Done;
			Py_DECREF(res);
		}
		/* No richcmp, or this particular richmp not implemented.
		   Try 3-way cmp. */
		fcmp = v->ob_type->tp_compare;
		if (fcmp != NULL) {
			int c = (*fcmp)(v, w);
			c = adjust_tp_compare(c);
			if (c == -2) {
				res = NULL;
				goto Done;
			}
			res = convert_3way_to_object(op, c);
			goto Done;
		}
	}

	/* Fast path not taken, or couldn't deliver a useful result. */
	res = do_richcmp(v, w, op);
Done:
	compare_nesting--;
	return res;
}

/* Return -1 if error; 1 if v op w; 0 if not (v op w). */
int
PyObject_RichCompareBool(PyObject *v, PyObject *w, int op)
{
	PyObject *res = PyObject_RichCompare(v, w, op);
	int ok;

	if (res == NULL)
		return -1;
	if (PyBool_Check(res))
		ok = (res == Py_True);
	else
		ok = PyObject_IsTrue(res);
	Py_DECREF(res);
	return ok;
}

/* If the comparison "v op w" is already in progress in this thread, returns
 * a borrowed reference to Py_None (the caller must not decref).
 * If it's not already in progress, returns "a token" which must eventually
 * be passed to delete_token().  The caller must not decref this either
 * (delete_token decrefs it).  The token must not survive beyond any point
 * where v or w may die.
 * If an error occurs (out-of-memory), returns NULL.
 */
static PyObject *
check_recursion(PyObject *v, PyObject *w, int op)
{
	PyObject *inprogress;
	PyObject *token;
	Py_uintptr_t iv = (Py_uintptr_t)v;
	Py_uintptr_t iw = (Py_uintptr_t)w;
	PyObject *x, *y, *z;

	inprogress = get_inprogress_dict();
	if (inprogress == NULL)
		return NULL;

	token = PyTuple_New(3);
	if (token == NULL)
		return NULL;

	if (iv <= iw) {
		PyTuple_SET_ITEM(token, 0, x = PyLong_FromVoidPtr((void *)v));
		PyTuple_SET_ITEM(token, 1, y = PyLong_FromVoidPtr((void *)w));
		if (op >= 0)
			op = swapped_op[op];
	} else {
		PyTuple_SET_ITEM(token, 0, x = PyLong_FromVoidPtr((void *)w));
		PyTuple_SET_ITEM(token, 1, y = PyLong_FromVoidPtr((void *)v));
	}
	PyTuple_SET_ITEM(token, 2, z = PyInt_FromLong((long)op));
	if (x == NULL || y == NULL || z == NULL) {
		Py_DECREF(token);
		return NULL;
	}

	if (PyDict_GetItem(inprogress, token) != NULL) {
		Py_DECREF(token);
		return Py_None; /* Without INCREF! */
	}

	if (PyDict_SetItem(inprogress, token, token) < 0) {
		Py_DECREF(token);
		return NULL;
	}

	return token;
}

static void
delete_token(PyObject *token)
{
	PyObject *inprogress;

	if (token == NULL || token == Py_None)
		return;
	inprogress = get_inprogress_dict();
	if (inprogress == NULL)
		PyErr_Clear();
	else
		PyDict_DelItem(inprogress, token);
	Py_DECREF(token);
}

/* Compare v to w.  Return
   -1 if v <  w or exception (PyErr_Occurred() true in latter case).
    0 if v == w.
    1 if v > w.
   XXX The docs (C API manual) say the return value is undefined in case
   XXX of error.
*/
int
PyObject_Compare(PyObject *v, PyObject *w)
{
	PyTypeObject *vtp;
	int result;

#if defined(USE_STACKCHECK)
	if (PyOS_CheckStack()) {
		PyErr_SetString(PyExc_MemoryError, "Stack overflow");
		return -1;
	}
#endif
	if (v == NULL || w == NULL) {
		PyErr_BadInternalCall();
		return -1;
	}
	if (v == w)
		return 0;
	vtp = v->ob_type;
	compare_nesting++;
	if (compare_nesting > NESTING_LIMIT &&
	    (vtp->tp_as_mapping || vtp->tp_as_sequence) &&
	    !PyString_CheckExact(v) &&
	    !PyTuple_CheckExact(v)) {
		/* try to detect circular data structures */
		PyObject *token = check_recursion(v, w, -1);

		if (token == NULL) {
			result = -1;
		}
		else if (token == Py_None) {
			/* already comparing these objects.  assume
			   they're equal until shown otherwise */
                        result = 0;
		}
		else {
			result = do_cmp(v, w);
			delete_token(token);
		}
	}
	else {
		result = do_cmp(v, w);
	}
	compare_nesting--;
	return result < 0 ? -1 : result;
}

/* Return (new reference to) Py_True or Py_False. */
static PyObject *
convert_3way_to_object(int op, int c)
{
	PyObject *result;
	switch (op) {
	case Py_LT: c = c <  0; break;
	case Py_LE: c = c <= 0; break;
	case Py_EQ: c = c == 0; break;
	case Py_NE: c = c != 0; break;
	case Py_GT: c = c >  0; break;
	case Py_GE: c = c >= 0; break;
	}
	result = c ? Py_True : Py_False;
	Py_INCREF(result);
	return result;
}

PyObject *
PyObject_GetAttrString(PyObject *v, char *name)
{
	PyObject *w, *res;

	if (v->ob_type->tp_getattr != NULL)
		return (*v->ob_type->tp_getattr)(v, name);
	w = PyString_InternFromString(name);
	if (w == NULL)
		return NULL;
	res = PyObject_GetAttr(v, w);
	Py_XDECREF(w);
	return res;
}

int
PyObject_HasAttrString(PyObject *v, char *name)
{
	PyObject *res = PyObject_GetAttrString(v, name);
	if (res != NULL) {
		Py_DECREF(res);
		return 1;
	}
	PyErr_Clear();
	return 0;
}

int
PyObject_SetAttrString(PyObject *v, char *name, PyObject *w)
{
	PyObject *s;
	int res;

	if (v->ob_type->tp_setattr != NULL)
		return (*v->ob_type->tp_setattr)(v, name, w);
	s = PyString_InternFromString(name);
	if (s == NULL)
		return -1;
	res = PyObject_SetAttr(v, s, w);
	Py_XDECREF(s);
	return res;
}

int
PyObject_SetAttr(PyObject *v, PyObject *name, PyObject *value)
{
	PyTypeObject *tp = v->ob_type;
	int err;

	if (!PyString_Check(name)){
		{
			PyErr_SetString(PyExc_TypeError,
					"attribute name must be string");
			return -1;
		}
	}
	else
		Py_INCREF(name);

	PyString_InternInPlace(&name);
	if (tp->tp_setattro != NULL) {
		err = (*tp->tp_setattro)(v, name, value);
		Py_DECREF(name);
		return err;
	}
	if (tp->tp_setattr != NULL) {
		err = (*tp->tp_setattr)(v, PyString_AS_STRING(name), value);
		Py_DECREF(name);
		return err;
	}
	Py_DECREF(name);
	if (tp->tp_getattr == NULL && tp->tp_getattro == NULL)
		PyErr_Format(PyExc_TypeError,
			     "'%.100s' object has no attributes "
			     "(%s .%.100s)",
			     tp->tp_name,
			     value==NULL ? "del" : "assign to",
			     PyString_AS_STRING(name));
	else
		PyErr_Format(PyExc_TypeError,
			     "'%.100s' object has only read-only attributes "
			     "(%s .%.100s)",
			     tp->tp_name,
			     value==NULL ? "del" : "assign to",
			     PyString_AS_STRING(name));
	return -1;
}

/* Generic GetAttr functions - put these in your tp_[gs]etattro slot */

PyObject *
PyObject_SelfIter(PyObject *obj)
{
	Py_INCREF(obj);
	return obj;
}

PyObject *
PyObject_GenericGetAttr(PyObject *obj, PyObject *name)
{
	PyTypeObject *tp = obj->ob_type;
	PyObject *descr = NULL;
	PyObject *res = NULL;
	descrgetfunc f;
	long dictoffset;
	PyObject **dictptr;

	if (!PyString_Check(name)){
		printf("attribute name must be string");
		return NULL;
	}
	else
		Py_INCREF(name);

	if (tp->tp_dict == NULL) {
		if (PyType_Ready(tp) < 0)
			goto done;
	}

	/* Inline _PyType_Lookup */
	{
		int i, n;
		PyObject *mro, *base, *dict;

		/* Look in tp_dict of types in MRO */
		mro = tp->tp_mro;
		n = PyTuple_GET_SIZE(mro);
		for (i = 0; i < n; i++) {
			base = PyTuple_GET_ITEM(mro, i);
//			if (PyClass_Check(base))
//				dict = ((PyClassObject *)base)->cl_dict;
//			else {
				dict = ((PyTypeObject *)base)->tp_dict;
//			}
			descr = PyDict_GetItem(dict, name);
			if (descr != NULL)
				break;
		}
	}

	f = NULL;
	if (descr != NULL &&
	    PyType_HasFeature(descr->ob_type, Py_TPFLAGS_HAVE_CLASS)) {
		f = descr->ob_type->tp_descr_get;
		if (f != NULL && PyDescr_IsData(descr)) {
			res = f(descr, obj, (PyObject *)obj->ob_type);
			goto done;
		}
	}

	/* Inline _PyObject_GetDictPtr */
	dictoffset = tp->tp_dictoffset;
	if (dictoffset != 0) {
		PyObject *dict;
		if (dictoffset < 0) {
			int tsize;
			size_t size;

			tsize = ((PyVarObject *)obj)->ob_size;
			if (tsize < 0)
				tsize = -tsize;
			size = _PyObject_VAR_SIZE(tp, tsize);

			dictoffset += (long)size;
		}
		dictptr = (PyObject **) ((char *)obj + dictoffset);
		dict = *dictptr;
		if (dict != NULL) {
			res = PyDict_GetItem(dict, name);
			if (res != NULL) {
				Py_INCREF(res);
				goto done;
			}
		}
	}

	if (f != NULL) {
		res = f(descr, obj, (PyObject *)obj->ob_type);
		goto done;
	}

	if (descr != NULL) {
		Py_INCREF(descr);
		res = descr;
		goto done;
	}
	printf("'%s' object has no attribute '%s'",
	       tp->tp_name, PyString_AS_STRING(name));
  done:
	Py_DECREF(name);
	return res;
}

/* Test whether an object can be called */

int
PyCallable_Check(PyObject *x)
{
	if (x == NULL)
		return 0;
	if (PyInstance_Check(x)) {
		PyObject *call = PyObject_GetAttrString(x, "__call__");
		if (call == NULL) {
			PyErr_Clear();
			return 0;
		}
		/* Could test recursively but don't, for fear of endless
		   recursion if some joker sets self.__call__ = self */
		Py_DECREF(call);
		return 1;
	}
	else {
		return x->ob_type->tp_call != NULL;
	}
}

/* Python's malloc wrappers (see pymem.h) */

void *
PyMem_Malloc(size_t nbytes)
{
	return PyMem_MALLOC(nbytes);
}

void *
PyMem_Realloc(void *p, size_t nbytes)
{
	return PyMem_REALLOC(p, nbytes);
}

void
PyMem_Free(void *p)
{
	PyMem_FREE(p);
}


/* These methods are used to control infinite recursion in repr, str, print,
   etc.  Container objects that may recursively contain themselves,
   e.g. builtin dictionaries and lists, should used Py_ReprEnter() and
   Py_ReprLeave() to avoid infinite recursion.

   Py_ReprEnter() returns 0 the first time it is called for a particular
   object and 1 every time thereafter.  It returns -1 if an exception
   occurred.  Py_ReprLeave() has no return value.

   See dictobject.c and listobject.c for examples of use.
*/

#define KEY "Py_Repr"

int
Py_ReprEnter(PyObject *obj)
{
	PyObject *dict;
	PyObject *list;
	int i;

	dict = PyThreadState_GetDict();
	if (dict == NULL)
		return 0;
	list = PyDict_GetItemString(dict, KEY);
	if (list == NULL) {
		list = PyList_New(0);
		if (list == NULL)
			return -1;
		if (PyDict_SetItemString(dict, KEY, list) < 0)
			return -1;
		Py_DECREF(list);
	}
	i = PyList_GET_SIZE(list);
	while (--i >= 0) {
		if (PyList_GET_ITEM(list, i) == obj)
			return 1;
	}
	PyList_Append(list, obj);
	return 0;
}

void
Py_ReprLeave(PyObject *obj)
{
	PyObject *dict;
	PyObject *list;
	int i;

	dict = PyThreadState_GetDict();
	if (dict == NULL)
		return;
	list = PyDict_GetItemString(dict, KEY);
	if (list == NULL || !PyList_Check(list))
		return;
	i = PyList_GET_SIZE(list);
	/* Count backwards because we always expect obj to be list[-1] */
	while (--i >= 0) {
		if (PyList_GET_ITEM(list, i) == obj) {
			PyList_SetSlice(list, i, i + 1, NULL);
			break;
		}
	}
}



/* ... */

static PyTypeObject PyNone_Type = {
  PyObject_HEAD_INIT(&PyType_Type)
  0,
  "NoneType",
  0,
  0,
  0, //(destructor)none_dealloc,	     /*tp_dealloc*/ /*never called*/
  0,		/*tp_print*/
  0,		/*tp_getattr*/
  0,		/*tp_setattr*/
  0,		/*tp_compare*/
  0, //(reprfunc)none_repr, /*tp_repr*/
  0,		/*tp_as_number*/
  0,		/*tp_as_sequence*/
  0,		/*tp_as_mapping*/
  0,		/*tp_hash */
};

PyObject _Py_NoneStruct = {
  PyObject_HEAD_INIT(&PyNone_Type)
};

static PyTypeObject PyNotImplemented_Type = {
  PyObject_HEAD_INIT(&PyType_Type)
  0,
  "NotImplementedType",
  0,
  0,
  0, //(destructor)none_dealloc,	     /*tp_dealloc*/ /*never called*/
  0,		/*tp_print*/
  0,		/*tp_getattr*/
  0,		/*tp_setattr*/
  0,		/*tp_compare*/
  0, //(reprfunc)NotImplemented_repr, /*tp_repr*/
  0,		/*tp_as_number*/
  0,		/*tp_as_sequence*/
  0,		/*tp_as_mapping*/
  0,		/*tp_hash */
};

PyObject _Py_NotImplementedStruct = {
	PyObject_HEAD_INIT(&PyNotImplemented_Type)
};

void
_Py_ReadyTypes(void)
{
	if (PyType_Ready(&PyType_Type) < 0)
		Py_FatalError("Can't initialize 'type'");

	if (PyType_Ready(&PyString_Type) < 0)
		Py_FatalError("Can't initialize 'str'");

	if (PyType_Ready(&PyNone_Type) < 0)
		Py_FatalError("Can't initialize type(None)");

	if (PyType_Ready(&PyNotImplemented_Type) < 0)
		Py_FatalError("Can't initialize type(NotImplemented)");
}
