#include "auth/steam_auth.h"
#include "util/logging.h"

namespace aa3 {
namespace auth {

SteamAuthManager::SteamAuthManager() = default;

SteamAuthManager::~SteamAuthManager() {
    if (initialized_) {
        Shutdown();
    }
}

bool SteamAuthManager::Initialize(uint32_t bind_ip,
                                  uint16_t game_port,
                                  uint16_t query_port,
                                  const SteamServerProperties& props) {
    LOG_INFO("SteamAuthManager::Initialize - GamePort=%u QueryPort=%u Secure=%d",
             game_port, query_port, props.secure ? 1 : 0);

    props_ = props;

    const EServerMode mode = props_.secure
        ? eServerModeAuthenticationAndSecure
        : eServerModeAuthentication;

    SteamErrMsg err = {0};
    const ESteamAPIInitResult init_result = SteamGameServer_InitEx(
        bind_ip,
        game_port,
        query_port,
        mode,
        props_.version.c_str(),
        &err);

    if (init_result != k_ESteamAPIInitResult_OK) {
        LOG_ERROR("SteamGameServer_InitEx failed (result=%d): %s",
                  static_cast<int>(init_result), err);
        return false;
    }

    ISteamGameServer* gs = SteamGameServer();
    if (!gs) {
        LOG_ERROR("SteamGameServer() returned null after successful Init");
        SteamGameServer_Shutdown();
        return false;
    }

    // Immutable-after-LogOn properties.
    gs->SetProduct        (props_.product.c_str());
    gs->SetGameDescription(props_.game_description.c_str());
    gs->SetModDir         (props_.mod_dir.c_str());
    gs->SetDedicatedServer(true);

    // Mutable browser/state properties.
    gs->SetServerName      (props_.server_name.c_str());
    gs->SetMapName         (props_.map_name.c_str());
    gs->SetMaxPlayerCount  (props_.max_players);
    gs->SetBotPlayerCount  (props_.bot_count);
    gs->SetPasswordProtected(props_.password_protected);
    if (!props_.game_tags.empty()) {
        gs->SetGameTags(props_.game_tags.c_str());
    }

    gs->LogOnAnonymous();

    initialized_ = true;
    LOG_INFO("SteamGameServer initialized (anonymous logon pending)");
    return true;
}

void SteamAuthManager::RunFrame() {
    if (!initialized_) {
        return;
    }
    SteamGameServer_RunCallbacks();
}

void SteamAuthManager::Shutdown() {
    if (!initialized_) {
        return;
    }

    if (ISteamGameServer* gs = SteamGameServer()) {
        if (advertising_) {
            gs->SetAdvertiseServerActive(false);
            advertising_ = false;
        }
        if (logged_on_) {
            gs->LogOff();
        }
    }

    SteamGameServer_Shutdown();
    initialized_ = false;
    logged_on_   = false;
    LOG_INFO("SteamGameServer shutdown complete");
}

bool SteamAuthManager::BeginAuthSession(uint64_t steam_id,
                                        const uint8_t* ticket,
                                        uint32_t ticket_len) {
    ISteamGameServer* gs = SteamGameServer();
    if (!initialized_ || !gs) {
        LOG_ERROR("BeginAuthSession called before init");
        return false;
    }

    const CSteamID id(static_cast<uint64>(steam_id));
    const EBeginAuthSessionResult result =
        gs->BeginAuthSession(ticket, static_cast<int>(ticket_len), id);

    if (result == k_EBeginAuthSessionResultOK) {
        LOG_DEBUG("BeginAuthSession OK for SteamID=%llu",
                  static_cast<unsigned long long>(steam_id));
        return true;
    }

    LOG_WARN("BeginAuthSession failed for SteamID=%llu (result=%d)",
             static_cast<unsigned long long>(steam_id),
             static_cast<int>(result));

    if (sink_) {
        sink_->OnAuthResult(steam_id, 0, false, static_cast<int>(result));
    }
    return false;
}

void SteamAuthManager::EndAuthSession(uint64_t steam_id) {
    ISteamGameServer* gs = SteamGameServer();
    if (!initialized_ || !gs) {
        return;
    }
    gs->EndAuthSession(CSteamID(static_cast<uint64>(steam_id)));
    LOG_DEBUG("EndAuthSession for SteamID=%llu",
              static_cast<unsigned long long>(steam_id));
}

void SteamAuthManager::TryAdvertise() {
    if (advertising_ || !logged_on_) {
        return;
    }
    ISteamGameServer* gs = SteamGameServer();
    if (!gs) {
        return;
    }
    gs->SetAdvertiseServerActive(true);
    advertising_ = true;
    LOG_INFO("Server advertised on Steam master server");
}

void SteamAuthManager::OnSteamServersConnected(SteamServersConnected_t*) {
    LOG_INFO("Connected to Steam game server backend");
    logged_on_ = true;

    // Non-secure mode advertises immediately; secure mode waits for
    // GSPolicyResponse_t to confirm VAC status before advertising.
    if (!props_.secure) {
        TryAdvertise();
    }
}

void SteamAuthManager::OnSteamServerConnectFailure(SteamServerConnectFailure_t* cb) {
    LOG_ERROR("Steam server connect failure (result=%d, retrying=%d)",
              static_cast<int>(cb->m_eResult),
              cb->m_bStillRetrying ? 1 : 0);
}

void SteamAuthManager::OnSteamServersDisconnected(SteamServersDisconnected_t* cb) {
    LOG_WARN("Disconnected from Steam (result=%d)",
             static_cast<int>(cb->m_eResult));
    logged_on_   = false;
    advertising_ = false;
}

void SteamAuthManager::OnPolicyResponse(GSPolicyResponse_t* cb) {
    LOG_INFO("GSPolicyResponse: secure=%d", cb->m_bSecure ? 1 : 0);
    if (props_.secure && cb->m_bSecure) {
        TryAdvertise();
    }
}

void SteamAuthManager::OnValidateAuthTicket(ValidateAuthTicketResponse_t* cb) {
    const uint64_t sid    = cb->m_SteamID.ConvertToUint64();
    const uint64_t owner  = cb->m_OwnerSteamID.ConvertToUint64();
    const bool     ok     = cb->m_eAuthSessionResponse == k_EAuthSessionResponseOK;

    if (ok) {
        LOG_INFO("Auth OK SteamID=%llu Owner=%llu",
                 static_cast<unsigned long long>(sid),
                 static_cast<unsigned long long>(owner));
    } else {
        LOG_WARN("Auth DENY SteamID=%llu reason=%d",
                 static_cast<unsigned long long>(sid),
                 static_cast<int>(cb->m_eAuthSessionResponse));
    }

    if (sink_) {
        sink_->OnAuthResult(sid, owner, ok,
                            ok ? 0 : static_cast<int>(cb->m_eAuthSessionResponse));
    }
}

void SteamAuthManager::OnClientApprove(GSClientApprove_t* cb) {
    LOG_DEBUG("Legacy GSClientApprove_t SteamID=%llu (ignored)",
              static_cast<unsigned long long>(cb->m_SteamID.ConvertToUint64()));
}

void SteamAuthManager::OnClientDeny(GSClientDeny_t* cb) {
    LOG_DEBUG("Legacy GSClientDeny_t SteamID=%llu reason=%d (ignored)",
              static_cast<unsigned long long>(cb->m_SteamID.ConvertToUint64()),
              static_cast<int>(cb->m_eDenyReason));
}

void SteamAuthManager::OnClientKick(GSClientKick_t* cb) {
    LOG_INFO("Steam requested kick: SteamID=%llu reason=%d",
             static_cast<unsigned long long>(cb->m_SteamID.ConvertToUint64()),
             static_cast<int>(cb->m_eDenyReason));
}

} // namespace auth
} // namespace aa3
