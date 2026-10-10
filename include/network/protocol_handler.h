#ifndef AA3_PROTOCOL_HANDLER_H
#define AA3_PROTOCOL_HANDLER_H

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>
#include "network/network.h"
#include "util/bits.h"
#include "game/server_actors.h"
#include "network/rpc_decode.h"
#include "network/rep_encode.h"
#include "game/stats_export.h"
#include "game/world.h"

namespace aa3 {
namespace engine { struct MapManifest; }
namespace network {

enum class ConnectionState {
    HANDSHAKE_HELLO,
    HANDSHAKE_CHALLENGE,
    AUTHENTICATING,
    ESTABLISHED
};

struct PendingSend {
    uint64_t deadline_us;        // when (monotonic μs) this should fire
    std::vector<uint8_t> bytes;  // exact bytes to ship
};

// A reliable bunch we sent that the client hasn't acked yet (UE3 FOutBunch on
// UChannel::OutRec). `bunch` is the complete bunch element -- header (with
// its ChSequence) plus payload -- so a retransmission is just a new packet
// wrapped around the same bits (UChannel::ReceivedNak -> SendRawBunch). See
// docs/AA3_ACTOR_REPLICATION.md sec 25.
struct OutReliableBunch {
    uint32_t ch_index = 0;
    uint32_t ch_seq = 0;
    uint32_t packet_id = 0;   // unwrapped PacketSequence of the latest transmission
    uint64_t sent_us = 0;     // monotonic time of the latest transmission
    uint32_t resends = 0;
    uint32_t late_rx = 0;     // client packets received past the resend deadline
    util::BitWriter bunch;
};

// One bunch parsed off an incoming packet, payload copied out so it can be
// queued until its reliable predecessors arrive (UE3 FInBunch / InRec).
struct InBunch {
    uint32_t ch_index = 0;
    uint32_t ch_seq = 0;      // raw (wrapped) ChSequence off the wire
    uint32_t ch_type = 0;
    bool open = false, close = false, reliable = false;
    uint32_t bits = 0;
    std::vector<uint8_t> data;
};

// Another player's actors as replicated to THIS connection (doc sec 21):
// their PRI and Pawn, on channels this connection allocated. Their PC is
// never replicated to anyone else (owner-only).
struct RemotePlayerChannels {
    uint32_t pri_ch = 0;
    uint32_t pawn_ch = 0;
    const game::ServerPlayerReplicationInfo* pri = nullptr;
    const game::ServerPawn* pawn = nullptr;
    game::ServerPlayerReplicationInfo last_pri;
    game::ServerPawn last_pawn_rest;   // snapshot for the reliable non-movement diff
    game::ServerPawn last_pawn_move;   // snapshot for the unreliable movement diff
    uint64_t next_move_us = 0;
    uint64_t next_keyframe_us = 0;
};

struct ClientSession {
    uint32_t client_id;
    ConnectionState state;
    uint32_t version;
    uint8_t in_seq;
    uint32_t out_seq;            // PacketSequence counter (RIM 0x4000 max)
    uint32_t reliable_out_seq;   // bunch ChSequence on control channel
    uint32_t last_packet_id;     // last seen client PacketSequence (for acking)
    std::vector<PendingSend> pending; // scheduled sends (post-storm keepalives etc.)

    // Post-storm bookkeeping. The client responds to our USES storm with one
    // reliable bunch per packet on Ch=0 ChType=1, each containing "HAVE
    // GUID=<32hex> GEN=1" FString records, terminating with a single "JOIN"
    // record. We count HAVEs and flip `join_seen` so the engine can drive the
    // post-handshake actor-replication phase.
    std::string player_name;     // from the LOGIN URL's ?Name= (stage 3)
    uint32_t phase_serial_seen = 0;  // last World::PhaseSerial() this client was told about
    // Generic data actors open on this connection (round actors and this
    // player's spawn inventory), diffed every reliable tick (doc sec 28/30).
    std::vector<const game::ServerRoundActor*> data_actors;
    // The pawn this client was last given (PossessPawn; doc sec 29).
    const game::ServerPawn* possessed_pawn = nullptr;
    bool match_opened = false;
    uint32_t confirm_reload_sent = 0, mag_count_sent = 0;  // ServerPawn reload serials sent
    uint32_t breath_end_sent = 0;                           // ServerPawn::breath_end_serial sent
    uint32_t hit_sent = 0, sling_sent = 0, vault_sent = 0;  // ServerPawn notice serials sent
    uint32_t incap_sent = 0;       // ServerPawn::incap_serial announced (doc sec 35)
    uint32_t current_weapon_ch = 0;   // last weapon confirmed to the client (doc sec 30)
    uint32_t loaded_mag_serial_sent = 0;  // ServerPawn::loaded_mag_serial last announced
    std::map<const game::ServerActor*, int> rof_sent;  // rifle CurrentROF last notified
    // Round actors (doc sec 28): opened once per client, then diffed.
    bool round_opened = false;
    std::map<const game::ServerRoundActor*,
             std::map<std::pair<uint32_t, int32_t>, std::string>> round_sent;
    std::array<std::map<std::pair<uint32_t, int32_t>, std::string>, 4> team_round_sent;
    uint32_t have_count;
    bool join_seen;
    bool actor_burst_sent;  // true once OpenInitialActorChannels has fired

