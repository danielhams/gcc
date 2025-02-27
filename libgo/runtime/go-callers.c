/* go-callers.c -- get callers for Go.

   Copyright 2012 The Go Authors. All rights reserved.
   Use of this source code is governed by a BSD-style
   license that can be found in the LICENSE file.  */

#include "config.h"

#include "backtrace.h"

#include "runtime.h"
#include "array.h"

/* This is set to non-zero when calling backtrace_full.  This is used
   to avoid getting hanging on a recursive lock in dl_iterate_phdr on
   older versions of glibc when a SIGPROF signal arrives while
   collecting a backtrace.  */

uint32 __go_runtime_in_callers;

/* Argument passed to callback function.  */

struct callers_data
{
  Location *locbuf;
  int skip;
  int index;
  int max;
  int keep_thunks;
};

/* Callback function for backtrace_full.  Just collect the locations.
   Return zero to continue, non-zero to stop.  */

static int
callback (void *data, uintptr_t pc, const char *filename, int lineno,
	  const char *function)
{
  struct callers_data *arg = (struct callers_data *) data;
  Location *loc;

  /* Skip split stack functions.  */
  if (function != NULL)
    {
      const char *p;

      p = function;
      if (__builtin_strncmp (p, "___", 3) == 0)
	++p;
      if (__builtin_strncmp (p, "__morestack_", 12) == 0)
	return 0;
    }
  else if (filename != NULL)
    {
      const char *p;

      p = strrchr (filename, '/');
      if (p == NULL)
	p = filename;
      if (__builtin_strncmp (p, "/morestack.S", 12) == 0)
	return 0;
    }

  /* Skip thunks and recover functions.  There is no equivalent to
     these functions in the gc toolchain, so returning them here means
     significantly different results for runtime.Caller(N).  */
  if (function != NULL && !arg->keep_thunks)
    {
      const char *p;

      p = function + __builtin_strlen (function);
      while (p > function && p[-1] >= '0' && p[-1] <= '9')
	--p;
      if (p - function > 7 && __builtin_strncmp (p - 7, "..thunk", 7) == 0)
	return 0;
      if (p - function > 3 && __builtin_strcmp (p - 3, "..r") == 0)
	return 0;
      if (p - function > 6 && __builtin_strcmp (p - 6, "..stub") == 0)
	return 0;
    }

  if (arg->skip > 0)
    {
      --arg->skip;
      return 0;
    }

  loc = &arg->locbuf[arg->index];

  /* On the call to backtrace_full the pc value was most likely
     decremented if there was a normal call, since the pc referred to
     the instruction where the call returned and not the call itself.
     This was done so that the line number referred to the call
     instruction.  To make sure the actual pc from the call stack is
     used, it is incremented here.

     In the case of a signal, the pc was not decremented by
     backtrace_full but still incremented here.  That doesn't really
     hurt anything since the line number is right and the pc refers to
     the same instruction.  */

  loc->pc = pc + 1;

  /* The libbacktrace library says that these strings might disappear,
     but with the current implementation they won't.  We can't easily
     allocate memory here, so for now assume that we can save a
     pointer to the strings.  */
  loc->filename = runtime_gostringnocopy ((const byte *) filename);
  loc->function = runtime_gostringnocopy ((const byte *) function);

  loc->lineno = lineno;
  ++arg->index;

  /* There is no point to tracing past certain runtime functions.
     Stopping the backtrace here can avoid problems on systems that
     don't provide proper unwind information for makecontext, such as
     Solaris (http://gcc.gnu.org/PR52583 comment #21).  */
  if (function != NULL)
    {
      if (__builtin_strcmp (function, "makecontext") == 0)
	return 1;
      if (filename != NULL)
	{
	  const char *p;

	  p = strrchr (filename, '/');
	  if (p == NULL)
	    p = filename;
	  if (__builtin_strcmp (p, "/proc.c") == 0)
	    {
	      if (__builtin_strcmp (function, "runtime_mstart") == 0)
		return 1;
	    }
	  else if (__builtin_strcmp (p, "/proc.go") == 0)
	    {
	      if (__builtin_strcmp (function, "runtime.kickoff") == 0
		  || __builtin_strcmp (function, "runtime.main") == 0)
		return 1;
	    }
	}
    }

  return arg->index >= arg->max;
}

