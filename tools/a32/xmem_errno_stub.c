/* SPDX-License-Identifier: GPL-2.0-or-later
   Guest-errno shim for the AArch32 differential harness.

   build.sh step0/diff link husk-tl-xmem.c as a two-file build (clang plus the
   two include dirs, no Apple frameworks). The truthfulness fix in
   tl_xmem_allow_exec() reports a refusal through the layer's guest-errno
   helper, which is defined in the bionic file set the harness does not link,
   so the link fails with "Undefined symbols: _tl_set_guest_errno".

   Defining it here keeps the harness a two-file build. It also RECORDS the
   value instead of discarding it, so a case can assert that the refused path
   really asked for EACCES (13) -- asserting only the false return would let a
   future edit forget the errno and still pass.

   Declarations: husk-tl-bionic.h (tl_errno_to_guest, tl_set_guest_errno). The
   real definitions in the app override nothing: this file is never linked into
   the app, and app builds never see it. */
#include "husk-tl-internal.h"

int a32_stub_last_guest_errno = -1;

void tl_set_guest_errno(int guest_errno) { a32_stub_last_guest_errno = guest_errno; }

int tl_errno_to_guest(int darwin_errno) { return darwin_errno; }