    // Synthesized post-JOIN replication tick. Zero = not scheduled yet.
    uint64_t next_keepalive_us;

    // Per-actor-channel reliable ChSequence counter. Each channel has its
    // own independent sequence (starts at 1 on the OPEN bunch); update
    // bunches on that channel must continue from there, not restart at 1.
    std::map<uint32_t, uint32_t> channel_reliable_seq;

    // Per-actor-channel class MaxIndex, recorded when we open each channel
    // (see OpenInitialActorChannels) — an incoming bunch on that channel
    // must decode its RepIndex against this same class's table, or the
    // RIM bit-width is wrong and everything after desyncs. See
    // DecodeActorBunch / docs/AA3_ACTOR_REPLICATION.md sec 17.
    std::map<uint32_t, uint32_t> channel_max_index;

    // Last property state actually sent to this client, used to diff
    // against the live world model each tick so update bunches carry only
    // changed fields (see network/rep_encode.h). Populated once
    // OpenInitialActorChannels has sent the full initial state.
    game::ServerWorldInfo last_sent_world_info;
    game::ServerGameReplicationInfo last_sent_gri;
    game::ServerPlayerStorageProxy last_sent_proxy;   // diff base for the proxy channel
    bool proxy_sent_valid = false;
    std::array<game::ServerTeamInfo, 4> last_sent_teams;
    game::ServerPlayerReplicationInfo last_sent_pri;
    game::ServerPlayerController last_sent_pc;
    game::ServerPawn last_sent_pawn;
    game::ServerSteamStatManager last_sent_stats;
    bool has_last_sent_state = false;

    // Client movement (doc sec 20). Mirrors PlayerController's
    // CurrentTimeStamp: moves with TimeStamp <= this are stale and ignored.
    // `pending_ack_timestamp` != 0 means a ClientAckGoodMove is owed; it's
    // flushed from Tick at most once per kMoveAckIntervalUs (retail sends
    // ~27/s, one per server tick, acking only the newest move).
    float current_move_timestamp = 0.0f;
    float pending_ack_timestamp = 0.0f;
    uint64_t next_move_ack_us = 0;
    uint32_t moves_received = 0;

    // Per-connection channel bookkeeping (doc sec 21). `actor_channel` is
    // every actor open on this connection, so actor references can be
    // written as their channel index here; `sent_refs` is the per-channel
    // RepIndex -> last-sent reference value (RepContext::sent_refs).
    std::map<const game::ServerActor*, uint32_t> actor_channel;
    std::map<uint32_t, std::map<uint32_t, uint32_t>> sent_refs;
    // Other players, keyed by their client id. Channels 2..12 are this
    // client's fixed initial burst; remote players allocate from 13 up.
    std::map<uint32_t, RemotePlayerChannels> remote_players;
    uint32_t next_free_channel = 13;

