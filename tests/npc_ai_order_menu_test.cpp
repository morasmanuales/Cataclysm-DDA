#include "cata_catch.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "avatar.h"
#include "faction.h"
#include "calendar.h"
#include "game.h"
#include "item.h"
#include "map.h"
#include "map_helpers.h"
#include "messages.h"
#include "npc.h"
#include "npc_ai_async.h"
#include "npc_ai_batch_pickup.h"
#include "npc_ai_context.h"
#include "npc_ai_equipment.h"
#include "npc_ai_fire.h"
#include "npc_ai_interior.h"
#include "npc_ai_order_menu.h"
#include "npc_ai_rescue.h"
#include "npc_ai_tactical.h"
#include "npc_ai_vehicle_unload.h"
#include "player_helpers.h"
#include "point.h"
#include "sounds.h"
#include "type_id.h"

namespace
{

static const faction_id faction_your_followers( "your_followers" );
static const itype_id itype_backpack( "backpack" );
static const itype_id itype_can_beans( "can_beans" );
static const itype_id itype_flashlight( "flashlight" );

npc &prepare_menu_follower()
{
    npc_ai::end_ai_session();
    clear_map();
    clear_avatar();
    clear_npcs();
    set_time_to_day();
    g->faction_manager_ptr->create_if_needed();
    g->place_player( tripoint_bub_ms{ 60, 60, 0 } );
    npc &who = spawn_npc( point_bub_ms{ 61, 60 }, "test_talker" );
    who.name = "Liam";
    who.set_fac( faction_your_followers );
    who.set_attitude( NPCATT_FOLLOW );
    Messages::clear_messages();
    return who;
}

} // namespace

TEST_CASE( "order_menu_catalogue_is_closed_and_well_formed", "[npc_ai][npc_ai_orders]" )
{
    const std::vector<npc_ai::menu_order_entry> catalogue = npc_ai::order_menu_catalogue();
    REQUIRE( catalogue.size() == 17 );

    std::set<int> hotkeys;
    std::set<int> ids;
    for( const npc_ai::menu_order_entry &entry : catalogue ) {
        CHECK( !entry.label.empty() );
        CHECK( !entry.description.empty() );
        CHECK( hotkeys.insert( entry.hotkey ).second );
        CHECK( ids.insert( static_cast<int>( entry.id ) ).second );
        // Entries that need a target must have a prompt; the rest must not.
        const bool needs_target = entry.target != npc_ai::menu_order_target::none;
        CHECK( npc_ai::order_menu_target_prompt( entry.id ).empty() != needs_target );
        // Without a target the phrase exists only for self-contained orders.
        CHECK( npc_ai::order_menu_phrase( entry.id, "" ).empty() == needs_target );
        CHECK( !npc_ai::order_menu_phrase( entry.id, "la mochila" ).empty() );
        CHECK( npc_ai::find_order_menu_entry( entry.id ).has_value() );
    }

    // Orders without a native group handler are fanned out by the menu; the
    // set must stay in sync with the group branch of ai_dispatch_player_line.
    const std::set<npc_ai::menu_order> fan_out = {
        npc_ai::menu_order::pickup_all_food, npc_ai::menu_order::search_food,
        npc_ai::menu_order::search_item,
        npc_ai::menu_order::wield, npc_ai::menu_order::start_fire,
        npc_ai::menu_order::unload_vehicle, npc_ai::menu_order::watch
    };
    for( const npc_ai::menu_order_entry &entry : catalogue ) {
        CHECK( entry.native_group_path == ( fan_out.count( entry.id ) == 0 ) );
    }

    // Groups are contiguous (the menu draws one header per change) and every
    // group has a title.
    std::set<int> seen_groups;
    std::optional<npc_ai::menu_order_group> last;
    for( const npc_ai::menu_order_entry &entry : catalogue ) {
        if( !last || *last != entry.group ) {
            CHECK( seen_groups.insert( static_cast<int>( entry.group ) ).second );
            CHECK( !npc_ai::menu_order_group_title( entry.group ).empty() );
            last = entry.group;
        }
    }
    CHECK( seen_groups.size() == 3 );
    CHECK( catalogue.front().group == npc_ai::menu_order_group::movement );
    CHECK( catalogue.back().group == npc_ai::menu_order_group::tasks );
}

