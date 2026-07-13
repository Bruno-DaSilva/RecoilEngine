/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef GUI_HANDLER_H
#define GUI_HANDLER_H

#include <mutex>
#include <vector>

#include "KeySet.h"
#include "InputReceiver.h"
#include "MouseHandler.h"
#include "Game/Camera.h"
#include "Sim/Units/BuildInfo.h"
#include "Sim/Units/CommandAI/Command.h"
#include "System/SpringMath.h" // FACING

#define DEFAULT_GUI_CONFIG "ctrlpanel.txt"

class CUnit;
class CFeature;
struct UnitDef;

class Action;
struct SCommandDescription;

/**
 * The C and part of the V in MVC (Model-View-Controller).
 */
class CGuiHandler : public CInputReceiver {
public:
	CGuiHandler();

	void Update();

	void Draw();
	void DrawMapStuff(bool onMiniMap);
	void DrawCentroidCursor();

	bool AboveGui(int x, int y);
	bool KeyPressed(int keyCode, int scanCode, bool isRepeat);
	bool KeyReleased(int keyCode, int scanCode);
	bool MousePress(int x, int y, int button);
	void MouseRelease(int x, int y, int button)
	{
		// We can not use default params for this,
		// because they get initialized at compile-time,
		// where camera and mouse are still undefined.
		MouseRelease(x, y, button, camera->GetPos(), mouse->dir);
	}
	void MouseRelease(int x, int y, int button, const float3& cameraPos, const float3& mouseDir);
	bool IsAbove(int x, int y);
	std::string GetTooltip(int x, int y);
	std::string GetBuildTooltip() const;

	Command GetOrderPreview();
	Command GetCommand(int mouseX, int mouseY, int buttonHint, bool preview)
	{
		// We can not use default params for this,
		// because they get initialized at compile-time,
		// where camera and mouse are still undefined.
		return GetCommand(mouseX, mouseY, buttonHint, preview, camera->GetPos(), mouse->dir);
	}
	Command GetCommand(int mouseX, int mouseY, int buttonHint, bool preview, const float3& cameraPos, const float3& mouseDir);
	/// startInfo.def has to be endInfo.def
	size_t GetBuildPositions(const BuildInfo& startInfo, const BuildInfo& endInfo, const float3& cameraPos, const float3& mouseDir);

	bool EnableLuaUI(bool enableCommand);
	bool DisableLuaUI(bool layoutIcons = true);

	bool LoadConfig(const std::string& cfg);
	bool LoadDefaultConfig() { return (LoadConfig(DEFAULT_GUI_CONFIG)); }
	bool ReloadConfigFromFile(const std::string& fileName);
	bool ReloadConfigFromString(const std::string& cfg);

	void ForceLayoutUpdate() { forceLayoutUpdate = true; }

	int GetMaxPage()    const { return maxPage; }
	int GetActivePage() const { return activePage; }

	void RunLayoutCommand(const std::string& command);
	void RunCustomCommands(const std::vector<std::string>& cmds, bool rightMouseButton);

	bool GetInvertQueueKey() const { return invertQueueKey; }
	void SetInvertQueueKey(bool value) { invertQueueKey = value; }
	bool GetQueueKeystate() const;

	bool GetGatherMode() const { return gatherMode; }
	void SetGatherMode(bool value) { gatherMode = value; }

	bool GetOutlineFonts() const { return outlineFonts; }

	int  GetDefaultCommand(int x, int y) const
	{
		// We can not use default params for this,
		// because they get initialized at compile-time,
		// where camera and mouse are still undefined.
		return GetDefaultCommand(x, y, camera->GetPos(), ::mouse->dir);
	}
	int  GetDefaultCommand(int x, int y, const float3& cameraPos, const float3& mouseDir) const;

