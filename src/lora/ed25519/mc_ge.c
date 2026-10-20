// MeshCore's bundled ed25519/ge.c, compiled with our symbol prefix in front of
// it. See mc_ed25519_prefix.h for why the rename exists; the upstream file is
// included verbatim and is never edited.
#include "mc_ed25519_prefix.h"

#include "../../../meshcore/lib/ed25519/ge.c"
