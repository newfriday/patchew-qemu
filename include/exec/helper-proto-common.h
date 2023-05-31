/*
 * Helper file for declaring TCG helper functions.
 * This one expands prototypes for the helper functions.
 */

#ifndef HELPER_PROTO_COMMON_H
#define HELPER_PROTO_COMMON_H

#define HELPER_H "accel/tcg/tcg-runtime.h"
#include "exec/helper-proto.h.inc"
#undef  HELPER_H

#define HELPER_H "accel/tcg/plugin-helpers.h"
#include "exec/helper-proto.h.inc"
#undef  HELPER_H

#endif /* HELPER_PROTO_COMMON_H */