    // Reliability (doc sec 25), mirroring UNetConnection/UChannel.
    // Outgoing: every reliable bunch stays in `out_rec` until acked; a
    // record whose packet is older than the newest acked packet is resent
    // (UE3's NAK + "missed NAK" sweep), with a timeout fallback for a
    // client that keeps talking but has acked nothing newer.
    std::deque<OutReliableBunch> out_rec;
    std::map<uint32_t, uint32_t> out_rec_per_channel;
    int32_t out_ack_packet_id = -1;          // newest acked packet (unwrapped)
    // Acks owed to the client. Like UE3 (QueuedAcks/ResendAcks) every ack
    // goes out twice: in the next packet we send, and once more after that.
    std::vector<uint32_t> queued_acks;
    std::vector<uint32_t> resend_acks;
    uint64_t next_ack_flush_us = 0;
    uint64_t last_receive_us = 0;
    // Incoming: last ChSequence processed per channel (unwrapped), and
    // reliable bunches that arrived ahead of a missing predecessor.
    std::map<uint32_t, int32_t> in_reliable;
    std::map<uint32_t, std::map<int32_t, InBunch>> in_rec;
    uint32_t stat_resent = 0, stat_dup_in = 0, stat_queued_in = 0;
};

class ProtocolHandler {
public:
    // `manifest` is owned elsewhere (Engine) and must outlive this handler.
    // It supplies the USES + DLMGR + WELCOME records the server emits on the
    // control channel right after a successful CHALLENGE round-trip.
    // `world` holds the server-side actor model; the protocol layer reads
    // it to populate channel opens and (eventually) property updates.
    ProtocolHandler(SocketManager& socket,
                    const engine::MapManifest* manifest,
                    game::World* world);
    ~ProtocolHandler();

    void OnDataReceived(uint32_t client_id, const uint8_t* data, uint32_t len);
    void OnClientConnected(uint32_t client_id, bool connected);

    // Drains scheduled sends (post-storm keepalives) whose deadline has elapsed.
    // Called from the engine tick loop with monotonic microseconds.
    void Tick(uint64_t now_us);

private:
    void HandleControlMessage(uint32_t client_id, uint8_t nmt_type, util::BitReader& reader);

    void SendHandshakeChallenge(uint32_t client_id);
    void SendStormAndWelcome(uint32_t client_id);   // bundles WELCOME into final packet
    void OpenInitialActorChannels(uint32_t client_id);  // post-JOIN: synthesize Ch=2+

    // Emit the open bunch for an actor channel: bDynamic=0 + ObjectIndex (31
    // bits, RIM(2^31)), optionally followed by the transient-actor
    // SerializeCompressedInitial preamble (Location/Rotation — only for
    // server-spawned actors like a PRI/PC, see network/rep_encode.h), then
    // (PlayerController only) a 1-byte NetPlayerIndex, then an optional
    // initial-state property payload (RepIndex+value pairs). With
    // `transient_actor`, `net_player_index` and `prop_payload` all null,
    // this is the original minimum-viable open — the client's
    // ReceivedBunch loop reads the first RepIndex, hits end-of-bunch
    // (BunchDataBits=32 exhausted), and exits cleanly.
    //
    // `class_obj_idx` must be a PackageMap index the client already has
    // (i.e., one of the objects from the USES manifest we shipped earlier).
    void OpenActorChannel(uint32_t client_id, uint32_t ch_index,
                          uint32_t class_obj_idx,
                          const util::BitWriter* prop_payload = nullptr,
                          const game::ServerActor* transient_actor = nullptr,
                          const uint8_t* net_player_index = nullptr);

    // Emit a property-update bunch (bOpen=false, ChType=2) on an
    // already-open actor channel, acking the client's last known packet.
    // `prop_payload` must be non-empty (caller checks the field count from
    // Encode*Rep first — an empty diff means nothing to send at all, per
    // UnChan.cpp:1985).
    // `reliable=false` for frequent fire-and-forget RPCs (move acks):
    // no ChSequence is consumed, matching retail's unreliable acks.
    void SendActorUpdateBunch(uint32_t client_id, uint32_t ch_index,
                              const util::BitWriter& prop_payload,
                              bool reliable = true);

    void ScheduleKeepalives(uint32_t client_id, uint64_t now_us);

    // Which acks a packet carries: none (the CHALLENGE, matching retail) or
    // the queued/resend ack lists (everything else).
    enum class AckMode { kNone, kQueued };

    // Send one packet: PacketSequence, acks per `acks`, then `bunch` (a
    // complete bunch element, or null for an ack-only packet). Returns the
    // unwrapped PacketSequence used.
    uint32_t TransmitPacket(uint32_t client_id, ClientSession& sess,
                            const util::BitWriter* bunch, AckMode acks);
    // Send a bunch built by WriteBunchHeader + payload. Reliable bunches
    // (ch_seq != 0) are recorded for retransmission until acked.
    void SendBunch(uint32_t client_id, ClientSession& sess, util::BitWriter bunch,
                   uint32_t ch_index, uint32_t ch_seq, AckMode acks = AckMode::kQueued);
    // Ack-only packet carrying the owed acks (no-op if none are owed).
    void FlushAcks(uint32_t client_id, ClientSession& sess);
    // An ack from the client for one of our packets (raw 14-bit value).
    void ReceivedAck(ClientSession& sess, uint32_t wire_ack);
    // Called per received packet, after its acks: resend every unacked
    // reliable bunch whose packet is older than the newest acked one (it
    // was lost), or that the client has kept talking past without acking.
    void ResendLost(uint32_t client_id, ClientSession& sess, uint64_t now_us);
    // False while a channel has too many unacked reliable bunches (UE3
    // UChannel::IsNetReady, RELIABLE_BUFFER): sending more would overrun the
    // client's ChSequence window.
    bool ChannelReady(const ClientSession& sess, uint32_t ch_index) const;

