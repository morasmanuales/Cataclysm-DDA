#include "npc_ai_batch_pickup.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "catacharset.h"
#include "character_id.h"
#include "game.h"
#include "item.h"
#include "item_location.h"
#include "map.h"
#include "map_selector.h"
#include "npc.h"
#include "pathfinding.h"
#include "npc_ai_context.h"
#include "npc_ai_debug.h"
#include "npc_ai_hide.h"
#include "npctalk.h"
#include "output.h"
#include "point.h"
#include "string_formatter.h"
#include "translations.h"
#include "unicode.h"


namespace
{
// The search range type and helpers are declared in npc_ai (header).
using npc_ai::search_range;
using npc_ai::search_range_radius;
using npc_ai::search_range_spot_limit;
using npc_ai::search_range_phrase;



constexpr int batch_room_radius = 8;
constexpr std::size_t batch_room_tile_limit = 256;
constexpr std::size_t batch_food_limit = 128;
constexpr int batch_target_timeout_ticks = 240;


struct queued_food_target {
    item_location location;
    std::string id;
    std::string name;

    // CDDA-AI BATCH FOOD PICKUP V1.1 PRIORITY BEGIN
    int priority = 80;
    std::string quota_group;
    std::string priority_reason;
    // CDDA-AI BATCH FOOD PICKUP V1.1 PRIORITY END
};


struct food_batch_state {
    bool active = false;
    bool waiting_for_target = false;

    item_location current_target;

    std::string current_id;
    std::string current_name;

    std::deque<queued_food_target> queue;

    std::size_t found = 0;
    std::size_t started = 0;
    std::size_t collected = 0;
    std::size_t skipped = 0;

