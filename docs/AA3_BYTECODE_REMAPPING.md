# AA3 UnrealScript Bytecode Opcode Remapping

## Opcode table so far

| Byte   | Vanilla name (`EExprToken`)   | Confidence | Evidence |
|--------|-------------------------------|------------|----------|
| `0x51` | `EX_EndOfScript`               | High | Absolute last byte, every function, no exceptions; engine source itself guarantees this (sec 4, sec 4c) |
| `0x0c` | `EX_Return`                    | High | Empty-body `Reset` cases, zero ambiguity (sec 4) |
| `0x1f` | `EX_Nothing`                   | High | `Return`'s void operand + reused in optional-parm preambles (sec 4) |
| `0x1a` | `EX_True`                      | High | ~15 independent `return true;` cases, zero exceptions (sec 4b) |
| `0x1e` | `EX_False`                     | High | ~15 independent `return false;` cases, zero exceptions (sec 4b) |
| `0x49` | `EX_ReturnNothing`             | High | Operand shape (4-byte property-index) + numeric matches, now explained exactly by vanilla's own documented semantics (sec 4a, sec 4c) |
| `0x20` | one of `NativeParm`/`LocalVariable`/`InstanceVariable`/`DefaultVariable`/`LocalOutVariable`/`StateVariable` (all share the identical "just a property-pointer" shape) | Medium (shape confirmed, exact identity among 6 candidates still open) | sec 4c |
| `0x60-0xFF` | **unmapped — identical to vanilla** (native-function-call range) | High | Numerically exact: `61 17 04` = `(0x61-0x60)*256 + 0x17` = 279 = `Destroy()`'s own real on-disk `iNative`, matching vanilla's `EX_ExtendedNative=0x60`/`EX_FirstNative=0x70` formula exactly (sec 4c) |

Still unresolved: control flow (`JumpIfNot`, `Jump`), calls to *script*
(non-native) functions (`FinalFunction`/`VirtualFunction`,
`EndFunctionParms`), and compound expressions (`Context`,
`InstanceVariable`) — see sec 6. Their exact **operand structure** is now
known precisely from `ScriptSerialization.h` (sec 4c) even though their
AA3 byte *values* aren't yet identified.

## 1. Motivation

Every function body decompiled via UELib (`OnlineSubsystemAA3/uscript_decompiled/`)
that has been checked so far — across this entire project, spanning many
sessions — fails partway through with either `/* Statement decompilation
error: Index was out of range. Must be non-negative and less than the size
of the collection. (Parameter 'index') */` or a garbled, structurally
impossible modifier line (e.g. `static final iterator singular exec event
operator(116)`). This was previously assumed to be an AA3-specific
oddity affecting only a handful of functions (see
`AA3_ACTOR_REPLICATION.md` sec 17-18: `aaController`, `ClientEndHoldBreath`,
`ServerVoteKick`, `ServerMapVote`). **That assumption was wrong.** Checking
`ClientMessage` — a function whose *signature* was independently confirmed
clean and which this whole project had been treating as "known good" —
shows the exact same corruption in its own decompiled body
(`OnlineSubsystemAA3/uscript_decompiled/Engine/PlayerController.uc:1056-1064`).
The corruption is not AA3-specific-function-specific. It affects
essentially every non-trivial function body in every package, base-engine
classes included.

## 2. Root cause: AA3 remaps the bytecode opcode table, exactly like its UE2 predecessor

`uelib-src` (the actual C# source for the UELib decompiler used throughout
this project, not just the compiled DLL) already has an `AA3` build entry
(`src/UnrealPackage.cs:685`, `[Build(568, 0)] AA3`), added in an earlier
session — this is confirmed by the very specific comment describing AA3's
LZO+XOR obfuscation, which this project independently reverse-engineered.
But unlike every *other* heavily-modified UE3 fork UELib supports
(`ME1`/`ME2`/`ME3` → `EngineBranchSFX`, `Borderlands` → `EngineBranchWillow`,
etc.), the `AA3` entry has **no `[BuildEngineBranch(...)]` attribute** — it
silently falls back to `DefaultEngineBranch`, i.e. the stock UE3 opcode
table (`EExprToken` from `UnrealEngine3/.../Core/Inc/UnStack.h`).

