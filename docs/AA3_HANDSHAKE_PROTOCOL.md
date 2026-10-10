# America's Army 3 Handshake Protocol Specification

This document defines the 4-stage handshake protocol used by the America's Army 3 (AA3) client and server. This protocol is a proprietary extension of the standard Unreal Engine 3 (UE3) handshake, designed to work with Steamworks sockets and custom authentication.

## Layer 1: UDP Packet Structure
Every UDP packet on port 8777 (or configured game port) follows this format:
- **SessionUID (3 bytes)**: Identifies the session. Initial packets use a null UID (`00 00 00`) or a client-generated UID. Subsequent packets use a UID derived from the handshake challenge.
- **UE3 Header (Variable Bits)**: 
  - `bIsAck (1 bit)`
  - `PacketSequence (14 bits)`
  - `bHasPackageMap (1 bit)`
  - `bHasAcks (1 bit)`
- **Bunches Loop**:
  - `ChannelIndex (10 bits)`: AA3 uses 10 bits instead of the standard 7.
  - `bReliable (1 bit)`
  - `bOpen (1 bit)`
  - `bClose (1 bit)`
  - `BunchSequence (14 bits)`: Only if `bReliable=1`.
  - `bPartial (1 bit)`: If set, followed by `bPartialInitial` and `bPartialFinal`.
  - `DataBitCount (10 bits)`: AA3 uses 10 bits instead of the standard 14 for the payload length.
  - **Payload**: Opcodes and data.

---

## Stage 1: Binary Handshake Start
**Client -> Server**
- **Opcode**: `21` (0x15) - `HandshakeStart`
- **Data**: (Optional) NetVersion, EngineVersion.

**Server -> Client**
- **Opcode**: `4` (0x04) - `HandshakeChallenge`
- **Data**: `DWORD Challenge` (Random 32-bit integer).
- **Side Effect**: Server sets `SessionUID[0..2]` to the top 24 bits of the challenge.

---

## Stage 2: Binary Handshake Verification
**Client -> Server**
- **Opcode**: `7` (0x07) - `HandshakeResponse`
- **Data**: `DWORD Response`
- **Algorithm**: `CRC32("895fcf626f55798667e4e94cb7a636af <challenge_decimal>")`
- **Note**: The salt is from `UnChan.cpp`. The CRC is standard MPEG-2 style (Poly 0x04C11DB7).

**Server -> Client**
- **Opcode**: `5` (0x05) - `HandshakeComplete`
- **Data**: None.

---

## Stage 3: Protocol Negotiation (Hello)
**Client -> Server**
- **Opcode**: `21` (0x15) - `NMT_Hello`
- **Data**: `INT NetVer`, `INT EngineVer`, `FString GUID`.

**Server -> Client**
- **Multiple Bunches**:
  - **`USES` messages**: One per required package (`AA3Game`, `Engine`, etc.).
    - Opcode: `7` (Standard UE3) or Opcode `0` (Text-based `USES`).
  - **`CHALLENGE` text**: 
    - Format: `CHALLENGE VER=<ver> CHALLENGE=<hex_val>`
    - Opcode: `0` (Text Message).

---

## Stage 4: Game Admission (Login)
**Client -> Server**
- **Opcode**: `7` (0x07) - `NMT_Login`
- **Data**: `FString Response`, `FString URL`.
- **Note**: Response is a hash of the text challenge.

**Server -> Client**
- **Opcode**: `5` (0x05) - `NMT_Welcome`
- **Data**: `FString MapName`, `FString GameClass`.
- **Format**: `WELCOME LEVEL=<map> CHALLENGE=<val>`

---

## Findings for Implementation:
- AA3 Opcodes are shifted: 21 (Hello), 4 (Challenge), 7 (Response/Login), 5 (Complete/Welcome).
- Bit-alignment must be strict: 10-bit channel, 10-bit data size.
- Every packet MUST echo the client's SessionUID to be accepted.
