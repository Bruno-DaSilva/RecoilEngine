// Workflow: land epoch-publish-plan PRs ONE AT A TIME, all-at-once, in place.
// Invoke with args = { prs: ["38e"] } (usually one PR; a short list is processed
// strictly serially). Operator directive (2026-07-07): consolidate to a single
// all-at-once step per PR that works DIRECTLY on the epoch-integration branch —
// implement + build engine-headless ONCE + commit in place. No isolated impl
// worktree, no cherry-pick, no duplicate integration build, no in-workflow gate
// (the orchestrator DIY-gates a consolidated headful run per replay between PRs).
// Flow per PR: land-in-place -> review the committed diff -> conditional fix.
// Model: all agents OPUS against doc/sim-draw-epoch-publish-specs.md (Fable
// authored the design upfront; specced/high-effort PRs carry the escalation rule
// — design ambiguities are escalated, not improvised). args.decisions (free-text)
// overrides plan decision points.

export const meta = {
	name: 'epoch-publish-land',
	description: 'Land one (or a few) epoch-publish-plan PRs, one at a time, all-at-once in place',
	whenToUse: 'One PR at a time from doc/sim-draw-epoch-publish-plan.md; pass args.prs (usually a single PR)',
	phases: [
		{ title: 'Land', detail: 'one agent implements + builds engine-headless once + commits on epoch-integration' },
		{ title: 'Review', detail: 'adversarial review of the committed diff' },
		{ title: 'Fix', detail: 'conditional: apply blocking/fix findings as a follow-up commit' },
	],
}

// Absolute paths into the MAIN checkout: the plan/specs docs (and CLAUDE.md) are
// untracked there, so agents read them at these paths (never write under REPO).
const REPO = '/www/projects/RecoilEngine'
const PLAN = `${REPO}/doc/sim-draw-epoch-publish-plan.md`
const SPECS = `${REPO}/doc/sim-draw-epoch-publish-specs.md`
const RESEARCH = `${REPO}/doc/sim-draw-thread-decoupling-research.md`
const CLAUDEMD = `${REPO}/CLAUDE.md`
const WT = `${REPO}/.claude/worktrees/epoch-integration`

// specced: true -> this PR has a dedicated SPECS section (former [F] tier); runs
// at high effort, escalation rule applies with extra force.
const PRS = {
	'28': { specced: true,  title: 'Map-layer mirrors (LOS/metal/typemap/smooth-mesh/orig-heightmap; dirty-rect pattern-setter)' },
	'29': { title: 'Blocking-map mirror + placement family', needs: ['28'] },
	'30': { specced: true,  title: 'Command queues + cmdDescs (decision-4 dirty-copy pattern-setter)' },
	'31': { title: 'Weapon/shield scalar family', needs: ['30'] },
	'32': { title: 'Deep per-unit state', needs: ['30'] },
	'33': { title: 'Pieces/scripts serving' },
	'34': { title: 'Spatial/list remainder + parse-gate getters' },
	'35': { specced: true,  title: 'Weapon trace tests recomputed draw-side', needs: ['28', '29'] },
	'36': { title: 'Pathing reads + team/player misc + misc tail', needs: ['30'] },
	'37': { specced: true,  title: 'Ctrl write-side completion (18 sync-pokes)' },
	'38': { specced: true,  title: 'Zero-sanction flip + boundary-dispatch conversion', needs: ['28', '29', '30', '31', '32', '33', '34', '35', '36', '37'] },
	// PR 38 was split during implementation (2026-07-07). '38' landed part 1 (game+team
	// rules-params serving). The tail was NOT empty; these finish the serving, then 38b flips.
	'38c': { title: 'Serve player/unit/feature rules-params (extends PR 38 part 1)', needs: ['38'] },
	'38d': { title: 'Serve GetGroundInfo from the PR-28 metal/typemap mirrors', needs: ['38'] },
	'38e': { title: 'Serve placement callouts (TestBuildOrder/TestMoveOrder/ClosestBuildPos) via a terrain-speedmod mirror + CMoveMath re-host', needs: ['38'] },
	// PR 38b was RE-SCOPED (2026-07-07) after the 38b analysis found sanctionedLive is NOT empty
	// (26 survivors). 38f = P3 (event-time, separable, lands now). 38b = P2 (the flip) now needs
	// the 26 survivors served/decided first (P1: 8 spatial via Wave 6, 11 pathing via decision-1,
	// 7 misc via serve-or-nil) — so the flip moves to POST-Wave-6.
	'38f': { specced: true, title: 'Event-time presentation (LOS-exit position + UnitCommand/UnitCmdDone queue) — PR 38b P3, additive', needs: ['38'] },
	'38g': { specced: true, title: 'Ghostradar LOS-exit master-fidelity: reproduce master radar-error position (operator ruled option b, no behavior change) — 38f follow-up', needs: ['38f'] },
	'38h': { specced: true, title: 'ROOT-CAUSE + FIX the 38f event-time mechanism (all 3 widget errors still fire under the split; override path not applying)', needs: ['38f'] },
	'38j': { specced: true, title: 'Event-time fix v2: consult the override WITHOUT blanket-strict enforce (38h over-reached, nil-ing the handlers other reads)', needs: ['38f'] },
	'38b': { specced: true, title: 'Zero-sanction flip + shell-machinery removal + DEAD_THIS_BATCH (PR 38b P2; needs the 26 survivors served/decided first)', needs: ['38', '38c', '38d', '38e', '38f'] },
	'39': { specced: true,  title: 'Draw-side render records (units/features)', needs: ['38'] },
	'40': { title: 'Projectile pass conversion', needs: ['39'] },
	'41': { title: 'id->owner seam + decals + hygiene (guRNG split, Push race)' },
	'42': { title: 'Window shrink (interim payoff)', needs: ['38', '39', '40', '41'] },
	'43': { specced: true,  title: 'Epoch infrastructure (mechanism swap)', needs: ['42'] },
	'44': { specced: true,  title: 'The producer flip', needs: ['43'] },
	'45': { title: 'Follow-ups (pipeline depth, telemetry, doc consolidation)', needs: ['44'] },
}

