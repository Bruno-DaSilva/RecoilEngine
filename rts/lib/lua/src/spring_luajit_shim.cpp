/* This file is part of the Recoil engine (GPL v2 or later), see LICENSE.html */

/*
** luajit-spike: implementations of the custom symbols the Recoil fork added to
** PUC Lua, provided here against vendored LuaJIT's public API so the engine
** links unchanged. This is a performance-spike shim: sync determinism and the
** io sandbox are intentionally out of scope on this pass.
*/

#include "LuaInclude.h"
#include "lua_privileges.h"

///////////////////////////////////////////////////////////////////////////
// Fast pre-hashed string interning (see LuaHashString)
//
// PUC let us push a string together with a precomputed hash to skip rehashing.
// LuaJIT interns with its own internal hash, so we just push the string and
// ignore the supplied hash. lua_calchash still returns a stable hash so the
// engine's own LuaHashString caching/comparisons keep working.

lua_Hash lua_calchash(const char* s, size_t l)
{
	unsigned int h = (unsigned int)l;
	const size_t step = (l >> 5) + 1;
	for (size_t i = l; i >= step; i -= step)
		h ^= ((h << 5) + (h >> 2) + (unsigned char)s[i - 1]);
	return h;
}

void lua_pushhstring(lua_State* L, lua_Hash /*h*/, const char* s, size_t l)
{
	lua_pushlstring(L, s, l);
}


///////////////////////////////////////////////////////////////////////////
// io/os access security setters (see LuaIO / LuaLibs)
//
// No-ops on this pass: LuaJIT's io/os libraries use their own libc-backed
// implementation. The setters exist only so the call sites compile.

void lua_set_fopen(lua_State*, lua_Func_fopen) {}
void lua_set_popen(lua_State*, lua_Func_popen, lua_Func_pclose) {}
void lua_set_system(lua_State*, lua_Func_system) {}
void lua_set_remove(lua_State*, lua_Func_remove) {}
void lua_set_rename(lua_State*, lua_Func_rename) {}


///////////////////////////////////////////////////////////////////////////
// Privileged chunk loader (see lua_privileges)
//
// PUC restricted loading bytecode to privileged contexts. On this pass we drop
// the distinction and accept source or LuaJIT bytecode unconditionally.

int luaL_loadbuffer_privileged(lua_State* L, const char* buff, size_t sz, const char* name, bool /*privileged*/)
{
	return luaL_loadbuffer(L, buff, sz, name);
}

int lua_load_privileged(lua_State* L, lua_Reader reader, void* dt, const char* chunkname)
{
	return lua_load(L, reader, dt, chunkname);
}