    int waiting_ticks = 0;
    std::string last_failure_name;
    std::string last_failure_reason;
    // Names of the items actually collected, for the search reports.
    std::vector<std::string> collected_names;
};


std::unordered_map<int, food_batch_state> food_batches;


int npc_key(
    const npc &who
)
{
    return who.getID().get_value();
}


std::string lower_ascii(
    std::string text
)
{
    for( char &c : text ) {

        if(
            c >= 'A' &&
            c <= 'Z'
        ) {

            c =
                static_cast<char>(
                    c - 'A' + 'a'
                );
        }
    }

    return text;
}


bool contains_any(
    const std::string &text,
    const std::vector<std::string> &needles
)
{
    for(
        const std::string &needle :
        needles
    ) {

        if(
            text.find(
                needle
            ) != std::string::npos
        ) {

            return true;
        }
    }

    return false;
}



// ============================================================
// CDDA-AI BATCH FOOD PICKUP V1.1 PRIORITY HELPERS BEGIN
// ============================================================

struct food_priority_info {
    int priority = 80;
    std::string quota_group;
    std::string reason;
};


food_priority_info classify_food_priority(
    const std::string &display_name
)
{
    food_priority_info result;


    const std::string name =
        lower_ascii(
            display_name
        );


    // --------------------------------------------------------
    // VENCIDO / PODRIDO
    // --------------------------------------------------------

    if(
        contains_any(
            name,
            {
                "fuera de caducidad",
                "caducado",
                "caducada",
                "vencido",
                "vencida",
                "podrido",
                "podrida",
                "expired",
                "rotten",
                "spoiled"
            }
        )
    ) {

        result.priority =
            900;

        result.quota_group =
            "expired";

        result.reason =
            "EXPIRED_LAST";

        return result;
    }


    // --------------------------------------------------------
    // ACEITE
    // --------------------------------------------------------

    if(
        contains_any(
            name,
            {
                "aceite de cocina",
                "cooking oil",
                "vegetable oil",
                "animal cooking oil"
            }
        )
    ) {

        result.priority =
            700;

        result.quota_group =
            "oil";

        result.reason =
            "COOKING_OIL_LOW_PRIORITY";

        return result;
    }


    // --------------------------------------------------------
    // AGUA
    // --------------------------------------------------------

    if(
        contains_any(
            name,
            {
                "agua potable",
                "clean water",
                "potable water"
            }
        )
    ) {

        result.priority =
            300;

        result.quota_group =
            "water";

        result.reason =
            "WATER_LIMITED";

        return result;
    }


    // --------------------------------------------------------
    // OTRAS BEBIDAS / CAFE
    // --------------------------------------------------------

    if(
        contains_any(
            name,
            {
                "cafe",
                "café",
                "coffee",
                "jugo",
                "juice",
                "refresco",
                "soda",
                "energy drink",
                "bebida energetica",
                "bebida energética"
            }
        )
    ) {

        result.priority =
            500;

        result.quota_group =
            "drink";

        result.reason =
            "OTHER_DRINK_LIMITED";

        return result;
    }


    // --------------------------------------------------------
    // COMIDA DE ALTO VALOR PRACTICO
    //
    // Proteina, conservas y alimentos densos/listos.
    // --------------------------------------------------------

    if(
        contains_any(
            name,
            {
                "pemmican",
                "pollo",
                "chicken",
                "pescado",
                "fish",
                "atun",
                "atún",
                "tuna",
                "queso",
                "pecorino",
                "cheese",
                "gallet",
                "cracker",
                "verduras enlatadas",
                "canned vegetables",
                "tomates enlatados",
                "canned tomatoes",
                "langosta",
                "lobster",
                "encurtid",
                "pickle",
                "carne",
                "meat",
                "jerky",
                "salchicha",
                "sausage"
            }
        )
    ) {

        result.priority =
            20;

        result.reason =
            "HIGH_VALUE_FOOD";

        return result;
    }


    // --------------------------------------------------------
    // INGREDIENTES
    // --------------------------------------------------------

    if(
        contains_any(
            name,
            {
                "harina",
                "flour",
                "gelatina",
                "gelativa",
                "gelatin",
                "pasta",
                "lasaña",
                "lasagna",
                "salsa de tomate",
                "tomato sauce",
                "leche condensada",
                "condensed milk",
                "azucar",
                "azúcar",
                "sugar"
            }
        )
    ) {

        result.priority =
            120;

        result.reason =
            "COOKING_INGREDIENT";

        return result;
    }


    // --------------------------------------------------------
    // RESTO DE COMIDA
    // --------------------------------------------------------

    result.priority =
        80;

    result.reason =
        "GENERAL_FOOD";

    return result;
}


int food_quota_limit(
    const std::string &group
)
{
    if(
        group ==
        "water"
    ) {

        return 4;
    }


    if(
        group ==
        "drink"
    ) {

        return 2;
    }


    if(
        group ==
        "oil"
    ) {

        return 1;
    }


    return 1000000;
}


// ============================================================
// CDDA-AI BATCH FOOD PICKUP V1.1 PRIORITY HELPERS END
// ============================================================

bool is_batch_food_command(
    const std::string &player_line
)
{
    const std::string line =
        lower_ascii(
            player_line
        );


    const bool pickup_verb =
        contains_any(
            line,
            {
                "recoge",
                "recoger",
                "recoja",
                "junta",
                "juntar",
                "agarra",
                "agarrar",
                "recolecta",
                "recolectar",
                "levanta",
                "levantar"
            }
        );


    const bool all_quantifier =
        contains_any(
            line,
            {
                "toda",
                "todo ",
                "todas",
                "todos"
            }
        );


    const bool food_word =
        contains_any(
            line,
            {
                "comida",
                "alimento",
                "alimentos",
                "viveres"
            }
        );


    return
        pickup_verb &&
        all_quantifier &&
        food_word;
}


void reset_debug(
    const npc &who,
    const std::string &player_line
)
{
    npc_ai::debug_stream output( "npc_ai_batch_food_v1_runtime.txt", true );


    if( !output ) {
        return;
    }


    output
        << "CDDA-AI BATCH FOOD PICKUP V1 RUNTIME\n"
        << "NPC="
        << who.get_name()
        << "\n"
        << "REQUEST="
        << player_line
        << "\n";
}


void debug_line(
    const std::string &line
)
{
    npc_ai::append_debug_line( "npc_ai_batch_food_v1_runtime.txt", line );
}


bool contains_point(
    const std::vector<tripoint_bub_ms> &points,
    const tripoint_bub_ms &wanted
)
{
    return
        std::find(
            points.begin(),
            points.end(),
            wanted
        ) != points.end();
}


std::vector<tripoint_bub_ms> build_room_core(
    npc &who,
    map &here
)
{
    const tripoint_bub_ms origin =
        who.pos_bub(
            here
        );


    const bool origin_outside =
        here.is_outside(
            origin
        );


    std::vector<tripoint_bub_ms> room;

    std::deque<tripoint_bub_ms> frontier;


    room.push_back(
        origin
    );

    frontier.push_back(
        origin
    );


    while(
        !frontier.empty() &&
        room.size() <
        batch_room_tile_limit
    ) {

        const tripoint_bub_ms current =
            frontier.front();

        frontier.pop_front();


        const int dx[4] = {
            1,
            -1,
            0,
            0
        };

        const int dy[4] = {
            0,
            0,
            1,
            -1
        };


        for(
            int direction = 0;
            direction < 4;
            ++direction
        ) {

            tripoint_bub_ms next =
                current;


            next.x() +=
                dx[direction];

            next.y() +=
                dy[direction];


            if(
                next.z() !=
                origin.z()
            ) {

                continue;
            }


            if(
                rl_dist(
                    origin,
                    next
                ) >
                batch_room_radius
            ) {

                continue;
            }


            if(
                contains_point(
                    room,
                    next
                )
            ) {

                continue;
            }


            // Una puerta constituye el limite de esta habitacion.
            // Incluso abierta, no seguimos al cuarto siguiente.
            if(
                here.has_flag(
                    "DOOR",
                    next
                )
            ) {

                continue;
            }


            // Tampoco atravesamos ventanas.
            if(
                here.has_flag(
                    "WINDOW",
                    next
                )
            ) {

                continue;
            }


            // No mezclamos interior y exterior.
            if(
                here.is_outside(
                    next
                ) !=
                origin_outside
            ) {

                continue;
            }


            // El nucleo de la habitacion se construye sobre
            // casillas fisicamente transitables.
            if(
                !here.passable(
                    next
                )
            ) {

                continue;
            }


            // No damos conocimiento de zonas que Liam
            // no puede percibir actualmente.
            if(
                !who.sees(
                    here,
                    next
                )
            ) {

                continue;
            }


            room.push_back(
                next
            );

            frontier.push_back(
                next
            );


            if(
                room.size() >=
                batch_room_tile_limit
            ) {

                break;
            }
        }
    }


    return room;
}


bool tile_belongs_to_room(
    map &here,
    const tripoint_bub_ms &origin,
    const bool origin_outside,
    const std::vector<tripoint_bub_ms> &room_core,
    const tripoint_bub_ms &tile
)
{
    if(
        tile.z() !=
        origin.z()
    ) {

        return false;
    }


    if(
        rl_dist(
            origin,
            tile
        ) >
        batch_room_radius
    ) {

        return false;
    }


    if(
        here.has_flag(
            "DOOR",
            tile
        ) ||
        here.has_flag(
            "WINDOW",
            tile
        )
    ) {

        return false;
    }


    if(
        here.is_outside(
            tile
        ) !=
        origin_outside
    ) {

        return false;
    }


    if(
        contains_point(
            room_core,
            tile
        )
    ) {

        return true;
    }


    // Permite objetos sobre mesas, mesones y otros muebles
    // impasables que estan pegados al suelo de la habitacion.
    for(
        const tripoint_bub_ms &room_tile :
        room_core
    ) {

        if(
            rl_dist(
                room_tile,
                tile
            ) <= 1
        ) {

            return true;
        }
    }


    return false;
}


std::vector<queued_food_target> find_room_food(
    npc &who
)
{
    map &here =
        get_map();


    const tripoint_bub_ms origin =
        who.pos_bub(
            here
        );


    const bool origin_outside =
        here.is_outside(
            origin
        );


    const std::vector<tripoint_bub_ms>
    room_core =
        build_room_core(
            who,
            here
        );


    debug_line(
        std::string(
            "ROOM_CORE_TILES="
        ) +
        std::to_string(
            room_core.size()
        )
    );


    std::vector<queued_food_target>
    targets;


    for(
        const tripoint_bub_ms &p :
        here.points_in_radius(
            origin,
            batch_room_radius
        )
    ) {

        if(
            targets.size() >=
            batch_food_limit
        ) {

            break;
        }


        if(
            !tile_belongs_to_room(
                here,
                origin,
                origin_outside,
                room_core,
                p
            )
        ) {

            continue;
        }


        if(
            !who.sees(
                here,
                p
            )
        ) {

            continue;
        }


        if(
            !here.could_see_items(
                p,
                who
            )
        ) {

            continue;
        }


        map_stack items =
            here.i_at(
                p
            );


        for(
            item &it :
            items
        ) {

            if(
                !( ( it.is_food() || it.is_food_container() ) )
            ) {

                continue;
            }


            item_location location(
                map_cursor(
                    p
                ),
                &it
            );


            if(
                !location
            ) {

                continue;
            }


            queued_food_target target;

            target.location =
                location;

            target.id =
                it.typeId().str();

            target.name =
                remove_color_tags( it.tname() );

            const food_priority_info priority_info =
                classify_food_priority(
                    target.name
                );


            target.priority =
                priority_info.priority;

            target.quota_group =
                priority_info.quota_group;

            target.priority_reason =
                priority_info.reason;


            targets.push_back(
                target
            );


            std::ostringstream candidate_debug;

            candidate_debug
                << "CANDIDATE"
                << targets.size()
                << "="
                << target.name
                << " | id="
                << target.id
                << " | pos=("
                << p.x()
                << ","
                << p.y()
                << ","
                << p.z()
                << ")"
                << " | distance="
                << rl_dist(
                       origin,
                       p
                   );


            candidate_debug
                << " | priority="
                << target.priority
                << " | group="
                << (
                    target.quota_group.empty() ?
                    "food" :
                    target.quota_group
                )
                << " | reason="
                << target.priority_reason;

            debug_line(
                candidate_debug.str()
            );


            if(
                targets.size() >=
                batch_food_limit
            ) {

                break;
            }
        }
    }


        // CDDA-AI BATCH FOOD PICKUP V1.1 PRIORITY SORT BEGIN
    //
    // Primero decide QUE merece ocupar la mochila.
    // Distancia solo desempata dentro de una prioridad.

    std::sort(
        targets.begin(),
        targets.end(),
        [&]( const queued_food_target &a,
             const queued_food_target &b ) {

            if(
                a.priority !=
                b.priority
            ) {

                return
                    a.priority <
                    b.priority;
            }


            return
                rl_dist(
                    origin,
                    a.location.pos_bub(
                        here
                    )
                ) <
                rl_dist(
                    origin,
                    b.location.pos_bub(
                        here
                    )
                );
        }
    );


    // --------------------------------------------------------
    // CUOTAS DE ESTA EXPEDICION
    //
    // agua:        4 recipientes
    // otras bebidas: 2
    // aceite:      1
    //
    // IMPORTANTE:
    // estas son cuotas de candidatos, no capacidad artificial.
    // ai_request_pickup() sigue siendo quien decide si
    // fisicamente cabe cada objeto.
    // --------------------------------------------------------

    std::unordered_map<std::string, int>
    quota_counts;


    quota_counts["water"] =
        0;

    quota_counts["drink"] =
        0;

    quota_counts["oil"] =
        0;


    std::vector<queued_food_target>
    filtered_targets;


    filtered_targets.reserve(
        targets.size()
    );


    for(
        const queued_food_target &target :
        targets
    ) {

        if(
            target.quota_group.empty() ||
            target.quota_group ==
            "expired"
        ) {

            filtered_targets.push_back(
                target
            );

            continue;
        }


        const int limit =
            food_quota_limit(
                target.quota_group
            );


        int &current =
            quota_counts[
                target.quota_group
            ];


        if(
            current >=
            limit
        ) {

            std::ostringstream quota_debug;

            quota_debug
                << "SKIP_QUOTA="
                << target.name
                << " | group="
                << target.quota_group
                << " | current="
                << current
                << " | limit="
                << limit;


            debug_line(
                quota_debug.str()
            );


            continue;
        }


        filtered_targets.push_back(
            target
        );


        ++current;
    }


    targets.swap(
        filtered_targets
    );


    debug_line(
        std::string(
            "SELECTED_WATER="
        ) +
        std::to_string(
            quota_counts["water"]
        )
    );


    debug_line(
        std::string(
            "SELECTED_DRINKS="
        ) +
        std::to_string(
            quota_counts["drink"]
        )
    );


    debug_line(
        std::string(
            "SELECTED_OIL="
        ) +
        std::to_string(
            quota_counts["oil"]
        )
    );


    debug_line(
        std::string(
            "CANDIDATES_AFTER_PRIORITY_QUOTAS="
        ) +
        std::to_string(
            targets.size()
        )
    );


    // Imprimir el orden REAL en que Liam intentara recoger.
    for(
        std::size_t index = 0;
        index < targets.size();
        ++index
    ) {

        const queued_food_target &target =
            targets[index];


        std::ostringstream order_debug;

        order_debug
            << "ORDER"
            << (
                index +
                1
            )
            << "="
            << target.name
            << " | priority="
            << target.priority
            << " | group="
            << (
                target.quota_group.empty() ?
                "food" :
                target.quota_group
            );


        debug_line(
            order_debug.str()
        );
    }

    // CDDA-AI BATCH FOOD PICKUP V1.1 PRIORITY SORT END

    return targets;
}


bool start_next_target(
    npc &who,
    food_batch_state &state
)
{
    map &here =
        get_map();


    while(
        !state.queue.empty()
    ) {

        queued_food_target target =
            state.queue.front();


        state.queue.pop_front();


        if(
            !target.location ||
            target.location.where() !=
            item_location::type::map
        ) {

            ++state.skipped;

            debug_line(
                std::string(
                    "SKIP_INVALID="
                ) +
                target.name
            );

            continue;
        }


        const tripoint_bub_ms target_position =
            target.location.pos_bub(
                here
            );


        if(
            !who.sees(
                here,
                target_position
            ) ||
            !here.could_see_items(
                target_position,
                who
            )
        ) {

            ++state.skipped;

            debug_line(
                std::string(
                    "SKIP_NOT_VISIBLE="
                ) +
                target.name
            );

            continue;
        }


        std::string error;


        const bool started =
            who.ai_request_pickup(
                target.location,
                target_position,
                error,
                false,
                "batch pickup"
            );


        if(
            !started
        ) {

            ++state.skipped;
            state.last_failure_name = target.name;
            state.last_failure_reason = error;


            std::ostringstream failure_debug;

            failure_debug
                << "SKIP_REQUEST_REJECTED="
                << target.name
                << " | id="
                << target.id
                << " | reason="
                << error;


            debug_line(
                failure_debug.str()
            );


            // Puede que este objeto ya no quepa,
            // pero otro mas pequeno aun si.
            continue;
        }


        state.waiting_for_target =
            true;

        state.current_target =
            target.location;

        state.current_id =
            target.id;

        state.current_name =
            target.name;

        state.waiting_ticks =
            0;

        ++state.started;


        std::ostringstream started_debug;

        started_debug
            << "TARGET_STARTED="
            << target.name
            << " | id="
            << target.id
            << " | remaining="
            << state.queue.size();


        debug_line(
            started_debug.str()
        );


        return true;
    }


    return false;
}


void finish_batch(
    npc &who,
    food_batch_state &state
)
{
    std::ostringstream debug;

    debug
        << "BATCH_FINISHED"
        << " | found="
        << state.found
        << " | started="
        << state.started
        << " | collected="
        << state.collected
        << " | skipped="
        << state.skipped;


    debug_line(
        debug.str()
    );


    state.active =
        false;

    state.waiting_for_target =
        false;

    state.current_target =
        item_location();


    // Habla mediante el sistema real de CDDA.
    if( state.skipped > 0 && !state.last_failure_reason.empty() ) {
        who.say( string_format( npc_ai::localized_ai_message(
                                    _( "I picked up everything I could, but I couldn't take %1$s: %2$s" ),
                                    "Recogí todo lo que pude, pero no pude llevarme %1$s: %2$s" ),
                                state.last_failure_name, state.last_failure_reason ) );
    } else {
        who.say( npc_ai::localized_ai_message(
                     _( "Done. I picked up all the food I could carry." ),
                     "Listo. Recogí toda la comida que podía cargar." ) );
    }
}




// ============================================================
// CDDA-AI FOOD SEARCH ("busca comida")
// ============================================================

// Radii and spot caps per search range; see search_range in the header.
constexpr int food_search_radius_near = 6;
constexpr int food_search_radius_medium = 10;
constexpr int food_search_radius_far = 16;
constexpr int food_search_radius_indoors = 20;
constexpr std::size_t food_search_spot_limit_near = 30;
constexpr std::size_t food_search_spot_limit_medium = 40;
constexpr std::size_t food_search_spot_limit_far = 80;
constexpr std::size_t food_search_spot_limit_indoors = 80;
constexpr std::size_t indoor_flood_tile_limit = 1200;
constexpr int food_search_stuck_limit = 30;

struct food_search_state {
    bool active = false;
    search_range range = search_range::medium;
    // false: any food.  true: items matching query_words.
    bool named = false;
    std::string query_display;
    std::vector<std::string> query_words;
    std::deque<tripoint_abs_ms> spots;
    std::optional<tripoint_abs_ms> current;
    // The pickup batch spawned for the last inspection is still running.
    bool batch_running = false;
    // Places already inspected: never visited or scanned twice.
    std::set<tripoint_abs_ms> checked_spots;
    std::size_t checked = 0;
    std::size_t spots_with_food = 0;
    std::size_t collected = 0;
    // name -> count, for the final report.
    std::map<std::string, int> collected_names;
    int stuck_ticks = 0;
    // The companion was following when the order arrived.  Following is
    // suspended for the tour (the follow logic keeps pulling the companion
    // back towards the player) and restored on the way back or on cancel.
    bool was_following = false;
    // Tour finished: walking back to the player with the report pending.
    bool returning = false;
    std::string report;
    int return_ticks = 0;
};

// Safety net: deliver the report anyway if the way back takes too long.
constexpr int food_search_return_timeout_ticks = 600;

std::vector<queued_food_target> search_targets_on_tile( map &here, const tripoint_bub_ms &p,
        const food_search_state &state );

// While the tour runs the companion is neither following nor guarding: the
// follow logic would pull it back to the player every idle turn, and a guard
// counts as stationary and never reaches the pickup action.  Activities can
// revert the attitude, so this is re-asserted every tick of the tour.
void keep_follow_suspended( npc &who )
{
    if( who.is_walking_with() ) {
        who.set_attitude( NPCATT_NULL );
    }
    if( who.is_guarding() ) {
        who.set_mission( NPC_MISSION_NULL );
    }
}

// Inspects every pending spot the companion can examine from where it stands
// -- open piles in line of sight, containers only when adjacent -- removes
// them from the tour and returns the matching items found.  Walking is only
// needed for what cannot be seen from here.
std::vector<queued_food_target> inspect_spots_from_here( npc &who, map &here,
        food_search_state &state )
{
    std::vector<queued_food_target> targets;
    const tripoint_bub_ms origin = who.pos_bub( here );
    for( auto it = state.spots.begin(); it != state.spots.end(); ) {
        const tripoint_bub_ms spot = here.get_bub( *it );
        const bool adjacent = rl_dist( origin, spot ) <= 1;
        const bool examinable = adjacent ||
                                ( who.sees( here, spot ) && here.could_see_items( spot, who ) );
        if( !examinable ) {
            ++it;
            continue;
        }
        state.checked_spots.insert( *it );
        ++state.checked;
        std::vector<queued_food_target> here_targets;
        if( here.could_see_items( spot, who ) ) {
            here_targets = search_targets_on_tile( here, spot, state );
        }
        debug_line( "SPOT_CHECKED=" + spot.to_string_writable() + " | from=" +
                    origin.to_string_writable() + " | matches=" +
                    std::to_string( here_targets.size() ) );
        if( !here_targets.empty() ) {
            ++state.spots_with_food;
            targets.insert( targets.end(), here_targets.begin(), here_targets.end() );
        }
        it = state.spots.erase( it );
    }
    std::sort( targets.begin(), targets.end(), [&]( const queued_food_target &a,
    const queued_food_target &b ) {
        if( a.priority != b.priority ) {
            return a.priority < b.priority;
        }
        return rl_dist( origin, a.location.pos_bub( here ) ) <
               rl_dist( origin, b.location.pos_bub( here ) );
    } );
    return targets;
}

// Next place to walk to: the pending spot closest to where the companion is
// now (greedy tour), not to where the order was given.
std::optional<tripoint_abs_ms> take_nearest_spot( npc &who, map &here, food_search_state &state )
{
    const tripoint_bub_ms origin = who.pos_bub( here );
    auto best = state.spots.end();
    int best_distance = std::numeric_limits<int>::max();
    for( auto it = state.spots.begin(); it != state.spots.end(); ++it ) {
        const int distance = rl_dist( origin, here.get_bub( *it ) );
        if( distance < best_distance ) {
            best_distance = distance;
            best = it;
        }
    }
    if( best == state.spots.end() ) {
        return std::nullopt;
    }
    const tripoint_abs_ms chosen = *best;
    state.spots.erase( best );
    return chosen;
}

// Queues a pickup batch for the matches found; false when nothing could start.
bool start_search_pickups( npc &who, food_search_state &state,
                           const std::vector<queued_food_target> &targets )
{
    if( targets.empty() ) {
        return false;
    }
    const int key = npc_key( who );
    food_batch_state pickup;
    pickup.active = true;
    pickup.found = targets.size();
    for( const queued_food_target &target : targets ) {
        pickup.queue.push_back( target );
    }
    food_batches[key] = pickup;
    if( !start_next_target( who, food_batches[key] ) ) {
        food_batches.erase( key );
        return false;
    }
    keep_follow_suspended( who );
    state.batch_running = true;
    return true;
}

std::string normalize_search_text( const std::string &text )
{
    std::u32string codepoints = utf8_to_utf32( text );
    for( char32_t &codepoint : codepoints ) {
        u32_to_lowercase( codepoint );
        remove_accent( codepoint );
    }
    std::string normalized;
    bool previous_space = true;
    for( const char32_t codepoint : codepoints ) {
        if( ( codepoint >= U'a' && codepoint <= U'z' ) ||
            ( codepoint >= U'0' && codepoint <= U'9' ) ) {
            normalized.push_back( static_cast<char>( codepoint ) );
            previous_space = false;
        } else if( codepoint > 127 ) {
            // Non-Latin letters survive as UTF-8 so names in other scripts
            // still match against themselves.
            normalized += utf32_to_utf8( codepoint );
            previous_space = false;
        } else if( !previous_space ) {
            normalized.push_back( ' ' );
            previous_space = true;
        }
    }
    while( !normalized.empty() && normalized.back() == ' ' ) {
        normalized.pop_back();
    }
    return normalized;
}

bool item_matches_search_words( const item &it, const std::vector<std::string> &words )
{
    if( words.empty() ) {
        return false;
    }
    const std::string name = normalize_search_text( remove_color_tags( it.tname() ) );
    const std::string id = normalize_search_text( it.typeId().str() );
    for( const std::string &word : words ) {
        if( name.find( word ) == std::string::npos && id.find( word ) == std::string::npos ) {
            return false;
        }
    }
    return true;
}

std::unordered_map<int, food_search_state> food_searches;

// A place worth checking: an item pile, or a container furniture whose
// contents can only be seen from an adjacent tile (fridge, cupboard, shelf).
bool tile_is_food_spot( map &here, const tripoint_bub_ms &p )
{
    static const std::string container_flag( "CONTAINER" );
    const bool container = here.has_flag_ter_or_furn( container_flag, p );
    if( container && here.has_flag_ter_or_furn( ter_furn_flag::TFLAG_SEALED, p ) ) {
        return false;
    }
    return container || here.has_items( p );
}

std::vector<queued_food_target> search_targets_on_tile( map &here, const tripoint_bub_ms &p,
        const food_search_state &state )
{
    std::vector<queued_food_target> targets;
    for( item &it : here.i_at( p ) ) {
        if( state.named ) {
            if( !item_matches_search_words( it, state.query_words ) ) {
                continue;
            }
        } else if( !( it.is_food() || it.is_food_container() ) ) {
            continue;
        }
        item_location location( map_cursor( p ), &it );
        if( !location ) {
            continue;
        }
        queued_food_target target;
        target.location = location;
        target.id = it.typeId().str();
        target.name = remove_color_tags( it.tname() );
        if( state.named ) {
            target.priority = 0;
            target.priority_reason = "requested";
        } else {
            const food_priority_info priority_info = classify_food_priority( target.name );
            target.priority = priority_info.priority;
            target.quota_group = priority_info.quota_group;
            target.priority_reason = priority_info.reason;
        }
        targets.push_back( target );
    }
    std::sort( targets.begin(), targets.end(), []( const queued_food_target &a,
    const queued_food_target &b ) {
        return a.priority < b.priority;
    } );
    return targets;
}

// Same approach as the fire task: path to the closest passable tile next to
// the spot (the spot itself may be impassable furniture).
bool route_next_to_spot( npc &who, const tripoint_bub_ms &spot )
{
    map &here = get_map();
    const tripoint_bub_ms origin = who.pos_bub( here );
    std::vector<tripoint_bub_ms> candidates = closest_points_first( spot, 1 );
    std::sort( candidates.begin(), candidates.end(), [&]( const tripoint_bub_ms &lhs,
    const tripoint_bub_ms &rhs ) {
        return rl_dist( origin, lhs ) < rl_dist( origin, rhs );
    } );
    for( const tripoint_bub_ms &candidate : candidates ) {
        if( candidate.z() != spot.z() || !here.passable_through( candidate ) ||
            g->is_dangerous_tile( candidate ) ) {
            continue;
        }
        if( candidate == origin ) {
            who.path.clear();
            return true;
        }
        if( who.update_path( candidate, true ) && !who.path.empty() ) {
            return true;
        }
    }
    return false;
}

void start_return_to_player( npc &who, food_search_state &state, const std::string &report );
void cancel_same_item_searches( const npc &finder, const food_search_state &done );

void finish_food_search( npc &who, food_search_state &state )
{
    std::ostringstream debug;
    debug << "SEARCH_FINISHED | checked=" << state.checked << " | spots_with_food="
          << state.spots_with_food << " | collected=" << state.collected;
    debug_line( debug.str() );
    const std::string what = state.named ? state.query_display :
                             npc_ai::localized_ai_message( _( "food" ), "comida" );
    if( !state.spots.empty() ) {
        debug_line( "SEARCH_STOPPED_EARLY | pending=" + std::to_string( state.spots.size() ) );
    }
    if( state.collected_names.empty() ) {
        state.report = string_format( npc_ai::localized_ai_message(
                                          _( "I checked %1$d places and found no %2$s I could take." ),
                                          "Revisé %1$d sitios y no encontré %2$s que pudiera llevarme." ),
                                      static_cast<int>( state.checked ), what );
    } else {
        std::string listing;
        for( const std::pair<const std::string, int> &entry : state.collected_names ) {
            if( !listing.empty() ) {
                listing += ", ";
            }
            listing += string_format( "%d x %s", entry.second, entry.first );
        }
        state.report = string_format( npc_ai::localized_ai_message(
                                          _( "I checked %1$d places.  I picked up: %2$s." ),
                                          "Revisé %1$d sitios.  Recogí: %2$s." ),
                                      static_cast<int>( state.checked ), listing );
    }

    // The tour is over: rejoin the player and deliver the report on arrival.
    // A guarding companion goes back to following, and the walk aims at
    // where the player is right now so the return is deliberate instead of
    // waiting for the follow logic to notice the distance.
    if( !who.is_player_ally() || who.is_dead_state() ) {
        who.say( state.report );
        food_searches.erase( npc_key( who ) );
        return;
    }
    if( state.named && state.collected > 0 ) {
        cancel_same_item_searches( who, state );
    }
    start_return_to_player( who, state, state.report );
}

// Sends a companion back to the player with a pending report.
void start_return_to_player( npc &who, food_search_state &state, const std::string &report )
{
    if( !who.is_following() ) {
        // Restores the follow suspended at the start of the tour (or ends a
        // guard post the companion held before the order).
        talk_function::stop_guard( who );
    }
    who.goto_to_this_pos = get_player_character().pos_abs();
    who.path.clear();
    state.current.reset();
    state.spots.clear();
    state.batch_running = false;
    state.report = report;
    state.returning = true;
    state.return_ticks = 0;
    // A pickup batch still running would fight the walk back.
    food_batches.erase( npc_key( who ) );
    debug_line( "SEARCH_RETURN_TO_PLAYER=" +
                get_player_character().pos_bub().to_string_writable() );
}

// A group "find X" order ends for everyone when one companion has X in hand:
// the others stop where they are and walk back too.
void cancel_same_item_searches( const npc &finder, const food_search_state &done )
{
    if( !done.named ) {
        return;
    }
    for( std::pair<const int, food_search_state> &entry : food_searches ) {
        food_search_state &other = entry.second;
        if( entry.first == npc_key( finder ) || !other.active || !other.named ||
            other.returning || other.query_words != done.query_words ) {
            continue;
        }
        npc *companion = g->find_npc( character_id( entry.first ) );
        if( companion == nullptr ) {
            other.active = false;
            continue;
        }
        debug_line( "SEARCH_CANCELLED_BY_GROUP | npc=" + companion->get_name() );
        start_return_to_player( *companion, other, string_format(
                                    npc_ai::localized_ai_message(
                                        _( "%1$s already found %2$s, I'm heading back." ),
                                        "%1$s ya encontró %2$s, vuelvo." ),
                                    finder.get_name(), done.query_display ) );
    }
}

// Returning phase: the normal follow logic walks the companion back (the
// goto destination was set on the player); once next to the player, or
// when the walk is over, the report is spoken and the search is closed.
void continue_return_to_player( npc &who, food_search_state &state )
{
    map &here = get_map();
    const Character &player = get_player_character();
    const bool next_to_player = rl_dist( who.pos_bub( here ), player.pos_bub( here ) ) <= 2 &&
                                who.sees( here, player.pos_bub( here ) );
    const bool walk_over = !who.goto_to_this_pos.has_value();
    const bool timed_out = ++state.return_ticks > food_search_return_timeout_ticks;
    if( !next_to_player && !walk_over && !timed_out ) {
        return;
    }
    debug_line( std::string( "SEARCH_REPORT_DELIVERED | " ) +
                ( next_to_player ? "next_to_player" : walk_over ? "walk_over" : "timeout" ) );
    who.goto_to_this_pos = std::nullopt;
    who.say( state.report );
    food_searches.erase( npc_key( who ) );
}

// Shared start of both search orders: collects the spots and arms the state.
npc_ai::search_food_command_result begin_search( npc &who, food_search_state state,
        const std::string &accept_message )
{
    npc_ai::search_food_command_result result;
    result.handled = true;
    if( who.has_player_activity() ) {
        result.message = npc_ai::localized_ai_message(
                             _( "I am busy with another task right now." ),
                             "Ahora mismo estoy ocupado con otra tarea." );
        return result;
    }

    map &here = get_map();
    const tripoint_bub_ms origin = who.pos_bub( here );
    const int radius = search_range_radius( state.range );
    const std::size_t spot_limit = search_range_spot_limit( state.range );

    // "Solo dentro de casa": the reachable roofed tiles around the companion,
    // flooding through doors but never onto an outside tile.  Spots must be
    // in that set or right next to it (containers are impassable).
    std::set<tripoint_bub_ms> indoor_tiles;
    if( state.range == search_range::indoors ) {
        if( here.is_outside( origin ) ) {
            result.message = npc_ai::localized_ai_message(
                                 _( "I'm not inside a building right now." ),
                                 "Ahora mismo no estoy dentro de un edificio." );
            debug_line( "RESULT=NOT_INDOORS" );
            return result;
        }
        std::deque<tripoint_bub_ms> frontier;
        indoor_tiles.insert( origin );
        frontier.push_back( origin );
        while( !frontier.empty() && indoor_tiles.size() < indoor_flood_tile_limit ) {
            const tripoint_bub_ms current = frontier.front();
            frontier.pop_front();
            for( const tripoint_bub_ms &next : here.points_in_radius( current, 1, 0 ) ) {
                if( next == current || indoor_tiles.count( next ) != 0 ||
                    rl_dist( origin, next ) > radius || here.is_outside( next ) ||
                    !here.passable( next ) ) {
                    continue;
                }
                indoor_tiles.insert( next );
                frontier.push_back( next );
            }
        }
        debug_line( "INDOOR_TILES=" + std::to_string( indoor_tiles.size() ) );
    }
    const auto indoors_ok = [&]( const tripoint_bub_ms & p ) {
        if( state.range != search_range::indoors ) {
            return true;
        }
        if( here.is_outside( p ) ) {
            return false;
        }
        if( indoor_tiles.count( p ) != 0 ) {
            return true;
        }
        for( const tripoint_bub_ms &neighbour : here.points_in_radius( p, 1, 0 ) ) {
            if( indoor_tiles.count( neighbour ) != 0 ) {
                return true;
            }
        }
        return false;
    };

    std::vector<tripoint_bub_ms> spots;
    for( const tripoint_bub_ms &p : here.points_in_radius( origin, radius, 0 ) ) {
        if( p == origin || !tile_is_food_spot( here, p ) || !indoors_ok( p ) ) {
            continue;
        }
        spots.push_back( p );
    }
    // Nearest first; unreachable spots are dropped while walking, so the
    // order only needs to be a sensible tour.
    std::sort( spots.begin(), spots.end(), [&]( const tripoint_bub_ms &lhs,
    const tripoint_bub_ms &rhs ) {
        return rl_dist( origin, lhs ) < rl_dist( origin, rhs );
    } );
    if( spots.size() > spot_limit ) {
        spots.resize( spot_limit );
    }
    debug_line( "SEARCH_RANGE=" + search_range_phrase( state.range ) + " | radius=" +
                std::to_string( radius ) + " | SEARCH_SPOTS=" + std::to_string( spots.size() ) );
    if( spots.empty() ) {
        result.message = npc_ai::localized_ai_message(
                             _( "There is nowhere nearby worth checking." ),
                             "No veo ningún sitio cercano donde buscar." );
        debug_line( "RESULT=NO_SPOTS" );
        return result;
    }

    state.active = true;
    // Suspend following for the tour: a follower is pulled back towards the
    // player every turn it is not walking, which ruins the search.  A guard
    // is released too (guards are stationary and never pick anything up);
    // both follow the player again on the way back.
    // A search supersedes a hideout: the companion leaves the room anyway.
    npc_ai::cancel_hide( who );
    state.was_following = who.is_following();
    keep_follow_suspended( who );
    debug_line( "SEARCH_FOLLOW_SUSPENDED" );
    for( const tripoint_bub_ms &p : spots ) {
        const tripoint_abs_ms abs = here.get_abs( p );
        if( state.checked_spots.count( abs ) == 0 &&
            std::find( state.spots.begin(), state.spots.end(), abs ) == state.spots.end() ) {
            state.spots.push_back( abs );
        }
    }
    food_searches[npc_key( who )] = state;
    // Any batch still running from an earlier order is superseded.
    food_batches.erase( npc_key( who ) );

    result.started = true;
    result.message = accept_message;
    debug_line( "RESULT=STARTED" );
    return result;
}

} // namespace

