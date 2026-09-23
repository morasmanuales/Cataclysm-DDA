#include "npc_ai_hide.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <set>

#include "calendar.h"
#include "catacharset.h"
#include "creature.h"
#include "creature_tracker.h"
#include "effect.h"
#include "game.h"
#include "gates.h"
#include "map.h"
#include "mapdata.h"
#include "move_mode.h"
#include "npc.h"
#include "npc_ai_async.h"
#include "npc_ai_batch_pickup.h"
#include "npc_ai_context.h"
#include "npc_ai_debug.h"
#include "npc_ai_interior.h"
#include "npctalk.h"
#include "pathfinding.h"
#include "point.h"
#include "string_formatter.h"
#include "translations.h"
#include "unicode.h"

namespace npc_ai
{
namespace
{

// How far around the companion a hideout is looked for.
constexpr int hide_search_radius = 20;
// A room bigger than this is a hall or a warehouse, not a place to hide.
constexpr std::size_t room_tile_limit = 80;
// Cap on candidate tiles probed per search; the map is scanned nearest first.
constexpr std::size_t max_candidates = 240;
// Routes are computed only for the best few fallback tiles.
constexpr std::size_t max_fallback_routes = 24;
// Enemies this close to a hidden companion count as "found" even outside the
// room (only used when the hideout is not an enclosed room).
constexpr int fallback_threat_radius = 2;
// Hostiles are looked for within this range of the companion.
constexpr int threat_scan_radius = 10;
// Do not bounce between hideouts every turn.
constexpr int relocation_cooldown_turns = 5;
// Quiet turns before a cornered companion goes back to not fighting.
constexpr int cornered_quiet_ticks = 10;
// Turns boxed in next to an enemy without moving before giving up the run.
constexpr int boxed_in_ticks = 3;
// Turns without moving and without an enemy in contact before the route is
// considered broken and the hideout is chosen again from where it stands.
constexpr int replan_ticks = 6;
// Safety net for the walk out: after this many turns the rules are restored
// wherever the companion is.
constexpr int leaving_timeout_ticks = 30;
// Evading: how often a hideout is looked for again while keeping away.
constexpr int evade_replan_ticks = 5;
// A door left open is only worth a trip every so often: if it cannot be
// reached or somebody keeps opening it, the companion must not loop.
constexpr int door_retry_ticks = 20;
// Turns a door is left alone after it is first seen open: whoever opened it
// is probably still walking through, and shutting it in their face is worse
// than the couple of turns it stays open.
constexpr int door_wait_turns = 2;
// Relocating after being found: the new room has to be a real escape, not the
// next tile over, and it has to put some ground between them.
constexpr int relocation_min_distance = 3;
constexpr int relocation_min_hostile_distance = 6;
// Visual profile of a prone medium creature (creature.cpp: 120 * 0.275).  An
// obstacle with more coverage than this, right next to the companion on the
// line towards the viewer, hides a prone companion completely.
constexpr int prone_profile = 33;

static const move_mode_id move_mode_prone( "prone" );
static const move_mode_id move_mode_run( "run" );
static const efftype_id effect_npc_run_away( "npc_run_away" );
static const move_mode_id move_mode_walk( "walk" );

std::map<int, hide_state> hideouts;

int npc_key( const npc &who )
{
    return who.getID().get_value();
}

// Runtime trace (CDDA_NPC_AI_DEBUG=1), one line per event, in the user dir.
void hide_log( const npc &who, const std::string &line )
{
    append_debug_line( "npc_ai_hide_v1_runtime.txt",
                       string_format( "turn=%d npc=%s pos=%s %s", to_turn<int>( calendar::turn ),
                                      who.get_name(), who.pos_abs().to_string(), line ) );
}

const char *phase_name( const hide_phase phase )
{
    switch( phase ) {
        case hide_phase::travelling:
            return "travelling";
        case hide_phase::hidden:
            return "hidden";
        case hide_phase::cornered:
            return "cornered";
        case hide_phase::evading:
            return "evading";
        case hide_phase::closing_door:
            return "closing_door";
        case hide_phase::leaving:
            return "leaving";
    }
    return "?";
}

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

// A door, open or closed, whole or broken: the tile a room is entered by.
bool is_door_tile( const map &here, const tripoint_bub_ms &p )
{
    if( here.has_flag_ter_or_furn( ter_furn_flag::TFLAG_DOOR, p ) ||
        here.has_flag_ter_or_furn( ter_furn_flag::TFLAG_BARRICADABLE_DOOR, p ) ||
        here.has_flag_ter_or_furn( ter_furn_flag::TFLAG_BARRICADABLE_DOOR_DAMAGED, p ) ||
        here.has_flag_ter_or_furn( ter_furn_flag::TFLAG_BARRICADABLE_DOOR_REINFORCED, p ) ) {
        return true;
    }
    const ter_t &ter = here.ter( p ).obj();
    // Anything that closes into something else and is not a window.
    return !ter.close.is_null() && !here.has_flag_ter_or_furn( ter_furn_flag::TFLAG_WINDOW, p );
}

bool is_window_tile( const map &here, const tripoint_bub_ms &p )
{
    return here.has_flag_ter_or_furn( ter_furn_flag::TFLAG_WINDOW, p ) ||
           here.has_flag_ter_or_furn( ter_furn_flag::TFLAG_BARRICADABLE_WINDOW_CURTAINS, p );
}

// Interior tile a companion can stand on.  `who` may already be standing there.
bool standable_interior_tile( map &here, const npc &who, const tripoint_bub_ms &tile )
{
    if( here.is_outside( tile ) || !here.has_floor_or_water( tile ) ||
        !here.passable_through( tile ) || g->is_dangerous_tile( tile ) || here.veh_at( tile ) ) {
        return false;
    }
    const Creature *occupant = get_creature_tracker().creature_at( tile );
    return occupant == nullptr || occupant == &who;
}

struct room_info {
    std::set<tripoint_bub_ms> tiles;
    std::vector<tripoint_bub_ms> doors;
    // Intact windows on the boundary: somebody outside can look in.
    std::vector<tripoint_bub_ms> windows;
    bool enclosed = false;
};

// Flood the room around `start` without crossing doors.  The room is enclosed
// when the flood never reaches an outside tile, stays small and has at least
// one door (a broken window frame or an empty door frame is a leak: nothing
// there can be closed).
room_info flood_room( map &here, const tripoint_bub_ms &start )
{
    room_info room;
    std::deque<tripoint_bub_ms> queue;
    std::set<tripoint_bub_ms> boundary;
    bool leak = false;
    room.tiles.insert( start );
    queue.push_back( start );
    while( !queue.empty() ) {
        const tripoint_bub_ms p = queue.front();
        queue.pop_front();
        if( room.tiles.size() > room_tile_limit ) {
            leak = true;
            break;
        }
        for( const tripoint_bub_ms &n : here.points_in_radius( p, 1, 0 ) ) {
            if( n == p || room.tiles.count( n ) != 0 ) {
                continue;
            }
            if( is_door_tile( here, n ) ) {
                if( boundary.insert( n ).second ) {
                    room.doors.push_back( n );
                }
                continue;
            }
            if( here.impassable( n ) ) {
                if( boundary.insert( n ).second && is_window_tile( here, n ) ) {
                    room.windows.push_back( n );
                }
                continue;
            }
            if( here.is_outside( n ) || !here.has_floor_or_water( n ) ) {
                leak = true;
                continue;
            }
            room.tiles.insert( n );
            queue.push_back( n );
        }
    }
    room.enclosed = !leak && !room.doors.empty() && room.tiles.size() <= room_tile_limit;
    return room;
}

std::vector<tripoint_bub_ms> visible_hostiles( const npc &who, map &here, const int radius )
{
    std::vector<tripoint_bub_ms> hostiles;
    creature_tracker &creatures = get_creature_tracker();
    const tripoint_bub_ms origin = who.pos_bub( here );
    for( const tripoint_bub_ms &p : here.points_in_radius( origin, radius, 0 ) ) {
        const Creature *critter = creatures.creature_at( p );
        if( critter == nullptr || critter == &who || critter->is_dead_state() ) {
            continue;
        }
        if( critter->attitude_to( who ) != Creature::Attitude::HOSTILE ) {
            continue;
        }
        if( who.sees( here, *critter ) ) {
            hostiles.push_back( p );
        }
    }
    return hostiles;
}

std::vector<tripoint_bub_ms> route_to( map &here, const npc &who, const tripoint_bub_ms &dest )
{
    if( who.pos_bub( here ) == dest ) {
        return { dest };
    }
    return here.route( who, pathfinding_target::point( dest ) );
}

struct hideout_choice {
    std::optional<tripoint_bub_ms> tile;
    std::set<tripoint_abs_ms> room;
    std::set<tripoint_abs_ms> doors;
    bool enclosed = false;
};

// Nearest enclosed room the companion can reach, scored by route length,
// number of doors and windows and size; the tile inside is the one farthest
// from the doors.  Without any enclosed room: the reachable interior tile
// farthest from the visible enemies (nearest one when nothing is in sight).
// Extra demands on the room chosen, used when running away from something:
// a hideout one step from here, or one the enemy is already next to, is no
// escape at all.
struct hideout_limits {
    int min_from_origin = 0;
    int min_from_hostiles = 0;
};

hideout_choice find_hideout( npc &who, map &here, const std::set<tripoint_abs_ms> &reserved,
                             const std::set<tripoint_abs_ms> &avoid_room,
                             const std::vector<tripoint_bub_ms> &hostiles,
                             const hideout_limits &limits = hideout_limits() )
{
    hideout_choice choice;
    const tripoint_bub_ms origin = who.pos_bub( here );
    std::vector<tripoint_bub_ms> candidates;
    for( const tripoint_bub_ms &tile : here.points_in_radius( origin, hide_search_radius, 0 ) ) {
        // A doorway is never a hideout, and a flood started on a door would
        // spill into both spaces it joins and mark the room as leaking.
        if( standable_interior_tile( here, who, tile ) && !is_door_tile( here, tile ) ) {
            candidates.push_back( tile );
        }
    }
    std::sort( candidates.begin(), candidates.end(), [&]( const tripoint_bub_ms &lhs,
    const tripoint_bub_ms &rhs ) {
        return rl_dist( origin, lhs ) < rl_dist( origin, rhs );
    } );
    if( candidates.size() > max_candidates ) {
        candidates.resize( max_candidates );
    }

    const auto is_reserved = [&]( const tripoint_bub_ms & tile ) {
        return reserved.count( here.get_abs( tile ) ) != 0;
    };
    const auto min_dist_to = [&]( const tripoint_bub_ms & tile,
    const std::vector<tripoint_bub_ms> &points ) {
        int best = std::numeric_limits<int>::max();
        for( const tripoint_bub_ms &p : points ) {
            best = std::min( best, rl_dist( tile, p ) );
        }
        return best;
    };
    // True when, lying on `tile`, the companion is behind an obstacle (a
    // counter, a table, a wall corner) from every one of `viewers`.
    const auto covered_from = [&]( const tripoint_bub_ms & tile,
    const std::vector<tripoint_bub_ms> &viewers ) {
        if( viewers.empty() ) {
            return false;
        }
        for( const tripoint_bub_ms &viewer : viewers ) {
            if( viewer == tile || here.obstacle_coverage( viewer, tile ) <= prone_profile ) {
                return false;
            }
        }
        return true;
    };

    std::set<tripoint_bub_ms> classified;
    std::vector<tripoint_bub_ms> open_tiles;
    int best_score = std::numeric_limits<int>::max();
    for( const tripoint_bub_ms &candidate : candidates ) {
        if( classified.count( candidate ) != 0 ) {
            continue;
        }
        const room_info room = flood_room( here, candidate );
        classified.insert( room.tiles.begin(), room.tiles.end() );
        if( !room.enclosed ) {
            open_tiles.push_back( candidate );
            continue;
        }
        // Never pick a room with an enemy already inside, seen or not: a
        // hideout is checked before committing to it.
        creature_tracker &creatures = get_creature_tracker();
        bool skip = false;
        for( const tripoint_bub_ms &tile : room.tiles ) {
            if( avoid_room.count( here.get_abs( tile ) ) != 0 ||
                std::find( hostiles.begin(), hostiles.end(), tile ) != hostiles.end() ) {
                skip = true;
                break;
            }
            const Creature *occupant = creatures.creature_at( tile );
            if( occupant != nullptr && occupant != &who &&
                occupant->attitude_to( who ) == Creature::Attitude::HOSTILE ) {
                skip = true;
                break;
            }
        }
        if( skip ) {
            continue;
        }
        // Best standable tile: behind cover from every door and window first
        // (lying there, nobody looking in sees the companion), then farthest
        // from the doors, then nearest to us.
        std::vector<tripoint_bub_ms> viewpoints = room.doors;
        viewpoints.insert( viewpoints.end(), room.windows.begin(), room.windows.end() );
        std::optional<tripoint_bub_ms> deepest;
        bool deepest_covered = false;
        int deepest_depth = -1;
        for( const tripoint_bub_ms &tile : room.tiles ) {
            if( is_reserved( tile ) || !standable_interior_tile( here, who, tile ) ) {
                continue;
            }
            const bool covered = covered_from( tile, viewpoints );
            const int depth = min_dist_to( tile, room.doors );
            const bool better = !deepest ||
                                ( covered && !deepest_covered ) ||
                                ( covered == deepest_covered && ( depth > deepest_depth ||
                                        ( depth == deepest_depth &&
                                          rl_dist( origin, tile ) < rl_dist( origin, *deepest ) ) ) );
            if( better ) {
                deepest = tile;
                deepest_covered = covered;
                deepest_depth = depth;
            }
        }
        if( !deepest ) {
            continue;
        }
        if( limits.min_from_origin > 0 && rl_dist( origin, *deepest ) < limits.min_from_origin ) {
            continue;
        }
        if( limits.min_from_hostiles > 0 && !hostiles.empty() &&
            min_dist_to( *deepest, hostiles ) < limits.min_from_hostiles ) {
            continue;
        }
        const std::vector<tripoint_bub_ms> route = route_to( here, who, *deepest );
        if( route.empty() ) {
            continue;
        }
        const int score = static_cast<int>( route.size() ) +
                          4 * static_cast<int>( room.doors.size() ) +
                          2 * static_cast<int>( room.windows.size() ) +
                          static_cast<int>( room.tiles.size() ) / 8;
        if( score < best_score ) {
            best_score = score;
            choice.tile = deepest;
            choice.enclosed = true;
            choice.room.clear();
            for( const tripoint_bub_ms &tile : room.tiles ) {
                choice.room.insert( here.get_abs( tile ) );
            }
            choice.doors.clear();
            for( const tripoint_bub_ms &door : room.doors ) {
                choice.doors.insert( here.get_abs( door ) );
            }
        }
    }
    if( choice.tile ) {
        return choice;
    }

    // Fallback: no enclosed room.  Interior tile farthest from the enemies
    // (nearest one when none is in sight), never next to one.
    std::vector<tripoint_bub_ms> pool;
    for( const tripoint_bub_ms &tile : open_tiles ) {
        if( is_reserved( tile ) || avoid_room.count( here.get_abs( tile ) ) != 0 ) {
            continue;
        }
        if( !hostiles.empty() && min_dist_to( tile, hostiles ) <= fallback_threat_radius ) {
            continue;
        }
        pool.push_back( tile );
    }
    std::sort( pool.begin(), pool.end(), [&]( const tripoint_bub_ms &lhs,
    const tripoint_bub_ms &rhs ) {
        if( !hostiles.empty() ) {
            const bool lhs_covered = covered_from( lhs, hostiles );
            const bool rhs_covered = covered_from( rhs, hostiles );
            if( lhs_covered != rhs_covered ) {
                return lhs_covered;
            }
            const int lhs_dist = min_dist_to( lhs, hostiles );
            const int rhs_dist = min_dist_to( rhs, hostiles );
            if( lhs_dist != rhs_dist ) {
                return lhs_dist > rhs_dist;
            }
        }
        return rl_dist( origin, lhs ) < rl_dist( origin, rhs );
    } );
    if( pool.size() > max_fallback_routes ) {
        pool.resize( max_fallback_routes );
    }
    for( const tripoint_bub_ms &tile : pool ) {
        if( !route_to( here, who, tile ).empty() ) {
            choice.tile = tile;
            choice.enclosed = false;
            return choice;
        }
    }
    return choice;
}

// Rules the hideout forces, and the value it forces them to.  Everything
// else stays as the player configured it.
struct forced_rule {
    ally_rule rule;
    bool value;
};
constexpr forced_rule hide_rules[] = {
    { ally_rule::forbid_engage, true },   // never start a fight (lifted while cornered)
    { ally_rule::close_doors, true },     // shut doors behind
    { ally_rule::ignore_noise, true },    // do not go looking for noises
    { ally_rule::use_silent, true },      // no gunfire from the hideout
    { ally_rule::avoid_doors, false },    // doors must be usable to get in and out
};

void backup_rules( const npc &who, hide_state &state )
{
    state.rule_backups.clear();
    for( const forced_rule &forced : hide_rules ) {
        hide_state::rule_backup backup;
        backup.rule = static_cast<int>( forced.rule );
        backup.was_enabled = who.rules.has_override_enable( forced.rule );
        backup.was_set = who.rules.has_override( forced.rule );
        state.rule_backups.push_back( backup );
    }
}

// Force the hideout's rules for the current phase.  Called at the order and
// again every turn, so a vanilla "clear overrides" never sticks.
void restore_rules( npc &who, const hide_state &state );

void apply_rules( npc &who, const hide_state &state )
{
    if( state.phase == hide_phase::leaving ) {
        // Walking out: the player's rules are back, except that the door
        // must stay openable until the companion is through it.
        restore_rules( who, state );
        who.rules.enable_override( ally_rule::avoid_doors );
        who.rules.clear_override( ally_rule::avoid_doors );
        return;
    }
    for( const forced_rule &forced : hide_rules ) {
        bool value = forced.value;
        if( forced.rule == ally_rule::forbid_engage && state.phase == hide_phase::cornered ) {
            // Cornered: the player's own engagement rules decide.
            who.rules.disable_override( forced.rule );
            who.rules.clear_override( forced.rule );
            continue;
        }
        who.rules.enable_override( forced.rule );
        if( value ) {
            who.rules.set_override( forced.rule );
        } else {
            who.rules.clear_override( forced.rule );
        }
    }
}

void restore_rules( npc &who, const hide_state &state )
{
    for( const hide_state::rule_backup &backup : state.rule_backups ) {
        const ally_rule rule = static_cast<ally_rule>( backup.rule );
        if( backup.was_enabled ) {
            who.rules.enable_override( rule );
        } else {
            who.rules.disable_override( rule );
        }
        if( backup.was_set ) {
            who.rules.set_override( rule );
        } else {
            who.rules.clear_override( rule );
        }
    }
}

void close_doors_around( npc &who, map &here )
{
    const tripoint_bub_ms origin = who.pos_bub( here );
    for( const tripoint_bub_ms &n : here.points_in_radius( origin, 1, 0 ) ) {
        if( n != origin && is_door_tile( here, n ) && here.passable( n ) ) {
            doors::close_door( here, who, n );
        }
    }
}

void stand_up( npc &who )
{
    if( who.is_prone() || who.is_running() ) {
        who.set_movement_mode( move_mode_walk );
    }
}

bool exhausted( const npc &who )
{
    return !who.can_run() || who.get_stamina() < who.get_stamina_max() / 4;
}

// Run while there is breath for it; the vanilla stamina code only slows a
// runner down, it never switches an NPC back to walking on its own.
void run_or_walk( npc &who )
{
    const bool winded = exhausted( who );
    if( winded ) {
        if( who.is_running() || who.is_prone() ) {
            who.set_movement_mode( move_mode_walk );
        }
    } else if( !who.is_running() ) {
        who.set_movement_mode( move_mode_run );
    }
}

void lie_down( npc &who )
{
    if( !who.is_prone() ) {
        who.set_movement_mode( move_mode_prone );
    }
}

void arrive_at_hideout( npc &who, hide_state &state, map &here )
{
    state.phase = hide_phase::hidden;
    state.quiet_ticks = 0;
    who.goto_to_this_pos = std::nullopt;
    close_doors_around( who, here );
    // Same transition as "stay here": a guard post on the hideout tile.
    talk_function::assign_guard( who );
    who.set_guard_pos( state.destination );
    // Flat on the floor: behind a counter or a table nobody sees it.
    lie_down( who );
    hide_log( who, string_format( "ARRIVED enclosed=%d room_tiles=%zu guard=%d prone=%d stamina=%d",
                                  state.enclosed ? 1 : 0, state.room.size(),
                                  who.is_guarding() ? 1 : 0, who.is_prone() ? 1 : 0,
                                  who.get_stamina() ) );
    say_command_reply( who, localized_ai_message( _( "Hidden.  I'm not moving until it's quiet." ),
                       "Escondido.  No me muevo hasta que pase el peligro." ) );
}

// Start walking to `tile`.  The walk needs the follow attitude (a guard never
// takes a goto), so a guarding companion is released first.
void send_to_hideout( npc &who, hide_state &state, map &here, const hideout_choice &choice )
{
    state.destination = here.get_abs( *choice.tile );
    state.room = choice.room;
    state.doors = choice.doors;
    state.enclosed = choice.enclosed;
    state.phase = hide_phase::travelling;
    state.quiet_ticks = 0;
    state.last_pos = who.pos_abs();
    state.stuck_ticks = 0;
    stand_up( who );
    // The order is the retreat: whatever it was doing, it turns and runs.
    who.remove_effect( effect_npc_run_away );
    run_or_walk( who );
    if( who.is_guarding() ) {
        talk_function::stop_guard( who );
    }
    if( !who.is_walking_with() ) {
        who.set_attitude( NPCATT_FOLLOW );
    }
    who.goto_to_this_pos = state.destination;
    hide_log( who, string_format( "GOTO dest=%s enclosed=%d room_tiles=%zu route_len=%zu "
                                  "following=%d avoid_doors=%d",
                                  state.destination.to_string(), state.enclosed ? 1 : 0,
                                  state.room.size(), route_to( here, who, *choice.tile ).size(),
                                  who.is_walking_with() ? 1 : 0,
                                  who.rules.has_flag( ally_rule::avoid_doors ) ? 1 : 0 ) );
    if( who.pos_abs() == state.destination ) {
        arrive_at_hideout( who, state, here );
    }
}

enum class threat_level : int {
    none,      // nothing in the room or nearby
    intruder,  // a hostile got into the room (or close, for the fallback)
    contact    // a hostile right next to the companion: no time to run
};

threat_level assess_threat( const npc &who, const hide_state &state, map &here,
                            const std::vector<tripoint_bub_ms> &hostiles )
{
    const tripoint_bub_ms origin = who.pos_bub( here );
    threat_level level = threat_level::none;
    for( const tripoint_bub_ms &hostile : hostiles ) {
        if( rl_dist( origin, hostile ) <= 1 ) {
            return threat_level::contact;
        }
        const bool inside = state.enclosed ? state.room.count( here.get_abs( hostile ) ) != 0 :
                            rl_dist( origin, hostile ) <= fallback_threat_radius;
        if( inside ) {
            level = threat_level::intruder;
        }
    }
    return level;
}

void become_cornered( npc &who, hide_state &state, const int now )
{
    state.phase = hide_phase::cornered;
    state.quiet_ticks = 0;
    state.last_relocation_turn = now;
    stand_up( who );
    // Standing still to fight: a walker would be pulled towards the player.
    if( !who.is_guarding() ) {
        talk_function::assign_guard( who );
        who.set_guard_pos( who.pos_abs() );
    }
    apply_rules( who, state );
    hide_log( who, "CORNERED forbid_engage lifted" );
}

// No hideout and enemies in sight: keep away from them without engaging.
// The companion walks (runs) with our own step choice each turn; the vanilla
// flight cannot be used because with engagement forbidden the threat map it
// steers by stays empty.
void begin_evading( npc &who, hide_state &state, const int now )
{
    state.phase = hide_phase::evading;
    state.quiet_ticks = 0;
    state.evade_ticks = 0;
    state.stuck_ticks = 0;
    state.last_pos = who.pos_abs();
    state.last_relocation_turn = now;
    stand_up( who );
    if( who.is_guarding() ) {
        talk_function::stop_guard( who );
    }
    if( !who.is_walking_with() ) {
        who.set_attitude( NPCATT_FOLLOW );
    }
    who.remove_effect( effect_npc_run_away );
    run_or_walk( who );
    apply_rules( who, state );
    hide_log( who, "EVADING keeping away, engagement forbidden" );
}

// The adjacent tile farthest from the nearest visible enemy; ties go to the
// tile nearer the player, then to one under a roof.  Empty when no tile beats
// standing still.  Occupied tiles are never returned: stepping onto a hostile
// is an attack, which is exactly what the order is trying to avoid.
std::optional<tripoint_bub_ms> best_evade_tile( npc &who, map &here,
        const std::vector<tripoint_bub_ms> &hostiles )
{
    const tripoint_bub_ms origin = who.pos_bub( here );
    const tripoint_bub_ms player_pos = get_player_character().pos_bub( here );
    const auto nearest_enemy = [&]( const tripoint_bub_ms & tile ) {
        int best = std::numeric_limits<int>::max();
        for( const tripoint_bub_ms &hostile : hostiles ) {
            best = std::min( best, rl_dist( tile, hostile ) );
        }
        return best;
    };
    const auto score = [&]( const tripoint_bub_ms & tile ) {
        return nearest_enemy( tile ) * 100 - rl_dist( tile, player_pos ) * 2 +
               ( here.is_outside( tile ) ? 0 : 1 );
    };
    tripoint_bub_ms best = origin;
    int best_score = score( origin );
    creature_tracker &creatures = get_creature_tracker();
    for( const tripoint_bub_ms &n : here.points_in_radius( origin, 1, 0 ) ) {
        if( n == origin || !who.can_move_to( n, true ) || g->is_dangerous_tile( n ) ||
            creatures.creature_at( n ) != nullptr ) {
            continue;
        }
        const int n_score = score( n );
        if( n_score > best_score ) {
            best = n;
            best_score = n_score;
        }
    }
    if( best == origin ) {
        return std::nullopt;
    }
    return best;
}

bool evade_step( npc &who, map &here, const std::vector<tripoint_bub_ms> &hostiles )
{
    const std::optional<tripoint_bub_ms> step = best_evade_tile( who, here, hostiles );
    if( !step ) {
        return false;
    }
    who.goto_to_this_pos = here.get_abs( *step );
    return true;
}

// Spend the companion's turn getting away, and say so, so npc::move() stops
// before the combat logic.  Towards the hideout when the route is clear,
// otherwise simply away from the enemies.  Never steps onto a creature.
bool take_flight_turn( npc &who, map &here, hide_state &state,
                       const std::vector<tripoint_bub_ms> &hostiles, const bool towards_hideout )
{
    creature_tracker &creatures = get_creature_tracker();
    const tripoint_bub_ms destination = here.get_bub( state.destination );
    if( towards_hideout && who.pos_bub( here ) != destination &&
        who.update_path( destination ) && !who.path.empty() &&
        creatures.creature_at( who.path.front() ) == nullptr ) {
        hide_log( who, string_format( "FLEE_STEP route to=%s", who.path.front().to_string() ) );
        who.move_to_next();
        state.turn_taken = true;
        return true;
    }
    const std::optional<tripoint_bub_ms> step = best_evade_tile( who, here, hostiles );
    if( !step ) {
        hide_log( who, "FLEE_STEP blocked" );
        return false;
    }
    hide_log( who, string_format( "FLEE_STEP away to=%s", step->to_string() ) );
    who.move_to( *step );
    state.turn_taken = true;
    return true;
}

// A door of the hideout that somebody left open (the player walked through
// and did not shut it).  The nearest one, or nothing when all are closed.
std::optional<tripoint_bub_ms> open_room_door( npc &who, map &here, const hide_state &state )
{
    std::optional<tripoint_bub_ms> nearest;
    int best = std::numeric_limits<int>::max();
    for( const tripoint_abs_ms &abs_door : state.doors ) {
        const tripoint_bub_ms door = here.get_bub( abs_door );
        // check_only: false for a door already shut, or one with nothing to
        // close (a broken frame, a curtain someone tore down).
        if( !here.close_door( door, true, true ) ) {
            continue;
        }
        const int dist = rl_dist( who.pos_bub( here ), door );
        if( dist < best ) {
            best = dist;
            nearest = door;
        }
    }
    return nearest;
}

// Where to stand to shut `door`: a tile beside it, inside the room when it
// can be, and one the companion can actually walk to.
std::optional<tripoint_bub_ms> door_approach_tile( npc &who, map &here, const hide_state &state,
        const tripoint_bub_ms &door )
{
    std::optional<tripoint_bub_ms> best;
    int best_score = std::numeric_limits<int>::max();
    for( const tripoint_bub_ms &tile : here.points_in_radius( door, 1, 0 ) ) {
        if( tile == door || !standable_interior_tile( here, who, tile ) ) {
            continue;
        }
        if( who.pos_bub( here ) != tile && route_to( here, who, tile ).empty() ) {
            continue;
        }
        // Inside the room first: shutting it from the doorway side leaves the
        // companion outside its own hideout.
        const int score = ( state.room.count( here.get_abs( tile ) ) != 0 ? 0 : 100 ) +
                          rl_dist( who.pos_bub( here ), tile );
        if( score < best_score ) {
            best_score = score;
            best = tile;
        }
    }
    return best;
}

void begin_closing_door( npc &who, hide_state &state, map &here, const tripoint_bub_ms &door,
                         const tripoint_bub_ms &approach )
{
    state.phase = hide_phase::closing_door;
    state.door_target = here.get_abs( door );
    state.return_pos = who.pos_abs();
    // The cooldown is for trips that fail; a door shut and reopened is
    // answered at once.
    state.last_door_turn = -1;
    state.stuck_ticks = 0;
    state.last_pos = who.pos_abs();
    stand_up( who );
    if( who.is_guarding() ) {
        talk_function::stop_guard( who );
    }
    if( !who.is_walking_with() ) {
        who.set_attitude( NPCATT_FOLLOW );
    }
    who.goto_to_this_pos = here.get_abs( approach );
    hide_log( who, string_format( "CLOSE_DOOR door=%s approach=%s return=%s",
                                  state.door_target.to_string(), approach.to_string(),
                                  state.return_pos.to_string() ) );
    say_command_reply( who, localized_ai_message( _( "That door is open.  I'll shut it." ),
                       "Esa puerta se ha quedado abierta.  Voy a cerrarla." ) );
}

// Back to the tile it was lying on, with the room it already had.
void return_to_hideout_tile( npc &who, hide_state &state, map &here )
{
    hideout_choice back;
    back.tile = here.get_bub( state.return_pos );
    back.room = state.room;
    back.doors = state.doors;
    back.enclosed = state.enclosed;
    send_to_hideout( who, state, here, back );
}

// One step towards `dest` spent from the companion's own turn, so the errand
// is not queued behind whatever else the ordinary turn decides to do.
bool take_walk_turn( npc &who, map &here, hide_state &state, const tripoint_bub_ms &dest )
{
    if( who.pos_bub( here ) == dest || !who.update_path( dest ) || who.path.empty() ||
        get_creature_tracker().creature_at( who.path.front() ) != nullptr ) {
        return false;
    }
    who.move_to_next();
    state.turn_taken = true;
    return true;
}

// An enemy this close would end the turn in melee if the ordinary npc logic
// ran: it is the companion's own turn that has to be spent walking away.
bool enemy_within_reach( const npc &who, map &here,
                         const std::vector<tripoint_bub_ms> &hostiles )
{
    const tripoint_bub_ms origin = who.pos_bub( here );
    for( const tripoint_bub_ms &hostile : hostiles ) {
        if( rl_dist( origin, hostile ) <= 2 ) {
            return true;
        }
    }
    return false;
}

} // namespace

bool parse_hide_order( const std::string &spoken )
{
    const std::string normalized = normalize_order( spoken );
    static const std::vector<std::string> commands = {
        "escondanse", "escondete", "escondeos", "escondanse todos", "todos a esconderse",
        "a esconderse", "esconderse", "ocultense", "ocultate", "escondete ya", "escondanse ya"
    };
    for( const std::string &command : commands ) {
        if( normalized == command ) {
            return true;
        }
        if( normalized.size() <= command.size() ||
            normalized.compare( normalized.size() - command.size(), command.size(), command ) != 0 ||
            normalized[normalized.size() - command.size() - 1] != ' ' ) {
            continue;
        }
        // "Liam, escondete" is an order; "no te escondas" never is.
        const std::string prefix = " " + normalized.substr(
                                       0, normalized.size() - command.size() - 1 ) + " ";
        if( prefix.find( " no " ) == std::string::npos &&
            prefix.find( " nunca " ) == std::string::npos ) {
            return true;
        }
    }
    return false;
}

hide_order_result execute_hide_order( const std::vector<npc *> &raw_targets )
{
    hide_order_result result;
    result.handled = true;
    std::vector<npc *> targets;
    for( npc *candidate : raw_targets ) {
        if( candidate != nullptr && candidate->is_active() && candidate->is_player_ally() &&
            !candidate->is_dead_state() && !candidate->is_hallucination() ) {
            targets.push_back( candidate );
        }
    }
    if( targets.empty() ) {
        result.message = _( "No eligible ally received that order." );
        return result;
    }

    map &here = get_map();
    std::set<tripoint_abs_ms> reserved;
    for( npc *who : targets ) {
        // The new order supersedes whatever the companion was doing.
        clear_interior_hold( *who );
        cancel_food_search( *who );
        cancel_hide( *who );

        // Rules first: the route search must be allowed to open doors even
        // if the player told the companion not to.
        hide_state state;
        backup_rules( *who, state );
        const bool told_not_to_open_doors = who->rules.has_flag( ally_rule::avoid_doors );
        apply_rules( *who, state );

        const std::vector<tripoint_bub_ms> hostiles = visible_hostiles( *who, here, threat_scan_radius );
        // Ordered while something is on them: the hideout has to be somewhere
        // else.  Without this the room they are standing and fighting in
        // counts as a hideout, the order "arrives" on the spot and nothing
        // changes, which reads as the companion ignoring it.
        hideout_limits limits;
        if( !hostiles.empty() ) {
            limits.min_from_origin = relocation_min_distance;
            limits.min_from_hostiles = relocation_min_hostile_distance;
        }
        const hideout_choice choice = find_hideout( *who, here, reserved, {}, hostiles, limits );
        hide_log( *who, string_format( "ORDER hostiles_visible=%zu told_not_to_open_doors=%d result=%s",
                                       hostiles.size(), told_not_to_open_doors ? 1 : 0,
                                       !choice.tile ? "NONE" : choice.enclosed ? "ROOM" : "FALLBACK" ) );
        if( !choice.tile ) {
            if( hostiles.empty() ) {
                restore_rules( *who, state );
                continue;
            }
            // Nowhere to go and an enemy in sight: standing there doing
            // nothing is the one wrong answer.  Keep away from them without
            // engaging, looking for a hideout as it goes; it only turns to
            // fight when exhausted or boxed in.
            state.destination = who->pos_abs();
            state.enclosed = false;
            begin_evading( *who, state, to_turn<int>( calendar::turn ) );
            hideouts[npc_key( *who )] = state;
            result.assignments.emplace_back( who->getID().get_value(), state.destination );
            ++result.fallback_count;
            say_command_reply( *who, localized_ai_message(
                                   _( "Nowhere to hide.  I'll keep away from them." ),
                                   "No tengo dónde esconderme.  Me alejo de ellos." ) );
            continue;
        }
        reserved.insert( here.get_abs( *choice.tile ) );
        if( told_not_to_open_doors ) {
            say_command_reply( *who, localized_ai_message(
                                   _( "I'll be opening doors for this, whatever you told me." ),
                                   "Para esto voy a abrir puertas, aunque me dijeras que no." ) );
        }
        send_to_hideout( *who, state, here, choice );
        result.assignments.emplace_back( who->getID().get_value(), state.destination );
        if( !choice.enclosed ) {
            ++result.fallback_count;
            if( targets.size() > 1 ) {
                say_command_reply( *who, localized_ai_message(
                                       _( "I can't find anywhere to lock myself in; I'll hide here." ),
                                       "No encuentro dónde encerrarme, me escondo aquí." ) );
            }
        }
        hideouts[npc_key( *who )] = state;
    }

    result.success = !result.assignments.empty();
    if( !result.success ) {
        result.message = localized_ai_message( _( "I can't reach anywhere to hide from here." ),
                                               "No encuentro ningún sitio donde esconderme." );
    } else if( result.fallback_count == result.assignments.size() ) {
        result.message = localized_ai_message(
                             _( "I can't find anywhere to lock myself in; I'll hide here." ),
                             "No encuentro dónde encerrarme, me escondo aquí." );
    } else if( result.assignments.size() < targets.size() ) {
        result.message = localized_ai_message( _( "Some of us have nowhere to hide." ),
                                               "Algunos no tenemos dónde escondernos." );
    } else {
        result.message = localized_ai_message( _( "We're hiding." ), "Nos escondemos." );
    }
    return result;
}

bool process_hide( npc &who )
{
    const auto found = hideouts.find( npc_key( who ) );
    if( found == hideouts.end() ) {
        return false;
    }
    hide_state &state = found->second;
    if( !who.is_player_ally() || who.is_dead_state() ) {
        cancel_hide( who );
        return false;
    }
    map &here = get_map();
    // The vanilla "relax (clear overrides)" wipes every override; put ours
    // back so the companion never fights from the hideout by accident.
    apply_rules( who, state );
    const std::vector<tripoint_bub_ms> hostiles = visible_hostiles( who, here, threat_scan_radius );
    const threat_level threat = assess_threat( who, state, here, hostiles );
    const int now = to_turn<int>( calendar::turn );

    if( state.phase == hide_phase::travelling ) {
        hide_log( who, string_format( "TICK phase=travelling dest=%s dist=%d goto=%d following=%d "
                                      "guarding=%d running=%d stamina=%d stuck=%d moves=%d threat=%d "
                                      "hostiles=%zu panic=%d",
                                      state.destination.to_string(),
                                      rl_dist( who.pos_abs(), state.destination ),
                                      who.goto_to_this_pos ? 1 : 0, who.is_walking_with() ? 1 : 0,
                                      who.is_guarding() ? 1 : 0, who.is_running() ? 1 : 0,
                                      who.get_stamina(), state.stuck_ticks, who.get_moves(),
                                      static_cast<int>( threat ), hostiles.size(),
                                      who.has_effect( effect_npc_run_away ) ? 1 : 0 ) );
        if( who.pos_abs() == state.destination ) {
            arrive_at_hideout( who, state, here );
            return true;
        }
        // The hideout IS the flight: the vanilla panic would pick its own
        // direction and drop the walk.  Keep running our way instead.
        who.remove_effect( effect_npc_run_away );
        run_or_walk( who );
        if( who.pos_abs() == state.last_pos ) {
            ++state.stuck_ticks;
        } else {
            state.stuck_ticks = 0;
            state.last_pos = who.pos_abs();
        }
        // Boxed in by an enemy it is not allowed to attack: fight through it,
        // resume the run once it is quiet.
        if( threat == threat_level::contact && state.stuck_ticks >= boxed_in_ticks ) {
            become_cornered( who, state, now );
            say_command_reply( who, localized_ai_message( _( "I can't get past!  I'll defend myself!" ),
                               "¡No puedo pasar!  ¡Me defiendo!" ) );
            return true;
        }
        if( state.stuck_ticks >= replan_ticks ) {
            // Not moving and nobody in contact: the route is broken (a door
            // that will not open, a blocked corridor).  Choose again from
            // here; failing that, hide where it stands.
            const hideout_choice again = find_hideout( who, here, {}, {}, hostiles );
            hide_log( who, string_format( "REPLAN result=%s", !again.tile ? "NONE" :
                                          again.enclosed ? "ROOM" : "FALLBACK" ) );
            if( again.tile && here.get_abs( *again.tile ) != state.destination ) {
                send_to_hideout( who, state, here, again );
            } else if( !hostiles.empty() ) {
                begin_evading( who, state, now );
                say_command_reply( who, localized_ai_message(
                                       _( "I can't get there.  I'll keep away from them." ),
                                       "No consigo llegar.  Me alejo de ellos." ) );
            } else {
                hideout_choice here_choice;
                here_choice.tile = who.pos_bub( here );
                here_choice.enclosed = false;
                send_to_hideout( who, state, here, here_choice );
                say_command_reply( who, localized_ai_message( _( "I can't get there.  I'll hide here." ),
                                   "No consigo llegar.  Me escondo aquí." ) );
            }
            return true;
        }
        if( !who.goto_to_this_pos ) {
            // Something (an activity, a scare) dropped the walk: resume it.
            who.goto_to_this_pos = state.destination;
        }
        if( enemy_within_reach( who, here, hostiles ) ) {
            take_flight_turn( who, here, state, hostiles, true );
        }
        return true;
    }

    if( state.phase == hide_phase::closing_door ) {
        const tripoint_bub_ms door = here.get_bub( state.door_target );
        if( who.pos_abs() == state.last_pos ) {
            ++state.stuck_ticks;
        } else {
            state.stuck_ticks = 0;
            state.last_pos = who.pos_abs();
        }
        hide_log( who, string_format( "TICK phase=closing_door door=%s dist=%d stuck=%d threat=%d",
                                      state.door_target.to_string(),
                                      rl_dist( who.pos_bub( here ), door ), state.stuck_ticks,
                                      static_cast<int>( threat ) ) );
        // Anything shows up and the door stops mattering: back to the tile,
        // and the hidden branch decides from there next turn.
        if( threat != threat_level::none || !hostiles.empty() ) {
            hide_log( who, "CLOSE_DOOR aborted, threat" );
            return_to_hideout_tile( who, state, here );
            return true;
        }
        if( rl_dist( who.pos_bub( here ), door ) <= 1 ) {
            if( here.close_door( door, true, true ) ) {
                // The vanilla helper: it refuses politely when a creature, a
                // vehicle or a pile of loot is in the doorway.
                doors::close_door( here, who, door );
                hide_log( who, string_format( "CLOSE_DOOR shut=%d",
                                              here.close_door( door, true, true ) ? 0 : 1 ) );
            }
            return_to_hideout_tile( who, state, here );
            return true;
        }
        if( state.stuck_ticks >= replan_ticks ) {
            hide_log( who, "CLOSE_DOOR gave up" );
            return_to_hideout_tile( who, state, here );
            return true;
        }
        if( !who.goto_to_this_pos ) {
            const std::optional<tripoint_bub_ms> approach =
                door_approach_tile( who, here, state, door );
            if( !approach ) {
                return_to_hideout_tile( who, state, here );
                return true;
            }
            who.goto_to_this_pos = here.get_abs( *approach );
        }
        take_walk_turn( who, here, state, here.get_bub( *who.goto_to_this_pos ) );
        return true;
    }

    if( state.phase == hide_phase::leaving ) {
        const bool at_door = who.pos_abs() == state.destination;
        const bool out_of_room = state.room.count( who.pos_abs() ) == 0;
        ++state.quiet_ticks;
        hide_log( who, string_format( "TICK phase=leaving door=%s at_door=%d out=%d goto=%d ticks=%d",
                                      state.destination.to_string(), at_door ? 1 : 0,
                                      out_of_room ? 1 : 0, who.goto_to_this_pos ? 1 : 0,
                                      state.quiet_ticks ) );
        if( at_door || out_of_room || state.quiet_ticks >= leaving_timeout_ticks ||
            !who.is_following() ) {
            hide_log( who, "LEFT rules_restored" );
            restore_rules( who, state );
            if( who.goto_to_this_pos && *who.goto_to_this_pos == state.destination ) {
                who.goto_to_this_pos = std::nullopt;
            }
            hideouts.erase( found );
            return false;
        }
        if( !who.goto_to_this_pos ) {
            who.goto_to_this_pos = state.destination;
        }
        return true;
    }

    if( state.phase == hide_phase::evading ) {
        ++state.evade_ticks;
        who.remove_effect( effect_npc_run_away );
        if( who.pos_abs() == state.last_pos ) {
            ++state.stuck_ticks;
        } else {
            state.stuck_ticks = 0;
            state.last_pos = who.pos_abs();
        }
        const bool moved_away = !hostiles.empty() && evade_step( who, here, hostiles );
        const bool close_by = enemy_within_reach( who, here, hostiles );
        hide_log( who, string_format( "TICK phase=evading hostiles=%zu threat=%d moved=%d stuck=%d "
                                      "stamina=%d quiet=%d",
                                      hostiles.size(), static_cast<int>( threat ), moved_away ? 1 : 0,
                                      state.stuck_ticks, who.get_stamina(), state.quiet_ticks ) );
        // Out of breath: it cannot keep this up.  Turn and fight, and say so.
        if( !hostiles.empty() && exhausted( who ) ) {
            become_cornered( who, state, now );
            say_command_reply( who, localized_ai_message(
                                   _( "I'm exhausted, I can't run any more.  I'll defend myself!" ),
                                   "Estoy agotado, no puedo seguir corriendo.  ¡Me defiendo!" ) );
            return true;
        }
        // Boxed in: an enemy in contact and no tile that takes it farther.
        if( threat == threat_level::contact && !moved_away && state.stuck_ticks >= boxed_in_ticks ) {
            become_cornered( who, state, now );
            say_command_reply( who, localized_ai_message( _( "I'm cornered!  I'll defend myself!" ),
                               "¡Me han acorralado!  ¡Me defiendo!" ) );
            return true;
        }
        run_or_walk( who );
        if( close_by && moved_away ) {
            take_flight_turn( who, here, state, hostiles, false );
        }
        if( hostiles.empty() ) {
            who.goto_to_this_pos = std::nullopt;
            if( ++state.quiet_ticks < cornered_quiet_ticks ) {
                return true;
            }
        } else {
            state.quiet_ticks = 0;
            if( state.evade_ticks % evade_replan_ticks != 0 ) {
                return true;
            }
        }
        // Quiet for a while, or a periodic look around: is there a hideout now?
        const hideout_choice again = find_hideout( who, here, {}, {}, hostiles );
        if( again.tile && ( again.enclosed || hostiles.empty() ) &&
            here.get_abs( *again.tile ) != who.pos_abs() ) {
            say_command_reply( who, hostiles.empty() ?
                               localized_ai_message( _( "Lost them.  Off to a hideout." ),
                                                     "Los he perdido de vista.  Voy a esconderme." ) :
                               localized_ai_message( _( "There!  I can hide there." ),
                                                     "¡Ahí!  Ahí puedo esconderme." ) );
            send_to_hideout( who, state, here, again );
            return true;
        }
        if( hostiles.empty() ) {
            // Lost them and nothing better around: hide where it stands.
            hideout_choice here_choice;
            here_choice.tile = who.pos_bub( here );
            here_choice.enclosed = false;
            say_command_reply( who, localized_ai_message( _( "Lost them.  I'll hide here." ),
                               "Los he perdido de vista.  Me escondo aquí." ) );
            send_to_hideout( who, state, here, here_choice );
        }
        return true;
    }

    // Hidden or cornered.  A follow order given through any other path ends
    // the hideout: the companion is walking away with the player anyway.
    if( who.is_following() ) {
        hide_log( who, "CANCEL reason=following_again" );
        cancel_hide( who, true );
        // Inside the room the hideout lives on as the walk out (leaving).
        return hideouts.count( npc_key( who ) ) != 0;
    }
    if( threat != threat_level::none || state.phase == hide_phase::cornered ) {
        hide_log( who, string_format( "TICK phase=%s threat=%d hostiles=%zu quiet_ticks=%d",
                                      phase_name( state.phase ), static_cast<int>( threat ),
                                      hostiles.size(), state.quiet_ticks ) );
    }

    if( state.phase == hide_phase::hidden ) {
        if( threat == threat_level::none ) {
            // Nothing in the room: never let the vanilla panic drag the
            // companion out of its hideout because of what it sees outside.
            if( who.has_effect( effect_npc_run_away ) ) {
                who.remove_effect( effect_npc_run_away );
            }
            if( !who.is_prone() ) {
                lie_down( who );
            }
            // A door of the room left open: shut it the same turn when it is
            // within arm's reach, otherwise get up, walk over and come back.
            // Only with nothing in sight at all: a trip to the doorway with
            // an enemy wandering about is how a hidden companion is found.
            if( state.enclosed && hostiles.empty() && who.pos_abs() == state.destination &&
                ( state.last_door_turn < 0 || now - state.last_door_turn >= door_retry_ticks ) ) {
                const std::optional<tripoint_bub_ms> door = open_room_door( who, here, state );
                if( !door ) {
                    // Everything shut: forget any door being waited on.
                    state.door_seen_turn = -1;
                } else if( state.door_seen_turn < 0 || state.door_seen != here.get_abs( *door ) ) {
                    // First turn it is seen open: give whoever opened it time
                    // to come through before shutting it behind them.
                    state.door_seen = here.get_abs( *door );
                    state.door_seen_turn = now;
                    hide_log( who, string_format( "DOOR_OPEN %s, waiting %d turns",
                                                  state.door_seen.to_string(), door_wait_turns ) );
                } else if( now - state.door_seen_turn >= door_wait_turns ) {
                    state.door_seen_turn = -1;
                    if( rl_dist( who.pos_bub( here ), *door ) <= 1 ) {
                        // Within reach: no trip and no getting up.
                        doors::close_door( here, who, *door );
                        const bool shut = !here.close_door( *door, true, true );
                        hide_log( who, string_format( "CLOSE_DOOR reach shut=%d", shut ? 1 : 0 ) );
                        if( !shut ) {
                            state.last_door_turn = now;
                        }
                    } else {
                        const std::optional<tripoint_bub_ms> approach =
                            door_approach_tile( who, here, state, *door );
                        if( approach ) {
                            begin_closing_door( who, state, here, *door, *approach );
                        } else {
                            // Cannot get to it; do not try again for a while.
                            state.last_door_turn = now;
                            hide_log( who, "CLOSE_DOOR unreachable" );
                        }
                    }
                }
            }
            return true;
        }
        // Found out.  The answer is another room, even with the enemy right
        // next to it: the rooms worth hiding in are small, so an intruder is
        // in contact the moment it steps through the door, and staying put
        // would mean never relocating at all.
        const bool cooling_down = threat != threat_level::contact &&
                                  state.last_relocation_turn >= 0 &&
                                  now - state.last_relocation_turn < relocation_cooldown_turns;
        if( !cooling_down ) {
            std::set<tripoint_abs_ms> avoid = state.room;
            avoid.insert( state.destination );
            hideout_limits limits;
            limits.min_from_origin = relocation_min_distance;
            limits.min_from_hostiles = relocation_min_hostile_distance;
            const hideout_choice next = find_hideout( who, here, {}, avoid, hostiles, limits );
            hide_log( who, string_format( "FOUND_OUT threat=%d hostiles=%zu relocate=%s",
                                          static_cast<int>( threat ), hostiles.size(),
                                          next.tile && next.enclosed ? "ROOM" : "NONE" ) );
            if( next.tile && next.enclosed ) {
                say_command_reply( who, localized_ai_message(
                                       _( "They've found me!  Running to another room." ),
                                       "¡Me han encontrado!  Corro a otra habitación." ) );
                ++state.relocations;
                state.last_relocation_turn = now;
                send_to_hideout( who, state, here, next );
                return true;
            }
        } else if( threat != threat_level::contact ) {
            // Just relocated: give the walk a few turns before trying again.
            return true;
        }
        // No other room: get out of this one and keep away without engaging,
        // enemy in contact or not.  Being next to something is not the same
        // as being cornered, and standing there trading blows is the one
        // thing the order is meant to prevent; it fights only once exhausted
        // or truly boxed in, which the evading phase decides.
        begin_evading( who, state, now );
        say_command_reply( who, localized_ai_message(
                               _( "They've found me and there's nowhere else.  I'll keep away." ),
                               "¡Me han encontrado y no hay otro sitio!  Me alejo." ) );
        return true;
    }

    // Cornered.
    if( threat != threat_level::none || !hostiles.empty() ) {
        state.quiet_ticks = 0;
        return true;
    }
    if( ++state.quiet_ticks < cornered_quiet_ticks ) {
        return true;
    }
    state.quiet_ticks = 0;
    if( who.pos_abs() != state.destination &&
        !route_to( here, who, here.get_bub( state.destination ) ).empty() ) {
        // Caught on the way there: finish the walk.
        state.phase = hide_phase::travelling;
        state.last_pos = who.pos_abs();
        state.stuck_ticks = 0;
        if( who.is_guarding() ) {
            talk_function::stop_guard( who );
        }
        if( !who.is_walking_with() ) {
            who.set_attitude( NPCATT_FOLLOW );
        }
        who.goto_to_this_pos = state.destination;
        run_or_walk( who );
        apply_rules( who, state );
        say_command_reply( who, localized_ai_message( _( "Quiet again.  Back to the hideout." ),
                           "Vuelve a estar tranquilo.  Sigo hacia el escondite." ) );
        return true;
    }
    // Nowhere to resume: is there a proper hideout now that it is quiet?
    const hideout_choice again = find_hideout( who, here, {}, {}, hostiles );
    if( again.tile && again.enclosed && here.get_abs( *again.tile ) != who.pos_abs() ) {
        say_command_reply( who, localized_ai_message( _( "Quiet again.  Off to a hideout." ),
                           "Vuelve a estar tranquilo.  Voy a esconderme." ) );
        send_to_hideout( who, state, here, again );
        return true;
    }
    state.phase = hide_phase::hidden;
    state.destination = who.pos_abs();
    if( !who.is_guarding() ) {
        talk_function::assign_guard( who );
    }
    who.set_guard_pos( state.destination );
    lie_down( who );
    apply_rules( who, state );
    say_command_reply( who, localized_ai_message( _( "Quiet again.  Staying hidden." ),
                       "Vuelve a estar tranquilo.  Sigo escondido." ) );
    return true;
}

bool hide_took_turn( const npc &who )
{
    const auto found = hideouts.find( npc_key( who ) );
    if( found == hideouts.end() || !found->second.turn_taken ) {
        return false;
    }
    found->second.turn_taken = false;
    return true;
}

bool is_hiding( const npc &who )
{
    return hideouts.count( npc_key( who ) ) != 0;
}

std::optional<hide_state> hide_state_for( const npc &who )
{
    const auto found = hideouts.find( npc_key( who ) );
    if( found == hideouts.end() ) {
        return std::nullopt;
    }
    return found->second;
}

void cancel_hide( const npc &who, const bool step_out )
{
    const auto found = hideouts.find( npc_key( who ) );
    if( found == hideouts.end() ) {
        return;
    }
    hide_state state = found->second;
    npc *companion = g->find_npc( who.getID() );
    if( companion == nullptr ) {
        hideouts.erase( found );
        return;
    }
    const bool inside_room = step_out && state.enclosed && state.phase != hide_phase::leaving &&
                             state.room.count( companion->pos_abs() ) != 0;
    hide_log( *companion, string_format( "CANCEL phase=%s step_out=%d inside_room=%d",
                                         phase_name( state.phase ), step_out ? 1 : 0,
                                         inside_room ? 1 : 0 ) );
    stand_up( *companion );
    if( companion->goto_to_this_pos && *companion->goto_to_this_pos == state.destination ) {
        companion->goto_to_this_pos = std::nullopt;
    }
    if( !inside_room ) {
        restore_rules( *companion, state );
        hideouts.erase( found );
        return;
    }
    // Inside the room it hid in: walk to the door nearest the player.  The
    // walk opens the door on the way and leaves the companion in the doorway,
    // where the ordinary follow takes over.  Door-opening stays allowed until
    // then (see apply_rules for the leaving phase).
    map &here = get_map();
    const tripoint_bub_ms player_pos = get_player_character().pos_bub( here );
    std::optional<tripoint_bub_ms> exit;
    int best = std::numeric_limits<int>::max();
    for( const tripoint_abs_ms &abs_tile : state.room ) {
        const tripoint_bub_ms tile = here.get_bub( abs_tile );
        for( const tripoint_bub_ms &n : here.points_in_radius( tile, 1, 0 ) ) {
            if( n == tile || state.room.count( here.get_abs( n ) ) != 0 || !is_door_tile( here, n ) ) {
                continue;
            }
            const int score = rl_dist( n, player_pos ) * 2 + rl_dist( n, companion->pos_bub( here ) );
            if( score < best ) {
                best = score;
                exit = n;
            }
        }
    }
    if( !exit ) {
        restore_rules( *companion, state );
        hideouts.erase( found );
        return;
    }
    hide_state &kept = found->second;
    kept.phase = hide_phase::leaving;
    kept.destination = here.get_abs( *exit );
    kept.quiet_ticks = 0;
    apply_rules( *companion, kept );
    companion->goto_to_this_pos = kept.destination;
    // Right next to a shut door: open it now, the follow needs no walk then.
    if( rl_dist( companion->pos_bub( here ), *exit ) <= 1 && !here.passable( *exit ) &&
        here.open_door( *companion, *exit, true, true ) ) {
        here.open_door( *companion, *exit, true );
    }
    hide_log( *companion, string_format( "STEP_OUT door=%s", here.get_abs( *exit ).to_string() ) );
}

void reset_all_hideouts()
{
    hideouts.clear();
}

} // namespace npc_ai
