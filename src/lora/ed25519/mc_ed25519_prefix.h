#pragma once

// Renames MeshCore's bundled Curve25519 field arithmetic out of the way of
// wolfSSL's.
//
// **The collision is real and it is not MeshCore's fault.** Both libraries
// carry a descendant of the reference `ref10` Ed25519 implementation, and both
// kept its original names, so `fe_0`, `fe_mul`, `fe_tobytes` and fifteen more
// are defined twice in one image. This firmware links `Arduino-wolfSSL` for
// TLS (SecureNet, WebSockets) and MeshCore's ed25519 for advert signatures, so
// both are present and the linker refuses the image:
//
//   multiple definition of `fe_0';
//   Arduino-wolfSSL/wolfcrypt/src/fe_operations.c.o: first defined here
//
// Eighteen symbols collide, every one of them `fe_*`; `ge_*`, `sc_*`,
// `sha512*` and the public `ed25519_*` entry points are MeshCore's alone
// (measured with nm against both archives, 2026-10-20).
//
// **Why not --allow-multiple-definition.** It links, and that is the problem:
// the linker would keep whichever definition it met first and bind MeshCore's
// `ge.c` to wolfSSL's field arithmetic. Two ref10 descendants are not
// guaranteed to share an internal representation, and a signature scheme that
// is subtly wrong does not fail loudly -- it produces adverts nobody can
// verify, or verifies ones it should not. A build error is the correct
// outcome here and it is answered rather than suppressed.
//
// **Why not patch the submodule.** `meshcore` is pinned upstream and reviewed
// per batch, never edited in place (CLAUDE.md, "Upstream is two tracks"). This
// header plus the thin wrappers next to it keep the whole workaround on our
// side of the line, so a version bump cannot silently drop it.

#define fe_0 mc_fe_0
#define fe_1 mc_fe_1
#define fe_add mc_fe_add
#define fe_cmov mc_fe_cmov
#define fe_copy mc_fe_copy
#define fe_cswap mc_fe_cswap
#define fe_frombytes mc_fe_frombytes
#define fe_invert mc_fe_invert
#define fe_isnegative mc_fe_isnegative
#define fe_isnonzero mc_fe_isnonzero
#define fe_mul mc_fe_mul
#define fe_mul121666 mc_fe_mul121666
#define fe_neg mc_fe_neg
#define fe_pow22523 mc_fe_pow22523
#define fe_sq mc_fe_sq
#define fe_sq2 mc_fe_sq2
#define fe_sub mc_fe_sub
#define fe_tobytes mc_fe_tobytes