const LAND_SCHEMA = {
	type: 'object',
	required: ['pr', 'landed'],
	properties: {
		pr: { type: 'string' },
		landed: { type: 'boolean', description: 'committed to epoch-integration AND engine-headless links clean' },
		commit: { type: 'string', description: 'the landed commit hash (or the pre-existing one if idempotent-skip)' },
		filesTouched: { type: 'array', items: { type: 'string' } },
		deviations: { type: 'array', items: { type: 'string' }, description: 'enumerated behavior deviations' },
		selfReview: { type: 'string', description: 'what you swept (choke points, POV masking, flag-off) + residual risk' },
		blockers: { type: 'array', items: { type: 'string' }, description: 'design escalations for the operator; landing a tractable part + escalating the rest is expected for oversized PRs' },
	},
}

const REVIEW_SCHEMA = {
	type: 'object',
	required: ['pr', 'verdict', 'findings'],
	properties: {
		pr: { type: 'string' },
		verdict: { enum: ['approve', 'approve-with-fixes', 'block'] },
		findings: {
			type: 'array',
			items: {
				type: 'object',
				required: ['severity', 'issue'],
				properties: {
					severity: { enum: ['blocking', 'fix-at-integration', 'note'] },
					issue: { type: 'string' },
					file: { type: 'string' },
				},
			},
		},
	},
}

const FIX_SCHEMA = {
	type: 'object',
	required: ['fixed'],
	properties: {
		fixed: { type: 'boolean' },
		fixCommit: { type: 'string' },
		applied: { type: 'array', items: { type: 'string' } },
		unresolved: { type: 'array', items: { type: 'string' }, description: 'findings that are design escalations, not clear fixes — left for the operator' },
	},
}

// args may arrive as a JSON-encoded string depending on the caller — normalize.
let ARGS = args
if (typeof ARGS === 'string') {
	try { ARGS = JSON.parse(ARGS) } catch (e) { ARGS = null }
}
if (typeof ARGS !== 'object' || ARGS === null)
	ARGS = {}

const decisionNote = ARGS.decisions
	? `Decision-point / mechanism overrides from the operator (BINDING): ${ARGS.decisions}`
	: 'Decision points 1-5: use the plan doc\'s recommended options unless a Batch-N amendment in the specs supersedes them.'

// This host: 32 cores / 60 GB. One PR at a time (serial), so builds NEVER run
// concurrently — the single build agent gets ALL cores (-j32), and the
// orchestrator's later engine-legacy gate build (also serial) is cheap via ccache.
const CORES = 32
const JOBS = CORES

