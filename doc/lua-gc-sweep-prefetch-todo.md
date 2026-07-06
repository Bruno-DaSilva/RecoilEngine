# Lua GC sweep: array-backed deep prefetch

## Problem

Tracy shows a heavy Lua GC sweep zone every few frames. The sweep (`sweeplist` in `rts/lib/lua/src/lgc.cpp`) walks the `rootgc` singly-linked list, touching each object's `CommonHeader` (`next; tt; marked`, first cache line) to test the mark. Objects are scattered across the smmalloc segregated bucket regions (`rts/lib/smmalloc`, via `LuaMemPool`), so the walk is one cache miss per object and is latency-bound.

## Why the obvious fixes don't help

- The linked-list link is free: `next` shares the header cache line we already touch, so the list adds no extra miss over visiting the object.
- An allocation-order array of `GCObject*` does *not* improve locality: objects stay scattered (size classes interleave across buckets; smmalloc's LIFO reuse scrambles allocation vs address order). Same one-miss-per-object cost, plus array traffic.
- Address-sorting the array would give sequential streaming, but addresses aren't deterministic across clients, so it perturbs sweep/finalizer order. Finalizer order is synced-visible (userdata `__gc`), so address-sort is **unsynced-only** — can't be the general fix.
- Deep prefetch on the list is impossible: node addresses form a dependency chain (`node[i+K]` needs chasing `i..i+K-1` first). A lookahead cursor just splits the same dependent-miss chain in two; net ~zero. So `prefetch(curr->next)` one-ahead is the list's ceiling, and the per-object work is too small to hide a ~100-300cy miss.

## The plan: array-backed sweep with deep prefetch

Flatten enumeration into a `GCObject*` array so addresses are known without chasing, then prefetch K ahead. This converts the sweep from latency-bound (one exposed miss at a time) to bandwidth-bound (K independent outstanding misses = memory-level parallelism). Buys MLP, not locality — so **no sorting needed**.

Allocation order only ⇒ deterministic ⇒ **synced-safe** (unlike address-sort). Works on both synced and unsynced handles. This is the mark-sweep prefetch technique from the GC literature (Garner/Blackburn et al.).

### Design

- Per Lua state: growable `std::vector<GCObject*>` sweep list; append on allocation (where `luaC_link` runs today).
- Sweep iterates the vector with tuned prefetch distance K (~8-32, latency / per-object work); compact survivors in place (swap-remove / write-forward) so the vector carries to the next cycle. No per-cycle flatten pass (flattening would itself pay the miss stream we're avoiding).
- Mark bits and write barriers stay in the objects, untouched — only the enumeration container changes. Far less invasive than a bitmap-sweep rewrite.
- Still handle the separate `strt` string-table sweep, the `tmudata` finalizer list, and thread `openupval` as today.

### Caveats

- Surgery on vendored `lgc.cpp` (replace `rootgc` enumeration). Cleaner to fully replace than to keep list + array in sync.
- K needs tuning. Memory ~8 bytes/object.

## Validate

Prototype, then Tracy-measure against (a) today's list, (b) list + one-ahead `prefetch(curr->next)`, (c) array + deep prefetch. Start on the unsynced handle (where the widget garbage lives per the Lua boundary profiler findings), but the approach generalizes to synced.

## Cheaper alternatives / fallbacks

- One-line `__builtin_prefetch(curr->gch.next)` in `sweeplist` — safe, zero determinism risk, covers both handles; weak but free. Good first measurement to size how much is pointer-chase latency vs `freeobj` work.
- Reduce allocation volume (fewer widget temp tables/strings) — shrinks the list itself; helps burst frequency and per-sweep cost without touching Lua.
- Bitmap/BiBOP sweep — removes the misses entirely (touch objects only when freeing), but a large GC rewrite with synced finalizer-order determinism risk. Last resort.

## Related scheduling note (separate lever)

The bursty "every few frames" pattern is the probabilistic skip in `spring_lua_alloc_skip_gc` (`rts/lib/lua/include/LuaUser.cpp`): skips most frames, then runs a multi-ms budget in one frame. Flatten via `Spring.GarbageCollectCtrl` / configs `LuaGarbageCollectionMemLoadMult` (toward 100) + `LuaGarbageCollectionRunTimeMult` (toward 1). Check whether the game (BAR) already overrides these at runtime before tuning engine config. This smooths spikes but doesn't reduce total work — orthogonal to the sweep-efficiency work above.
