#pragma once
#ifndef CATA_SRC_NPC_AI_INTERIOR_H
#define CATA_SRC_NPC_AI_INTERIOR_H

#include <string>
#include <utility>
#include <vector>

#include "coordinates.h"

class npc;

namespace npc_ai
{

enum class structured_voice_order : int {
    none,
    enter_nearest_reachable_safe_interior
};

struct interior_order_result {
    bool handled = false;
    bool success = false;
    std::vector<std::pair<int, tripoint_abs_ms>> assignments;
    std::string message;
};

structured_voice_order parse_structured_voice_order( const std::string &spoken );
interior_order_result execute_enter_nearest_reachable_safe_interior(
    const std::vector<npc *> &targets );

// Called from npc::move when a goto_to_this_pos destination has been reached.
// If it is the interior tile of a pending "get inside" order, the companion
// stops following and guards that spot instead of walking back out to the
// player.  Returns true when it switched the companion to guard.
bool on_move_destination_reached( npc &who, const tripoint_abs_ms &reached );

// Forget a pending interior hold and its walk (a newer order supersedes it).
void clear_interior_hold( npc &who );

} // namespace npc_ai

#endif // CATA_SRC_NPC_AI_INTERIOR_H