// Robust build/run completion — supersedes the broken `echo SENTINEL >log`
// + `until grep SENTINEL log` idiom, which silently never fires when a build or
// spring run crashes/hangs WITHOUT writing the sentinel (spring can crash
// without exiting). Embedded verbatim into every prompt that builds or runs.
const RUN_DISCIPLINE = [
	`BUILD/RUN COMPLETION DISCIPLINE (mandatory). The pattern of writing a sentinel (echo DONE >log) and waiting with "until grep -q DONE log" is BANNED: the sentinel never appears when a build or run crashes or hangs without exiting, so you either wait forever or mis-read a failed/partial run as done. Detect completion by EXIT CODE, not by any log string:`,
	`- Bound EVERY build and EVERY spring run with timeout, and use its numeric exit code as the completion signal:`,
	`      timeout -k 30 <BOUND_SECS> <cmd> > "$log" 2>&1 ; rc=$?`,
	`  Run spring/ninja as timeout's DIRECT child (no wrapping subshell) so the kill signal reaches it. Exit codes: 0=clean, 124=hit the BOUND (HUNG — a real failure, never a pass), 137=SIGKILL, 139=segfault, 134=abort/assert-fail, 130=interrupt.`,
	`- Any run that can exceed ~9 minutes (clean builds) MUST be launched with the Bash tool's run_in_background:true, so the harness detects the real process exit and re-invokes you; a foreground Bash call is capped at 10 minutes and would itself masquerade as a hang. Never sleep-poll a sentinel to decide a run finished.`,
	`- NEVER detach a run with shell "nohup cmd &" / "cmd & disown". That orphans it beyond your control: you cannot wait for it, cannot read its exit code, and will end your turn with no results — which FAILS the workflow ("subagent completed without calling StructuredOutput").`,
	`- If you background inside the shell, capture the PID and:  wait "$PID"; rc=$?  — wait blocks until the process actually terminates and yields its true exit code. Never use "until grep".`,
	`- Decide pass/fail from rc AND a scan of the log for failure signatures — never from a sentinel's presence. Grep case-insensitively for: error:, FAILED, Segmentation fault, Spring has crashed, caught signal, terminate called, Assertion, core dumped.`,
	`- Suggested BOUNDs on this host (GCC + ccache; raise if a stage legitimately needs longer, never lower): incremental engine-headless build 1800, clean build 2700.`,
].join('\n')

