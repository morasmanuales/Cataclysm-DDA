#pragma once

#include <string>

class npc;

namespace npc_ai
{

struct batch_pickup_command_result {
    bool handled = false;
    bool success = false;
    std::string message;
};


// Interpreta ordenes como:
// "recoge toda la comida"
batch_pickup_command_result try_handle_batch_pickup_command(
    npc &who,
    const std::string &player_line
);


// Continua una orden por lote ya iniciada.
// Se llama desde el turno normal del NPC.
void process_batch_pickup(
    npc &who
);


void reset_all_food_batches();


// "Busca comida": the companion walks the surroundings (limited radius,
// through doors) checking every place that may hold food -- item piles and
// non-sealed containers such as fridges, cupboards or shelves -- and picks up
// what it finds through the same food batch used by "recoge toda la comida".
// No model request is ever made.
struct search_food_command_result {
    bool handled = false;
    bool started = false;
    std::string message;
};

bool is_search_food_command( const std::string &player_line );

search_food_command_result try_handle_search_food_command( npc &who,
        const std::string &player_line );

// "Busca y trae <objeto>": same tour of nearby spots, but collecting the
// items whose name or id contains every word of the (normalised) request.
// Normalisation: lowercase, accents removed, punctuation dropped, articles
// and filler words ("el", "la", "un", "the", "some"...) discarded.
std::vector<std::string> normalize_search_words( const std::string &request );

// Extracts the object words from a spoken search order ("busca y trae la
// linterna", "look for a flashlight").  Empty when the line is not one.
std::string parse_search_item_request( const std::string &player_line );

search_food_command_result try_handle_search_item_command( npc &who,
        const std::string &player_line );

// Per-turn continuation.  Returns true when it spent the NPC's action (walking
// to or inspecting a spot).  While a pickup batch spawned by the search is
// running it returns false and process_batch_pickup drives the pickups.
bool process_food_search( npc &who );

bool has_food_search( const npc &who );
// True while the tour is over and the companion is walking back to the
// player to deliver the report.
bool food_search_is_returning( const npc &who );
void cancel_food_search( const npc &who );


} // namespace npc_ai