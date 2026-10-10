# AA3 Server Execution Path — Consolidated Control-Flow Map

## Purpose

This project has accumulated static-analysis findings about the real
`AA3Game_console.exe` dedicated server across three separate, disconnected
sources:

1. `DissAssembledFunctions/analysis/*.txt` — a May 2026 phase that mapped
   process boot, engine init, module loading, and the main loop from 508
   Ghidra-decompiled functions, but explicitly left **"Subsystem 6: Network &
   Replication"** as `Status: NEED TO IDENTIFY NETWORK FUNCTIONS`.
2. `PROTOCOL_FUNCTION_MAP.md` — maps the connection handshake
   (`NMT_Handshake*`/`USES`/`LOGIN`/`WELCOME`) against real UE3 source.
3. `AA3_ACTOR_REPLICATION.md` — this project's most recent phase, which used
   live gdb breakpoints against the real running server (verified against
   real gameplay) to pin down the actor-replication wire format and the exact
   binary addresses involved.

Nobody had connected these three into one picture of the actual path the
server's code takes from process start to sending a replicated property on
the wire. That's what this document does. Addresses below are file offset ==
VA (image base `0x400000`, confirmed via `objdump -h`: RVA equals raw file
offset for every section in this PE, no alignment slop) unless noted.

## 1. Process entry point

`AddressOfEntryPoint` (PE optional header) = RVA `0x105dab4` → VA `0x145dab4`.
Confirmed byte-identical between `AA3Game.exe` and `AA3Game_console.exe` (same
build; the console variant is the 1-byte GUI→Console subsystem-flag patch,
see `aa3_server_headless_patch` memory) — same entry point in both.

```
0145dab4: CALL 0145e5d8
0145dab9: JMP  0145d79b        ; real startup continues here
0145dabe: JMP  dword ptr [01de58a0]   ; adjacent IAT thunk, not part of this function
0145dac4: JMP  dword ptr [01de589c]   ; adjacent IAT thunk, not part of this function
```

`0x145dab4` is a tiny trampoline; the real CRT startup body is at `0x145d79b`
(confirmed by direct disassembly — TEB access via `FS:[0x18]`, one-time-init
guard flags at globals `0x2688628`/`0x261662c`, `HeapSetInformation`/CRT-mode
setup calls through IAT thunks at `0x1de5148`/`0x1de5274`/`0x1de5288`/etc.).
This is ordinary MSVC CRT boilerplate (`__scrt_common_main_seh`-style), not
AA3-specific, and wasn't traced further — not useful for reimplementation.

**Open item:** the exact call site where CRT startup reaches the
historically-documented `WinMain` (`FUN_004024b0`, RVA `0x24b0` — see below)
was not re-traced this session. `0x24b0` is a completely ordinary low
early-`.text` address for a linker-placed `WinMain`, while `0x145dab4` sits
far into a huge binary (36 MB) — consistent with the entry point being a
small appended/post-link trampoline (e.g. from a code-signing or copy-
protection post-processing step) that eventually falls through to the
conventionally-linked CRT chain containing `WinMain`. Treat the existing
map below as trustworthy; only worth re-deriving if the boot sequence itself
comes into question.

## 2. Boot → engine init → module load

Inherited from `DissAssembledFunctions/analysis/{AA3_SERVER_IMPLEMENTATION_TRACKER,SUBSYSTEM_BREAKDOWN}.txt`
(May 2026 phase, 508 functions from an earlier Ghidra pass). Re-confirmed
this session that the entry addresses still contain the expected code in the
*current* binary (spot-checked `FUN_004024b0` and `FUN_00406870` byte-for-
byte — both still match: SEH-wrapped prologues, expected global references).