TEST_CASE( "order_menu_phrases_hit_the_deterministic_fast_path", "[npc_ai][npc_ai_orders]" )
{
    using npc_ai::menu_order;
    using npc_ai::order_menu_phrase;

    // Tactical orders never reach the model.
    CHECK( npc_ai::parse_tactical_order( order_menu_phrase( menu_order::follow, "" ) ) ==
           npc_ai::tactical_order::follow );
    CHECK( npc_ai::parse_tactical_order( order_menu_phrase( menu_order::guard, "" ) ) ==
           npc_ai::tactical_order::guard );

    CHECK( npc_ai::parse_structured_voice_order( order_menu_phrase( menu_order::enter_interior, "" ) )
           == npc_ai::structured_voice_order::enter_nearest_reachable_safe_interior );

    CHECK( npc_ai::parse_rescue_order( order_menu_phrase( menu_order::drag_casualty, "Liam" ) ) );

    // Equipment orders resolve to the intended deterministic action.
    CHECK( npc_ai::detect_equipment_action( order_menu_phrase( menu_order::drop, "el cuchillo" ) ) ==
           npc_ai::equipment_action::drop );
    CHECK( npc_ai::detect_equipment_action( order_menu_phrase( menu_order::wear, "la mochila" ) ) ==
           npc_ai::equipment_action::wear );
    CHECK( npc_ai::detect_equipment_action( order_menu_phrase( menu_order::take_off, "el casco" ) ) ==
           npc_ai::equipment_action::take_off );
    CHECK( npc_ai::detect_equipment_action( order_menu_phrase( menu_order::store, "el arma" ) ) ==
           npc_ai::equipment_action::store );
    CHECK( npc_ai::detect_equipment_action( order_menu_phrase( menu_order::recover, "mochila" ) ) ==
           npc_ai::equipment_action::recover );
    // A plain pickup must not be mistaken for an equipment order.
    CHECK( npc_ai::detect_equipment_action( order_menu_phrase( menu_order::pickup, "la linterna" ) ) ==
           npc_ai::equipment_action::none );

    // Surrounding whitespace and a trailing period typed by the player are
    // absorbed so the phrase stays canonical.
    CHECK( order_menu_phrase( menu_order::pickup, "  la linterna. " ) == "Recoge la linterna." );
}

TEST_CASE( "order_menu_task_phrases_are_claimed_by_their_handlers", "[npc_ai][npc_ai_orders]" )
{
    using npc_ai::menu_order;
    using npc_ai::order_menu_phrase;
    npc &who = prepare_menu_follower();

    // On an empty map every handler rejects, but each one must first claim the
    // phrase (handled == true) so it never falls through to dialogue.
    CHECK( npc_ai::try_handle_start_fire_command( who,
            order_menu_phrase( menu_order::start_fire, "" ) ).handled );
    CHECK( npc_ai::try_handle_vehicle_unload_command( who,
            order_menu_phrase( menu_order::unload_vehicle, "" ) ).handled );
    CHECK( npc_ai::try_handle_batch_pickup_command( who,
            order_menu_phrase( menu_order::pickup_all_food, "" ) ).handled );
    // The search phrase is claimed by the search handler and by no other
    // deterministic parser that runs before it on the single-companion path.
    const std::string search = order_menu_phrase( menu_order::search_food, "" );
    CHECK( npc_ai::is_search_food_command( search ) );
    CHECK_FALSE( npc_ai::try_handle_batch_pickup_command( who, search ).handled );
    CHECK( npc_ai::detect_equipment_action( search ) == npc_ai::equipment_action::none );
    CHECK( npc_ai::try_handle_search_food_command( who, search ).handled );
    npc_ai::cancel_food_search( who );

    // The item search phrase is claimed only by the item search handler.
    const std::string item_search = order_menu_phrase( menu_order::search_item, "la linterna" );
    CHECK( item_search == "Busca y trae la linterna." );
    CHECK_FALSE( npc_ai::is_search_food_command( item_search ) );
    CHECK( npc_ai::detect_equipment_action( item_search ) == npc_ai::equipment_action::none );
    CHECK_FALSE( npc_ai::try_handle_batch_pickup_command( who, item_search ).handled );
    CHECK( npc_ai::parse_search_item_request( item_search ) == "la linterna" );
    // "Busca la mochila" stays an equipment recovery order, as before.
    CHECK( npc_ai::detect_equipment_action( "Busca la mochila." ) ==
           npc_ai::equipment_action::recover );
}