	// sim|draw PR 44 (prereq D): the served read of the context-cursor default
	// command for the per-frame draw-path callers (DrawMapStuff, DrawCentroidCursor,
	// LuaUnsyncedRead::GetDefaultCommand). All query the default command at the
	// CURRENT mouse ray -- the same single standing slot SetCursorIcon uses -- so
	// under the running split they read the last barrier's published reply (and
	// request a re-eval) instead of parking to run the live deep-CAI GetDefaultCmd.
	// This removes the per-frame GUI_GET_DEFAULT_CMD park those callers engaged.
	// Flag-off / sim parked / pregame / a non-cursor (x,y): the live parking path
	// (byte-identical). The DefaultCommand widget override is computed once per
	// barrier (in the reply) and shared by every reader -- same value, fewer event
	// fires; the Gap-B cursor path already made this tradeoff.
	int  GetDefaultCommandServed(int x, int y) const
	{
		return GetDefaultCommandServed(x, y, camera->GetPos(), ::mouse->dir);
	}
	int  GetDefaultCommandServed(int x, int y, const float3& cameraPos, const float3& mouseDir) const;

	// sim|draw PR 44 (Gap B): SimDrawBarrier hook, LOCKSTEP form. The context-
	// cursor default command bottoms out in a live deep-CAI virtual call
	// (GetDefaultCmd) + a GuiTraceRay, which cannot run from draw context under
	// the running split. SetCursorIcon requests a re-evaluation and reads the
	// LAST barrier's reply; this hook, called at the lockstep barrier with the
	// sim quiescent, recomputes the answer against live sim and publishes it
	// for the next draw frame. No-op unless a request is pending. Fires the
	// DefaultCommand event once (like the live path).
	void EvaluateDefaultCmdQuery();

	// PR 44b (the no-park flip): the default-cmd query/reply is RE-HOSTED --
	// evaluation on the sim thread, presentation on the draw side:
	//  - StageDefaultCmdQuery() (draw, at request time): runs the DRAW-side
	//    half -- input-receiver early-out + the snapshot-backed pick
	//    (GuiTraceRay / minimap GetSelectUnit, PR-25 draw-side searches over
	//    the pick grid; they may NOT run on the sim thread) -- and captures
	//    the picked ids + the selection-id snapshot into a mutex-guarded
	//    standing slot.
	//  - EvaluateDefaultCmdQueryAtSimEdge() (SIM thread, at its frame edges
	//    AND from the paused-idle query servicing, so the cursor stays live
	//    while the game is paused): re-resolves the picked ids against live
	//    sim (a died-since-stage id degrades to no-target) and runs the
	//    deep-CAI leader walk (CSelectedUnitsHandler::GetDefaultCmdEval),
	//    staging the result {unit, feature, raw cmd}.
	//  - CommitDefaultCmdReply() (draw, at the barrier): fires the main-
	//    thread-bound DefaultCommand widget callin with the staged results
	//    (widget cursor overrides apply here), maps the command id to its
	//    commands[] index and publishes the reply SetCursorIcon reads.
	// Staged unit/feature pointers stay readable through the commit: an
	// object that died since the eval is a parked shell whose ack comes after
	// the commit point in the barrier (dispatch-window ordering).
	void StageDefaultCmdQuery() const;
	void EvaluateDefaultCmdQueryAtSimEdge();
	void CommitDefaultCmdReply();

	bool SetActiveCommand(int cmdIndex, bool rightMouseButton);
	bool SetActiveCommand(int cmdIndex, int button, bool leftMouseButton, bool rightMouseButton, bool alt, bool ctrl, bool meta, bool shift);
	bool SetActiveCommand(const Action& action, const CKeySet& ks, int actionIndex);

	void SetDrawSelectionInfo(bool dsi) { drawSelectionInfo = dsi; }
	bool GetDrawSelectionInfo() const { return drawSelectionInfo; }

	void SetBuildFacing(unsigned int facing) { buildFacing = facing % NUM_FACINGS; }
	void SetBuildSpacing(int spacing) { buildSpacing = std::max(spacing, 0); }

	void LayoutIcons(bool useSelectionPage);

private:
	void GiveCommand(const Command& cmd, bool fromUser = true);
	void GiveCommandsNow();
	bool LayoutCustomIcons(bool useSelectionPage);
	void ResizeIconArray(size_t size);
	void AppendPrevAndNext(std::vector<SCommandDescription>& cmds);
	void ConvertCommands(std::vector<SCommandDescription>& cmds);

