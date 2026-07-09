/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef UNSYNCED_ACTION_EXECUTOR_H
#define UNSYNCED_ACTION_EXECUTOR_H

#include "IActionExecutor.h"

#include <string>

class Action;


class UnsyncedAction : public IAction
{
public:
	UnsyncedAction(const Action& action, bool repeat)
		: IAction(action)
		, repeat(repeat)
	{}

	/**
	 * Returns whether the action is to be executed repeatedly.
	 */
	bool IsRepeat() const { return repeat; }

private:
	bool repeat;
};


class IUnsyncedActionExecutor : public IActionExecutor<UnsyncedAction, false>
{
protected:
	IUnsyncedActionExecutor(const std::string& command, const std::string& description, bool cheatRequired = false, std::vector<std::pair<std::string, std::string>> arguments = {})
		: IActionExecutor<UnsyncedAction, false>(command, description, cheatRequired, arguments)
	{

	}

public:
	bool ExecuteActionRelease(const UnsyncedAction& action) const {
		if (IsCheatRequired() && !gs->cheatEnabled) {
			LOG_L(L_WARNING, "Chat command /%s (%s) cannot be executed (release) (cheats required)!",
					GetCommand().c_str(),
					(IsSynced() ? "synced" : "unsynced"));
			return false;
		} else {
			return ExecuteRelease(action);
		}
	}

	virtual ~IUnsyncedActionExecutor() {}

	// Sim/draw split (PR 44 prereq E): does this console-action executor read
	// or write LIVE sim state when it runs on the draw thread? Defaults TRUE
	// (conservative -- an unclassified executor gets the sim parked around it,
	// preserving the old full-batch behaviour). The DRAW-UI / NET-SEND families
	// (camera, rendering, config, sound, chat, net-send) are marked FALSE at
	// registration (UnsyncedGameCommands.cpp) so per-action park scoping
	// (CGuiHandler::RunCustomCommands) can skip the park for them -- they touch
	// no live sim, so parking is pure overhead (measured ~per-draw-frame headful
	// via stock-BAR widgets calling Spring.SendCommands). Only the SIM-POKE
	// minority (selection/group/team/give/destroy/particle-limits/DumpState)
	// keeps the default TRUE and still parks. Flag-off the park is inert.
	bool TouchesSimState() const { return touchesSimState; }
	void SetTouchesSimState(bool v) { touchesSimState = v; }

private:
	virtual bool ExecuteRelease(const UnsyncedAction& action) const { return false; }

	bool touchesSimState = true;
};

#endif // UNSYNCED_ACTION_EXECUTOR_H