Decisive precedent: `src/Branch/UE2/AA2/EngineBranch.AA2.cs` — a *complete,
already-solved* custom `BuildTokenMap()` for **America's Army 2** (the direct
predecessor, UE2-based). Someone already reverse-engineered AA2's entire
opcode table because AA2's compiler emits opcodes in a different order than
stock UE2/UE3. Given AA3 comes from the same developer lineage and the
observed symptom (garbage from nearly the first byte of almost every
function) is structurally identical to what a wrong opcode table produces,
**AA3 almost certainly needs its own `EngineBranchAA3` with a from-scratch
`BuildTokenMap()`, the same kind of work that already exists for AA2.**
This is not a data-alignment bug or a decompiler defect to patch around —
it's a genuine "derive the opcode substitution table" reverse-engineering
task, comparable in scope to what AA2's contributor already did.

## 3. Tooling built

- **`aa3-linux-server/tools/build_manifests/main.cpp --dump-all-bytecode
  <pkg.u> <max_size>`**: batch-dumps every `Function` export's raw on-disk
  bytecode (hex), filtered to `ScriptBytecodeSize <= max_size`, so short
  functions (the most tractable starting corpus) can be gathered in bulk.
  Bytecode starts exactly 44 bytes into a Function export's serialized body
  (`NetIndex(4)+None-tag(8)+SuperStruct(4)+Next(4)+ScriptText(4)+
  Children(4)+CppText(4)+Line(4)+TextPos(4)+ScriptBytecodeSize(4)`,
  `UStructReader::ReadHeader`'s own already-verified layout for a
  non-class `UStruct`).
- **`aa3-decomp/BytecodeProbe.cs` (`dotnet run -- bytecode <pkg> <FuncName>`)**:
  loads a package via the real UELib, finds a named `UFunction`, and dumps
  its `ByteCodeManager.DeserializedTokens` one at a time (index, byte
  position, size, C# token *type name*, and a per-token `Decompile()`
  wrapped in its own try/catch) — unlike the whole-statement decompiler's
  single generic catch block, this pinpoints exactly which token and byte
  position a given exception comes from. Also prints `FunctionFlags`,
  `RepOffset`, and `RepReliable` via UELib's own already-versioned flag
  decoder (not a hand-rolled byte-offset guess — see sec 5 for why that
  matters).

Both tools work directly off the real cooked packages; neither depends on
or is affected by UELib's own token-decoding failure — `--dump-all-bytecode`
never touches `ByteCodeManager` at all, and `BytecodeProbe` only uses it to
*observe* where things go wrong, not to trust its conclusions.

## 4. Confirmed finding: three distinct zero/small-operand tokens, not one "tail pair"

Every function body examined — from trivial 2-byte native stubs
(`DetachFromAny`, `GetRotation`, `FindBase`, hundreds of others across
`Engine.u`) up to complex 84-163-byte scripted functions
(`ServerViewSelf`, `ServerMapVote`) — ends in the same two bytes, `1f 51`.
The original framing of this (a single indivisible "tail pair") was too
coarse; adding outer-class names to `--dump-all-bytecode` (sec 3) made it
possible to pair specific same-named functions 1:1 against their real,
clean UE3 source (`AIController.uc`, `Actor.uc`, `SequenceOp.uc`,
`GameInfo.uc`), which separates this into three independent single-byte,
zero-*extra*-operand tokens:

- **`0x51` = end-of-function-body terminator.** Always the absolute last
  byte, in every function examined without exception (natives, empty
  stubs, complex bodies alike).
- **`0x0c` = `EX_Return`.** Takes one following expression as the return
  value. Decisive evidence: `Actor.Reset` and `SequenceOp.Reset` are both
  declared as bare `event Reset();` / `function Reset();` — a semicolon,
  **zero statements**, nothing to misgroup against — and both compile to
  exactly `0c 1f 51` (3 bytes). `AIController.Reset` (`{ Super.Reset(); }`,
  one real statement, no explicit return) compiles to 17 bytes ending in
  the same `...0c 1f 51`, i.e. real-statement-bytes + implicit
  Return(Nothing) + terminator — matching vanilla UE3's own behavior of
  always emitting a trailing implicit return, executed or not.
