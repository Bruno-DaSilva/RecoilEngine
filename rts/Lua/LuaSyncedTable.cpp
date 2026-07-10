/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "LuaSyncedTable.h"

#include "LuaInclude.h"

#include "LuaHandleSynced.h"
#include "LuaHashString.h"
#include "LuaSplitContract.h"
#include "LuaUtils.h"
#include "Game/Game.h"        // PR 44b: the dispatch-scoped lazy park
#include "System/SimDrawSplit.h"


static int SyncTableIndex(lua_State* L);
static int SyncTableNewIndex(lua_State* L);
static int SyncTableMetatable(lua_State* L);

/******************************************************************************/

static int SyncTableIndex(lua_State* dstL)
{
	if (lua_isnoneornil(dstL, -1))
		return 0;

	// cross-hop handling (PR 27a ban -> PR 44b §9 BINDING ruling, option 3+1
	// hybrid): the read below walks the synced lua_State's globals and
	// allocates in its heap -- the state the sim thread owns and RUNS under
	// the no-park split. A DISPATCH-WINDOW read (a deferred sim-fired
	// handler; master ran these same-thread mid-frame) is served from the
	// HELD EPOCH's SYNCED-globals mirror when the key's value is a scalar
	// (read-set-driven; first touch registers the key for the producer's
	// next edge). FIDELITY: the mirrored value is the epoch edge's -- it
	// matches the deferred events' frame window, MORE master-faithful than
	// the live read the parked dispatch used to make. Reads the mirror
	// cannot serve (first touch, non-scalar values) engage the dispatch-
	// scoped LAZY PARK: the sim parks at its next edge, the live copy below
	// is quiescent-safe, and ErrorOnCrossHop's parked-window branch COUNTS
	// the engage instead of erroring (~a handful per game, the §9 gate
	// telemetry). Draw-context reads OUTSIDE the dispatch window keep the
	// hard error (strict-gate-proven unused by stock games).
	if (LuaSplitContract::Enforced(dstL) && SimDrawSplit::BoundaryShellWindowActive()) {
		CSplitLuaHandle* pair = CSplitLuaHandle::GetSplitHandle(dstL);

		if (pair->ServeSyncedGlobalFromMirror(dstL) == 1) {
			LuaSplitContract::CountCrossHopMirrorServe("SYNCED table read (mirror)");
			return 1;
		}

		if (game != nullptr)
			game->AcquireLazyDispatchPark();
	}

	LuaSplitContract::ErrorOnCrossHop(dstL, "SYNCED table read");

	auto slh = CSplitLuaHandle::GetSyncedHandle(dstL);
	if (!slh->IsValid())
		return 0;
	auto srcL = slh->GetLuaState();

	const int srcTop = lua_gettop(srcL);
	const int dstTop = lua_gettop(dstL);

	// copy the index & get value
	lua_pushvalue(srcL, LUA_GLOBALSINDEX);
	const int keyCopied = LuaUtils::CopyData(srcL, dstL, 1);
	assert(keyCopied > 0);
	lua_rawget(srcL, -2);

	// copy to destination
	const int valueCopied = LuaUtils::CopyData(dstL, srcL, 1);
	if (lua_istable(dstL, -1)) {
		// disallow writing in SYNCED[...]
		lua_createtable(dstL, 0, 2); {
			LuaPushNamedCFunc(dstL, "__newindex",  SyncTableNewIndex);
			LuaPushNamedCFunc(dstL, "__metatable", SyncTableMetatable);
		}
		lua_setmetatable(dstL, -2);
	}

	assert(valueCopied == 1);
	assert(dstTop + 1 == lua_gettop(dstL));

	lua_settop(srcL, srcTop);
	return valueCopied;
}


static int SyncTableNewIndex(lua_State* L)
{
	luaL_error(L, "Attempt to write to SYNCED table");
	return 0;
}


static int SyncTableMetatable(lua_State* L)
{
	luaL_error(L, "Attempt to access SYNCED metatable");
	return 0;
}


/******************************************************************************/
/******************************************************************************/

/***
 * Proxy table for reading synced global state in unsynced code.
 * 
 * **Generally not recommended.** Instead, listen to the same events as synced
 * and build the table in parallel
 * 
 * Unsynced code can read from the synced global table (`_G`) using the `SYNCED`
 * proxy table. e.g. `_G.foo` can be access from unsynced via `SYNCED.foo`.
 * 
 * This table makes *a copy* of the object on the other side, and only copies
 * numbers, strings, bools and tables (recursively but with the type
 * restriction), in particular this does not allow access to functions.
 * 
 * Note that this makes a copy on each access, so is very slow and will not
 * reflect changes. Cache it, but remember to refresh.
 * 
 * 
 * @global SYNCED table<string, any>
 */
bool LuaSyncedTable::PushEntries(lua_State* L)
{
	HSTR_PUSH(L, "SYNCED");
	lua_newtable(L); { // the proxy table

		lua_createtable(L, 0, 3); { // the metatable
			LuaPushNamedCFunc(L, "__index",     SyncTableIndex);
			LuaPushNamedCFunc(L, "__newindex",  SyncTableNewIndex);
			LuaPushNamedCFunc(L, "__metatable", SyncTableMetatable);
		}

		lua_setmetatable(L, -2);
	}
	lua_rawset(L, -3);

	return true;
}


/******************************************************************************/
/******************************************************************************/
