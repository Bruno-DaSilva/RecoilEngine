/* This file is part of the Recoil engine (GPL v2 or later), see LICENSE.html */

/*
** luajit-spike Tier 1 (synced determinism): deterministic transcendental math
** for LuaJIT's interpreter. LuaJIT's VM normally calls libc's libm for
** math.sin/cos/.../pow and for the `^` operator; libm results differ in the
** last ULP across platforms (glibc vs msvcrt vs macOS), which desyncs
** multiplayer. We route those through streflop's bundled fdlibm port, which is
** bit-identical on every platform -- exactly what the fork's lmathlib did for
** PUC Lua.
**
** vm_x86.dasc calls these as `lj_sfm_<fn>` (see the math_extern macros and the
** pow/log sites). The synced VM runs interpreter-only (jit.off), so only the
** interpreter path matters for sync; the JIT (unsynced) keeps native libm.
*/

#include "lib/streflop/streflop_cond.h"

extern "C" {

double lj_sfm_sin  (double x)           { return streflop::sin(x); }
double lj_sfm_cos  (double x)           { return streflop::cos(x); }
double lj_sfm_tan  (double x)           { return streflop::tan(x); }
double lj_sfm_asin (double x)           { return streflop::asin(x); }
double lj_sfm_acos (double x)           { return streflop::acos(x); }
double lj_sfm_atan (double x)           { return streflop::atan(x); }
double lj_sfm_sinh (double x)           { return streflop::sinh(x); }
double lj_sfm_cosh (double x)           { return streflop::cosh(x); }
double lj_sfm_tanh (double x)           { return streflop::tanh(x); }
double lj_sfm_exp  (double x)           { return streflop::exp(x); }
double lj_sfm_log  (double x)           { return streflop::log(x); }
double lj_sfm_log10(double x)           { return streflop::log10(x); }
double lj_sfm_pow  (double x, double y) { return streflop::pow(x, y); }
double lj_sfm_atan2(double x, double y) { return streflop::atan2(x, y); }
double lj_sfm_fmod (double x, double y) { return streflop::fmod(x, y); }

} // extern "C"