- **`0x1f` = `EX_Nothing`.** A true zero-operand placeholder value/expr
  token, used as `EX_Return`'s operand for void functions (the case above)
  — *and* reused in unrelated contexts, e.g. three `1f`s in a row appear at
  the very start of `AddImpulse`'s optional-parameter preamble. This means
  `0x1f` is a general-purpose placeholder, not something inherently about
  returning — which **resolves the sec 6 ambiguity from the first pass**:
  `ServerViewSelf`'s leading `0x1f` byte is not in tension with the
  Return-tail finding at all, since `0x1f` was never uniquely "the return
  op." It's still most consistent with default-value/optional-parameter
  bytecode for the `TransitionParams` param (sec 6, still not fully
  nailed down, but no longer a live contradiction).

**These three are genuinely confirmed, high-confidence findings** — not
guesses dressed as fact, unlike the `FunctionFlags`-direction attempt
earlier in this project (`AA3_ACTOR_REPLICATION.md` sec 18) that got
reverted for exactly that reason.

## 4a. A real breakthrough: `0x49` + 4-byte value is an export-index reference, numerically proven

`AIController.CanFireWeapon(Weapon Wpn, byte FireModeNum) { return TRUE; }`
(real source, one line, trivial) compiles to `0c 1a 0c 49 56 1b 00 00 51`
(9 bytes). `GameInfo.ShouldReset(Actor ActorToReset) { return true; }`
(also real, also trivial) compiles to `0c 1a 0c 49 b3 31 00 00 51`.  Both
have an unexplained `0c 49 XX XX 00 00` group between the boolean literal
and the terminator. Cross-checking the 4-byte little-endian value in each
against `--dump-exports`' actual export table is decisive:

- `ShouldReset`: value `b3 31 00 00` = `0x000031b3` = **12723**. Export
  12723 in `Engine.u` is `ObjectProperty ActorToReset`, outer
  `ShouldReset` — **`ShouldReset`'s own parameter**, by exact numeric
  match, not a guess.
- `CanFireWeapon`: value `56 1b 00 00` = `0x00001b56` = **6998**. Export
  6998 is `ByteProperty FireModeNum`, outer `CanFireWeapon` — **its own
  second/last parameter**, again an exact match.