TEST_CASE( "search_item_request_is_normalised", "[npc_ai][npc_ai_orders][npc_ai_food_search]" )
{
    using npc_ai::normalize_search_words;
    CHECK( normalize_search_words( "  La LINTERNA.  " ) == std::vector<std::string>{ "linterna" } );
    CHECK( normalize_search_words( "una lata de judías" ) ==
           std::vector<std::string>{ "lata", "judias" } );
    CHECK( normalize_search_words( "the flashlight, please" ) ==
           std::vector<std::string>{ "flashlight" } );
    CHECK( normalize_search_words( "el el la" ).empty() );
    CHECK( npc_ai::parse_search_item_request( "Search for a flashlight" ) == "a flashlight" );
    CHECK( npc_ai::parse_search_item_request( "Busca comida." ).empty() );
    CHECK( npc_ai::parse_search_item_request( "Recoge la linterna." ).empty() );
}

TEST_CASE( "search_food_walks_to_a_fridge_and_collects_what_it_holds",
           "[npc_ai][npc_ai_orders][npc_ai_food_search]" )
{
    npc &who = prepare_menu_follower();
    npc_ai::reset_all_food_batches();
    map &here = get_map();
    clear_items( 0 );
    const tripoint_bub_ms origin = who.pos_bub( here );
    // Food in a fridge three tiles away, out of the current room logic: the
    // contents cannot be seen from here, only from an adjacent tile.
    const tripoint_bub_ms fridge = origin + tripoint_rel_ms{ 3, 0, 0 };
    here.furn_set( fridge, furn_str_id( "f_fridge" ) );
    here.add_item_or_charges( fridge, item( itype_can_beans, calendar::turn ) );
    // Give the follower somewhere to store it.
    item backpack( itype_backpack, calendar::turn );
    REQUIRE( who.wear_item( backpack, false ).has_value() );

    const npc_ai::search_food_command_result result =
        npc_ai::try_handle_search_food_command( who, "Busca comida." );
    REQUIRE( result.handled );
    REQUIRE( result.started );
    REQUIRE( npc_ai::has_food_search( who ) );
    REQUIRE( here.i_at( fridge ).size() == 1 );

    // Drive the task: walking turns consume the action; once adjacent the
    // inspection queues a pickup batch that the directed pickup engine runs.
    for( int turn = 0; turn < 120 && !npc_ai::food_search_is_returning( who ); ++turn ) {
        who.set_moves( 100 );
        if( !npc_ai::process_food_search( who ) ) {
            if( who.ai_directed_pickup ) {
                who.pick_up_item();
            }
            npc_ai::process_batch_pickup( who );
        }
    }
    // The tour is over: the companion walks back and reports on arrival.
    REQUIRE( npc_ai::food_search_is_returning( who ) );
    CHECK( who.is_following() );
    CHECK( who.goto_to_this_pos == get_avatar().pos_abs() );
    who.setpos( here, get_avatar().pos_bub( here ) + point::east );
    who.goto_to_this_pos = std::nullopt;
    who.set_moves( 100 );
    npc_ai::process_food_search( who );
    CHECK_FALSE( npc_ai::has_food_search( who ) );
    CHECK( here.i_at( fridge ).empty() );
    CHECK( who.has_item_with( []( const item & it ) {
        return it.typeId() == itype_can_beans;
    } ) );
}

