/* This file is part of the Recoil engine (GPL v2 or later), see LICENSE.html */

/* luajit-spike: forward to vendored LuaJIT's auxiliary library header. */

#ifndef SPRING_LUAJIT_FWD_LAUXLIB_H
#define SPRING_LUAJIT_FWD_LAUXLIB_H

#ifdef __cplusplus
extern "C" {
#endif
#include "../../luajit/src/lauxlib.h"
#ifdef __cplusplus
}
#endif

// luajit-spike: PUC Lua 5.1 compatibility aliases for older client code
// (luasocket) that predates the luaL_reg -> luaL_Reg / luaI_openlib ->
// luaL_openlib renames. LuaJIT only ships the new names.
typedef luaL_Reg luaL_reg;
#ifndef luaI_openlib
#define luaI_openlib luaL_openlib
#endif

#endif // SPRING_LUAJIT_FWD_LAUXLIB_H