	int  FindInCommandPage();
	void SetActiveCommandIndex(int newIndex);
	void RevertToCmdDesc(const SCommandDescription& cmdDesc, bool defaultCommand, bool samePage);

	unsigned char CreateOptions(bool rightMouseButton);
	unsigned char CreateOptions(int button);
	void FinishCommand(int button);
	void SetShowingMetal(const SCommandDescription *cmdDesc);
	float GetNumberInput(const SCommandDescription& cmdDesc) const;

	void ProcessFrontPositions(float3& pos0, const float3& pos1);

	struct IconInfo;

	void DrawButtons();
	void DrawCustomButton(const IconInfo& icon, bool highlight);
	bool DrawUnitBuildIcon(const IconInfo& icon, int unitDefID);
	bool DrawTexture(const IconInfo& icon, const std::string& texName);
	void DrawName(const IconInfo& icon, const std::string& text, bool offsetForLEDs);
	void DrawNWtext(const IconInfo& icon, const std::string& text);
	void DrawSWtext(const IconInfo& icon, const std::string& text);
	void DrawNEtext(const IconInfo& icon, const std::string& text);
	void DrawSEtext(const IconInfo& icon, const std::string& text);
	void DrawPrevArrow(const IconInfo& icon);
	void DrawNextArrow(const IconInfo& icon);
	void DrawHilightQuad(const IconInfo& icon);
	void DrawIconFrame(const IconInfo& icon);
	void DrawOptionLEDs(const IconInfo& icon);
	void DrawMenuName();
	void DrawSelectionInfo();
	void DrawNumberInput();
	void DrawMiniMapMarker(const float3& cameraPos);
	void DrawFormationFrontOrder(int button, float maxSize, float sizeDiv, bool onMiniMap, const float3& cameraPos, const float3& mouseDir);
	void DrawArea(float3 pos, float radius, const float* color);
	void DrawSelectBox(const float3& start, const float3& end, const float3& cameraPos);
	void DrawSelectCircle(const float3& pos, float radius, const float* color);

	void DrawStencilCone(const float3& pos, float radius, float height);
	void DrawStencilRange(const float3& pos, float radius);


	int  IconAtPos(int x, int y);
	void SetCursorIcon() const;
	bool TryTarget(const SCommandDescription& cmdDesc) const;

	// sim|draw PR 44 (Gap B): the body of GetDefaultCommand WITHOUT the
	// ScopedExternalSimPause (the caller/hook owns the park). `fireEvent` gates the
	// eventHandler.DefaultCommand callin: true reproduces the live behaviour (used
	// by GetDefaultCommand and the barrier hook so widget overrides apply once);
	// false is the raw engine answer used by the armed diff-gate dual-run to
	// compare the served-channel plumbing without double-firing the event.
	int  GetDefaultCommandImpl(int x, int y, const float3& cameraPos, const float3& mouseDir, bool fireEvent) const;

	void LoadDefaults();
	void SanitizeConfig();
	void ParseFillOrder(const std::string& text);

	bool ProcessLocalActions(const Action& action);
	bool ProcessBuildActions(const Action& action);
	int  GetIconPosCommand(int slot) const;
	int  ParseIconSlot(const std::string& text) const;


public:
	int inCommand = -1;
	int buildFacing = FACING_SOUTH;
	int buildSpacing = 0;
	bool autoShowMetal = false;

private:
	int maxPage = 0;
	int activePage = 0;
	int defaultCmdMemory = -1;

	// sim|draw PR 44 (Gap B): context-cursor default-command query/reply slot.
	// mutable because SetCursorIcon() (const) sets the request flag. Single
	// standing slot -- the cursor is one logical query; every SetCursorIcon
	// consult uses the current mouse ray, so there is no pos-keyed-map miss.
	mutable bool defaultCmdQueryPending = false; // SetCursorIcon requested a re-eval
	mutable int  defaultCmdReplyCmd = -1;         // last barrier's evaluated index
	mutable bool defaultCmdReplyValid = false;    // a reply has been published

