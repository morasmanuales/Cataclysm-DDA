#include "cata_catch.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "effect.h"
#include "faction.h"
#include "game.h"
#include "map.h"
#include "map_helpers.h"
#include "messages.h"
#include "monster.h"
#include "move_mode.h"
#include "npc.h"
#include "npc_ai_hide.h"
#include "npc_ai_order_menu.h"
#include "npc_ai_tactical.h"
#include "player_helpers.h"
#include "point.h"
#include "sounds.h"
#include "type_id.h"

namespace
{

static const faction_id faction_your_followers( "your_followers" );
static const ter_str_id ter_t_floor( "t_floor" );
static const ter_str_id ter_t_flat_roof( "t_flat_roof" );
static const ter_str_id ter_t_wall( "t_wall" );
static const ter_str_id ter_t_door_c( "t_door_c" );
static const ter_str_id ter_t_door_o( "t_door_o" );
static const furn_str_id furn_f_counter( "f_counter" );
static const furn_str_id furn_f_table( "f_table" );
static const ter_str_id ter_t_window_domestic( "t_window_domestic" );

npc &prepare_hider()
{
    npc_ai::reset_all_hideouts();
    g->faction_manager_ptr->create_if_needed();
    clear_map();
    clear_avatar();
    clear_npcs();
    set_time_to_day();
    g->place_player( tripoint_bub_ms{ 60, 60, 0 } );
    npc &who = spawn_npc( point_bub_ms{ 61, 60 }, "test_talker" );
    who.name = "Liam";
    who.set_fac( faction_your_followers );
    who.set_attitude( NPCATT_FOLLOW );
    REQUIRE( who.is_player_ally() );
    REQUIRE( who.is_following() );
    Messages::clear_messages();
    return who;
}

void rebuild_caches()
{
    map &here = get_map();
    here.invalidate_map_cache( 0 );
    here.build_map_cache( 0, true );
    here.invalidate_visibility_cache();
    here.update_visibility_cache( 0 );
}

// A roofed floor square of the given half-size around `center`, walled on
// every side, with a single door in the middle of the west wall.
tripoint_bub_ms build_room( const tripoint_bub_ms &center, const int half )
{
    map &here = get_map();
    for( const tripoint_bub_ms &tile : here.points_in_radius( center, half + 1, 0 ) ) {
        const bool edge = std::abs( tile.x() - center.x() ) > half ||
                          std::abs( tile.y() - center.y() ) > half;
        here.ter_set( tile, edge ? ter_t_wall : ter_t_floor );
        here.ter_set( tile + tripoint::above, ter_t_flat_roof );
    }
    const tripoint_bub_ms door = center + tripoint_rel_ms{ -( half + 1 ), 0, 0 };
    here.ter_set( door, ter_t_door_c );
    rebuild_caches();
    return door;
}

// Roofed floor with no walls: interior for the game, but nothing to lock.
void build_open_shelter( const tripoint_bub_ms &center, const int half )
{
    map &here = get_map();
    for( const tripoint_bub_ms &tile : here.points_in_radius( center, half, 0 ) ) {
        here.ter_set( tile, ter_t_floor );
        here.ter_set( tile + tripoint::above, ter_t_flat_roof );
    }
    rebuild_caches();
}

bool said_something_containing( const std::string &fragment )
{
    sounds::process_sound_markers( &get_avatar() );
    for( const auto &message : Messages::recent_messages( 0 ) ) {
        if( message.second.find( fragment ) != std::string::npos ) {
            return true;
        }
    }
    return false;
}

// The replies follow the dialogue language: English by default, Spanish
// fallback when the game runs in Spanish.
bool said_either( const std::string &english, const std::string &spanish )
{
    return said_something_containing( english ) || said_something_containing( spanish );
}

bool mentions_either( const std::string &text, const std::string &english,
                      const std::string &spanish )
{
    return text.find( english ) != std::string::npos || text.find( spanish ) != std::string::npos;
}

// The companion leaves an open door alone for a couple of turns before it
// walks over to shut it; tests have to let those turns go by.
void pass_door_wait( npc &who )
{
    for( int i = 0; i < 3; ++i ) {
        CHECK( npc_ai::process_hide( who ) );
        calendar::turn += 1_turns;
    }
}

bool inside( const tripoint_bub_ms &p, const tripoint_bub_ms &center, const int half )
{
    return std::abs( p.x() - center.x() ) <= half && std::abs( p.y() - center.y() ) <= half;
}

} // namespace

TEST_CASE( "hide_order_parser_is_deterministic_and_accent_tolerant",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    CHECK( npc_ai::parse_hide_order( "Escondanse." ) );
    CHECK( npc_ai::parse_hide_order( "¡Escóndanse!" ) );
    CHECK( npc_ai::parse_hide_order( "Liam, escóndete" ) );
    CHECK( npc_ai::parse_hide_order( "todos a esconderse" ) );
    CHECK( npc_ai::parse_hide_order( "Ocúltense" ) );
    CHECK_FALSE( npc_ai::parse_hide_order( "No te escondas" ) );
    CHECK_FALSE( npc_ai::parse_hide_order( "Nunca se escondan" ) );
    CHECK_FALSE( npc_ai::parse_hide_order( "Vengan conmigo." ) );
    CHECK_FALSE( npc_ai::parse_hide_order( "Todos adentro." ) );
    CHECK_FALSE( npc_ai::parse_hide_order( "¿Dónde nos escondemos?" ) );
    CHECK( npc_ai::parse_hide_order( npc_ai::order_menu_phrase( npc_ai::menu_order::hide, "" ) ) );
}