| Order | Function | Purpose |
|---|---|---|
| 1 | `FUN_004024b0` — WinMain | SEH-wrapped entry; Steam API init/shutdown; calls GuardedMain; saves profiles/leaderboards on exit |
| 2 | `FUN_004011b0` — GuardedMain | Main guard wrapper (not yet re-read this session) |
| 3 | `FUN_00407e30` — EngineBootstrapper | Command-line parsing; branches Server vs Game vs Editor mode |
| 4 | `FUN_00407740` — Engine_InitCoreObject | Constructs `GameEngine`/`UnrealEdEngine`; sets global `GEngine` pointer at `DAT_023f221c`; calls `FUN_004070d0` |
| 5 | `FUN_004070d0` — Engine_GlobalStartupSequence | Orchestrates module loading + map preload |
| 6 | `FUN_00405680` — Engine_LoadUnrealCoreSubsystems | Loads `Core`, `Engine`, `GameFramework`, `IpDrv`, `GFxUI` (+ `Editor`/`UnrealEd` if not server) |
| 7 | `FUN_004054b0` — Engine_LoadCoreSubsystems (game-specific) | Loads `OnlineSubsystemAA3`, `AA3Baseline`, `AA3Game` |
| 8 | `FUN_00406f00` — Engine_LoadStartupPackages | Loads `Engine.StartupPackages` list from config |
| 9 | `FUN_00406d00` — Engine_InitDefaultPlayer | `DefaultPlayer` config section, CLI identity overrides |
| 10 | `FUN_00404e70` — Engine_CoreSubsystemInit | Zeroes/populates a 1600-byte global state buffer (`DAT_025cd918`) — config/network/scripting subsystem init |
| — | `FUN_00404fc0` — timing (`QueryPerformanceCounter`-based delta time) | Called every tick from the main loop, see §3 |

This confirms and preserves the existing `IMPLEMENTATION_ROADMAP.md`/
`DECOMPILATION_TARGETS.md` guidance — nothing here changed, it's restated so
this document is self-contained.

## 3. Main loop → `GEngine->Tick()` (bridges the gap — new this session)

`FUN_00406870` is the main engine heartbeat. Disassembled it directly this
session (in ~90-byte increments — see technique note in §6) to find exactly
*where* it hands off into per-frame world/network processing, since the old
map only had it as "drives subsystem updates (virtual calls at +0x110,
+0xC8)" without resolving what those calls actually reach.

```
00406887: MOV EAX,[023bdbd0]          ; stack-cookie xor'd into SEH slot (unrelated)
...
004068c7: MOV ECX,[023a7d64]          ; global object #1 (background task processor)
004068cd: MOV EAX,[ECX]               ; vtable
004068cf: MOV EDX,[EAX+0x1c]          ; vtable slot 7
004068d2: CALL EDX                    ; virtual call #1 — background task tick
004068d4..: (perf-counter frequency check / one-time GC-related call at 00c30450 / 004ab110)
004068f8: MOV ECX,[025a45f0]          ; global object #2 (a second ticked subsystem)
004068fe: CMP ECX,ESI  / JZ  (skip if null)
00406902: MOV EAX,[ECX]               ; vtable
00406904: MOV EDX,[EAX+0x8]           ; vtable slot 2
00406907: CALL EDX                    ; virtual call #2 — second subsystem tick
00406909..0040694d: FPS-cap throttle — compares EDI+0x14 (current time, double)
                     against EDI+0xc / a config-derived rate limit; skips the
                     next block if not enough time has elapsed yet (frame
                     pacing, matches "Manages thread priority"/FPS-cap notes
                     in the old doc)
0040694d: MOV ECX,[023f221c]          ; ECX = GEngine (the global set by
                                       ; FUN_00407740 in §2, step 4)
00406953: MOV EDX,[ECX]               ; GEngine's vtable
00406955: MOV EAX,[EDX+0x13c]         ; vtable slot 79 (0x13c / 4)
0040695c: CALL EAX                    ; *** GEngine->Tick(DeltaSeconds) ***
```

