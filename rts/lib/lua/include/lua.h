/* This file is part of the Recoil engine (GPL v2 or later), see LICENSE.html */

/*
** luajit-spike: this header used to be the modified PUC Lua 5.1 public API.
** On this branch the engine is built against vendored LuaJIT 2.1 instead, so
** we forward to LuaJIT's public headers and re-declare the handful of custom
** symbols the Recoil fork added on top of stock Lua.
**
** LuaJIT's headers are plain C headers with no `extern "C"` guard (unlike the
** fork's, which were compiled as C++); wrap them so the C++ engine links
** against LuaJIT's C-linkage symbols. The fork's own extra symbols keep C++
** linkage to match their existing engine call sites.
*/

#ifndef SPRING_LUAJIT_FWD_LUA_H
#define SPRING_LUAJIT_FWD_LUA_H

#ifdef __cplusplus
extern "C" {
#endif
#include "../../luajit/src/lua.h"

/*
** JIT engine control. The real declarations live in luajit.h, which is
** generated from luajit_rolling.h at build time (the latter has a #error guard
** against direct inclusion). Rather than depend on a generated header, declare
** the small, stable LuaJIT 2.1 public-ABI subset we need to run the synced VM
** interpreter-only (see CSyncedLuaHandle::Init).
*/
#ifndef LUAJIT_MODE_ENGINE
#define LUAJIT_MODE_ENGINE 0       /* first entry of luajit.h's mode enum */
#define LUAJIT_MODE_OFF    0x0000  /* turn feature off */
LUA_API int luaJIT_setmode(lua_State *L, int idx, int mode);
#endif

#ifdef __cplusplus
}
#endif

#include <stdio.h>
#include <stddef.h>

/*
** SPRING additions for io access security (see LuaIO / LuaLibs).
** Kept as no-ops on the luajit-spike pass (io routes through LuaJIT's own
** libc-backed implementation); the setters exist only so call sites compile.
*/
typedef FILE* (*lua_Func_fopen)(lua_State* L, const char* path, const char* mode);
typedef FILE* (*lua_Func_popen)(lua_State* L, const char* command, const char* type);
typedef int   (*lua_Func_pclose)(lua_State* L, FILE* stream);
typedef int   (*lua_Func_system)(lua_State* L, const char* command);
typedef int   (*lua_Func_remove)(lua_State* L, const char* pathname);
typedef int   (*lua_Func_rename)(lua_State* L, const char* oldpath, const char* newpath);

void lua_set_fopen(lua_State* L, lua_Func_fopen);
void lua_set_popen(lua_State* L, lua_Func_popen, lua_Func_pclose);
void lua_set_system(lua_State* L, lua_Func_system);
void lua_set_remove(lua_State* L, lua_Func_remove);
void lua_set_rename(lua_State* L, lua_Func_rename);

/* SPRING fast pre-hashed string interning helpers (see LuaHashString). */
typedef unsigned int lua_Hash;
lua_Hash lua_calchash(const char* s, size_t l);
void     lua_pushhstring(lua_State* L, lua_Hash h, const char* s, size_t l);

#endif // SPRING_LUAJIT_FWD_LUA_H