function landPrompt(pr) {
	const spec = PRS[pr]
	return [
		`Implement AND land PR ${pr} — "${spec.title}" — all in ONE step, directly on the epoch-integration branch. There is NO separate implementation worktree and NO cherry-pick: you write, build, and commit IN PLACE. This is the whole design — exactly one build of the code, here.`,
		``,
		`WORKING TREE — the persistent integration worktree (NEVER the operator's main checkout at ${REPO}, which has live uncommitted state; read-only git/reads there are fine, writes are FORBIDDEN):`,
		`- Path: ${WT}, branch: epoch-integration. It accumulates every landed PR.`,
		`- If it does not exist, create it: git -C ${REPO} worktree add ${WT} -b epoch-integration bruno/poc-split-sim-draw`,
		`- Before writing: confirm you are on epoch-integration; if the tree has uncommitted leftovers from an interrupted run, "git reset --hard HEAD && git clean -fd" (never touch files you did not create). Confirm clean status.`,
		`- IDEMPOTENCY: "git log --oneline -20" — if this PR's subject is already present, it landed earlier; verify + report landed=true with the existing commit hash and STOP (never re-apply).`,
		``,
		`READ FIRST, in order (untracked docs — read at these absolute paths; they are NOT in the worktree): ${SPECS} (BINDING cross-cutting contracts + this PR's spec section if any + any "Batch-N amendment" or PR-specific ruling that NAMES this PR — amendments SUPERSEDE the base sections; the escalation rule at the top binds you), then ${PLAN} (this PR's entry + serving-inventory row), then the matching ${RESEARCH} patterns (SimSnapshot add-a-field recipe, §E.2 Route()/twin serving, eviction discipline, dirty-rect mirror precedent), then ${CLAUDEMD} for code style.`,
		``,
		`${decisionNote}`,
		``,
		`HOUSE RULES (non-negotiable):`,
		`- Flag-off behavior must be BIT-IDENTICAL — no new locks on the flag-off path, no synced-state write, no gsRNG/streflop change. If you cannot serve something without an approximation that would move a flag-off byte, ESCALATE it (blockers) rather than approximate.`,
		`- Shared-file discipline: APPEND ONLY your own clearly-marked section to SimSnapshot.{h,cpp}, SnapshotHash.cpp, SnapshotDiffGate.cpp, LuaSnapshotServe.cpp; keep the FIELD_NAMES/enum static_assert aligned; delete exactly your served callouts from sanctionedLive in LuaSplitContract.cpp. Never reorder/reformat other sections. (You commit onto the CURRENT epoch-integration, so your appends land after every prior PR's section — there are NO conflicts to resolve, just append in order.)`,
		`- Every new served callout gets: snapshot rows (all 5 add-a-field steps), a serving twin, diff-gate dual-run coverage, and a POV/masking audit. Prefer compile-caught designs: route new dirty-marking through single choke-point setters; remove/rename members rather than shadowing.`,
		`- Commit style: match the branch's existing commit messages (git log); include the deviation enumeration + a sync-audit note.`,
		``,
		`SELF-REVIEW before committing (an independent reviewer runs after you, but catch it first): sweep EVERY mutation site of any state you mirror for choke-point coverage (a bypassed choke = silent staleness — this class has bitten real PRs); verify each twin reproduces the live POV masking / nil shapes / table structure; confirm flag-off is byte-identical; enumerate EVERY behavior deviation.`,
		``,
		`BUILD — exactly ONE build: configure the worktree build dir if needed (cmake+ninja, GCC toolchain per CLAUDE.md, ccache launchers) and build ENGINE-HEADLESS to a clean LINK with -j${JOBS} (0 errors). Do NOT build engine-legacy/engine-dedicated — the orchestrator builds engine-legacy for the headful gate, and ccache makes it cheap. Run any test_* your change touches.`,
		``,
		RUN_DISCIPLINE,
		``,
		`COMMIT once engine-headless links clean. If the PR is OVERSIZED or hits a genuine design ambiguity, do NOT rush or approximate — commit the cleanly-landable, flag-off-verifiable PART, and ESCALATE the rest in blockers (this is expected; PR 35 and PR 38 both split this way).`,
		``,
		`RETURN your structured output: landed (true iff committed AND engine-headless links clean), commit (hash), filesTouched, deviations (complete), selfReview (what you swept + residual risk), blockers (design escalations for the operator).`,
		`REPORTING PRIORITY (mandatory — a landed PR that never returns StructuredOutput FAILS the workflow, wasting the work): the moment engine-headless links clean and you have committed, you have enough to report landed=true. Budget your window so the StructuredOutput call ALWAYS happens; never let extra polishing or verification starve it.`,
	].join('\n')
}

function reviewPrompt(pr, land) {
	const spec = PRS[pr]
	return [
		`Adversarial post-landing review of PR ${pr} — "${spec.title}". It was just committed to epoch-integration as ${land.commit}. It is ALREADY on the branch, so your job is to catch anything that must be FIXED forward (a follow-up commit) or REVERTED before the next PR builds on it.`,
		``,
		`Review the diff (read-only — modify NOTHING): git -C ${WT} show ${land.commit}  (and "git -C ${WT} show ${land.commit} --stat" for scope).`,
		``,
		`Read ${SPECS} (cross-cutting contracts + this PR's spec section + any Batch-N amendment naming it) and ${PLAN} first. Attack the diff on these axes, priority order:`,
		`1. Sync safety: any synced-code/synced-state change, gsRNG consumption, flag-off behavior change — flag-off bit-identity is THE invariant; anything that could move a synced byte is BLOCKING.`,
		`2. Serving-value correctness: does each twin reproduce the live path's POV masking, nil shapes, table structure? Missed masking = hidden-enemy info leak = BLOCKING.`,
		`3. Dirty-tracking completeness: sweep EVERY writer of mirrored state for a bypassed choke point (grep the setters) — a missed site is BLOCKING (silent staleness, the class the gates catch late).`,
		`4. Lifetime/ordering: reads before rebuild/drain, id-reuse windows, creation-clear vs delta ordering.`,
		`5. Shared-file discipline: append-only sections, static_assert alignment, exact sanctionedLive deletions.`,
		`6. Spec compliance: any departure from SPECS/amendments — improvised design decisions are findings even when they look correct (they go to the operator per the escalation rule).`,
		``,
		`Implementer's deviation list: ${JSON.stringify(land.deviations || [])}; self-review: ${JSON.stringify(land.selfReview || '')}. Verify completeness — an unlisted deviation or an unswept choke is a finding.`,
		``,
		`Verdict: 'block' = must be reverted/fixed before ANY further PR builds on it; 'approve-with-fixes' = a follow-up fix commit is enough; 'approve' = clean. Be specific enough that a fixer acts without re-deriving your analysis.`,
	].join('\n')
}