TEST_CASE( "search_item_finds_the_named_object_and_reports_it",
           "[npc_ai][npc_ai_orders][npc_ai_food_search]" )
{
    npc &who = prepare_menu_follower();
    npc_ai::reset_all_food_batches();
    map &here = get_map();
    clear_items( 0 );
    const tripoint_bub_ms origin = who.pos_bub( here );
    const tripoint_bub_ms cupboard = origin + tripoint_rel_ms{ 0, 3, 0 };
    here.furn_set( cupboard, furn_str_id( "f_cupboard" ) );
    here.add_item_or_charges( cupboard, item( itype_flashlight, calendar::turn ) );
    // A decoy that must be left alone.
    here.add_item_or_charges( cupboard, item( itype_can_beans, calendar::turn ) );
    const tripoint_bub_ms far_shelf = origin + tripoint_rel_ms{ 0, 8, 0 };
    here.furn_set( far_shelf, furn_str_id( "f_cupboard" ) );
    here.add_item_or_charges( far_shelf, item( itype_flashlight, calendar::turn ) );
    item backpack( itype_backpack, calendar::turn );
    REQUIRE( who.wear_item( backpack, false ).has_value() );

    const npc_ai::search_food_command_result result =
        npc_ai::try_handle_search_item_command( who, "Busca y trae la FLASHLIGHT." );
    REQUIRE( result.handled );
    REQUIRE( result.started );
    CHECK( result.message.find( "flashlight" ) != std::string::npos );

    sounds::reset_sounds();
    Messages::clear_messages();
    for( int turn = 0; turn < 120 && !npc_ai::food_search_is_returning( who ); ++turn ) {
        who.set_moves( 100 );
        if( !npc_ai::process_food_search( who ) ) {
            if( who.ai_directed_pickup ) {
                who.pick_up_item();
            }
            npc_ai::process_batch_pickup( who );
        }
    }
    // The tour is over: the companion walks back and reports on arrival.
    REQUIRE( npc_ai::food_search_is_returning( who ) );
    CHECK( who.is_following() );
    CHECK( who.goto_to_this_pos == get_avatar().pos_abs() );
    who.setpos( here, get_avatar().pos_bub( here ) + point::east );
    who.goto_to_this_pos = std::nullopt;
    who.set_moves( 100 );
    npc_ai::process_food_search( who );
    CHECK_FALSE( npc_ai::has_food_search( who ) );
    CHECK( who.has_item_with( []( const item & it ) {
        return it.typeId() == itype_flashlight;
    } ) );
    CHECK_FALSE( who.has_item_with( []( const item & it ) {
        return it.typeId() == itype_can_beans;
    } ) );
    CHECK( here.i_at( cupboard ).size() == 1 );
    // A second flashlight further away is left alone: the search stops as
    // soon as the requested object is in hand.
    CHECK( here.i_at( far_shelf ).size() == 1 );
    // The closing report names what was brought back.
    sounds::process_sound_markers( &get_avatar() );
    bool reported = false;
    for( const auto &message : Messages::recent_messages( 0 ) ) {
        if( message.second.find( "1 x " ) != std::string::npos &&
            message.second.find( "lashlight" ) != std::string::npos ) {
            reported = true;
        }
    }
    CHECK( reported );
}

