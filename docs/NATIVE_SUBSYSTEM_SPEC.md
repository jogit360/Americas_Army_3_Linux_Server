# AA3 Native Subsystem Specification (libaa3)

This document outlines the original native C API exported by `libaa3.dll` (and `libaa3.so`) that `OnlineSubsystemAA3.uc` binds to, and defines how these subsystems are handled in the new `aa3-linux-server` implementation.

## Overview of Native Subsystems

The original AA3 dedicated server relied heavily on an external shared library (`libaa3.dll` / `libaa3.so`) to interface with America's Army backend infrastructure. The `OnlineSubsystemAA3` UnrealScript class wraps these native calls.

The native API is broadly divided into four distinct subsystems:

1.  **Auth (Authentication & Profiles):** `aa3_auth_*`
2.  **MBS (Master Browser System):** `aa3_mbs_*`
3.  **ATS (Achievement Tracking System):** `aa3_ats_*`
4.  **STS (Statistics Tracking System):** `aa3_sts_*`

---

## 1. Auth Subsystem

### Original Native API (`libaa3.dll.0.c`)
The Auth subsystem managed logging into the AA3 central servers, retrieving player profiles, and handling server bans/admins.

*   `aa3_auth_init`
*   `aa3_auth_free`
*   `aa3_auth_server_login`
*   `aa3_auth_server_logout`
*   `aa3_auth_build_message_login`
*   `aa3_auth_build_message_logout`
*   `aa3_auth_communicate`
*   `aa3_auth_get_message_response_*`

### New Implementation Strategy
The legacy AA3 authentication servers no longer exist.
*   **Standalone Linux Server:** We bypass the need for this system by overriding `OnlineSubsystemAA3` methods in a custom UnrealScript package (if possible) or by forcing `bRequireAuthorization=false` in the server configuration.
*   **Steamworks Replacement:** For the standalone C++ server, we replace the `aa3_auth_server_login` logic with `SteamGameServer_Init` and `SteamGameServer_LogOnAnonymous()`. Player authentication is handled via `ISteamGameServer::BeginAuthSession` using Steam Auth Tickets.

---

## 2. MBS Subsystem (Master Browser System)

### Original Native API (`libaa3.dll.0.c`)
The MBS subsystem handled server discovery, heartbeats, and reporting current game state to `mbs.aa3.americasarmy.com`.

*   `aa3_mbs_init`
*   `aa3_mbs_free`
*   `aa3_mbs_server_start_server`
*   `aa3_mbs_server_stop_server`
*   `aa3_mbs_server_login`
*   `aa3_mbs_server_set_state_attribute_*` (integer, real, string)
*   `aa3_mbs_server_set_state_player_online`

### New Implementation Strategy
*   **Standalone Linux Server:** We stub out all MBS functionality. The server does not need to report to the legacy MBS.
*   **Steamworks Replacement:** Server visibility is handled entirely by the Steam Master Server. We map the `aa3_mbs_server_set_state_attribute_*` logic to Steam's key-value tagging system (`ISteamGameServer::SetKeyValue`, `SetMapName`, `SetMaxPlayerCount`).

---

## 3. ATS Subsystem (Achievement Tracking System)

### Original Native API (`libaa3.dll.0.c`)
The ATS is the most critical subsystem for actual gameplay. It acts as an offline rules engine that tracks events, updates attributes, and triggers in-game scoring notifications (which determine round wins).

*   `aa3_ats_init`
*   `aa3_ats_register_rule_engine`
*   `aa3_ats_new_event`
*   `aa3_ats_set_event_real_attribute`
*   `aa3_ats_process_event`
*   `aa3_ats_get_result_players`
*   `aa3_ats_get_result_attributes`

### New Implementation Strategy
Since ATS dictates in-game scoring (e.g., neutralizing VIPs, completing objectives), we **must** emulate its behavior for the game mode to function correctly.
*   **Standalone Linux Server:** We will build a lightweight C++ scoring engine within the `aa3-linux-server` that mimics the ATS state machine. When the game engine calls `NativeAA3AtsProcessEvent`, we evaluate the event locally and trigger the `OnNewScoringEvent` UnrealScript delegate.

---

## 4. STS Subsystem (Statistics Tracking System)

### Original Native API (`libaa3.dll.0.c`)
STS was used to log granular telemetry (shots fired, movement paths, session info) to the backend for analytics.

*   `aa3_sts_init`
*   `aa3_sts_login`
*   `aa3_sts_new_event`
*   `aa3_sts_set_event_real_attribute`
*   `aa3_sts_process_event`
*   `aa3_sts_send_events`

### New Implementation Strategy
*   **Standalone Linux Server:** STS is entirely non-critical for gameplay. It can be safely stubbed to return success codes (e.g., `AA3_ARC_SUCCEEDED`) without doing any actual work. 

---

## Summary of Action Items for Native Mocking

To successfully run the AA3 dedicated server logic without `libaa3`, we must intercept the native calls declared in `OnlineSubsystemAA3.uc`.

1.  **Mock Auth:** Return success (`1`) for initialization and simulate an authorized state.
2.  **Mock MBS:** Discard all outgoing updates.
3.  **Emulate ATS:** Implement a basic rule engine to echo back scoring events to `OnNewScoringEvent`.
4.  **Mock STS:** Discard all outgoing statistic events.