**Update: the vtable literal is now resolved, and it changes the picture.**
Rather than keep guessing statically, did a one-shot (no breakpoint, no
`continue` loop — just three pointer dereferences via `gdb -p <pid> --batch
-ex 'print/x ...' -ex detach -ex quit`) memory read against the live,
already-running server. Zero risk of the freeze hazard from
`aa3_objectindex_solved.md` since nothing ever stops — confirmed the server
was still `S (sleeping)` immediately after. Results:

```
GEngine                (*0x23f221c)        = 0x22b36c00
GEngine's vtable                            = 0x1e993d8
GEngine vtable[0x13c]  (the call target)   = 0xa3b380
background-task obj #1 (*0x23a7d64)        = 0x25abec8
its vtable[0x1c]        (the call target)  = 0xc39df0
background-task obj #2 (*0x25a45f0)        = 0x0  (not instantiated right now)
```

**`0xa3b380` disassembled cleanly as a complete, self-contained function**
(`SUB ESP,0x14` prologue through `RET 0x4`, span `0xa3b380`–`0xa3b5df`, ~607
bytes) — and it is **not** `UWorld::Tick`. Its actual behavior: increments a
64-bit tick counter (`ADD [025abe68],1` / `ADC [025abe6c],0` — matches the
old map's documented tick-counter globals exactly, confirming this address
really is reached from here), reads `QueryPerformanceCounter`-style timing,
does an FPS-cap/hitch-warning check with several distinct log-string
arguments, and calls a handful of *other* virtuals through its own vtable at
even higher offsets (`0x14c`, `0x150`, `0x154`) passing float args — all
consistent with **FPS-chart / hitch-detection stats bookkeeping**, not world
simulation. So the `vtable+0x13c` slot is a real, confirmed call site, but
it's some per-frame stats/profiling hook, not `UGameEngine::Tick` itself in
the "drives `GWorld->Tick()`" sense originally assumed.