TEST_CASE( "search_food_scans_visible_piles_from_afar_and_never_rechecks_a_place",
           "[npc_ai][npc_ai_orders][npc_ai_food_search]" )
{
    npc &who = prepare_menu_follower();
    npc_ai::reset_all_food_batches();
    map &here = get_map();
    clear_items( 0 );
    const tripoint_bub_ms origin = who.pos_bub( here );
    // Three open piles in plain sight at different distances.
    const std::vector<tripoint_bub_ms> piles = {
        origin + tripoint_rel_ms{ 4, 0, 0 }, origin + tripoint_rel_ms{ -4, 2, 0 },
        origin + tripoint_rel_ms{ 0, 6, 0 }
    };
    for( const tripoint_bub_ms &pile : piles ) {
        here.add_item_or_charges( pile, item( itype_can_beans, calendar::turn ) );
    }
    item backpack( itype_backpack, calendar::turn );
    REQUIRE( who.wear_item( backpack, false ).has_value() );

    REQUIRE( who.is_following() );
    REQUIRE( npc_ai::try_handle_search_food_command( who, "Busca comida." ).started );
    // Following is suspended for the tour so the follow logic cannot pull
    // the companion back; it is restored on the way back.
    CHECK_FALSE( who.is_following() );
    // ...but not as a guard either: a guard is stationary and never picks up.
    CHECK_FALSE( who.is_stationary( true ) );

    // First tick: every pile is examinable from here, so all three are
    // checked at once and the pickups start without walking a single step.
    const tripoint_bub_ms start = who.pos_bub( here );
    who.set_moves( 100 );
    CHECK_FALSE( npc_ai::process_food_search( who ) );
    CHECK( who.pos_bub( here ) == start );
    CHECK( who.ai_directed_pickup );

    int ticks = 0;
    for( ; ticks < 200 && !npc_ai::food_search_is_returning( who ); ++ticks ) {
        who.set_moves( 100 );
        if( !npc_ai::process_food_search( who ) ) {
            if( who.ai_directed_pickup ) {
                who.pick_up_item();
            }
            npc_ai::process_batch_pickup( who );
        }
    }
    for( const tripoint_bub_ms &pile : piles ) {
        CHECK( here.i_at( pile ).empty() );
    }
    // Once done the companion heads back to the player and keeps following;
    // nothing is said until it gets there.
    REQUIRE( npc_ai::food_search_is_returning( who ) );
    CHECK( who.is_following() );
    CHECK( who.goto_to_this_pos == get_avatar().pos_abs() );
    sounds::process_sound_markers( &get_avatar() );
    for( const auto &message : Messages::recent_messages( 0 ) ) {
        CHECK( message.second.find( "3 x " ) == std::string::npos );
    }
    // Still far away: the walk is not over, so no report yet.
    who.set_moves( 100 );
    CHECK_FALSE( npc_ai::process_food_search( who ) );
    CHECK( npc_ai::has_food_search( who ) );
    // Arrival next to the player delivers it.
    who.setpos( here, get_avatar().pos_bub( here ) + point::east );
    who.set_moves( 100 );
    npc_ai::process_food_search( who );
    CHECK_FALSE( npc_ai::has_food_search( who ) );
    // The report counts each place exactly once.
    sounds::process_sound_markers( &get_avatar() );
    bool reported = false;
    for( const auto &message : Messages::recent_messages( 0 ) ) {
        if( message.second.find( "3 " ) != std::string::npos &&
            message.second.find( "3 x " ) != std::string::npos ) {
            reported = true;
        }
    }
    CHECK( reported );
}

