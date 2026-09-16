#pragma once
#ifndef CATA_SRC_NPC_AI_ORDER_MENU_H
#define CATA_SRC_NPC_AI_ORDER_MENU_H

#include <optional>
#include <string>
#include <vector>

namespace npc_ai
{

// Closed catalogue of companion orders that the game can execute without the
// model deciding anything.  Every entry maps to a canonical spoken phrase that
// the existing keyword parsers (npc_ai_tactical, npc_ai_interior, npc_ai_hide,
// npc_ai_pickup, npc_ai_wield, npc_ai_equipment, npc_ai_rescue, npc_ai_fire,
// npc_ai_vehicle_unload, npc_ai_batch_pickup, npc_ai_action_parser) already
// recognise, so picking an entry from the menu is exactly the same as typing
// that phrase in the AI conversation.
enum class menu_order : int {
    follow,
    guard,
    enter_interior,
    hide,
    pickup,
    pickup_all_food,
    search_food,
    search_item,
    wield,
    drop,
    wear,
    take_off,
    store,
    recover,
    drag_casualty,
    start_fire,
    unload_vehicle,
    watch
};

// What the menu has to ask the player before the phrase can be built.
enum class menu_order_target : int {
    none,          // the order is complete by itself
    item_words,    // free text naming an object ("la mochila", "el rifle")
    ally_name      // one allied companion, chosen from a list
};

// Visual grouping of the catalogue.  Entries are listed group by group and
// the menu draws a disabled header line when the group changes.
enum class menu_order_group : int {
    movement,
    items,
    tasks
};

std::string menu_order_group_title( menu_order_group group );

struct menu_order_entry {
    menu_order id;
    menu_order_group group;
    menu_order_target target;
    // Every order can be addressed to "everyone".  When true the group path
    // of the AI conversation handles the order natively (tactical, interior,
    // rescue, equipment, acquisition).  When false the handler only exists on
    // the single-companion path, so the menu fans the same phrase out to each
    // companion in turn instead of letting it degrade into group dialogue.
    bool native_group_path;
    // The order walks a search: the menu asks how far (near / medium / far /
    // only inside the building) and appends the spoken range to the phrase.
    bool asks_range;
    int hotkey;
    // Menu text and one-line description, already in the dialogue language.
    std::string label;
    std::string description;
};

// Built on each call so labels follow the dialogue language in the options.
std::vector<menu_order_entry> order_menu_catalogue();
std::optional<menu_order_entry> find_order_menu_entry( menu_order id );

// Canonical phrase for an order.  `target` is ignored when the entry needs
// none, and an empty `target` yields an empty phrase when one is required.
std::string order_menu_phrase( menu_order id, const std::string &target );

// Same, with the spoken range suffix understood by detect_search_range:
// "Busca comida cerca.", "Busca y trae la linterna dentro de casa."
std::string order_menu_phrase_with_range( menu_order id, const std::string &target,
        const std::string &range_phrase );

// Text shown when asking for the target of an order that needs one.
std::string order_menu_target_prompt( menu_order id );

} // namespace npc_ai

#endif // CATA_SRC_NPC_AI_ORDER_MENU_H
