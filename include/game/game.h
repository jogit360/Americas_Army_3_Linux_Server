#ifndef AA3_GAME_H
#define AA3_GAME_H

#include <cstdint>
#include <string>
#include <vector>

namespace aa3 {
namespace game {

struct Player {
    uint32_t id;
    uint64_t steam_id;
    std::string username;
    uint16_t team_id;
    float position_x, position_y, position_z;
    float rotation_pitch, rotation_yaw, rotation_roll;
    uint8_t health;
    uint8_t ammo_primary;
    uint8_t ammo_secondary;
    bool is_alive;
};

class PlayerManager {
public:
    PlayerManager();
    ~PlayerManager();

    Player* CreatePlayer(uint64_t steam_id);
    bool RemovePlayer(uint32_t player_id);
    Player* GetPlayer(uint32_t player_id);

    void Update(double delta_time);

private:
    std::vector<Player*> players_;
    uint32_t next_player_id_;
};

} // namespace game
} // namespace aa3

#endif // AA3_GAME_H
