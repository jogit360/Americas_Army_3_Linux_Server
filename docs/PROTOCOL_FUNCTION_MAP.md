# AA3 Protocol Function Map: Client & Server Handshake

This document maps the network protocol logic between the **Unreal Engine 3 Source** (provided in the repo) and the **AA3 Binary Disassembly** (`DissAssembledFunctions/`).

## 1. Core Network Entry Points (Layer 1)

These functions handle the raw UDP packets before they are parsed into game messages.

| UE3 Source Function | Role | AA3 Binary (Approx) |
|---|---|---|
| `UNetConnection::ReceivedPacket` | Entry point for all UDP data; handles headers/acks. | `FUN_00ca3260` |
| `UChannel::ReceivedBunch` | Base bunch processing and channel routing. | `FUN_00ca3f40` |
| `UControlChannel::ReceivedBunch` | **CRITICAL:** Processes all `NMT_` handshake opcodes. | `FUN_009f6940` |

---

## 2. Binary Handshake Implementation (Stages 1 & 2)

Found in `UnrealEngine3/Development/Src/Engine/Src/UnChan.cpp`.

| Stage | Message Type | Opcode | Source Logic | Binary Location |
|---|---|---|---|---|
| **C->S** | `NMT_HandshakeStart` | **21** (0x15) | `UnChan.cpp:687` | `FUN_009f6940` (Switch Case 21) |
| **S->C** | `NMT_HandshakeChallenge` | **4** (0x04) | `UnChan.cpp:704` | `FUN_0046e830` (Dispatcher) |
| **C->S** | `NMT_HandshakeResponse` | **7** (0x07) | `UnChan.cpp:726` | `FUN_009f6940` (Switch Case 7) |
| **S->C** | `NMT_HandshakeComplete` | **5** (0x05) | `UnChan.cpp:757` | Trace-Matched Output |

---

## 3. Text-Based Handshake & Login (Stages 3 & 4)

After the binary handshake "unlocks" the connection, the game moves to text-based command negotiation.

| Command | Role | Opcode | Binary Reference |
|---|---|---|---|
| `USES` | Server tells client which packages are needed. | 7 (Std) or 0 (Text) | `FUN_009f6940` (Switch CaseUSes) |
| `CHALLENGE` | Server sends text-based salt challenge. | 0 (Text) | `FUN_009f6940` (Line 975030) |
| `LOGIN` | Client sends credentials and hash response. | 7 (Text) | `FUN_009f6940` (Line 975085) |
| `WELCOME` | Server tells client to load the map. | 5 (Text) | `FUN_009f6940` (Line 975110) |

---

## 4. Proprietary AA3 Cryptography (`appStrCrc`)

Found in `UnrealEngine3/Development/Src/Core/Src/UnMisc.cpp`.

- **Function**: `DWORD appStrCrc( const TCHAR* Data )`
- **Location**: `UnMisc.cpp:1641`
- **Binary Equivalent**: `FUN_004106c0` (Standard UE3 CRC Utility)
- **The Salt**: `"895fcf626f55798667e4e94cb7a636af"` (Found at `UnChan.cpp:737`)

---

## 5. Packet Framing (Steamworks Extension)

AA3 uses a custom socket implementation that adds a **24-bit SessionUID** prefix to all UDP traffic.

- **Source Reference**: `OnlineSubsystemSteamworks/Src/UnNetSteamworks.cpp`
- **Header Structure**:
  - `Connection->SessionUID[0..2]`
  - Derived from `HandshakeChallenge` bits.
  - Set in `UnChan.cpp:712`.

---

## 6. How to verify the "Hello" in the binary

If you want to see exactly how the binary handles the incoming 0x15 (21) opcode, look at **`FUN_009f6940`**.
1. Open `AA3Decompiled.c` or the function file.
2. Search for `case 0x15:`.
3. You will see it reading `param_2 + 0xf4` (The NetVersion) and then calling a Challenge generator.