This proves `0x49` takes a **raw 4-byte export-table index** as its
operand (not vanilla UE3's compact variable-length object reference) —
i.e. it's in the same family as vanilla's `EX_InstanceVariable`/
`EX_LocalVariable`, just with a fixed-width index instead of a compact
one.

**Follow-up (much larger batch, ~30 additional functions):** widening the
outer-class-tagged `--dump-all-bytecode` cross-check to every
zero-parameter `return true;`/`return false;` getter findable in real UE3
source (`Weapon`, `Pawn`, `Controller`, `GameInfo`, `PlayerController`,
`Actor`, `Vehicle`, `GameReplicationInfo`, `PlayerReplicationInfo` —
`HasAnyAmmo`, `IsDead`, `IsSpectating`, `IsStationary`, `CanCommunicate`,
`CanSplash`, `CheatWalk/Ghost/Fly`, `IsCoopMultiplayerGame`, etc., ~15 of
each polarity) resolved the "why does it reference an unused parameter"
puzzle cleanly: **the `0x49` marker's target is always the *last
Children-chain entry* of the function — its last declared
parameter/local if one exists, and (numerically verified on 5 independent
zero-parameter cases: `Controller.IsDead`, `Dead.IsDead` ×2,
`PlayerController.CanCommunicate`, `AIController.GetOrderObject`) **the
function object itself when there are none.** And it is present *only* on
functions that declare a return type — every checked `void`/no-return
function (`AIController.Reset`, `Actor.Reset`, `SequenceOp.Reset`) ends in
plain `0c 1f 51` with no `49`-marker at all, while every checked
`function bool X(...)`-style getter, regardless of body complexity, ends
in `[[0c][1a-or-1e]] 0c 49 <last-child-or-self> 51`. This is best
explained as AA3/UE3 tagging the function's implicit **return-value
slot** — for primitive types like `bool` that get no separate exported
`ReturnValue` property in this cooked build, the marker structurally
falls back to "last real child" (or self, if the function is niladic) —
consistent with real UE3 compiler behavior of always auto-appending a
hidden `ReturnValue` property to a value-returning function's parameter
chain.

## 4b. High-confidence: `0x1a` = `EX_True`, `0x1e` = `EX_False`

The same ~30-function batch gives clean, zero-exception, two-sided
cross-validation: **every** function whose real UE3 source is exactly
`return true;`/`return TRUE;` places `0x1a` right after the `0x0c`
(`Return`) opener, and **every** `return false;`/`FALSE;` case places
`0x1e` in that slot instead — checked across ~15 independent functions
per polarity, multiple classes, zero contradictions (e.g.
`Actor.IsStationary`→true→`1a`, `Pawn.IsStationary`→false→`1e`,
`RoundEnded.GamePlayEndedState`→true→`1a`, `Controller.GamePlayEndedState`
→false→`1e`, `Dead.IsDead`→true→`1a` ×2, ...). This is now high, not
medium, confidence.

## 4c. Major discovery: the ground-truth operand parser exists in-repo, and the native-call range is *not remapped at all*

`UnrealEngine3/Development/Src/Core/Inc/ScriptSerialization.h` (included
into `UStruct::SerializeExpr` via `#include` + macro) is the *authoritative,
already-in-this-repo* reference for **every vanilla opcode's exact operand
structure** — line 234 onward is a straight `switch(Expr)` over
`EExprToken` with one case per token, each showing exactly what it reads
(a recursive sub-expression, a raw value, a property/function pointer, a
2-byte code-skip offset, etc.). This resolves two things at once:

1. **What `0x49`'s shape actually corresponds to.** Vanilla
   `case EX_ReturnNothing: { XFER_PROP_POINTER; /* the return value
   property */ break; }` — a single property-pointer operand, no
   recursion. That's *exactly* `0x49`'s shape (sec 4a), and its own
   comment ("the return value property") is *exactly* what the
   export-index cross-checks found. This means `0x0c` (`EX_Return`, which
   recursively serializes a real value-expression) and `0x49`
   (`EX_ReturnNothing`, a separate, simpler token used for a value-typed
   function's implicit/fallthrough exit path) are two genuinely distinct
   vanilla tokens — not one construct as sec 4a first framed it. Every
   value-returning function gets a mandatory `0x49`-tagged exit appended
   regardless of whether it already has an explicit `return`, matching
   vanilla UE3's own always-append-a-safety-net-return compiler behavior.
2. **The property/function-pointer wire format.** `XFER_PROP_POINTER` /
   `XFER_FUNC_POINTER` both expand to `XFERPTR(T)`, which — despite
   reserving a full 8-byte (`ScriptPointerType`/`QWORD`, *always* 8 bytes
   even on 32-bit platforms, confirmed at `UnVcWin32.h:143`) slot in the
   **in-memory** reconstructed bytecode buffer — serializes to disk via
   `Ar << AlignedPtr`, which for a `UObject*` resolves to
   `ULinkerSave::operator<<(UObject*&)` (`UnLinker.cpp:5239`), which does
   `INT Save = Obj ? ObjectIndices(Obj->GetIndex()) : 0; return Ar << Save;`
   — a **plain, fixed 4-byte `INT`**, not a variable-length compact index.
   This is an exact, textual, in-repo confirmation that the sec 4a/4b
   4-byte little-endian export-index reads are the *correct* on-disk
   format, not a lucky guess.

**The biggest structural finding:** `DroppedPickup.Reset`/
`Projectile.Reset` (both `{ Destroy(); }`, verified real source) compile
to `61 17 04 0c 1f 51`. `Destroy` is `native(279) final noexport function
bool Destroy();` in `Actor.uc`, and directly reading its own on-disk
`iNative` field (added to `--dump-all-bytecode`'s output, sitting exactly
at `body_off + script_bytecode_size` per `UFunction::Serialize`,
`UnClass.cpp:2643`, which calls `Super::Serialize` — the bytecode-writing
call — *before* serializing `iNative`) gives **279**, confirming the
source literal wasn't renumbered. Vanilla's own native-call dispatch
(`ScriptSerialization.h:240-252`) is: opcode byte `>= EX_FirstNative`(0x70)
means *the opcode byte itself is the 1-127 native ID*; opcode byte in
`[EX_ExtendedNative(0x60), EX_FirstNative)` means a 2-byte extended
native ID via `(opcode - 0x60)*256 + next_byte`. Solving
`(0x61 - EX_ExtendedNative)*256 + 0x17 = 279` for `EX_ExtendedNative`
gives exactly **0x60** — vanilla's own unmodified value
(`UnStack.h:206-207`). **AA3 did not remap the native-function-call
opcode space (`0x60`-`0xFF`) at all** — only the "special" token range
below it (`0x00`-`0x5F`, covering `Return`/`Nothing`/`Jump`/`Context`/
`FinalFunction`/etc.) was shuffled. This roughly *halves* the size of the
actual unknown: any call to a `native` function can already be decoded
correctly today (opcode byte ⇒ iNative index ⇒ look up which UFunction in
the relevant class registers that iNative, exactly as vanilla does), and
only calls to *non-native* (pure script) functions still need their
`FinalFunction`/`VirtualFunction` byte values identified.

## 5. A related, separate open thread: `FunctionFlags` bit layout

`UFunction.FunctionFlags` (a `UnrealFlags<FunctionFlag>`) is UELib's own
*properly-typed, version-aware* flags accessor — not something this project
needs to hand-parse from raw bytes (the earlier raw-byte-offset attempt in
`AA3_ACTOR_REPLICATION.md` sec 18 was the wrong tool for this job; this is
the right one). But UELib's decode of `ServerMapVote`'s flags
(`0x4:Iterator;0x80000:Protected;0x10000;0x20000000;0x40000000;`) and
`ClientMessage`'s (`0x1:Final;0x4:Iterator;0x10000;`) don't match either
function's known real declaration (`unreliable server function
ServerMapVote(...)`, `reliable client event ClientMessage(...)`) — no
`Reliable`/`Client`/`Server`/`Event` flags show up at all, and `Iterator`
shows up on functions that have no business being UnrealScript iterators.
Since `DefaultEngineBranch` is also what's supplying the
bit-position-to-name table here, **this may be the exact same root cause
as sec 2** (AA3 needs its own branch profile) rather than a second,
independent bug — worth re-checking once a real `EngineBranchAA3` exists,
rather than chasing it separately.

## 6. What's NOT yet resolved

`ServerViewSelf`'s leading `0x1f` byte (sec 6's original open question) is
no longer a live contradiction — sec 4's revision already explains it:
`0x1f`(`EX_Nothing`) is a general-purpose placeholder reused in multiple
contexts, not uniquely "the return op", so it appearing at both the start
(likely `TransitionParams`-optional-parameter bytecode, still not
pinpointed exactly) and structurally-near-the-end of a function is no
longer inconsistent. What's still genuinely open:

- **`JumpIfNot`'s byte value.** Known operand shape from `ScriptSerialization.h`
  (`XFER(CodeSkipSizeType)` — a `WORD`, 2-byte jump-target — then a
  recursive boolean sub-expression), but no AA3 byte identified yet. Needs
  a function with a single, simple `if` and **zero optional parameters**
  to avoid the sec-4-`0x1f`-preamble confound `ServerViewSelf` had.
- **`FinalFunction`/`VirtualFunction`'s byte values** (calls to *non-native*　
  script functions — sec 4c showed native calls are already solved via
  the unmapped `0x60`-`0xFF` range, so this is specifically about calls to
  plain `function`/`event` targets). Known shapes:
  `FinalFunction` = `XFER_FUNC_POINTER` (4-byte function export-index,
  same encoding as sec 4c) + repeated `SerializeExpr` until
  `EndFunctionParms`; `VirtualFunction`/`GlobalFunction` = `XFER_FUNC_NAME`
  (an `FName`, not an index) + same repeated-until-`EndFunctionParms` loop.
  `AIController.Reset`'s `Super.Reset();` call is a **known FinalFunction
  call site** (super calls are always statically resolved, never virtual
  dispatch) not yet decoded byte-by-byte — a good next target since its
  full bytecode (`1c 3b 01 2d 1b 00 00 1e 28 04 12 00 00 04 0c 1f 51`) is
  already in hand and its call target (`AIController.Reset`'s own super,
  i.e. `Actor.Reset`, export-index known) can be checked numerically the
  same way sec 4a/4c did.
- **`EndFunctionParms`'s byte value** — needed to know where a call's
  argument list ends; likely identifiable from the same `Super.Reset()`
  case (zero arguments, so it should sit immediately after the function
  pointer with no argument expressions in between).
- **`Context`'s byte value and full 4-part shape**
  (`SerializeExpr`+`WORD`+`UField*`+`BYTE`+`SerializeExpr`) — needed for
  any member access through an object reference (`self.Foo`,
  `SomeStruct.Field`), which is extremely common and likely one of the
  highest-value remaining targets.
- `0x20`'s *exact* identity among 6 vanilla candidates that share its
  shape (`NativeParm`/`LocalVariable`/`InstanceVariable`/`DefaultVariable`/
  `LocalOutVariable`/`StateVariable`) — structural shape confirmed, exact
  one not disambiguated (sec 4c).
- Nothing here has been turned into an actual `EngineBranchAA3` class or
  fed back into UELib yet.

## 7. Suggested next steps (partially attempted)

1. ~~Decode `AIController.Reset`'s full 17-byte body byte-by-byte~~ —
   **attempted, inconclusive.** Naive vanilla-shape math (`FinalFunction`
   opcode + 4-byte func-pointer + `EndFunctionParms`, ≈6 bytes for a
   zero-arg call) predicts far fewer bytes than the observed 14
   (`1c 3b 01 2d 1b 00 00 1e 28 04 12 00 00 04`, before the already-known
   `0c 1f 51` tail). Leading hypothesis, **not verified**: this cooked
   build may actually retain `EX_DebugInfo` tokens after calls (vanilla's
   `HANDLE_OPTIONAL_DEBUG_INFO` macro, `ScriptSerialization.h:207-230`,
   only strips these under `#if CONSOLE`; a PC-server cooked build might
   keep them) — `EX_DebugInfo`'s shape is `INT Version + INT Line +
   INT CharacterPos + BYTE OpCode` (13 bytes), which would explain a lot
   of the "extra" bytes seen not just here but throughout this whole
   session's traces. Not confirmed; needs a dedicated check (e.g. do the
   candidate `INT`s decode to plausible source line numbers for the
   right `.uc` file?) before relying on it. **Do not carry the `3b 01`
   grouping from earlier drafts of this doc forward as settled — it was
   a tentative reading, superseded by this open debug-info question.**
2. Find a **zero-optional-parameter**, single-`if` function (real source,
   cleanly cross-referenceable) to nail `JumpIfNot` without sec-4's
   `0x1f`-preamble confound.
3. Build a frequency table across a much larger corpus (all of `Engine.u`'s
   short functions, not just size<=15) — the workhorse opcodes
   (`LocalVariable`, `InstanceVariable`, `Context`, `FinalFunction`,
   `EndFunctionParms`) should dominate by a wide margin and may be
   separable by position-within-function statistics even without full
   manual tracing of any single function.
4. Once 8-10 opcodes are confirmed with the same cross-validation rigor as
   sec 4 (not fewer, and not on single-example confidence), write a real
   `EngineBranchAA3 : DefaultEngineBranch` in `uelib-src`, wire it into the
   `AA3` build entry via `[BuildEngineBranch(typeof(EngineBranchAA3))]`,
   and re-run `aa3-decomp` against a handful of already-known-corrupted
   functions to check for a qualitative improvement (clean decompile, or
   at least a *different*, more plausible failure point) before trusting
   it broadly.
4. This is realistically a multi-session effort, comparable in scope to
   the `AA3 ObjectIndex mystery` and `AA3 RepIndex reconstruction` threads
   — not a single-sitting fix. Treat it that way rather than rushing.