function fixPrompt(pr, land, review) {
	return [
		`Apply the review's blocking + fix-at-integration findings for PR ${pr} (landed on epoch-integration as ${land.commit}), directly in the integration worktree ${WT} (branch epoch-integration). Never touch the operator's main checkout.`,
		``,
		`Findings to address: ${JSON.stringify((review.findings || []).filter(f => f.severity !== 'note'))}.`,
		``,
		`Fix them in place, rebuild ENGINE-HEADLESS to a clean link (-j${JOBS}, ccache), and commit as a NEW commit "Integration fix (PR ${pr}): ..." — do NOT rewrite ${land.commit}. Keep flag-off BIT-IDENTICAL. If a finding is actually a design escalation (not a clear, safe fix), do NOT guess — leave it and report it in "unresolved" for the operator.`,
		``,
		RUN_DISCIPLINE,
		``,
		`RETURN: fixed (bool), fixCommit (hash or null), applied (list), unresolved (escalations left for the operator). REPORTING PRIORITY: once committed + built, report — never let polishing starve the StructuredOutput call.`,
	].join('\n')
}

// ---- run: ONE PR at a time, all-at-once (land in place) -> review -> conditional fix ----

const batch = Array.isArray(ARGS.prs) ? ARGS.prs.map(String) : null
if (!batch || batch.length === 0)
	throw new Error('Pass args = { prs: ["38e"] } — one PR (or a short list) processed strictly one at a time.')
for (const pr of batch) {
	if (!PRS[pr])
		throw new Error(`Unknown PR ${pr} — valid: ${Object.keys(PRS).join(', ')}`)
}

log(`Land one-at-a-time (all-at-once, in place): ${batch.map(pr => `PR ${pr}${PRS[pr].specced ? ' [high]' : ''}`).join(', ')}`)

const results = []
for (const pr of batch) {
	phase('Land')
	const land = await agent(landPrompt(pr), {
		label: `land:PR${pr}`,
		phase: 'Land',
		model: 'opus',
		effort: PRS[pr].specced ? 'high' : undefined,
		schema: LAND_SCHEMA,
	})
	if (!land || !land.landed) {
		results.push({ pr, landed: false, commit: land ? land.commit : null, blockers: land ? (land.blockers || []) : ['land agent returned nothing'] })
		log(`PR ${pr}: NOT landed${land && land.blockers && land.blockers.length ? ' — ' + JSON.stringify(land.blockers).slice(0, 240) : ''}`)
		continue
	}

	phase('Review')
	const review = await agent(reviewPrompt(pr, land), {
		label: `review:PR${pr}`,
		phase: 'Review',
		model: 'opus',
		effort: 'high',
		schema: REVIEW_SCHEMA,
	})

	let fix = null
	const needsFix = review && (review.verdict === 'block' ||
		(review.findings || []).some(f => f.severity === 'blocking' || f.severity === 'fix-at-integration'))
	if (needsFix) {
		phase('Fix')
		fix = await agent(fixPrompt(pr, land, review), {
			label: `fix:PR${pr}`,
			phase: 'Fix',
			model: 'opus',
			effort: 'high',
			schema: FIX_SCHEMA,
		})
	}

	results.push({
		pr,
		landed: true,
		commit: (fix && fix.fixCommit) ? fix.fixCommit : land.commit,
		landCommit: land.commit,
		deviations: land.deviations || [],
		blockers: land.blockers || [],
		reviewVerdict: review ? review.verdict : null,
		reviewFindings: review ? review.findings : [],
		fixApplied: fix ? (fix.applied || []) : [],
		fixUnresolved: fix ? (fix.unresolved || []) : [],
	})
	log(`PR ${pr}: LANDED ${land.commit}${fix ? ' (+fix ' + (fix.fixCommit || 'none') + ')' : ''}; review=${review ? review.verdict : 'n/a'}`)
}

return {
	batch,
	landed: results.filter(r => r.landed).map(r => r.pr),
	failed: results.filter(r => !r.landed),
	details: results,
	note: 'Orchestrator DIY-gates a consolidated headful run per replay after this run; there is no in-workflow gate. Any blockers/fixUnresolved are operator escalations.',
}