TEST_CASE( "hide_order_picks_the_deepest_tile_of_an_enclosed_room_and_forbids_fighting",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    const tripoint_bub_ms center{ 70, 60, 0 };
    const tripoint_bub_ms door = build_room( center, 2 );
    // Open the door so the room is reachable and there is something to shut.
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();

    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    REQUIRE( result.handled );
    REQUIRE( result.success );
    REQUIRE( result.assignments.size() == 1 );
    CHECK( result.fallback_count == 0 );
    CHECK( mentions_either( result.message, "We're hiding", "Nos escondemos" ) );

    const tripoint_bub_ms dest = here.get_bub( result.assignments[0].second );
    CHECK( inside( dest, center, 2 ) );
    // Farthest from the (west) door: the east side of the room.
    CHECK( dest.x() == center.x() + 2 );
    CHECK( liam.goto_to_this_pos == result.assignments[0].second );
    CHECK( liam.is_following() );

    REQUIRE( npc_ai::is_hiding( liam ) );
    const std::optional<npc_ai::hide_state> state = npc_ai::hide_state_for( liam );
    REQUIRE( state );
    CHECK( state->phase == npc_ai::hide_phase::travelling );
    CHECK( state->enclosed );
    CHECK( state->room.size() == 25 );
    // Combat rules changed for the duration of the order.
    CHECK( liam.rules.has_flag( ally_rule::forbid_engage ) );
    CHECK( liam.rules.has_flag( ally_rule::close_doors ) );
    CHECK( liam.rules.has_flag( ally_rule::ignore_noise ) );
    CHECK( liam.rules.has_flag( ally_rule::use_silent ) );
    CHECK_FALSE( liam.rules.has_flag( ally_rule::avoid_doors ) );

    SECTION( "arrival becomes a guard post, shuts the door and reports" ) {
        liam.setpos( here, dest );
        liam.goto_to_this_pos = std::nullopt;
        Messages::clear_messages();
        CHECK( npc_ai::process_hide( liam ) );
        const std::optional<npc_ai::hide_state> hidden = npc_ai::hide_state_for( liam );
        REQUIRE( hidden );
        CHECK( hidden->phase == npc_ai::hide_phase::hidden );
        CHECK( liam.mission == NPC_MISSION_GUARD_ALLY );
        CHECK_FALSE( liam.is_following() );
        CHECK( said_either( "Hidden.", "Escondido." ) );
        CHECK( liam.rules.has_flag( ally_rule::forbid_engage ) );
        CHECK( liam.is_prone() );

        // Nothing around and the door shut: keeps holding, turn after turn.
        here.ter_set( door, ter_t_door_c );
        rebuild_caches();
        for( int i = 0; i < 5; ++i ) {
            CHECK( npc_ai::process_hide( liam ) );
        }
        CHECK( liam.mission == NPC_MISSION_GUARD_ALLY );
        CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::hidden );
    }

    SECTION( "the walk is resumed if something dropped it" ) {
        liam.goto_to_this_pos = std::nullopt;
        CHECK( npc_ai::process_hide( liam ) );
        CHECK( liam.goto_to_this_pos == result.assignments[0].second );
    }

    SECTION( "follow me ends the hideout and restores the combat rules" ) {
        const npc_ai::tactical_order_result follow =
            npc_ai::execute_tactical_order( { &liam }, npc_ai::tactical_order::follow );
        REQUIRE( follow.affected.size() == 1 );
        CHECK_FALSE( npc_ai::is_hiding( liam ) );
        CHECK_FALSE( liam.goto_to_this_pos.has_value() );
        CHECK( liam.is_following() );
        CHECK_FALSE( liam.is_prone() );
        CHECK_FALSE( liam.rules.has_flag( ally_rule::forbid_engage ) );
        CHECK_FALSE( liam.rules.has_override_enable( ally_rule::forbid_engage ) );
        CHECK_FALSE( liam.rules.has_override_enable( ally_rule::close_doors ) );
        CHECK_FALSE( liam.rules.has_override_enable( ally_rule::ignore_noise ) );
        CHECK_FALSE( liam.rules.has_override_enable( ally_rule::use_silent ) );
        CHECK_FALSE( liam.rules.has_override_enable( ally_rule::avoid_doors ) );
    }

    SECTION( "the vanilla relax (clear overrides) does not leave it fighting" ) {
        liam.rules.clear_overrides();
        CHECK_FALSE( liam.rules.has_flag( ally_rule::forbid_engage ) );
        CHECK( npc_ai::process_hide( liam ) );
        CHECK( liam.rules.has_flag( ally_rule::forbid_engage ) );
        CHECK( liam.rules.has_flag( ally_rule::use_silent ) );
    }
}

TEST_CASE( "hide_door_is_shut_on_arrival", "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    // A one-tile closet: its only tile is next to the door.
    const tripoint_bub_ms center{ 68, 60, 0 };
    const tripoint_bub_ms door = build_room( center, 0 );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();

    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    REQUIRE( result.success );
    REQUIRE( here.get_bub( result.assignments[0].second ) == center );
    REQUIRE( npc_ai::hide_state_for( liam )->enclosed );

    liam.setpos( here, center );
    liam.goto_to_this_pos = std::nullopt;
    REQUIRE( here.ter( door ) == ter_t_door_o );
    REQUIRE( npc_ai::process_hide( liam ) );
    CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::hidden );
    CHECK( here.ter( door ) == ter_t_door_c );
}

TEST_CASE( "hide_order_falls_back_to_an_interior_tile_when_nothing_can_be_locked",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    const tripoint_bub_ms center{ 68, 60, 0 };
    build_open_shelter( center, 2 );

    Messages::clear_messages();
    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    REQUIRE( result.success );
    REQUIRE( result.assignments.size() == 1 );
    CHECK( result.fallback_count == 1 );
    CHECK( mentions_either( result.message, "I'll hide here", "me escondo aquí" ) );
    const tripoint_bub_ms dest = here.get_bub( result.assignments[0].second );
    CHECK_FALSE( here.is_outside( dest ) );
    const std::optional<npc_ai::hide_state> state = npc_ai::hide_state_for( liam );
    REQUIRE( state );
    CHECK_FALSE( state->enclosed );
    CHECK( state->room.empty() );
}

TEST_CASE( "hide_order_fails_cleanly_with_no_interior_at_all",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    CHECK( result.handled );
    CHECK_FALSE( result.success );
    CHECK( result.assignments.empty() );
    CHECK_FALSE( npc_ai::is_hiding( liam ) );
    CHECK_FALSE( liam.rules.has_flag( ally_rule::forbid_engage ) );
    CHECK( liam.is_following() );
}