TEST_CASE( "group_item_search_ends_for_everyone_once_one_companion_has_it",
           "[npc_ai][npc_ai_orders][npc_ai_food_search]" )
{
    npc &liam = prepare_menu_follower();
    npc &kim = spawn_npc( point_bub_ms{ 59, 60 }, "test_talker" );
    kim.name = "Kim";
    kim.set_fac( faction_your_followers );
    kim.set_attitude( NPCATT_FOLLOW );
    npc_ai::reset_all_food_batches();
    map &here = get_map();
    clear_items( 0 );
    // One flashlight next to Liam; a far cupboard keeps Kim's tour busy.
    const tripoint_bub_ms near_liam = liam.pos_bub( here ) + tripoint_rel_ms{ 2, 0, 0 };
    here.add_item_or_charges( near_liam, item( itype_flashlight, calendar::turn ) );
    const tripoint_bub_ms far_cupboard = kim.pos_bub( here ) + tripoint_rel_ms{ -7, 6, 0 };
    here.furn_set( far_cupboard, furn_str_id( "f_cupboard" ) );
    here.add_item_or_charges( far_cupboard, item( itype_can_beans, calendar::turn ) );
    for( npc *who : { &liam, &kim } ) {
        item backpack( itype_backpack, calendar::turn );
        REQUIRE( who->wear_item( backpack, false ).has_value() );
    }

    // Same order to both, as the menu does with "everyone".
    REQUIRE( npc_ai::try_handle_search_item_command( liam, "Busca y trae la flashlight." ).started );
    REQUIRE( npc_ai::try_handle_search_item_command( kim, "Busca y trae la flashlight." ).started );

    // Kim starts walking towards her far spot.
    kim.set_moves( 100 );
    npc_ai::process_food_search( kim );
    REQUIRE( npc_ai::has_food_search( kim ) );
    REQUIRE_FALSE( npc_ai::food_search_is_returning( kim ) );

    // Liam sees the flashlight from where he stands and picks it up.
    for( int turn = 0; turn < 60 && !npc_ai::food_search_is_returning( liam ); ++turn ) {
        liam.set_moves( 100 );
        if( !npc_ai::process_food_search( liam ) ) {
            if( liam.ai_directed_pickup ) {
                liam.pick_up_item();
            }
            npc_ai::process_batch_pickup( liam );
        }
    }
    REQUIRE( npc_ai::food_search_is_returning( liam ) );
    CHECK( liam.has_item_with( []( const item & it ) {
        return it.typeId() == itype_flashlight;
    } ) );

    // Kim's search was cut short by Liam's find: she is walking back too.
    CHECK( npc_ai::food_search_is_returning( kim ) );
    CHECK( kim.goto_to_this_pos == get_avatar().pos_abs() );
    CHECK( here.i_at( far_cupboard ).size() == 1 );

    // Both report on arrival; Kim's report names who found it.
    sounds::reset_sounds();
    Messages::clear_messages();
    for( npc *who : { &liam, &kim } ) {
        who->setpos( here, get_avatar().pos_bub( here ) + ( who == &liam ? point::east : point::west ) );
        who->set_moves( 100 );
        npc_ai::process_food_search( *who );
        CHECK_FALSE( npc_ai::has_food_search( *who ) );
    }
    sounds::process_sound_markers( &get_avatar() );
    bool liam_reported = false;
    bool kim_reported = false;
    for( const auto &message : Messages::recent_messages( 0 ) ) {
        if( message.second.find( "1 x " ) != std::string::npos ) {
            liam_reported = true;
        }
        if( message.second.find( "Liam" ) != std::string::npos &&
            message.second.find( "heading back" ) != std::string::npos ) {
            kim_reported = true;
        }
    }
    CHECK( liam_reported );
    CHECK( kim_reported );
}

TEST_CASE( "cancelled_search_restores_following", "[npc_ai][npc_ai_orders][npc_ai_food_search]" )
{
    npc &who = prepare_menu_follower();
    npc_ai::reset_all_food_batches();
    map &here = get_map();
    clear_items( 0 );
    here.add_item_or_charges( who.pos_bub( here ) + tripoint_rel_ms{ 5, 0, 0 },
                              item( itype_can_beans, calendar::turn ) );
    REQUIRE( who.is_following() );
    REQUIRE( npc_ai::try_handle_search_food_command( who, "Busca comida." ).started );
    REQUIRE_FALSE( who.is_following() );

    // "Follow me" mid-tour: the search is dropped and the companion follows.
    npc_ai::execute_tactical_order( { &who }, npc_ai::tactical_order::follow );
    CHECK_FALSE( npc_ai::has_food_search( who ) );
    CHECK( who.is_following() );

    // "Stay here" mid-tour: the search is dropped and the guard order wins.
    REQUIRE( npc_ai::try_handle_search_food_command( who, "Busca comida." ).started );
    npc_ai::execute_tactical_order( { &who }, npc_ai::tactical_order::guard );
    CHECK_FALSE( npc_ai::has_food_search( who ) );
    CHECK( who.mission == NPC_MISSION_GUARD_ALLY );
    CHECK_FALSE( who.is_following() );
}