namespace npc_ai
{


batch_pickup_command_result try_handle_batch_pickup_command(
    npc &who,
    const std::string &player_line
)
{
    batch_pickup_command_result result;


    if(
        !is_batch_food_command(
            player_line
        )
    ) {

        return result;
    }


    result.handled =
        true;


    reset_debug(
        who,
        player_line
    );


    debug_line(
        "INTENT=BATCH_FOOD_PICKUP"
    );


    const std::vector<queued_food_target>
    food =
        find_room_food(
            who
        );


    if(
        food.empty()
    ) {

        result.message =
            "No veo comida suelta que pueda recoger en esta habitacion.";


        debug_line(
            "RESULT=NO_VISIBLE_FOOD"
        );


        return result;
    }


    food_batch_state state;

    state.active =
        true;

    state.found =
        food.size();


    for(
        const queued_food_target &target :
        food
    ) {

        state.queue.push_back(
            target
        );
    }


    food_batches[
        npc_key(
            who
        )
    ] =
        state;


    food_batch_state &stored =
        food_batches[
            npc_key(
                who
            )
        ];


    const bool started =
        start_next_target(
            who,
            stored
        );


    if(
        !started
    ) {

        stored.active =
            false;


        result.message = stored.last_failure_reason.empty() ?
                         npc_ai::localized_ai_message(
                             _( "I can see food, but I can't pick any of it up right now." ),
                             "Veo comida, pero ahora mismo no puedo recogerla." ) :
                         string_format( _( "I can't pick up %1$s: %2$s" ),
                                        stored.last_failure_name, stored.last_failure_reason );


        debug_line(
            "RESULT=NO_TARGET_COULD_START"
        );


        return result;
    }


    result.success =
        true;


    result.message =
        "Vale. Voy a recoger toda la comida de aqui que pueda cargar.";


    std::ostringstream result_debug;

    result_debug
        << "RESULT=STARTED"
        << " | candidates="
        << food.size();


    debug_line(
        result_debug.str()
    );


    return result;
}


void process_batch_pickup(
    npc &who
)
{
    const int key =
        npc_key(
            who
        );


    const auto found =
        food_batches.find(
            key
        );


    if(
        found ==
        food_batches.end()
    ) {

        return;
    }


    food_batch_state &state =
        found->second;


    if(
        !state.active
    ) {

        return;
    }


    if(
        state.waiting_for_target
    ) {

        // Mientras el item siga realmente en el mapa,
        // la accion dirigida todavia esta en curso.
        if(
            state.current_target &&
            state.current_target.where() ==
            item_location::type::map
        ) {

            ++state.waiting_ticks;


            if(
                state.waiting_ticks <=
                batch_target_timeout_ticks
            ) {

                return;
            }


            ++state.skipped;


            std::ostringstream timeout_debug;

            timeout_debug
                << "TARGET_TIMEOUT="
                << state.current_name
                << " | id="
                << state.current_id;


            debug_line(
                timeout_debug.str()
            );


            state.waiting_for_target =
                false;

            state.current_target =
                item_location();

            state.current_id.clear();

            state.current_name.clear();

            state.waiting_ticks =
                0;
        }
        else {

            ++state.collected;
            state.collected_names.push_back( state.current_name );


            std::ostringstream collected_debug;

            collected_debug
                << "TARGET_COMPLETED="
                << state.current_name
                << " | id="
                << state.current_id
                << " | collected="
                << state.collected;


            debug_line(
                collected_debug.str()
            );


            state.waiting_for_target =
                false;

            state.current_target =
                item_location();

            state.current_id.clear();

            state.current_name.clear();

            state.waiting_ticks =
                0;
        }
    }


    if(
        start_next_target(
            who,
            state
        )
    ) {

        return;
    }


    finish_batch(
        who,
        state
    );
}


bool is_search_food_command( const std::string &player_line )
{
    const std::string line = lower_ascii( player_line );
    if( contains_any( line, { "no busques", "no busquen", "don't look for", "do not look for" } ) ) {
        return false;
    }
    return contains_any( line, {
        "busca comida", "buscar comida", "busquen comida", "buscad comida",
        "busca algo de comer", "busquen algo de comer", "busca alimentos", "busquen alimentos",
        "search for food", "look for food", "find food", "find some food", "forage for food"
    } );
}

search_food_command_result try_handle_search_food_command( npc &who,
        const std::string &player_line )
{
    search_food_command_result result;
    if( !is_search_food_command( player_line ) ) {
        return result;
    }
    reset_debug( who, player_line );
    debug_line( "INTENT=FOOD_SEARCH" );
    food_search_state state;
    state.named = false;
    state.range = detect_search_range( player_line );
    return begin_search( who, state, npc_ai::localized_ai_message(
                             _( "Alright, I'll check around here for food." ),
                             "Vale, voy a revisar los alrededores en busca de comida." ) );
}

std::vector<std::string> normalize_search_words( const std::string &request )
{
    static const std::vector<std::string> filler = {
        "el", "la", "los", "las", "un", "una", "unos", "unas", "de", "del", "al", "mi", "mis",
        "tu", "tus", "su", "sus", "algun", "alguna", "algo", "por", "favor", "y", "trae", "traeme",
        "the", "a", "an", "some", "my", "any", "for", "me", "please", "and", "bring"
    };
    std::vector<std::string> words;
    std::istringstream stream( normalize_search_text( request ) );
    std::string word;
    while( stream >> word ) {
        if( word.size() < 2 ||
            std::find( filler.begin(), filler.end(), word ) != filler.end() ) {
            continue;
        }
        if( std::find( words.begin(), words.end(), word ) == words.end() ) {
            words.push_back( word );
        }
    }
    return words;
}

std::string parse_search_item_request( const std::string &player_line )
{
    if( is_search_food_command( player_line ) ) {
        return std::string();
    }
    const std::string normalized = normalize_search_text( player_line );
    static const std::vector<std::string> prefixes = {
        "busca y trae ", "buscad y traed ", "busquen y traigan ", "busca y traeme ",
        "busca ", "buscar ", "busquen ", "buscad ", "buscame ", "busqueme ",
        "search for ", "look for ", "find me ", "find "
    };
    for( const std::string &prefix : prefixes ) {
        if( normalized.compare( 0, prefix.size(), prefix ) == 0 &&
            normalized.size() > prefix.size() ) {
            std::string request = normalized.substr( prefix.size() );
            // A trailing range phrase belongs to the order, not to the object.
            static const std::vector<std::string> range_suffixes = {
                " solo dentro de casa", " dentro de casa", " dentro del edificio", " solo dentro",
                " a media distancia", " por la zona", " cerca", " lejos"
            };
            for( const std::string &suffix : range_suffixes ) {
                if( request.size() > suffix.size() &&
                    request.compare( request.size() - suffix.size(), suffix.size(), suffix ) == 0 ) {
                    request.erase( request.size() - suffix.size() );
                    break;
                }
            }
            return request;
        }
    }
    return std::string();
}

search_food_command_result try_handle_search_item_command( npc &who,
        const std::string &player_line )
{
    search_food_command_result result;
    const std::string request = parse_search_item_request( player_line );
    if( request.empty() ) {
        return result;
    }
    const std::vector<std::string> words = normalize_search_words( request );
    reset_debug( who, player_line );
    debug_line( "INTENT=ITEM_SEARCH | request=" + request );
    if( words.empty() ) {
        result.handled = true;
        result.message = npc_ai::localized_ai_message(
                             _( "What exactly should I look for?" ),
                             "¿Qué debo buscar exactamente?" );
        return result;
    }
    food_search_state state;
    state.named = true;
    state.range = detect_search_range( player_line );
    state.query_words = words;
    std::string display;
    for( const std::string &word : words ) {
        display += ( display.empty() ? "" : " " ) + word;
    }
    state.query_display = display;
    return begin_search( who, state, string_format( npc_ai::localized_ai_message(
                             _( "Alright, I'll look around here for %s." ),
                             "Vale, voy a buscar %s por los alrededores." ), display ) );
}

bool process_food_search( npc &who )
{
    const int key = npc_key( who );
    const auto found = food_searches.find( key );
    if( found == food_searches.end() || !found->second.active ) {
        return false;
    }
    food_search_state &state = found->second;

    if( state.returning ) {
        continue_return_to_player( who, state );
        // The follow logic owns the movement while walking back.
        return false;
    }
    keep_follow_suspended( who );

    // A pickup batch spawned by the last inspection drives itself through
    // process_batch_pickup and the directed pickup engine; wait for it.
    const auto batch = food_batches.find( key );
    if( batch != food_batches.end() && batch->second.active ) {
        return false;
    }
    if( state.batch_running ) {
        state.batch_running = false;
        if( batch != food_batches.end() ) {
            state.collected += batch->second.collected;
            for( const std::string &name : batch->second.collected_names ) {
                ++state.collected_names[name];
            }
            food_batches.erase( batch );
        }
    }

    map &here = get_map();

    // A named search ends as soon as the requested object is in hand.
    if( state.named && state.collected > 0 ) {
        finish_food_search( who, state );
        return false;
    }

    // Examine everything examinable from this position before walking
    // anywhere: piles in sight and any adjacent container.
    if( !state.current ) {
        const std::vector<queued_food_target> seen = inspect_spots_from_here( who, here, state );
        if( start_search_pickups( who, state, seen ) ) {
            // The directed pickup engine acts on this same turn.
            return false;
        }
        state.current = take_nearest_spot( who, here, state );
        if( !state.current ) {
            finish_food_search( who, state );
            return false;
        }
        state.stuck_ticks = 0;
        who.path.clear();
    }
    const tripoint_bub_ms spot = here.get_bub( *state.current );

    if( rl_dist( who.pos_bub( here ), spot ) > 1 ) {
        if( who.current_target() != nullptr ) {
            // Combat first; the search resumes afterwards.
            return false;
        }
        if( who.path.empty() && !route_next_to_spot( who, spot ) ) {
            debug_line( "SPOT_UNREACHABLE=" + spot.to_string_writable() );
            state.checked_spots.insert( *state.current );
            state.current.reset();
            who.mod_moves( -who.get_speed() );
            return true;
        }
        const tripoint_bub_ms before = who.pos_bub( here );
        who.move_to_next();
        keep_follow_suspended( who );
        if( who.pos_bub( here ) == before && ++state.stuck_ticks > food_search_stuck_limit ) {
            debug_line( "SPOT_STUCK=" + spot.to_string_writable() );
            state.checked_spots.insert( *state.current );
            state.current.reset();
            who.path.clear();
        }
        return true;
    }

    // Adjacent (or on top): the spot goes back to the pending list for one
    // tick so the shared inspection handles it together with everything else
    // examinable from here (neighbouring piles, the container next to it).
    state.spots.push_front( *state.current );
    state.current.reset();
    const std::vector<queued_food_target> targets = inspect_spots_from_here( who, here, state );
    if( start_search_pickups( who, state, targets ) ) {
        // The directed pickup engine acts on this same turn.
        return false;
    }
    // Nothing to take here; checking takes a moment.
    who.mod_moves( -who.get_speed() );
    return true;
}

search_range detect_search_range( const std::string &player_line )
{
    const std::string line = " " + normalize_search_text( player_line ) + " ";
    const auto has = [&]( const char *needle ) {
        return line.find( needle ) != std::string::npos;
    };
    if( has( " dentro de casa " ) || has( " dentro del edificio " ) || has( " solo dentro " ) ||
        has( " sin salir " ) || has( " indoors " ) || has( " inside the building " ) ) {
        return search_range::indoors;
    }
    if( has( " lejos " ) || has( " far " ) || has( " far away " ) ) {
        return search_range::distant;
    }
    if( has( " cerca " ) || has( " cerquita " ) || has( " nearby " ) || has( " close by " ) ) {
        return search_range::close;
    }
    return search_range::medium;
}

int search_range_radius( const search_range range )
{
    switch( range ) {
        case search_range::close:
            return food_search_radius_near;
        case search_range::medium:
            return food_search_radius_medium;
        case search_range::distant:
            return food_search_radius_far;
        case search_range::indoors:
            return food_search_radius_indoors;
    }
    return food_search_radius_medium;
}

std::size_t search_range_spot_limit( const search_range range )
{
    switch( range ) {
        case search_range::close:
            return food_search_spot_limit_near;
        case search_range::medium:
            return food_search_spot_limit_medium;
        case search_range::distant:
            return food_search_spot_limit_far;
        case search_range::indoors:
            return food_search_spot_limit_indoors;
    }
    return food_search_spot_limit_medium;
}

std::string search_range_phrase( const search_range range )
{
    switch( range ) {
        case search_range::close:
            return "cerca";
        case search_range::medium:
            return "a media distancia";
        case search_range::distant:
            return "lejos";
        case search_range::indoors:
            return "dentro de casa";
    }
    return "a media distancia";
}

std::string search_range_label( const search_range range )
{
    switch( range ) {
        case search_range::close:
            return npc_ai::localized_ai_message( _( "Nearby" ), "Cerca" );
        case search_range::medium:
            return npc_ai::localized_ai_message( _( "Medium" ), "Mediano" );
        case search_range::distant:
            return npc_ai::localized_ai_message( _( "Far" ), "Lejos" );
        case search_range::indoors:
            return npc_ai::localized_ai_message( _( "Only inside the building" ),
                                                 "Solo dentro de casa" );
    }
    return std::string();
}

std::string search_range_description( const search_range range )
{
    switch( range ) {
        case search_range::close:
            return string_format( npc_ai::localized_ai_message(
                                      _( "Checks up to %d tiles around the companion.  Quick, stays close to you." ),
                                      "Revisa hasta %d casillas alrededor del compañero.  Rápido, se queda cerca de ti." ),
                                  food_search_radius_near );
        case search_range::medium:
            return string_format( npc_ai::localized_ai_message(
                                      _( "Checks up to %d tiles around the companion.  A house and its yard." ),
                                      "Revisa hasta %d casillas alrededor del compañero.  Una casa y su patio." ),
                                  food_search_radius_medium );
        case search_range::distant:
            return string_format( npc_ai::localized_ai_message(
                                      _( "Checks up to %d tiles around the companion.  Several buildings; a long walk." ),
                                      "Revisa hasta %d casillas alrededor del compañero.  Varios edificios; un paseo largo." ),
                                  food_search_radius_far );
        case search_range::indoors:
            return string_format( npc_ai::localized_ai_message(
                                      _( "Only the building the companion is standing in, through its doors but never outside (up to %d tiles)." ),
                                      "Solo el edificio donde está el compañero, pasando por sus puertas pero sin salir fuera (hasta %d casillas)." ),
                                  food_search_radius_indoors );
    }
    return std::string();
}

std::size_t food_search_pending_spots( const npc &who )
{
    const auto found = food_searches.find( npc_key( who ) );
    return found == food_searches.end() ? 0 : found->second.spots.size();
}

bool has_food_search( const npc &who )
{
    const auto found = food_searches.find( npc_key( who ) );
    return found != food_searches.end() && found->second.active;
}

bool food_search_is_returning( const npc &who )
{
    const auto found = food_searches.find( npc_key( who ) );
    return found != food_searches.end() && found->second.active && found->second.returning;
}

void cancel_food_search( const npc &who )
{
    const auto found = food_searches.find( npc_key( who ) );
    if( found == food_searches.end() ) {
        return;
    }
    const bool restore_follow = !who.is_following() && who.is_player_ally();
    food_searches.erase( found );
    food_batches.erase( npc_key( who ) );
    if( restore_follow ) {
        // The tour suspended the follow; a cancelled tour gives it back before
        // whatever superseded it (a tactical order) applies its own state.
        if( npc *companion = g->find_npc( who.getID() ) ) {
            talk_function::stop_guard( *companion );
        }
    }
}

void reset_all_food_batches()
{
    food_batches.clear();
    food_searches.clear();
}


} // namespace npc_ai