TEST_CASE( "hidden_companion_relocates_when_an_enemy_enters_and_fights_when_cornered",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    const tripoint_bub_ms first_center{ 68, 60, 0 };
    const tripoint_bub_ms second_center{ 68, 70, 0 };
    const tripoint_bub_ms first_door = build_room( first_center, 2 );
    const tripoint_bub_ms second_door = build_room( second_center, 2 );
    here.ter_set( first_door, ter_t_door_o );
    here.ter_set( second_door, ter_t_door_o );
    rebuild_caches();

    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    REQUIRE( result.success );
    const tripoint_bub_ms dest = here.get_bub( result.assignments[0].second );
    REQUIRE( inside( dest, first_center, 2 ) );
    liam.setpos( here, dest );
    liam.goto_to_this_pos = std::nullopt;
    REQUIRE( npc_ai::process_hide( liam ) );
    REQUIRE( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::hidden );
    // The door is shut again for the second room to stay reachable.
    here.ter_set( first_door, ter_t_door_o );
    rebuild_caches();

    SECTION( "an enemy inside the room sends it to the other room" ) {
        monster &zombie = spawn_test_monster( "mon_zombie", first_center );
        REQUIRE( liam.sees( here, zombie ) );
        Messages::clear_messages();
        CHECK( npc_ai::process_hide( liam ) );
        const std::optional<npc_ai::hide_state> state = npc_ai::hide_state_for( liam );
        REQUIRE( state );
        CHECK( state->phase == npc_ai::hide_phase::travelling );
        CHECK( state->relocations == 1 );
        CHECK( inside( here.get_bub( state->destination ), second_center, 2 ) );
        CHECK( liam.goto_to_this_pos == state->destination );
        CHECK( liam.is_following() );
        CHECK_FALSE( liam.is_prone() );
        CHECK( said_either( "another room", "otra habitación" ) );
        CHECK( liam.rules.has_flag( ally_rule::forbid_engage ) );
    }

    SECTION( "an enemy outside the room, not adjacent, changes nothing" ) {
        spawn_test_monster( "mon_zombie", first_door + point::west + point::west );
        CHECK( npc_ai::process_hide( liam ) );
        CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::hidden );
        CHECK( liam.mission == NPC_MISSION_GUARD_ALLY );
    }

    SECTION( "an enemy right next to it also sends it running to the other room" ) {
        // The rooms worth hiding in are small: an intruder is in contact as
        // soon as it is inside, and that must not nail the companion down.
        monster &intruder = spawn_test_monster( "mon_zombie", dest + point::west );
        REQUIRE( liam.sees( here, intruder ) );
        Messages::clear_messages();
        CHECK( npc_ai::process_hide( liam ) );
        const std::optional<npc_ai::hide_state> state = npc_ai::hide_state_for( liam );
        REQUIRE( state );
        CHECK( state->phase == npc_ai::hide_phase::travelling );
        CHECK( state->relocations == 1 );
        CHECK( inside( here.get_bub( state->destination ), second_center, 2 ) );
        CHECK( liam.rules.has_flag( ally_rule::forbid_engage ) );
        CHECK( said_either( "another room", "otra habitación" ) );
    }

    SECTION( "an enemy next to it with no other room left still leaves the room" ) {
        // Nowhere to relocate to, but being next to something is not being
        // cornered: it gets out and keeps away instead of trading blows.
        spawn_test_monster( "mon_zombie", second_center );
        monster &intruder = spawn_test_monster( "mon_zombie", dest + point::west );
        REQUIRE( liam.sees( here, intruder ) );
        const int intruder_hp = intruder.get_hp();
        const tripoint_bub_ms start = liam.pos_bub( here );
        Messages::clear_messages();
        CHECK( npc_ai::process_hide( liam ) );
        REQUIRE( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::evading );
        CHECK( liam.rules.has_flag( ally_rule::forbid_engage ) );
        CHECK_FALSE( liam.is_prone() );
        CHECK( said_either( "keep away", "Me alejo" ) );

        // And the next turn it is the order that moves it, away from the
        // intruder, without a single swing.
        CHECK( npc_ai::process_hide( liam ) );
        CHECK( npc_ai::hide_took_turn( liam ) );
        CHECK( liam.pos_bub( here ) != start );
        // Backed against the far wall it can only sidestep, so the gap may
        // not grow on this turn; what matters is that it moves and does not
        // swing.
        CHECK( rl_dist( liam.pos_bub( here ), intruder.pos_bub( here ) ) >=
               rl_dist( start, intruder.pos_bub( here ) ) );
        CHECK( intruder.get_hp() == intruder_hp );
    }

    SECTION( "the new room is a real escape, never the next tile over" ) {
        // A one-tile closet right beside the hideout is not an escape.
        const tripoint_bub_ms closet_door = build_room( dest + point::north + point::north, 0 );
        here.ter_set( closet_door, ter_t_door_o );
        rebuild_caches();
        monster &intruder = spawn_test_monster( "mon_zombie", first_center );
        REQUIRE( liam.sees( here, intruder ) );
        CHECK( npc_ai::process_hide( liam ) );
        const std::optional<npc_ai::hide_state> state = npc_ai::hide_state_for( liam );
        REQUIRE( state );
        REQUIRE( state->phase == npc_ai::hide_phase::travelling );
        CHECK( rl_dist( here.get_bub( state->destination ), liam.pos_bub( here ) ) >= 3 );
        CHECK( rl_dist( here.get_bub( state->destination ), intruder.pos_bub( here ) ) >= 6 );
    }

    SECTION( "with no other room it keeps away without engaging, then hides again once quiet" ) {
        // Fill the second room so it is not a hideout any more; the intruder
        // is inside the first room but not adjacent.
        spawn_test_monster( "mon_zombie", second_center );
        monster &intruder = spawn_test_monster( "mon_zombie", first_center );
        REQUIRE( liam.sees( here, intruder ) );
        Messages::clear_messages();
        CHECK( npc_ai::process_hide( liam ) );
        REQUIRE( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::evading );
        CHECK( liam.rules.has_flag( ally_rule::forbid_engage ) );
        CHECK_FALSE( liam.is_prone() );
        CHECK( said_either( "keep away", "Me alejo" ) );

        // Enemies gone: after the quiet turns it settles down again.
        clear_creatures();
        for( int i = 0; i < 9; ++i ) {
            CHECK( npc_ai::process_hide( liam ) );
            CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::evading );
        }
        Messages::clear_messages();
        CHECK( npc_ai::process_hide( liam ) );
        CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::hidden );
        CHECK( liam.rules.has_flag( ally_rule::forbid_engage ) );
        CHECK( liam.is_prone() );
        CHECK( said_either( "Lost them", "perdido de vista" ) );
    }
}

