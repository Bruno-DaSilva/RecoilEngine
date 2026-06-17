/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/*
** luajit-spike: the original implementation serialized a live PUC Lua 5.1 state
** by walking its internal object layout (lstate/lobject/ltable/Proto...). None
** of that exists in LuaJIT, whose state layout is entirely different. CReg Lua
** state save/load is therefore disabled on this branch.
**
** This is acceptable for the performance spike: a headless AI-bot benchmark
** never saves or loads. RegisterCFunction / AutoRegisterCFunctions /
** CopyLuaContext are called during normal handle setup, so they remain as
** no-ops; the actual (de)serialization entry points log an error if hit.
*/

#include "SerializeLuaState.h"

#include "System/Log/ILog.h"

namespace creg {

void SerializeLuaState(creg::ISerializer* /*s*/, lua_State** /*L*/)
{
	LOG_L(L_ERROR, "[%s] Lua state (de)serialization is not supported on the LuaJIT build", __func__);
}

void SerializeLuaThread(creg::ISerializer* /*s*/, lua_State** /*L*/)
{
	LOG_L(L_ERROR, "[%s] Lua thread (de)serialization is not supported on the LuaJIT build", __func__);
}

void RegisterCFunction(const char* /*name*/, lua_CFunction /*f*/)
{
	// no-op: only used to build the name<->pointer maps for serialization
}

void AutoRegisterCFunctions(const std::string& /*handle*/, lua_State* /*L*/)
{
	// no-op: see RegisterCFunction
}

void UnregisterAllCFunctions()
{
	// no-op
}

void CopyLuaContext(lua_State* /*L*/)
{
	// no-op: snapshotting the global_State for serialization is unsupported
}

} // namespace creg
