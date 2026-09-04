#pragma once
#ifndef CATA_SRC_NPC_AI_TACTICAL_H
#define CATA_SRC_NPC_AI_TACTICAL_H

#include <string>
#include <vector>

class npc;

namespace npc_ai
{

enum class tactical_order {
    none,
    follow,
    guard
};

struct tactical_order_result {
    tactical_order order = tactical_order::none;
    bool handled = false;
    std::vector<npc *> affected;
};

tactical_order parse_tactical_order( const std::string &player_line );
tactical_order_result execute_tactical_order( const std::vector<npc *> &targets,
        const std::string &player_line );
// Same execution for an order already classified elsewhere (model-backed
// intent path in npc_ai_order_intent.cpp).
tactical_order_result execute_tactical_order( const std::vector<npc *> &targets,
        tactical_order order );

} // namespace npc_ai

#endif // CATA_SRC_NPC_AI_TACTICAL_H