TEST_CASE( "hide_group_order_sends_each_companion_to_its_own_tile",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    npc &kim = spawn_npc( point_bub_ms{ 60, 61 }, "test_talker" );
    kim.name = "Kim";
    kim.set_fac( faction_your_followers );
    kim.set_attitude( NPCATT_FOLLOW );
    map &here = get_map();
    const tripoint_bub_ms center{ 68, 60, 0 };
    const tripoint_bub_ms door = build_room( center, 1 );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();

    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam, &kim } );
    REQUIRE( result.success );
    REQUIRE( result.assignments.size() == 2 );
    CHECK( result.assignments[0].second != result.assignments[1].second );
    CHECK( inside( here.get_bub( result.assignments[0].second ), center, 1 ) );
    CHECK( inside( here.get_bub( result.assignments[1].second ), center, 1 ) );
    CHECK( npc_ai::is_hiding( liam ) );
    CHECK( npc_ai::is_hiding( kim ) );
    CHECK( kim.rules.has_flag( ally_rule::forbid_engage ) );
}

TEST_CASE( "hide_order_leaves_the_players_own_overrides_alone",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    const tripoint_bub_ms door = build_room( tripoint_bub_ms{ 68, 60, 0 }, 1 );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    // The player already forces "close doors" on.
    liam.rules.set_override( ally_rule::close_doors );
    liam.rules.enable_override( ally_rule::close_doors );

    REQUIRE( npc_ai::execute_hide_order( { &liam } ).success );
    npc_ai::execute_tactical_order( { &liam }, npc_ai::tactical_order::follow );
    CHECK( liam.rules.has_override_enable( ally_rule::close_doors ) );
    CHECK( liam.rules.has_flag( ally_rule::close_doors ) );
    CHECK_FALSE( liam.rules.has_override_enable( ally_rule::forbid_engage ) );
}

TEST_CASE( "hide_prefers_cover_and_lying_behind_it_hides_from_the_door",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    // 3x3 room, door in the west wall, a counter right inside the door.
    const tripoint_bub_ms center{ 68, 60, 0 };
    const tripoint_bub_ms door = build_room( center, 1 );
    here.ter_set( door, ter_t_door_o );
    // A counter along the whole column inside the door.
    const tripoint_bub_ms counter = door + point::east;
    for( int dy = -1; dy <= 1; ++dy ) {
        here.furn_set( counter + tripoint_rel_ms{ 0, dy, 0 }, furn_f_counter );
    }
    rebuild_caches();

    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    REQUIRE( result.success );
    const tripoint_bub_ms dest = here.get_bub( result.assignments[0].second );
    // Not the deepest column (east wall) but the one right behind the counter.
    CHECK( dest.x() == counter.x() + 1 );

    liam.setpos( here, dest );
    liam.goto_to_this_pos = std::nullopt;
    REQUIRE( npc_ai::process_hide( liam ) );
    REQUIRE( liam.is_prone() );

    // A zombie standing in the doorway: the counter hides a prone companion
    // and not a standing one.
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    monster &zombie = spawn_test_monster( "mon_zombie", door );
    CHECK_FALSE( zombie.sees( here, liam ) );
    liam.set_movement_mode( move_mode_id( "walk" ) );
    CHECK( zombie.sees( here, liam ) );
}

TEST_CASE( "hide_cover_also_counts_windows_as_viewpoints",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    // 3x3 room: door west, window in the middle of the east wall, a counter
    // column inside the door and a table inside the window.
    const tripoint_bub_ms center{ 68, 60, 0 };
    const tripoint_bub_ms door = build_room( center, 1 );
    here.ter_set( door, ter_t_door_o );
    const tripoint_bub_ms window = center + point::east + point::east;
    here.ter_set( window, ter_t_window_domestic );
    for( int dy = -1; dy <= 1; ++dy ) {
        here.furn_set( door + point::east + tripoint_rel_ms{ 0, dy, 0 }, furn_f_counter );
    }
    here.furn_set( center + point::east, furn_f_table );
    rebuild_caches();

    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    REQUIRE( result.success );
    REQUIRE( npc_ai::hide_state_for( liam )->enclosed );
    const tripoint_bub_ms dest = here.get_bub( result.assignments[0].second );
    // Behind cover from the door AND from the window.
    CHECK( here.obstacle_coverage( door, dest ) > 33 );
    CHECK( here.obstacle_coverage( window, dest ) > 33 );
}

TEST_CASE( "hidden_companion_shrugs_off_the_vanilla_panic_when_nothing_is_in_the_room",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    const tripoint_bub_ms center{ 68, 60, 0 };
    const tripoint_bub_ms door = build_room( center, 1 );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    REQUIRE( result.success );
    liam.setpos( here, here.get_bub( result.assignments[0].second ) );
    liam.goto_to_this_pos = std::nullopt;
    REQUIRE( npc_ai::process_hide( liam ) );
    REQUIRE( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::hidden );

    // With the door shut there is nothing to do but lie there.
    here.ter_set( door, ter_t_door_c );
    rebuild_caches();
    liam.add_effect( efftype_id( "npc_run_away" ), 20_turns );
    REQUIRE( liam.has_effect( efftype_id( "npc_run_away" ) ) );
    CHECK( npc_ai::process_hide( liam ) );
    CHECK_FALSE( liam.has_effect( efftype_id( "npc_run_away" ) ) );
    CHECK( liam.is_prone() );
}

