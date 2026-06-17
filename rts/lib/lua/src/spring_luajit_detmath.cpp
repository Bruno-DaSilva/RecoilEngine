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

/*
** Args MUST be wrapped in streflop::Double. streflop's math functions are
** overloaded on its Simple/Double/Extended wrapper types (SMath.h); calling
** them with a raw `double` resolves to the wrong overload (the Extended /
** long-double path, broken under STREFLOP_SSE) and returns garbage -- e.g.
** pow(2,3) came back as 92581. Double(x) selects the fdlibm double overload,
** matching what the engine's own math:: wrappers (streflop_cond.h) do.
*/
using streflop::Simple;

/*
** Route through streflop's Simple (32-bit float) functions, exactly as the PUC
** fork's lmathlib did (math::pow -> streflop::pow(Simple,Simple)). The engine
** runs synced code with the FPU limited to float precision (streflop_init<
** Simple>, see LuaUser.cpp), and streflop's Double fdlibm routines misbehave in
** that context (pow(2,3) came back as 92581). The Simple routines match the
** ambient mode and are what the fork used for determinism. We take a small
** precision hit vs LuaJIT's native doubles, but that is precisely the float
** precision BAR's synced code was written against.
*/
extern "C" {

double lj_sfm_sin  (double x)           { return (double)streflop::sin  (Simple((float)x)); }
double lj_sfm_cos  (double x)           { return (double)streflop::cos  (Simple((float)x)); }
double lj_sfm_tan  (double x)           { return (double)streflop::tan  (Simple((float)x)); }
double lj_sfm_asin (double x)           { return (double)streflop::asin (Simple((float)x)); }
double lj_sfm_acos (double x)           { return (double)streflop::acos (Simple((float)x)); }
double lj_sfm_atan (double x)           { return (double)streflop::atan (Simple((float)x)); }
double lj_sfm_sinh (double x)           { return (double)streflop::sinh (Simple((float)x)); }
double lj_sfm_cosh (double x)           { return (double)streflop::cosh (Simple((float)x)); }
double lj_sfm_tanh (double x)           { return (double)streflop::tanh (Simple((float)x)); }
double lj_sfm_exp  (double x)           { return (double)streflop::exp  (Simple((float)x)); }
double lj_sfm_log  (double x)           { return (double)streflop::log  (Simple((float)x)); }
double lj_sfm_log10(double x)           { return (double)streflop::log10(Simple((float)x)); }
double lj_sfm_pow  (double x, double y) { return (double)streflop::pow  (Simple((float)x), Simple((float)y)); }
double lj_sfm_atan2(double x, double y) { return (double)streflop::atan2(Simple((float)x), Simple((float)y)); }
double lj_sfm_fmod (double x, double y) { return (double)streflop::fmod (Simple((float)x), Simple((float)y)); }

} // extern "C"

/*
** luajit-spike: satisfy a latent streflop mangling bug. mpsqrt.cpp forward-
** declares fastiroot() at *global* scope but defines it inside namespace
** streflop_libm, so __mpsqrt's call references ::fastiroot(double)
** (_Z9fastirootd) while only streflop_libm::fastiroot(double) is defined. The
** engine's own synced math never pulls mpsqrt.o, but routing LuaJIT's pow/sqrt
** through streflop does -- which is the first thing to expose the missing ref.
** Forward the global symbol to the real definition rather than patching the
** streflop submodule (kept self-contained on this spike branch).
*/
namespace streflop_libm { double fastiroot(double); }
double fastiroot(double x) { return streflop_libm::fastiroot(x); }