    // Process a post-storm client packet's bunches: dedups and orders
    // reliable bunches per channel (UChannel::ReceivedRawBunch), dispatches
    // them, and acks the packet.
    void ProcessPostStormPacket(uint32_t client_id, std::vector<InBunch>& bunches,
                                uint32_t packet_seq);
    // Handle one in-order bunch: Ch=0 FString records (HAVE/JOIN) or an
    // actor-channel RPC bunch.
    void DispatchBunch(uint32_t client_id, ClientSession& sess, const InBunch& b);

    // Decode a ChType=2 (Actor) bunch's payload: RepIndex(es) against the
    // channel's recorded class MaxIndex, dispatching known RepIndex/
    // function pairs to a handler. `reader` is positioned at the start of
    // the bunch payload on entry. Reads as many recognized entries as it
    // can, but stops the instant it hits a RepIndex it can't safely
    // interpret (unmapped property, or a function whose parameters aren't
    // decoded) — it does NOT guess a bit width for anything unknown. The
    // caller (DispatchBunch) hands it a reader spanning exactly the bunch's
    // BunchDataBits, since BDB — not this function's own understanding — is
    // the sole authority on where the bunch ends (doc sec 10.3). Safe to
    // under-decode; never safe to guess.
    void DecodeActorBunch(uint32_t client_id, uint32_t ch_index,
                          util::BitReader& reader, uint32_t bdb, bool b_open);

    // Applies one decoded client->server PlayerController RPC to the world
    // (movement -> pawn location/rotation + pending ClientAckGoodMove).
    void ApplyPcRpc(uint32_t client_id, ClientSession& sess, const DecodedRpc& rpc);

    // Replication context for `ch` on this connection (resolver + per-
    // channel sent-reference memory).
    RepContext MakeRepContext(ClientSession& sess, uint32_t ch, bool for_owner);

    // Opens/closes/updates other players' PRI+Pawn channels on this
    // connection. `reliable_tick` = the 200ms tick (non-movement diffs);
    // movement goes out unreliably on its own faster cadence.
    void SyncRemotePlayers(uint32_t client_id, ClientSession& sess, uint64_t now_us,
                           bool reliable_tick);
    void CloseActorChannel(uint32_t client_id, uint32_t ch_index);

    // The map's streaming sublevels and whether each is loaded+visible for
    // the current game mode (doc sec 27).
    struct StreamingLevel { std::string package; bool visible; };
    std::vector<StreamingLevel> StreamingLevels() const;
    // Retail's login RPCs (ClientSetHUD ... OpenPostAuthMenu), appended to the
    // PlayerController's open bunch after its properties (doc sec 27).
    void AppendLoginRPCs(util::BitWriter& w, const game::ServerPlayerController& pc);
    // Retail's spawn sequence: opens the pawn and hands it to the client.
    void PossessPawn(uint32_t client_id, ClientSession& sess,
                     const game::ServerPlayerController& pc);
    // Opens one generic data actor on the next free channel; returns it.
    uint32_t OpenDataActor(uint32_t client_id, ClientSession& sess, const game::ServerRoundActor* a);
    void OpenMatchActors(uint32_t client_id, ClientSession& sess);
    // Opens every round actor on this connection (doc sec 28).
    void OpenRoundActors(uint32_t client_id, ClientSession& sess);
    // The PC-channel RPCs that announce a round phase (doc sec 28).
    void SendPhaseRPCs(uint32_t client_id, game::World::MatchPhase phase);

public:
    // Optional: snapshot leaving players into the stats export (doc sec 23).
    void SetStatsExporter(game::StatsExporter* stats) { stats_ = stats; }

private:
    game::StatsExporter* stats_ = nullptr;

    SocketManager& socket_;
    const engine::MapManifest* manifest_;
    game::World* world_;
    std::map<uint32_t, ClientSession> sessions_;
};

} // namespace network
} // namespace aa3

#endif // AA3_PROTOCOL_HANDLER_H