TEST_CASE( "companion_caught_on_the_way_keeps_running_and_only_fights_when_boxed_in",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    const tripoint_bub_ms center{ 70, 60, 0 };
    const tripoint_bub_ms door = build_room( center, 1 );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    // Mid-fight when the order arrives: panicking, enemy right there.
    monster &zombie = spawn_test_monster( "mon_zombie", liam.pos_bub( here ) + point::east );
    liam.add_effect( efftype_id( "npc_run_away" ), 20_turns );
    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    REQUIRE( result.success );
    REQUIRE( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::travelling );
    // It turns and runs: our walk, not the vanilla flight, and at a run.
    CHECK_FALSE( liam.has_effect( efftype_id( "npc_run_away" ) ) );
    CHECK( liam.is_running() );
    CHECK( liam.goto_to_this_pos == result.assignments[0].second );
    CHECK( liam.rules.has_flag( ally_rule::forbid_engage ) );
    CHECK( liam.rules.has_flag( ally_rule::close_doors ) );

    REQUIRE( liam.sees( here, zombie ) );
    Messages::clear_messages();
    // Walled in on every side but the zombie's: it cannot get away.
    for( const tripoint_bub_ms &n : here.points_in_radius( liam.pos_bub( here ), 1, 0 ) ) {
        if( n != liam.pos_bub( here ) && n != zombie.pos_bub( here ) ) {
            here.ter_set( n, ter_t_wall );
        }
    }
    rebuild_caches();
    // Adjacent enemy and nowhere to step: still running, for a couple of turns...
    for( int i = 0; i < 2; ++i ) {
        liam.add_effect( efftype_id( "npc_run_away" ), 20_turns );
        CHECK( npc_ai::process_hide( liam ) );
        CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::travelling );
        CHECK_FALSE( liam.has_effect( efftype_id( "npc_run_away" ) ) );
        CHECK( liam.goto_to_this_pos == result.assignments[0].second );
    }
    // ...then it is boxed in and fights through.
    CHECK( npc_ai::process_hide( liam ) );
    CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::cornered );
    CHECK_FALSE( liam.rules.has_flag( ally_rule::forbid_engage ) );
    CHECK_FALSE( liam.is_running() );
    CHECK( said_either( "can't get past", "No puedo pasar" ) );

    // Quiet again: the walls come down and it resumes the run to the hideout.
    clear_creatures();
    for( const tripoint_bub_ms &n : here.points_in_radius( liam.pos_bub( here ), 1, 0 ) ) {
        if( here.ter( n ) == ter_t_wall ) {
            here.ter_set( n, ter_t_floor );
        }
    }
    rebuild_caches();
    for( int i = 0; i < 9; ++i ) {
        CHECK( npc_ai::process_hide( liam ) );
    }
    Messages::clear_messages();
    CHECK( npc_ai::process_hide( liam ) );
    const std::optional<npc_ai::hide_state> state = npc_ai::hide_state_for( liam );
    REQUIRE( state );
    CHECK( state->phase == npc_ai::hide_phase::travelling );
    CHECK( state->destination == result.assignments[0].second );
    CHECK( liam.goto_to_this_pos == state->destination );
    CHECK( liam.is_following() );
    CHECK( liam.is_running() );
    CHECK( liam.rules.has_flag( ally_rule::forbid_engage ) );
    CHECK( said_either( "Back to the hideout", "Sigo hacia el escondite" ) );
}

TEST_CASE( "companion_moving_towards_the_hideout_is_never_cornered",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    const tripoint_bub_ms center{ 70, 60, 0 };
    const tripoint_bub_ms door = build_room( center, 1 );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    REQUIRE( result.success );
    // A zombie keeps pace next to it while it advances one tile a turn.
    monster &zombie = spawn_test_monster( "mon_zombie", liam.pos_bub( here ) + point::south );
    for( int i = 0; i < 5; ++i ) {
        liam.setpos( here, liam.pos_bub( here ) + point::east );
        zombie.setpos( here, zombie.pos_bub( here ) + point::east );
        CHECK( npc_ai::process_hide( liam ) );
        CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::travelling );
        CHECK( liam.rules.has_flag( ally_rule::forbid_engage ) );
    }
}

TEST_CASE( "hide_order_opens_doors_despite_the_players_avoid_doors_rule_and_restores_it",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    // The only hideout is behind a CLOSED door.
    const tripoint_bub_ms center{ 68, 60, 0 };
    const tripoint_bub_ms door = build_room( center, 1 );
    REQUIRE( here.ter( door ) == ter_t_door_c );
    liam.rules.set_flag( ally_rule::avoid_doors );
    REQUIRE( liam.rules.has_flag( ally_rule::avoid_doors ) );

    Messages::clear_messages();
    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    REQUIRE( result.success );
    CHECK( npc_ai::hide_state_for( liam )->enclosed );
    // Forced off for the duration, and the companion says so.
    CHECK_FALSE( liam.rules.has_flag( ally_rule::avoid_doors ) );
    CHECK( said_either( "opening doors", "abrir puertas" ) );

    npc_ai::execute_tactical_order( { &liam }, npc_ai::tactical_order::follow );
    CHECK( liam.rules.has_flag( ally_rule::avoid_doors ) );
    CHECK_FALSE( liam.rules.has_override_enable( ally_rule::avoid_doors ) );
}

TEST_CASE( "hide_order_restores_a_prepare_for_danger_override_exactly",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    const tripoint_bub_ms door = build_room( tripoint_bub_ms{ 68, 60, 0 }, 1 );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    // Vanilla "prepare for danger": avoid_doors forced ON, close_doors forced OFF.
    liam.rules.set_danger_overrides();
    REQUIRE( liam.rules.has_flag( ally_rule::avoid_doors ) );
    REQUIRE( liam.rules.has_override_enable( ally_rule::close_doors ) );
    REQUIRE_FALSE( liam.rules.has_flag( ally_rule::close_doors ) );

    REQUIRE( npc_ai::execute_hide_order( { &liam } ).success );
    // The hideout wins while it lasts...
    CHECK_FALSE( liam.rules.has_flag( ally_rule::avoid_doors ) );
    CHECK( liam.rules.has_flag( ally_rule::close_doors ) );
    // ...and the player's danger overrides come back untouched afterwards.
    npc_ai::execute_tactical_order( { &liam }, npc_ai::tactical_order::follow );
    CHECK( liam.rules.has_flag( ally_rule::avoid_doors ) );
    CHECK( liam.rules.has_override_enable( ally_rule::close_doors ) );
    CHECK_FALSE( liam.rules.has_flag( ally_rule::close_doors ) );
    CHECK( liam.rules.has_flag( ally_rule::follow_close ) );
}