/* Syminfo callback.  */

void
__go_syminfo_fnname_callback (void *data,
			      uintptr_t pc __attribute__ ((unused)),
			      const char *symname,
			      uintptr_t address __attribute__ ((unused)),
			      uintptr_t size __attribute__ ((unused)))
{
  String* strptr = (String*) data;

  if (symname != NULL)
    *strptr = runtime_gostringnocopy ((const byte *) symname);
}

/* Error callback.  */

static void
error_callback (void *data __attribute__ ((unused)),
		const char *msg, int errnum)
{
  if (errnum == -1)
    {
      /* No debug info available.  Carry on as best we can.  */
      return;
    }
  if (errnum != 0)
    runtime_printf ("%s errno %d\n", msg, errnum);
  runtime_throw (msg);
}

/* Return whether we are already collecting a stack trace. This is
   called from the signal handler.  */

bool alreadyInCallers(void)
  __attribute__ ((no_split_stack));
bool alreadyInCallers(void)
  __asm__ (GOSYM_PREFIX "runtime.alreadyInCallers");

bool
alreadyInCallers()
{
  return runtime_atomicload(&__go_runtime_in_callers) > 0;
}

/* Gather caller PC's.  */

int32
runtime_callers (int32 skip, Location *locbuf, int32 m, bool keep_thunks)
{
  struct callers_data data;
  struct backtrace_state* state;
  int32 i;

  data.locbuf = locbuf;
  data.skip = skip + 1;
  data.index = 0;
  data.max = m;
  data.keep_thunks = keep_thunks;
  state = __go_get_backtrace_state ();
  runtime_xadd (&__go_runtime_in_callers, 1);
  backtrace_full (state, 0, callback, error_callback, &data);
  runtime_xadd (&__go_runtime_in_callers, -1);

  /* For some reason GCC sometimes loses the name of a thunk function
     at the top of the stack.  If we are skipping thunks, skip that
     one too.  */
  if (!keep_thunks
      && data.index > 2
      && locbuf[data.index - 2].function.len == 0
      && locbuf[data.index - 1].function.str != NULL
      && __builtin_strcmp ((const char *) locbuf[data.index - 1].function.str,
			   "runtime.kickoff") == 0)
    {
      locbuf[data.index - 2] = locbuf[data.index - 1];
      --data.index;
    }

  /* Try to use backtrace_syminfo to fill in any missing function
     names.  This can happen when tracing through an object which has
     no debug info; backtrace_syminfo will look at the symbol table to
     get the name.  This should only happen when tracing through code
     that is not written in Go and is not part of libgo.  */
  for (i = 0; i < data.index; ++i)
    {
      if (locbuf[i].function.len == 0 && locbuf[i].pc != 0)
	backtrace_syminfo (state, locbuf[i].pc, __go_syminfo_fnname_callback,
			   error_callback, &locbuf[i].function);
    }

  return data.index;
}

int Callers (int, struct __go_open_array)
  __asm__ (GOSYM_PREFIX "runtime.Callers");

int
Callers (int skip, struct __go_open_array pc)
{
  Location *locbuf;
  int ret;
  int i;

  /* Note that calling mallocgc here assumes that we are not going to
     store any allocated Go pointers in the slice.  */
  locbuf = (Location *) runtime_mallocgc (pc.__count * sizeof (Location),
					  nil, false);

  /* In the Go 1 release runtime.Callers has an off-by-one error,
     which we can not correct because it would break backward
     compatibility.  Normally we would add 1 to SKIP here, but we
     don't so that we are compatible.  */
  ret = runtime_callers (skip, locbuf, pc.__count, false);

  for (i = 0; i < ret; i++)
    ((uintptr *) pc.__values)[i] = locbuf[i].pc;

  return ret;
}