**`0xc39df0`** (the other main-loop virtual call, through the "background
task processor" object) disassembled just as cleanly and confirms the old
map's own description was right: SEH-wrapped, guarded by
`EnterCriticalSection`/`TryEnterCriticalSection`-style IAT calls
(`0x1de5170`/`0x1de516c`), draining what looks like a generic task queue
(`CALL 0x00c39d60` sub-helper). Genuinely a background task-queue drain, not
world/network tick either.

**Continued past the `GEngine` tick call in `FUN_00406870` itself** (up to
`0x4069af`) and found the call into the old map's own documented timing
function right where expected:

```
0040695c: CALL EAX            ; GEngine vtable[0x13c] = 0xa3b380 (FPS/stats, above)
0040695f: CALL 00beeed0       ; unidentified helper
00406967: CALL 00404fc0       ; *** FUN_00404fc0 — Engine_UpdateTicksAndDeltaTime,
                               ;     confirmed same address as the old map's
                               ;     documented timing function ***
0040696c: CMP [02579e60],ESI
00406984: LEA ECX,[EBP+-0x20]
00406987: CALL 00c29ec0       ; container/array growth pattern (TArray::AddZeroed-style)
```

This is a good cross-check (the old map's timing-function address is
independently confirmed reachable from exactly where expected), but it still
hasn't reached a `GWorld`/`UNetDriver` call — the trace was stopped here for
this session, not because the function ended (it continues further; the
`0xc29ec0` array-growth call is a live, unexplored lead worth following
first: it's shaped like adding an item to a queue, which is at least
consistent with the — still unconfirmed — hypothesis below).

**Open hypothesis worth testing next:** given *neither* of `FUN_00406870`'s
two direct virtual calls reaches `GWorld`/`NetDriver`, and one of them is
confirmably a task-queue drain, it's plausible that world simulation and/or
networking are driven through that **same queued-task mechanism** rather
than a direct synchronous call from the main loop — i.e. "tick the world" is
itself an enqueued task, not an inline call. This would be a genuinely
useful, non-obvious architectural finding for the Linux reimplementation if
confirmed (`aa3-linux-server/src/game/game_loop.cpp` currently assumes a
direct synchronous tick). Not yet confirmed either way.

**Everything downstream — whenever `UWorld::Tick` is actually reached —**
still leads to `UNetDriver::TickDispatch` (packet receive) /
`UNetDriver::TickFlush` (bunch send, where `UActorChannel::ReplicateActor`
lives), i.e. exactly the "Subsystem 6: Network & Replication" the old map
left as a stub. That part of the picture (§4/§5 below) is unchanged and
independently well-verified (live-captured against real gameplay) — only the
*exact call chain reaching it* from the main loop remains open.

### 3.1 `UNetDriver::TickDispatch` / `TickFlush` — found directly, via its vtable in `.rdata`

Rather than keep chasing the main-loop trace, found `UNetDriver`'s actual
vtable **statically, no live process needed at all**: the `MOV [ESI],
0x1ef8730`-style instruction is a C++ constructor's vtable assignment (`this
->vfptr = &vtable`); found a match at `0x9e1050` by walking function
boundaries backward from the known `UNetDriver::LowLevelDestroy` (`0x9e0ff0`,
itself found by locating a debug string it logs, `"UNetDriver::
LowLevelDestroy"`, then scanning `.text` for the one `PUSH <string VA>` that
references it). That first vtable hit turned out to belong to a different,
adjacent class — but scanning `.rdata` for the *value* `0x9e0ff0` (i.e. "does
this address appear as a vtable entry anywhere") found the **real**
`UNetDriver` vtable at `0x1ef8888`, confirmed by `LowLevelDestroy` and
`LowLevelGetNetworkNumber` sitting at consecutive slots 70/71, and the whole
table terminating right before the literal UTF-16LE string `"UNetDriver"` in
`.rdata` — about as strong a confirmation as static analysis gets. Dumped all
82 real slots and disassembled the ones in the same `0x9d`/`0x9e` code
region as the two known functions (slots 72–81, i.e. `UNetDriver`-specific
virtuals, as opposed to slots 0–69's generic `0x41ae`/`0xbdxxxx`-range
`UObject`-family boilerplate seen everywhere else in the binary):

| Vtable slot | Address | Identified as | Evidence |
|---|---|---|---|
| 75 (+0x12c) | `0x9de9c0` | **`UNetDriver::TickDispatch`** | Updates bandwidth-rate stats (fields `+0xb8`/`+0xbc`) if the stat interval elapsed; checks `this->ServerConnection` (`+0x54`) — null in dedicated-server mode; calls a virtual **on itself** (`vtable+0x13c`) for the raw socket receive/demux step (this is where the per-tick "recv packets, find-or-create the right `UNetConnection` by sender address" work happens); then loops `ClientConnections` (`+0x48`/`+0x4c`, a `TArray`) **forward**, calling `vtable+0x134` on each — per-connection housekeeping after dispatch |
| 76 (+0x130) | `0x9dec60` | **`UNetDriver::TickFlush`** | `this->TimeSeconds (+0x60) += DeltaSeconds`; resets per-tick `InBytes`/`OutBytes`-shaped counters (`+0xa0`/`+0xa4`); loops `ClientConnections` **backward** (`+0x4c` count down to 0 — the standard UE3 safe-removal-during-iteration idiom), and for each connection with `State==1` (open) calls `vtable+0x148` — this is `UNetConnection::Tick()`/flush, the path that eventually reaches `UActorChannel::ReplicateActor` and the already-verified `WriteIntWrapped` (§5) |

**Found a bonus, more direct lead while checking for static callers.**
`TickDispatch` has 0 direct callers (fully virtual, as expected). `TickFlush`
has exactly 2 — `0xca32b5` (unexplored) and **`0x9fa490`**, which
disassembles as a `UNetDriver`-own method (`this` passthrough confirmed: no
intervening pointer load before the call, so the caller's own `this` — same
field layout, `+0x54` checked the same way — is directly `UNetDriver*`) whose
very first action after the SEH-less prologue is calling `TickFlush(
DeltaSeconds)` **non-virtually** (compiler-devirtualized, since it statically
knows the concrete type), then checks its own `ServerConnection` state
(`==2` pending or `==3` open) and calls a further virtual through a member
sub-object at `+0x198`. This is very likely **`UNetDriver::Tick(FLOAT
DeltaSeconds)`** — a higher-level per-driver orchestrator. It also has 0
direct callers, but *does* appear once in `.rdata` at `0x1efe678` — a third,
distinct vtable slot.

**Tried to identify that vtable's owning class and hit a real technique
limitation, worth recording.** The "scan backward for the first non-code
dword" trick that reliably found `UNetDriver`'s vtable start (§3.1 top)
**does not work when vtables are packed back-to-back with no separator** —
running it here walked straight through the actual table boundary and
merged in an adjacent, unrelated class's vtable (surfaced a real but
irrelevant class name, `UBeastLightEnvironmentBox`, a Lightmass/Beast
lighting actor with zero connection to networking — a coincidental
neighbor in `.rdata`, not the answer). **Don't trust this heuristic's
"start" result without a corroborating check** (e.g. a known-function slot
match, like what confirmed the real `UNetDriver` table) — when no such
anchor is available, this specific sub-question is better suited to a live
read (`GWorld`'s vtable pointer directly, once its global address is found)
than more static guessing.

**Net effect: the actual network-tick functions are now fully identified
and characterized purely from static data** (`.rdata` vtables + `.text`
disassembly, zero live-process interaction for this specific part) — only
the very top of the call chain connecting them back to `FUN_00406870`/
`GEngine::Tick` remains open.

## 4. Connection / handshake layer (inherited from `PROTOCOL_FUNCTION_MAP.md`)

| Stage | Function | Role |
|---|---|---|
| Driver-level dispatch | `UNetDriver::TickDispatch` — `0x9de9c0` (§3.1, new this session) | Per-tick raw socket receive/demux (server mode: `vtable+0x13c` call on itself), then per-connection housekeeping loop |
| Packet receive entry | `UNetConnection::ReceivedPacket` — `FUN_00ca3260` | Raw UDP entry; header/ack handling. Almost certainly what `TickDispatch`'s `vtable+0x13c` call reaches, one level down — not yet directly confirmed (0 static callers found, expected since it's virtual) |
| Channel dispatch | `UChannel::ReceivedBunch` — `FUN_00ca3f40` | Base bunch routing to the right channel type |
| Control channel | `UControlChannel::ReceivedBunch` — `FUN_009f6940` | All `NMT_*` handshake opcodes (`HandshakeStart`/`Challenge`/`Response`/`Complete`, then text `USES`/`CHALLENGE`/`LOGIN`/`WELCOME`) |