TEST_CASE( "follow_me_walks_a_hidden_companion_out_through_the_door",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    const tripoint_bub_ms center{ 68, 60, 0 };
    const tripoint_bub_ms door = build_room( center, 1 );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    REQUIRE( result.success );
    const tripoint_bub_ms dest = here.get_bub( result.assignments[0].second );
    liam.setpos( here, dest );
    liam.goto_to_this_pos = std::nullopt;
    REQUIRE( npc_ai::process_hide( liam ) );
    REQUIRE( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::hidden );
    // Shut in: the door was closed on arrival or by the player.
    here.ter_set( door, ter_t_door_c );
    rebuild_caches();

    SECTION( "through the AI follow order" ) {
        npc_ai::execute_tactical_order( { &liam }, npc_ai::tactical_order::follow );
        REQUIRE( npc_ai::hide_state_for( liam ) );
        CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::leaving );
        CHECK( liam.is_following() );
        CHECK_FALSE( liam.is_prone() );
        // Combat rules are the player's again; only the door stays openable.
        CHECK_FALSE( liam.rules.has_flag( ally_rule::forbid_engage ) );
        CHECK_FALSE( liam.rules.has_flag( ally_rule::avoid_doors ) );
        REQUIRE( liam.goto_to_this_pos.has_value() );
        CHECK( here.get_bub( *liam.goto_to_this_pos ) == door );

        // Standing in the doorway ends the hideout for good.
        liam.setpos( here, door );
        CHECK_FALSE( npc_ai::process_hide( liam ) );
        CHECK_FALSE( npc_ai::is_hiding( liam ) );
        CHECK_FALSE( liam.goto_to_this_pos.has_value() );
    }

    SECTION( "through the vanilla follow (attitude set elsewhere)" ) {
        liam.set_attitude( NPCATT_FOLLOW );
        liam.set_mission( NPC_MISSION_NULL );
        CHECK( npc_ai::process_hide( liam ) );
        REQUIRE( npc_ai::hide_state_for( liam ) );
        CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::leaving );
        REQUIRE( liam.goto_to_this_pos.has_value() );
        CHECK( here.get_bub( *liam.goto_to_this_pos ) == door );
    }

    SECTION( "the walk out ignores the player's avoid-doors rule, then gives it back" ) {
        liam.rules.set_flag( ally_rule::avoid_doors );
        npc_ai::execute_tactical_order( { &liam }, npc_ai::tactical_order::follow );
        REQUIRE( npc_ai::hide_state_for( liam ) );
        CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::leaving );
        CHECK_FALSE( liam.rules.has_flag( ally_rule::avoid_doors ) );
        // A vanilla "clear overrides" in between does not shut it in again.
        liam.rules.clear_overrides();
        CHECK( npc_ai::process_hide( liam ) );
        CHECK_FALSE( liam.rules.has_flag( ally_rule::avoid_doors ) );
        liam.setpos( here, door + point::west );
        CHECK_FALSE( npc_ai::process_hide( liam ) );
        CHECK_FALSE( npc_ai::is_hiding( liam ) );
        CHECK( liam.rules.has_flag( ally_rule::avoid_doors ) );
    }

    SECTION( "guard here does not walk it anywhere" ) {
        npc_ai::execute_tactical_order( { &liam }, npc_ai::tactical_order::guard );
        CHECK_FALSE( npc_ai::is_hiding( liam ) );
        CHECK_FALSE( liam.goto_to_this_pos.has_value() );
    }
}

TEST_CASE( "hide_order_with_nowhere_to_go_keeps_away_and_fights_only_when_exhausted_or_boxed_in",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    // No interior anywhere; a zombie right next to it, to the east.
    monster &zombie = spawn_test_monster( "mon_zombie", liam.pos_bub( here ) + point::east );
    REQUIRE( liam.sees( here, zombie ) );
    Messages::clear_messages();
    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    REQUIRE( result.success );
    REQUIRE( npc_ai::hide_state_for( liam ) );
    CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::evading );
    // Keeping away: no engagement, on the move, not a guard.
    CHECK( liam.rules.has_flag( ally_rule::forbid_engage ) );
    CHECK( liam.is_walking_with() );
    CHECK_FALSE( liam.is_guarding() );
    CHECK( said_either( "keep away", "Me alejo" ) );

    SECTION( "each turn it steps away from the enemy, spending its own turn" ) {
        const tripoint_bub_ms start = liam.pos_bub( here );
        const int start_gap = rl_dist( start, zombie.pos_bub( here ) );
        const int zombie_hp = zombie.get_hp();
        CHECK( npc_ai::process_hide( liam ) );
        CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::evading );
        // The order moves it itself: an ordinary turn would swing instead.
        CHECK( npc_ai::hide_took_turn( liam ) );
        CHECK( rl_dist( liam.pos_bub( here ), start ) == 1 );
        CHECK( rl_dist( liam.pos_bub( here ), zombie.pos_bub( here ) ) > start_gap );
        CHECK( zombie.get_hp() == zombie_hp );
        CHECK( liam.rules.has_flag( ally_rule::forbid_engage ) );
    }

    SECTION( "exhausted, it turns and fights, and says so" ) {
        liam.set_stamina( 0 );
        Messages::clear_messages();
        CHECK( npc_ai::process_hide( liam ) );
        CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::cornered );
        CHECK( liam.mission == NPC_MISSION_GUARD_ALLY );
        CHECK_FALSE( liam.rules.has_flag( ally_rule::forbid_engage ) );
        CHECK( said_either( "exhausted", "agotado" ) );
    }

    SECTION( "boxed in with no tile that takes it farther, it fights, and says so" ) {
        // Walls on every side but the zombie's.
        for( const tripoint_bub_ms &n : here.points_in_radius( liam.pos_bub( here ), 1, 0 ) ) {
            if( n != liam.pos_bub( here ) && n != zombie.pos_bub( here ) ) {
                here.ter_set( n, ter_t_wall );
            }
        }
        rebuild_caches();
        Messages::clear_messages();
        for( int i = 0; i < 2; ++i ) {
            CHECK( npc_ai::process_hide( liam ) );
            CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::evading );
        }
        CHECK( npc_ai::process_hide( liam ) );
        CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::cornered );
        CHECK_FALSE( liam.rules.has_flag( ally_rule::forbid_engage ) );
        CHECK( said_either( "cornered", "acorralado" ) );
    }

    SECTION( "a room coming into reach while evading is taken at once" ) {
        const tripoint_bub_ms door = build_room( tripoint_bub_ms{ 68, 60, 0 }, 1 );
        here.ter_set( door, ter_t_door_o );
        rebuild_caches();
        Messages::clear_messages();
        bool travelling = false;
        for( int i = 0; i < 5 && !travelling; ++i ) {
            CHECK( npc_ai::process_hide( liam ) );
            travelling = npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::travelling;
        }
        CHECK( travelling );
        CHECK( npc_ai::hide_state_for( liam )->enclosed );
        CHECK( said_either( "I can hide there", "puedo esconderme" ) );
    }

    SECTION( "losing sight of every enemy, it hides where it stands" ) {
        clear_creatures();
        for( int i = 0; i < 9; ++i ) {
            CHECK( npc_ai::process_hide( liam ) );
            CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::evading );
        }
        Messages::clear_messages();
        CHECK( npc_ai::process_hide( liam ) );
        CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::hidden );
        CHECK( npc_ai::hide_state_for( liam )->destination == liam.pos_abs() );
        CHECK( said_either( "Lost them", "perdido de vista" ) );
    }
}

