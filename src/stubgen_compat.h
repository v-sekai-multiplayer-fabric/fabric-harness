/* What the generated dispatch table needs that weft does not otherwise provide.
 *
 * generate_stubs.py is Chromium's, so its output expects Chromium's base library.
 * `--macro-include` points it at this file instead. Three things have to be here.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/* The iceoryx2 types every generated declaration names. */
#include "iox2_decls.h" /* IWYU pragma: keep */

/* generate_stubs.py emits this on every dispatched call, to opt out of Chromium's
 * Control Flow Integrity check on an indirect call. weft does not build with CFI on that
 * path, so it stays empty. */
#define DISABLE_CFI_ICALL

/* MSVC has no __attribute__, and the generator writes __attribute__((weak)) on every
 * forwarding declaration it emits. cl.exe stops at the first one with 'unknown override
 * specifier' and then reports the definition below it as a redefinition, because the
 * declaration it could not parse left the body standing alone -- eighty errors in a file
 * nobody wrote.
 *
 * The attribute is not load-bearing here. Chromium marks the forward declaration weak so a
 * directly linked real symbol can override the stub; this repository never links iceoryx2 at
 * all, which is the entire point of the dispatch table. So on MSVC the attribute is defined
 * away, and the declaration becomes an ordinary extern.
 *
 * `__attribute__` is not a reserved identifier under MSVC, so defining it is legal there and
 * invisible to every other compiler. This is deliberately narrower than a general shim: it
 * removes exactly the one construct MSVC lacks, in the one file the generator writes.
 */
#ifdef _MSC_VER
#define __attribute__(x)
#endif

/* The umbrella initializer needs a reachable namespace name. `-p native/harness` makes
 * the generator write `native_harness`. The guard keeps this header valid in C, because
 * an editor indexes it on its own. */
#ifdef __cplusplus
namespace native_harness {}
#endif