Cross-checked this session: none of `ReceivedPacket`, `ControlChannel::
ReceivedBunch`, nor `UPackageMapLevel::SerializeObject` (`0x9dfe20`, from
`AA3_ACTOR_REPLICATION.md` §11.7.2) have *any* direct (`E8 rel32`) static
callers anywhere in `.text` — confirming they're only ever reached via
virtual dispatch (expected: `ReceivedBunch` is overridden per channel type,
`ReceivedPacket`/`SerializeObject` are UE3 virtuals too). `UChannel::
ReceivedBunch` (`0xca3f40`) *does* have 2 direct callers (`0xc8fb0e`,
`0xc90024`) — worth checking those two sites next if the receive-side
dispatch chain needs to be nailed down precisely.

## 5. Actor replication internals (this session — `AA3_ACTOR_REPLICATION.md`)

Everything below was found and **live-verified against the real running
server** (not just static guesswork) in the immediately preceding phase of
this project (§11.7 of `AA3_ACTOR_REPLICATION.md`):

| Function | Address | Notes |
|---|---|---|
| `Bunch.WriteIntWrapped` | `0xc80fa0` | Writes the `RepIndex`/`FieldNetIndex` (and other bounded counters — see below); live-captured 8000+ real hits, exact match against independently-computed class `MaxIndex` for `WorldInfo`=28 and `AA_GameReplicationInfo`=133 |
| `UPackageMapLevel::SerializeObject` | `0x9dfe20` | Object-reference (de)serialize; `this` in `ECX` |
| `UPackageMap::List` read (thiscall) | offset `0x40`/`0x44` in `UPackageMap` | `FPackageInfo` array (`Data`/`Num`); struct is `0x44` bytes, `ObjectBase`@`0x1c`, `ObjectCount`@`0x20` |
| `FName` → string resolution | call site `0xc09d10` → `0xc09a70` | `Names = *(FNameEntry**)0x25d2418`; **1164 direct static callers found this session** — this is a ubiquitous low-level string utility (every log/error-format call in the engine goes through it), not useful as a "path" node, just a reusable resolver |
| `ReplicateActor`'s property loop (approx.) | `~0x9f3641`–`0x9f3cb2` | 2 of `WriteIntWrapped`'s 9 static call sites live here; not yet pinned to an exact function boundary |
| Packet/bunch header field writer(s) (approx.) | `0x9e6e79`–`0x9e761b` (6 call sites) | Tight cluster, almost certainly ack/sequence-number header fields (explains the round power-of-two `MaxIndex` values — `4096`/`2048`/`16384`/`1024` — seen in every capture log, which obviously aren't class property counts) |
| Unidentified 9th `WriteIntWrapped` caller | `0x6ec8b7` | Isolated, far from the other 8 — leading candidate for a *static-object-reference* bound (package `ObjectCount`, not class `MaxIndex`) per the `MaxIndex=139` investigation in `AA3_ACTOR_REPLICATION.md` §11.7.8c |

`WriteIntWrapped` having **9 distinct static call sites** (found via a raw
`.text` scan for `E8 rel32` targeting `0xc80fa0` — see §6) is the concrete
reason the long-running `MaxIndex=139` "which class is this?" investigation
turned out to have the wrong premise: not every captured `(Value, MaxIndex)`
pair is a class `RepIndex` bound. See `AA3_ACTOR_REPLICATION.md` §11.7.8c for
the full writeup.

## 6. Technique notes (for continuing this map)

- **Static disassembly of an arbitrary address, without full binary
  analysis**: use `analyzeHeadless <proj> <name> -process AA3Game.exe
  -readOnly -noanalysis -scriptPath <dir> -postScript DisasmCalls.java
  <hexAddr> <hexLen>` (script lives in the session scratchpad, not
  committed — trivial to recreate: a `GhidraScript` that builds a
  `DisassembleCommand(set, set, false)` — **restricted to its own window,
  `followFlow=false`** — then lists instructions, flagging `CALL`). The
  `-readOnly` flag discards all changes on exit so the shared
  `AA3_Decomp/AA3_Reversed.gpr` project is never mutated.
  - **Keep windows small (≤ ~150 bytes) and step incrementally.** Passing
    `restrictedSet=null` with `followFlow=true` (an earlier attempt) spirals
    into disassembling unbounded reachable code from wherever you start —
    from the process entry point this attempted to flood-fill a huge
    fraction of the 26 MB `.text` section and had to be killed. Even with
    `followFlow=false`, some regions of this binary (e.g. right after the
    entry trampoline, likely dense IAT-thunk tables) are pathologically slow
    to disassemble in one large request for reasons not fully root-caused —
    small windows + manually continuing from the last instruction's address
    avoids the issue entirely and is fast (sub-5-second) every time it's
    been tried.
  - **Stale lock hazard**: killing a hung `analyzeHeadless` process can leave
    `AA3_Decomp/AA3_Reversed.lock` behind, which makes every subsequent
    invocation hang silently (no error, just never proceeds past "Execute
    script"). Symptom: the script never prints anything, even its first
    `println`. Fix: confirm no Ghidra/java process is running
    (`ps aux | grep -i ghidra`), then `lsof <project>.lock` to confirm
    nothing holds it, then delete it.
- **Finding all direct (`E8 rel32`) static callers of a known address**: a
  hand-rolled PE section-table parser + linear `.text` scan (no Ghidra
  needed at all for this — pure Python, `struct.unpack`) — much faster than
  Ghidra's reference database for a project that's never had full
  auto-analysis run (`ReferenceManager.getReferencesTo()` returns empty for
  any address Ghidra hasn't actually disassembled/analyzed, which is nearly
  all of `.text` in this project — confirmed via `FindWIWCallers.java`
  returning 0 xrefs for `0xc80fa0` despite 9 real static callers existing).
  **This technique is blind to indirect/virtual calls** (`CALL
  [reg+offset]`) — confirmed this is the reason `ReceivedPacket`,
  `ControlChannel::ReceivedBunch`, and `SerializeObject` all show 0 static
  callers despite obviously being called (they're UE3 virtuals). For
  virtual-heavy code (i.e. most of this engine), resolving real call targets
  requires either finding the constructing function's vtable-assignment
  literal, or a live memory read.
- **One-shot live memory reads are much lower-risk than the breakpoint
  captures used earlier in this project.** `gdb -p <pid> --batch -ex 'print
  ...' -ex detach -ex quit` (no `break`, no `continue` loop) just peeks
  memory while the process keeps running — none of the "gdb killed
  mid-breakpoint-stop freezes the whole server" hazard documented in
  `aa3_objectindex_solved.md` applies, since nothing ever stops. Only
  constraint: only one process can `ptrace` a given PID at a time, so this
  fails cleanly (not dangerously) if another gdb session already has it.

## 7. What's still open

1. ~~Resolve `GEngine` vtable slot `0x13c`'s real address~~ **DONE** — one-shot
   live read resolved it to `0xa3b380`, but disassembling it revealed it's an
   FPS-chart/stats hook, not `UWorld::Tick` itself (see §3 update). The
   *literal-resolution technique* worked exactly as planned; the *content*
   just wasn't what was assumed going in.
2. ~~Find where `GWorld`/`UNetDriver` actually gets ticked from~~ **Mostly
   DONE** — §3.1 found `UNetDriver::TickDispatch` (`0x9de9c0`) and
   `UNetDriver::TickFlush` (`0x9dec60`) directly and conclusively, purely
   from static data (found the real `UNetDriver` vtable in `.rdata` at
   `0x1ef8888`, confirmed by two known-named functions sitting at consecutive
   slots + the literal string `"UNetDriver"` immediately following the
   table). **What's still open is only the very top of the chain**: a
   non-virtual `UNetDriver` method at `0x9fa490` (likely `UNetDriver::
   Tick(FLOAT)`, calls `TickFlush` directly) is itself dispatched through a
   *third*, not-yet-identified vtable at `.rdata` VA `0x1efe678` — find that
   vtable's owning class (same "scan `.rdata` for the value" technique used
   throughout §3.1) and that will very likely BE the `GWorld`/`UEngine::Tick`
   connection this item originally asked for. The `FUN_00406870` main-loop
   trace and the "queued task" hypothesis are no longer the most promising
   path — this vtable lead is far more direct.
3. **Resolve the `UChannel::ReceivedBunch` (`0xca3f40`) direct callers**
   (`0xc8fb0e`, `0xc90024`) — these are real, static, already-found leads for
   nailing down exactly where receive-side channel dispatch happens.
4. **Identify `WriteIntWrapped`'s 9th, isolated call site** (`0x6ec8b7`) —
   confirms or refutes the `EngineMaterials`/static-object-reference
   hypothesis for the `MaxIndex=139` mystery (`AA3_ACTOR_REPLICATION.md`
   §11.7.8c).