TEST_CASE( "companion_with_a_broken_route_chooses_again_or_hides_where_it_stands",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    const tripoint_bub_ms center{ 70, 60, 0 };
    const tripoint_bub_ms door = build_room( center, 1 );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    REQUIRE( result.success );
    // The doorway gets walled up behind the order: the route is gone.
    here.ter_set( door, ter_t_wall );
    rebuild_caches();
    for( int i = 0; i < 5; ++i ) {
        CHECK( npc_ai::process_hide( liam ) );
        CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::travelling );
        CHECK( npc_ai::hide_state_for( liam )->destination == result.assignments[0].second );
    }
    Messages::clear_messages();
    CHECK( npc_ai::process_hide( liam ) );
    const std::optional<npc_ai::hide_state> state = npc_ai::hide_state_for( liam );
    REQUIRE( state );
    // Nothing else around: it hides right where it stands.
    CHECK( state->destination == liam.pos_abs() );
    CHECK( state->phase == npc_ai::hide_phase::hidden );
    CHECK_FALSE( state->enclosed );
    CHECK( said_either( "I'll hide here", "Me escondo aquí" ) );
}

TEST_CASE( "follow_me_opens_a_shut_door_right_next_to_the_hidden_companion",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    // One-tile closet: the door is adjacent to the hideout tile.
    const tripoint_bub_ms center{ 68, 60, 0 };
    const tripoint_bub_ms door = build_room( center, 0 );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    REQUIRE( npc_ai::execute_hide_order( { &liam } ).success );
    liam.setpos( here, center );
    liam.goto_to_this_pos = std::nullopt;
    REQUIRE( npc_ai::process_hide( liam ) );
    REQUIRE( here.ter( door ) == ter_t_door_c );

    npc_ai::execute_tactical_order( { &liam }, npc_ai::tactical_order::follow );
    CHECK( here.ter( door ) == ter_t_door_o );
    REQUIRE( npc_ai::hide_state_for( liam ) );
    CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::leaving );
}

TEST_CASE( "hide_order_given_mid_fight_breaks_off_and_runs",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    // A room well away, and a zombie already trading blows with the companion.
    const tripoint_bub_ms center{ 71, 60, 0 };
    const tripoint_bub_ms door = build_room( center, 1 );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    monster &zombie = spawn_test_monster( "mon_zombie", liam.pos_bub( here ) + point::east );
    REQUIRE( liam.sees( here, zombie ) );
    const int zombie_hp = zombie.get_hp();
    const tripoint_bub_ms start = liam.pos_bub( here );

    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    REQUIRE( result.success );
    const std::optional<npc_ai::hide_state> state = npc_ai::hide_state_for( liam );
    REQUIRE( state );
    // It must actually go somewhere: the tile it is fighting on is no hideout.
    CHECK( state->phase == npc_ai::hide_phase::travelling );
    CHECK( state->destination != liam.pos_abs() );
    CHECK( rl_dist( here.get_bub( state->destination ), start ) >= 3 );
    CHECK( rl_dist( here.get_bub( state->destination ), zombie.pos_bub( here ) ) >= 6 );

    // With the enemy in reach the order spends the turn itself: the ordinary
    // turn would swing at whatever is next to it instead of leaving.
    CHECK( npc_ai::process_hide( liam ) );
    CHECK( npc_ai::hide_took_turn( liam ) );
    CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::travelling );
    CHECK( liam.pos_bub( here ) != start );
    CHECK( rl_dist( liam.pos_bub( here ), here.get_bub( state->destination ) ) <
           rl_dist( start, here.get_bub( state->destination ) ) );
    // And it never traded a blow on the way out.
    CHECK( zombie.get_hp() == zombie_hp );
    CHECK( liam.rules.has_flag( ally_rule::forbid_engage ) );
}

TEST_CASE( "hide_walk_with_nothing_near_leaves_the_turn_to_the_ordinary_logic",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    const tripoint_bub_ms door = build_room( tripoint_bub_ms{ 71, 60, 0 }, 1 );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    REQUIRE( npc_ai::execute_hide_order( { &liam } ).success );
    const tripoint_bub_ms start = liam.pos_bub( here );

    CHECK( npc_ai::process_hide( liam ) );
    CHECK_FALSE( npc_ai::hide_took_turn( liam ) );
    CHECK( liam.pos_bub( here ) == start );
    REQUIRE( liam.goto_to_this_pos.has_value() );
}