	// PR 44b: the re-hosted query/reply staging (see the method comments).
	// Cross-thread: draw stages the input + commits the output, the sim
	// thread evaluates -- both slots share the one mutex. The PICK (trace /
	// minimap closest-unit) happens at STAGE time on the DRAW side: since
	// PR 25 GuiTraceRay and CGameHelper::GetClosestUnit are snapshot/pick-
	// grid-backed draw-side searches (with draw-owned scratch) -- running
	// them on the sim thread races the pick grid's per-epoch rebuild and the
	// pick scratch (gate-found mimalloc corruption, strict Rosetta f~9700).
	// Only the deep-CAI leader walk is live-sim work and evaluates sim-side.
	struct DefaultCmdQueryInput {
		bool pending = false;
		bool noCommand = false;    // receiver hit / out-of-map miss: publish -1, no eval
		int tracedUnitID = -1;     // the stage-time pick (draw-side, snapshot-backed)
		int tracedFeatureID = -1;
		std::vector<int> selectedIDs; // request-time selection snapshot
	};
	struct DefaultCmdEvalResult {
		bool valid = false;
		bool fireCallin = false;   // a trace hit / leader walk ran (live fires there)
		bool leaderFound = false;
		const CUnit* unit = nullptr;
		const CFeature* feature = nullptr;
		int rawCmdID = 0;          // command ID (not a commands[] index)
	};
	mutable std::mutex defaultCmdQueryMtx;
	mutable DefaultCmdQueryInput defaultCmdQueryIn;
	mutable DefaultCmdEvalResult defaultCmdEvalOut;
	int explicitCommand = -1;
	int curIconCommand = -1;

	int actionOffset = 0;

	int deadIconSlot = -1;
	int prevPageSlot = -1;
	int nextPageSlot = -1;

	int xIcons = 2;
	int yIcons = 8;
	// number of slots taken up in <icons>
	int iconsCount = 0;
	int iconsPerPage = 0;

	int failedSound = -1;


	float xPos = 0.000f;
	float yPos = 0.175f;
	float textBorder = 0.003f;
	float iconBorder = 0.003f;
	float frameBorder = 0.003f;
	float xIconSize = 0.060f;
	float yIconSize = 0.060f;
	float xSelectionPos = 0.018f;
	float ySelectionPos = 0.127f;

	float xIconStep = 0.0f;
	float yIconStep = 0.0f;
	float xBpos = 0.0f;
	float yBpos = 0.0f; // center of the buildIconsFirst indicator

	float frameAlpha = -1.0f;
	float textureAlpha = 0.8f;

	bool needShift = false;
	bool invertQueueKey = false;
	bool activeMousePress = false;
	bool forceLayoutUpdate = false;

	bool dropShadows = true;
	bool useOptionLEDs = true;
	bool selectGaps = true;
	bool selectThrough = false;
	bool outlineFonts = false;
	bool drawSelectionInfo = true;

	bool gatherMode = false;
	bool miniMapMarker = true;
	bool newAttackMode = true;
	bool attackRect = false;
	bool invColorSelect = true;
	bool frontByEnds = false;
	bool useStencil = false;

	struct Box {
		float x1;
		float y1;
		float x2;
		float y2;
	};
	Box buttonBox;
	CKeySet lastKeySet;

	struct IconInfo {
		int commandsID; // index into commands list (or -1)
		Box visual;
		Box selection;
	};

	std::string menuName;

	std::vector<int> fillOrder;
	std::vector<IconInfo> icons;

	std::vector<std::string> layoutCommands;
	std::vector< std::pair<Command, bool> > commandsToGive;

	// DrawMapStuff caches
	std::vector<BuildInfo> buildInfos;
	std::vector<Command> buildCommands;

public:
	std::vector<SCommandDescription> commands;
};

extern CGuiHandler* guihandler;

#endif /* GUI_HANDLER_H */

