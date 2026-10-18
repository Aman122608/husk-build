/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stores into code, done through the JIT region's writable view (husk-tl-codewrite.c). */
#ifndef HUSK_TL_CODEWRITE_H
#define HUSK_TL_CODEWRITE_H

#include <stdbool.h>

/* From now on, stores into the executable view are carried out through the writable one, and the guest may map executable
 * memory of its own (it gets a piece of the JIT region). For mod loaders that hook the game (Geode). */
void tl_codewrite_enable(void);
bool tl_codewrite_enabled(void);
/* Stores carried out so far, for the log. */
long tl_codewrite_count(void);
/* Stores the guard REFUSED (a form its decoder does not understand, chained to the previous handler). Counted so the
 * trap rate can be summarised without a line per store -- the carry line is throttled to the first few. */
long tl_codewrite_refused_count(void);
/* Of the stores CARRIED, how many faulted on an address that falls in a PT_GNU_RELRO page (tl_ld_va_relro). tl_page_plan
 * deliberately keeps RELRO out of TL_PAGE_W, so the loader names those pages through the executable view and a store
 * there reaches the guard even though the bytes are really data. This counter is the MEASUREMENT that decides whether
 * naming RELRO through the writable view is worth the change -- bounded (one relaxed increment per carried store), and
 * never a log line per store. */
long tl_codewrite_relro_count(void);

#endif