TEST_CASE( "hidden_companion_shuts_a_door_left_open_and_goes_back_to_its_tile",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    const tripoint_bub_ms center{ 70, 60, 0 };
    const tripoint_bub_ms door = build_room( center, 1 );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    REQUIRE( result.success );
    const tripoint_bub_ms dest = here.get_bub( result.assignments[0].second );
    liam.setpos( here, dest );
    liam.goto_to_this_pos = std::nullopt;
    REQUIRE( npc_ai::process_hide( liam ) );
    REQUIRE( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::hidden );
    REQUIRE( liam.is_prone() );

    // Somebody walks through and leaves the door open behind them.
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    Messages::clear_messages();
    // The first turns it is left alone: they may still be coming through.
    CHECK( npc_ai::process_hide( liam ) );
    CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::hidden );
    CHECK( here.ter( door ) == ter_t_door_o );
    calendar::turn += 2_turns;
    CHECK( npc_ai::process_hide( liam ) );
    const std::optional<npc_ai::hide_state> going = npc_ai::hide_state_for( liam );
    REQUIRE( going );
    CHECK( going->phase == npc_ai::hide_phase::closing_door );
    CHECK( going->return_pos == liam.pos_abs() );
    CHECK_FALSE( liam.is_prone() );
    CHECK_FALSE( liam.is_guarding() );
    REQUIRE( liam.goto_to_this_pos.has_value() );
    // It heads for a tile beside the door, on the inside of the room.
    const tripoint_bub_ms approach = here.get_bub( *liam.goto_to_this_pos );
    CHECK( rl_dist( approach, door ) == 1 );
    CHECK( going->room.count( here.get_abs( approach ) ) == 1 );
    CHECK( said_either( "shut it", "cerrarla" ) );

    // Standing next to it, it shuts the door and walks back to its tile.
    liam.setpos( here, approach );
    liam.goto_to_this_pos = std::nullopt;
    CHECK( npc_ai::process_hide( liam ) );
    CHECK( here.ter( door ) == ter_t_door_c );
    const std::optional<npc_ai::hide_state> back = npc_ai::hide_state_for( liam );
    REQUIRE( back );
    CHECK( back->phase == npc_ai::hide_phase::travelling );
    CHECK( back->destination == here.get_abs( dest ) );

    // And back on its tile it lies down again.
    liam.setpos( here, dest );
    liam.goto_to_this_pos = std::nullopt;
    CHECK( npc_ai::process_hide( liam ) );
    CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::hidden );
    CHECK( liam.is_prone() );
    CHECK( liam.mission == NPC_MISSION_GUARD_ALLY );
}

TEST_CASE( "an_open_door_never_costs_the_companion_its_cover",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    // The only room has its door wide open: it is still a proper hideout,
    // the flood stops at the doorway either way.
    const tripoint_bub_ms center{ 70, 60, 0 };
    const tripoint_bub_ms door = build_room( center, 1 );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    REQUIRE( result.success );
    const std::optional<npc_ai::hide_state> state = npc_ai::hide_state_for( liam );
    REQUIRE( state );
    CHECK( state->enclosed );
    CHECK( state->room.size() == 9 );
    CHECK( state->doors.count( here.get_abs( door ) ) == 1 );
}

TEST_CASE( "a_door_trip_is_dropped_the_moment_something_shows_up",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    const tripoint_bub_ms center{ 70, 60, 0 };
    const tripoint_bub_ms door = build_room( center, 1 );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    REQUIRE( result.success );
    const tripoint_bub_ms dest = here.get_bub( result.assignments[0].second );
    liam.setpos( here, dest );
    liam.goto_to_this_pos = std::nullopt;
    REQUIRE( npc_ai::process_hide( liam ) );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    pass_door_wait( liam );
    REQUIRE( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::closing_door );

    // A zombie in the doorway: the door stops being the problem.
    monster &zombie = spawn_test_monster( "mon_zombie", door );
    REQUIRE( liam.sees( here, zombie ) );
    CHECK( npc_ai::process_hide( liam ) );
    CHECK( npc_ai::hide_state_for( liam )->phase != npc_ai::hide_phase::closing_door );
    CHECK( liam.rules.has_flag( ally_rule::forbid_engage ) );
}

TEST_CASE( "a_door_within_reach_is_shut_the_same_turn_without_getting_up",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    // One-tile closet: the hideout tile is right beside the door.
    const tripoint_bub_ms center{ 68, 60, 0 };
    const tripoint_bub_ms door = build_room( center, 0 );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    REQUIRE( npc_ai::execute_hide_order( { &liam } ).success );
    liam.setpos( here, center );
    liam.goto_to_this_pos = std::nullopt;
    REQUIRE( npc_ai::process_hide( liam ) );
    REQUIRE( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::hidden );

    // Opened again from outside: left alone at first, whoever opened it may
    // still be stepping through.
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    CHECK( npc_ai::process_hide( liam ) );
    CHECK( here.ter( door ) == ter_t_door_o );
    calendar::turn += 1_turns;
    CHECK( npc_ai::process_hide( liam ) );
    CHECK( here.ter( door ) == ter_t_door_o );
    // Two turns later, and it is shut on the spot without getting up.
    calendar::turn += 1_turns;
    CHECK( npc_ai::process_hide( liam ) );
    CHECK( here.ter( door ) == ter_t_door_c );
    CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::hidden );
    CHECK( liam.is_prone() );
    CHECK( liam.mission == NPC_MISSION_GUARD_ALLY );

    // And again: shutting one door never blocks the next one.
    for( int i = 0; i < 2; ++i ) {
        here.ter_set( door, ter_t_door_o );
        rebuild_caches();
        pass_door_wait( liam );
        CHECK( here.ter( door ) == ter_t_door_c );
    }
}

TEST_CASE( "the_walk_to_a_far_door_is_driven_by_the_order_itself",
           "[npc_ai][npc_ai_hide][npc_ai_orders]" )
{
    npc &liam = prepare_hider();
    map &here = get_map();
    const tripoint_bub_ms center{ 70, 60, 0 };
    const tripoint_bub_ms door = build_room( center, 2 );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    const npc_ai::hide_order_result result = npc_ai::execute_hide_order( { &liam } );
    REQUIRE( result.success );
    const tripoint_bub_ms dest = here.get_bub( result.assignments[0].second );
    liam.setpos( here, dest );
    liam.goto_to_this_pos = std::nullopt;
    REQUIRE( npc_ai::process_hide( liam ) );
    here.ter_set( door, ter_t_door_o );
    rebuild_caches();
    pass_door_wait( liam );
    REQUIRE( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::closing_door );

    // Every turn of the errand is spent walking, not waiting its turn behind
    // whatever else the companion might feel like doing.
    const int start_gap = rl_dist( liam.pos_bub( here ), door );
    CHECK( npc_ai::process_hide( liam ) );
    CHECK( npc_ai::hide_took_turn( liam ) );
    CHECK( rl_dist( liam.pos_bub( here ), door ) < start_gap );
}
