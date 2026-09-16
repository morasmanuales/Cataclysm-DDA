#include "npc_ai_interior.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>

#include "catacharset.h"
#include "creature.h"
#include "creature_tracker.h"
#include "game.h"
#include "map.h"
#include "npc.h"
#include "npc_ai_async.h"
#include "npc_ai_context.h"
#include "npctalk.h"
#include "pathfinding.h"
#include "point.h"
#include "translations.h"
#include "unicode.h"

namespace npc_ai
{
namespace
{

constexpr int interior_search_radius = 20;
constexpr int interior_assignment_radius = 5;
constexpr std::size_t max_anchor_candidates = 160;

std::string normalize_order( const std::string &text )
{
    std::u32string codepoints = utf8_to_utf32( text );
    for( char32_t &codepoint : codepoints ) {
        u32_to_lowercase( codepoint );
        remove_accent( codepoint );
    }
    std::string normalized;
    normalized.reserve( text.size() );
    bool previous_space = true;
    for( const char32_t codepoint : codepoints ) {
        if( ( codepoint >= U'a' && codepoint <= U'z' ) ||
            ( codepoint >= U'0' && codepoint <= U'9' ) ) {
            normalized.push_back( static_cast<char>( codepoint ) );
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

bool safe_interior_tile( map &here, const tripoint_bub_ms &tile )
{
    if( here.is_outside( tile ) || !here.has_floor_or_water( tile ) ||
        !here.passable_through( tile ) || g->is_dangerous_tile( tile ) || here.veh_at( tile ) ) {
        return false;
    }
    const Creature *occupant = get_creature_tracker().creature_at( tile );
    return occupant == nullptr;
}

std::vector<tripoint_bub_ms> reachable_route( map &here, const npc &who,
        const tripoint_bub_ms &destination )
{
    if( who.pos_bub() == destination ) {
        return { destination };
    }
    return here.route( who, pathfinding_target::point( destination ) );
}

} // namespace

structured_voice_order parse_structured_voice_order( const std::string &spoken )
{
    const std::string normalized = normalize_order( spoken );
    static const std::vector<std::string> commands = {
        "entren a la casa", "todos adentro", "metanse dentro", "refugiense dentro",
        "entremos a la casa", "entren", "adentro", "entra a la casa", "entra",
        "metete dentro", "refugiate dentro", "vamos adentro"
    };
    for( const std::string &command : commands ) {
        if( normalized == command ) {
            return structured_voice_order::enter_nearest_reachable_safe_interior;
        }
        if( normalized.size() <= command.size() ||
            normalized.compare( normalized.size() - command.size(), command.size(), command ) != 0 ||
            normalized[normalized.size() - command.size() - 1] != ' ' ) {
            continue;
        }
        // Permit a direct address ("Liam, entra...") or a short positive
        // lead-in, but never turn an explicit negation into an order.
        const std::string prefix = " " + normalized.substr(
                                       0, normalized.size() - command.size() - 1 ) + " ";
        if( prefix.find( " no " ) == std::string::npos &&
            prefix.find( " nunca " ) == std::string::npos ) {
            return structured_voice_order::enter_nearest_reachable_safe_interior;
        }
    }
    return structured_voice_order::none;
}

interior_order_result execute_enter_nearest_reachable_safe_interior(
    const std::vector<npc *> &raw_targets )
{
    interior_order_result result;
    result.handled = true;
    std::vector<npc *> targets;
    for( npc *candidate : raw_targets ) {
        if( candidate != nullptr && candidate->is_player_ally() && candidate->is_following() &&
            !candidate->is_dead_state() && !candidate->is_hallucination() ) {
            targets.push_back( candidate );
        }
    }
    if( targets.empty() ) {
        result.message = _( "No eligible ally received that order." );
        return result;
    }

    map &here = get_map();
    const tripoint_bub_ms origin = get_player_character().pos_bub();
    std::vector<tripoint_bub_ms> candidates;
    for( const tripoint_bub_ms &tile : here.points_in_radius( origin, interior_search_radius, 0 ) ) {
        if( safe_interior_tile( here, tile ) ) {
            candidates.push_back( tile );
        }
    }
    std::sort( candidates.begin(), candidates.end(), [&]( const tripoint_bub_ms &lhs,
    const tripoint_bub_ms &rhs ) {
        return rl_dist( origin, lhs ) < rl_dist( origin, rhs );
    } );
    if( candidates.size() > max_anchor_candidates ) {
        candidates.resize( max_anchor_candidates );
    }

    std::optional<tripoint_bub_ms> anchor;
    int best_anchor_score = std::numeric_limits<int>::max();
    for( const tripoint_bub_ms &candidate : candidates ) {
        int score = rl_dist( origin, candidate );
        bool reachable_by_all = true;
        for( const npc *who : targets ) {
            const std::vector<tripoint_bub_ms> route = reachable_route( here, *who, candidate );
            if( route.empty() ) {
                reachable_by_all = false;
                break;
            }
            score += static_cast<int>( route.size() );
        }
        if( reachable_by_all && score < best_anchor_score ) {
            anchor = candidate;
            best_anchor_score = score;
        }
    }
    if( !anchor ) {
        result.message = _( "I can't reach a safe interior from here." );
        return result;
    }

    std::vector<tripoint_bub_ms> destination_pool;
    for( const tripoint_bub_ms &tile : here.points_in_radius( *anchor,
            interior_assignment_radius, 0 ) ) {
        if( safe_interior_tile( here, tile ) ) {
            destination_pool.push_back( tile );
        }
    }
    std::sort( destination_pool.begin(), destination_pool.end(), [&]( const tripoint_bub_ms &lhs,
    const tripoint_bub_ms &rhs ) {
        return rl_dist( *anchor, lhs ) < rl_dist( *anchor, rhs );
    } );

    std::set<tripoint_bub_ms> reserved;
    for( npc *who : targets ) {
        std::optional<tripoint_bub_ms> chosen;
        int best_score = std::numeric_limits<int>::max();
        for( const tripoint_bub_ms &candidate : destination_pool ) {
            if( reserved.count( candidate ) != 0 ) {
                continue;
            }
            const std::vector<tripoint_bub_ms> route = reachable_route( here, *who, candidate );
            if( route.empty() ) {
                continue;
            }
            const int score = static_cast<int>( route.size() ) + rl_dist( *anchor, candidate );
            if( score < best_score ) {
                chosen = candidate;
                best_score = score;
            }
        }
        if( !chosen ) {
            continue;
        }
        reserved.insert( *chosen );
        const tripoint_abs_ms destination = here.get_abs( *chosen );
        who->goto_to_this_pos = destination;
        // Remember the tile so arrival turns into a guard post (see
        // on_move_destination_reached) instead of resuming the follow.
        who->ai_interior_hold_pos = destination;
        result.assignments.emplace_back( who->getID().get_value(), destination );
    }

    result.success = result.assignments.size() == targets.size();
    result.message = result.success ? _( "We're heading inside." ) :
                     result.assignments.empty() ? _( "I can't reach a safe interior from here." ) :
                     _( "Some of us can't reach a safe interior from here." );
    return result;
}

bool on_move_destination_reached( npc &who, const tripoint_abs_ms &reached )
{
    if( !who.ai_interior_hold_pos || *who.ai_interior_hold_pos != reached ) {
        // A manual "move here" order or an unrelated walk: nothing to do.
        return false;
    }
    who.ai_interior_hold_pos = std::nullopt;
    if( !who.is_player_ally() ) {
        return false;
    }
    // Same transition as the spoken "stay here": the companion stops
    // following and guards the interior tile it just reached.
    talk_function::assign_guard( who );
    who.set_guard_pos( reached );
    say_command_reply( who, localized_ai_message( _( "I'm inside.  I'll hold here." ),
                       "Ya estoy dentro.  Me quedo aquí." ) );
    return true;
}

void clear_interior_hold( npc &who )
{
    if( !who.ai_interior_hold_pos ) {
        return;
    }
    // The walk itself was only issued for the hold; drop it too so the
    // companion obeys the newer order at once.
    if( who.goto_to_this_pos && *who.goto_to_this_pos == *who.ai_interior_hold_pos ) {
        who.goto_to_this_pos = std::nullopt;
    }
    who.ai_interior_hold_pos = std::nullopt;
}

} // namespace npc_ai
