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

        // Nothing around: keeps holding, turn after turn.
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
        CHECK( said_either( "another hideout", "otro escondite" ) );
        CHECK( liam.rules.has_flag( ally_rule::forbid_engage ) );
    }

    SECTION( "an enemy outside the room, not adjacent, changes nothing" ) {
        spawn_test_monster( "mon_zombie", first_door + point::west + point::west );
        CHECK( npc_ai::process_hide( liam ) );
        CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::hidden );
        CHECK( liam.mission == NPC_MISSION_GUARD_ALLY );
    }

    SECTION( "an enemy already next to it means fighting, not running" ) {
        monster &intruder = spawn_test_monster( "mon_zombie", dest + point::west );
        REQUIRE( liam.sees( here, intruder ) );
        Messages::clear_messages();
        CHECK( npc_ai::process_hide( liam ) );
        REQUIRE( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::cornered );
        CHECK( npc_ai::hide_state_for( liam )->relocations == 0 );
        CHECK( liam.mission == NPC_MISSION_GUARD_ALLY );
        CHECK_FALSE( liam.rules.has_flag( ally_rule::forbid_engage ) );
        CHECK_FALSE( liam.is_prone() );
        CHECK( said_either( "It's on me", "Lo tengo encima" ) );
    }

    SECTION( "with no other room it defends itself, then hides again once quiet" ) {
        // Fill the second room so it is not a hideout any more; the intruder
        // is inside the first room but not adjacent.
        spawn_test_monster( "mon_zombie", second_center );
        monster &intruder = spawn_test_monster( "mon_zombie", first_center );
        REQUIRE( liam.sees( here, intruder ) );
        Messages::clear_messages();
        CHECK( npc_ai::process_hide( liam ) );
        REQUIRE( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::cornered );
        CHECK_FALSE( liam.rules.has_flag( ally_rule::forbid_engage ) );
        CHECK_FALSE( liam.is_prone() );
        CHECK( said_either( "Nowhere to go", "No tengo adónde ir" ) );

        // Enemies gone: quiet turns bring the no-fighting rule back.
        clear_creatures();
        for( int i = 0; i < 9; ++i ) {
            CHECK( npc_ai::process_hide( liam ) );
            CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::cornered );
        }
        Messages::clear_messages();
        CHECK( npc_ai::process_hide( liam ) );
        CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::hidden );
        CHECK( liam.rules.has_flag( ally_rule::forbid_engage ) );
        CHECK( liam.is_prone() );
        CHECK( said_either( "Staying hidden", "Sigo escondido" ) );
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

    liam.add_effect( efftype_id( "npc_run_away" ), 20_turns );
    REQUIRE( liam.has_effect( efftype_id( "npc_run_away" ) ) );
    CHECK( npc_ai::process_hide( liam ) );
    CHECK_FALSE( liam.has_effect( efftype_id( "npc_run_away" ) ) );
    CHECK( liam.is_prone() );
}

TEST_CASE( "companion_caught_on_the_way_fights_then_resumes_the_walk",
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
    REQUIRE( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::travelling );

    monster &zombie = spawn_test_monster( "mon_zombie", liam.pos_bub( here ) + point::east );
    REQUIRE( liam.sees( here, zombie ) );
    Messages::clear_messages();
    CHECK( npc_ai::process_hide( liam ) );
    CHECK( npc_ai::hide_state_for( liam )->phase == npc_ai::hide_phase::cornered );
    CHECK_FALSE( liam.rules.has_flag( ally_rule::forbid_engage ) );
    CHECK( said_either( "It's on me", "Lo tengo encima" ) );

    clear_creatures();
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
    CHECK( liam.rules.has_flag( ally_rule::forbid_engage ) );
    CHECK( said_either( "Back to the hideout", "Sigo hacia el escondite" ) );
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
        CHECK_FALSE( npc_ai::is_hiding( liam ) );
        CHECK( liam.is_following() );
        CHECK_FALSE( liam.is_prone() );
        REQUIRE( liam.goto_to_this_pos.has_value() );
        CHECK( here.get_bub( *liam.goto_to_this_pos ) == door );
    }

    SECTION( "through the vanilla follow (attitude set elsewhere)" ) {
        liam.set_attitude( NPCATT_FOLLOW );
        liam.set_mission( NPC_MISSION_NULL );
        CHECK_FALSE( npc_ai::process_hide( liam ) );
        CHECK_FALSE( npc_ai::is_hiding( liam ) );
        REQUIRE( liam.goto_to_this_pos.has_value() );
        CHECK( here.get_bub( *liam.goto_to_this_pos ) == door );
    }

    SECTION( "guard here does not walk it anywhere" ) {
        npc_ai::execute_tactical_order( { &liam }, npc_ai::tactical_order::guard );
        CHECK_FALSE( npc_ai::is_hiding( liam ) );
        CHECK_FALSE( liam.goto_to_this_pos.has_value() );
    }
}
