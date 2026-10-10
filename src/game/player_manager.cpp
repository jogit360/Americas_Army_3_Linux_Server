#include "game/game.h"
#include "util/logging.h"

namespace aa3 {
namespace game {

PlayerManager::PlayerManager() : next_player_id_(1) {
}

PlayerManager::~PlayerManager() {
    for (Player* p : players_) {
        delete p;
    }
    players_.clear();
}

Player* PlayerManager::CreatePlayer(uint64_t steam_id) {
    Player* p = new Player();
    p->id = next_player_id_++;
    p->steam_id = steam_id;
    p->is_alive = true;
    p->health = 100;
    
    players_.push_back(p);
    LOG_INFO("Created player: ID=%u, SteamID=%llu", p->id, static_cast<unsigned long long>(steam_id));
    return p;
}

bool PlayerManager::RemovePlayer(uint32_t player_id) {
    for (auto it = players_.begin(); it != players_.end(); ++it) {
        if ((*it)->id == player_id) {
            LOG_INFO("Removing player: ID=%u", player_id);
            delete *it;
            players_.erase(it);
            return true;
        }
    }
    return false;
}

Player* PlayerManager::GetPlayer(uint32_t player_id) {
    for (Player* p : players_) {
        if (p->id == player_id) {
            return p;
        }
    }
    return nullptr;
}

void PlayerManager::Update(double delta_time) {
    // Basic update logic for players
}

} // namespace game
} // namespace aa3
