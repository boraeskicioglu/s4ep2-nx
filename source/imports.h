/* imports.h -- .so import resolution for libchrono.so / libc++_shared.so
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include <stdio.h>
#include <stdlib.h>
#include "so_util.h"

extern FILE *stderr_fake;
extern DynLibFunction dynlib_functions[];
extern size_t dynlib_numfunctions;

// relocate + resolve a single module against the shim table (and the other
// already-loaded modules, for libfox <-> system dynamic library references).
void fox_resolve_imports(so_module *mod);

#endif
