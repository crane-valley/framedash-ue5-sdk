// Copyright 2026 Crane Valley. All Rights Reserved.

// UBT only compiles sources beneath a module directory. Fab requires vendored
// dependencies at plugin-level Source/ThirdParty, so this module-owned bridge
// preserves that reviewable layout while keeping nanopb's C linkage intact.
extern "C"
{
#include "../../ThirdParty/nanopb/pb_common.c"
#include "../../ThirdParty/nanopb/pb_decode.c"
#include "../../ThirdParty/nanopb/pb_encode.c"
}
