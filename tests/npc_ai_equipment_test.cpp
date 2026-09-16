#include "cata_catch.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>

#include "avatar.h"
#include "bodypart.h"
#include "calendar.h"
#include "damage.h"
#include "game.h"
#include "item.h"
#include "item_location.h"
#include "map.h"
#include "map_helpers.h"
#include "map_selector.h"
#include "martialarts.h"
#include "messages.h"
#include "monattack.h"
#include "monster.h"
#include "npc.h"
#include "npc_ai_async.h"
#include "npc_ai_context.h"
#include "npc_ai_equipment.h"
#include "npc_ai_equipment_memory.h"
#include "npc_ai_pickup.h"
#include "npc_ai_wield.h"
#include "player_helpers.h"
#include "point.h"
#include "sounds.h"
#include "translations.h"
#include "type_id.h"
#if defined( LOCALIZE )
#include "translation_manager.h"
#endif

namespace
{

static const itype_id itype_backpack( "backpack" );
static const itype_id itype_bottle_plastic( "bottle_plastic" );
static const itype_id itype_debug_backpack( "debug_backpack" );
static const itype_id itype_fire_ax( "fire_ax" );
static const itype_id itype_glass_shard( "glass_shard" );
static const itype_id itype_hat_hard( "hat_hard" );
static const itype_id itype_knife_hunting( "knife_hunting" );
static const itype_id itype_sword_sheets_welded_large( "sword_sheets_welded_large" );
static const itype_id itype_test_armguard( "test_armguard" );
static const itype_id itype_test_rock( "test_rock" );
static const itype_id itype_twig( "twig" );

struct reset_ai_requests {
    ~reset_ai_requests() {
        npc_ai::reset_ai_request_system_for_test();
    }
};

#if defined( LOCALIZE )
struct restore_equipment_test_language {
    std::string previous = TranslationManager::GetInstance().GetCurrentLanguage();
    ~restore_equipment_test_language() {
        set_language( previous );
    }
};
#endif

npc &prepare_equipment_npc()
{
    clear_map();
    clear_avatar();
    set_time_to_day();
    g->place_player( tripoint_bub_ms{ 60, 60, 0 } );
    npc &who = spawn_npc( point_bub_ms{ 65, 60 }, "test_talker" );
    clear_character( who );
    who.set_moves( 1000 );
    npc_ai::clear_dropped_equipment_memories_for_test( who );
    map &here = get_map();
    here.invalidate_map_cache( who.posz() );
    here.build_map_cache( who.posz(), true );
    here.invalidate_visibility_cache();
    here.update_visibility_cache( who.posz() );
    who.recalc_sight_limits();
    return who;
}

item_location wear_backpack( npc &who, const bool with_contents = false )
{
    item backpack( itype_backpack, calendar::turn );
    if( with_contents ) {
        REQUIRE( backpack.put_in( item( itype_test_rock, calendar::turn ),
                                  pocket_type::CONTAINER ).success() );
    }
    const std::optional<std::list<item>::iterator> worn =
        who.worn.wear_item( who, backpack, false, false );
    REQUIRE( worn.has_value() );
    return item_location( who, &**worn );
}

item *map_item_of_type( const tripoint_bub_ms &where, const itype_id &type )
{
    for( item &it : get_map().i_at( where ) ) {
        if( it.typeId() == type ) {
            return &it;
        }
    }
    return nullptr;
}

int map_item_count_of_type( const tripoint_bub_ms &where, const itype_id &type )
{
    int count = 0;
    for( const item &it : get_map().i_at( where ) ) {
        count += it.typeId() == type;
    }
    return count;
}

int owned_item_count_of_type( npc &who, const itype_id &type )
{
    const std::vector<item_location> owned = who.all_items_loc();
    return std::count_if( owned.begin(), owned.end(),
    [&]( const item_location & location ) {
        return location && location->typeId() == type;
    } );
}

std::size_t substring_count( const std::string &text, const std::string &needle )
{
    std::size_t count = 0;
    for( std::size_t position = text.find( needle ); position != std::string::npos;
         position = text.find( needle, position + needle.size() ) ) {
        ++count;
    }
    return count;
}

npc_ai::pickup_command_result resolve_pickup_command( npc &who, const std::string &line,
        const int selected_index = 1 )
{
    npc_ai::set_ai_request_executor_for_test( [selected_index]( const std::string & ) {
        return npc_ai::ai_response{ true,
                                   "PICKUP_INDEX=" + std::to_string( selected_index ), "" };
    }, false );
    npc_ai::begin_ai_session();
    const npc_ai::pickup_command_result result =
        npc_ai::try_handle_pickup_command( who, line );
    if( result.pending ) {
        npc_ai::get_ai_request_queue().start();
        REQUIRE( npc_ai::get_ai_request_queue().wait_until_idle_for_test(
                     std::chrono::seconds( 1 ) ) );
        npc_ai::process_ai_completions();
    }
    return result;
}

void finish_directed_pickup( npc &who )
{
    for( int step = 0; step < 20 && who.ai_directed_pickup; ++step ) {
        who.set_moves( 2000 );
        who.pick_up_item();
    }
    REQUIRE_FALSE( who.ai_directed_pickup );
}

std::optional<tripoint_bub_ms> nearby_item_position_with_uid( const tripoint_bub_ms &origin,
        const std::string &uid )
{
    map &here = get_map();
    for( const tripoint_bub_ms &position : here.points_in_radius( origin, 1 ) ) {
        for( item &it : here.i_at( position ) ) {
            if( it.get_var( "npc_ai_equipment_uid" ) == uid ) {
                return position;
            }
        }
    }
    return std::nullopt;
}

bool owns_item_type( npc &who, const itype_id &type )
{
    const std::vector<item_location> items = who.all_items_loc();
    return std::any_of( items.begin(), items.end(), [&]( const item_location &location ) {
        return location && location->typeId() == type;
    } );
}

item_location owned_item_with_uid( npc &who, const std::string &uid )
{
    for( const item_location &location : who.all_items_loc() ) {
        if( location && location->get_var( "npc_ai_equipment_uid" ) == uid ) {
            return location;
        }
    }
    return item_location::nowhere;
}

item_location owned_item_with_test_identity( npc &who, const std::string &identity )
{
    for( const item_location &location : who.all_items_loc() ) {
        if( location && location->get_var( "npc_ai_test_identity" ) == identity ) {
            return location;
        }
    }
    return item_location::nowhere;
}

void run_equipment_recovery( npc &who, const int max_steps = 20 )
{
    for( int step = 0; step < max_steps; ++step ) {
        who.regen_ai_cache();
        who.set_moves( 2000 );
        npc_ai::process_equipment_recovery( who );
        const std::vector<npc_ai::dropped_equipment_memory> records =
            npc_ai::get_dropped_equipment_memories( who );
        if( std::any_of( records.begin(), records.end(),
        []( const npc_ai::dropped_equipment_memory & record ) {
            return record.status == npc_ai::equipment_memory_status::recovered;
        } ) ) {
            return;
        }
    }
}

} // namespace

TEST_CASE( "npc_ai_equipment_detects_general_spanish_actions",
           "[npc_ai][npc_ai_equipment]" )
{
    CHECK( npc_ai::detect_equipment_action( "Suelta la mochila ahora." ) ==
           npc_ai::equipment_action::drop );
    CHECK( npc_ai::detect_equipment_action( "Quítate el casco." ) ==
           npc_ai::equipment_action::take_off );
    CHECK( npc_ai::detect_equipment_action( "Ponte el casco." ) ==
           npc_ai::equipment_action::wear );
    CHECK( npc_ai::detect_equipment_action( "Recoge tu mochila y póntela." ) ==
           npc_ai::equipment_action::recover );
    CHECK( npc_ai::detect_equipment_action( "Vuelve a ponerte la mochila." ) ==
           npc_ai::equipment_action::wear );
    CHECK( npc_ai::detect_equipment_action( "Busca la mochila que dejaste." ) ==
           npc_ai::equipment_action::recover );
    CHECK( npc_ai::detect_equipment_action( "Guarda el arma." ) ==
           npc_ai::equipment_action::store );
    CHECK( npc_ai::detect_equipment_action( "Todos suelten sus armas." ) ==
           npc_ai::equipment_action::drop );
    CHECK( npc_ai::detect_equipment_action( "Todos recojan sus armas." ) ==
           npc_ai::equipment_action::recover );
    CHECK( npc_ai::detect_equipment_action( "¿Cómo estás?" ) ==
           npc_ai::equipment_action::none );
}

TEST_CASE( "npc_ai_equipment_drops_actual_worn_container_with_contents",
           "[npc_ai][npc_ai_equipment]" )
{
    npc &who = prepare_equipment_npc();
    wear_backpack( who, true );
    const tripoint_bub_ms drop_position = who.pos_bub( get_map() );

    const npc_ai::equipment_command_result result =
        npc_ai::try_handle_equipment_command( who, "Suelta la mochila." );

    CHECK( result.handled );
    CHECK( result.success );
    CHECK_FALSE( who.is_wearing( itype_backpack ) );
    item *dropped = map_item_of_type( drop_position, itype_backpack );
    REQUIRE( dropped != nullptr );
    const std::list<item *> contents = dropped->all_items_top( pocket_type::CONTAINER );
    CHECK( std::any_of( contents.begin(), contents.end(), []( const item *it ) {
        return it != nullptr && it->typeId() == itype_test_rock;
    } ) );
}

TEST_CASE( "npc_ai_equipment_drops_wielded_weapon_through_native_drop_activity",
           "[npc_ai][npc_ai_equipment]" )
{
    npc &who = prepare_equipment_npc();
    wear_backpack( who );
    item_location knife = who.i_add( item( itype_knife_hunting, calendar::turn ) );
    REQUIRE( knife );
    REQUIRE( who.wield( knife ) );
    item_location weapon = who.get_wielded_item();
    REQUIRE( weapon );

    const npc_ai::equipment_command_result result =
        npc_ai::execute_equipment_action( who, npc_ai::equipment_action::drop, weapon );

    CHECK( result.success );
    CHECK_FALSE( who.get_wielded_item() );
    CHECK( map_item_of_type( who.pos_bub( get_map() ), itype_knife_hunting ) != nullptr );
}

TEST_CASE( "npc_ai_melee_disarm_remembers_the_npcs_lost_weapon",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_memory][npc_ai_phase6]" )
{
    npc &who = prepare_equipment_npc();
    wear_backpack( who );
    item_location knife = who.i_add( item( itype_knife_hunting, calendar::turn ) );
    REQUIRE( knife );
    REQUIRE( who.wield( knife ) );
    REQUIRE( who.is_armed() );

    avatar &attacker = get_avatar();
    damage_instance damage;
    int move_cost = 100;
    item_location attacker_weapon = attacker.get_wielded_item();
    attacker.perform_technique( matec_id( "WHIP_DISARM" ).obj(), who, damage,
                                move_cost, attacker_weapon );

    CHECK_FALSE( who.get_wielded_item() );
    item *dropped = map_item_of_type( who.pos_bub( get_map() ), itype_knife_hunting );
    REQUIRE( dropped != nullptr );
    CHECK_FALSE( dropped->get_var( "npc_ai_equipment_uid" ).empty() );
    const std::vector<npc_ai::dropped_equipment_memory> records =
        npc_ai::get_dropped_equipment_memories( who );
    REQUIRE( records.size() == 1 );
    CHECK( records.front().reason == "melee_disarm" );
    CHECK( records.front().retrieval_expected );
    CHECK( records.front().location == who.pos_abs() );
    CHECK( dropped->get_var( "npc_ai_equipment_uid" ) == records.front().item_uid );
    CHECK( dropped->get_var( "npc_ai_equipment_owner", -1 ) == who.getID().get_value() );
}

TEST_CASE( "npc_ai_bio_op_disarm_remembers_the_npcs_lost_weapon",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_memory][npc_ai_phase6]" )
{
    npc &who = prepare_equipment_npc();
    wear_backpack( who );
    item_location axe = who.i_add( item( itype_fire_ax, calendar::turn ) );
    REQUIRE( axe );
    REQUIRE( who.wield( axe ) );
    who.set_dodges_left( 0 );

    map &here = get_map();
    get_avatar().setpos( here, tripoint_bub_ms{ 55, 55, 0 } );
    monster &attacker = spawn_test_monster( "mon_zombie", who.pos_bub( here ) + point::east );
    attacker.set_dest( who.pos_abs() );
    REQUIRE( attacker.attack_target() == &who );

    for( int attempt = 0; attempt < 100 && who.is_armed(); ++attempt ) {
        attacker.set_moves( 1000 );
        attacker.set_dest( who.pos_abs() );
        REQUIRE( mattack::bio_op_disarm( &attacker ) );
    }
    REQUIRE_FALSE( who.is_armed() );

    const std::vector<npc_ai::dropped_equipment_memory> records =
        npc_ai::get_dropped_equipment_memories( who );
    REQUIRE( records.size() == 1 );
    CHECK( records.front().reason == "bio_op_disarm" );
    CHECK( records.front().retrieval_expected );
    const std::optional<tripoint_bub_ms> dropped_at = nearby_item_position_with_uid(
                who.pos_bub( here ), records.front().item_uid );
    REQUIRE( dropped_at.has_value() );
    CHECK( records.front().location == here.get_abs( *dropped_at ) );
}

TEST_CASE( "npc_ai_injury_drop_remembers_the_npcs_lost_weapon",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_memory][npc_ai_phase6]" )
{
    npc &who = prepare_equipment_npc();
    wear_backpack( who );
    item_location axe = who.i_add( item( itype_fire_ax, calendar::turn ) );
    REQUIRE( axe );
    REQUIRE( who.wield( axe ) );
    who.set_part_hp_cur( body_part_arm_l, 0 );
    who.set_part_hp_cur( body_part_arm_r, 0 );
    REQUIRE_FALSE( who.can_wield( *who.get_wielded_item() ).success() );

    who.apply_damage( nullptr, body_part_torso, 1 );

    CHECK_FALSE( who.get_wielded_item() );
    item *dropped = map_item_of_type( who.pos_bub( get_map() ), itype_fire_ax );
    REQUIRE( dropped != nullptr );
    const std::vector<npc_ai::dropped_equipment_memory> records =
        npc_ai::get_dropped_equipment_memories( who );
    REQUIRE( records.size() == 1 );
    CHECK( records.front().reason == "injury_tumbling" );
    CHECK( records.front().retrieval_expected );
    CHECK( dropped->get_var( "npc_ai_equipment_uid" ) == records.front().item_uid );
}

TEST_CASE( "npc_ai_equipment_wears_and_removes_owned_clothing",
           "[npc_ai][npc_ai_equipment]" )
{
    npc &who = prepare_equipment_npc();
    wear_backpack( who );
    item_location hat = who.i_add( item( itype_hat_hard, calendar::turn ) );
    REQUIRE( hat );

    const npc_ai::equipment_command_result wear_result =
        npc_ai::execute_equipment_action( who, npc_ai::equipment_action::wear, hat );
    REQUIRE( wear_result.success );
    CHECK( wear_result.action_started );
    who.set_moves( 1000 );
    who.do_player_activity();
    REQUIRE( who.is_wearing( itype_hat_hard ) );

    item_location worn_hat;
    for( const item_location &location : who.top_items_loc() ) {
        if( location && location->typeId() == itype_hat_hard ) {
            worn_hat = location;
            break;
        }
    }
    REQUIRE( worn_hat );
    const int moves_before = who.get_moves();
    const npc_ai::equipment_command_result remove_result =
        npc_ai::execute_equipment_action( who, npc_ai::equipment_action::take_off, worn_hat );

    CHECK( remove_result.success );
    CHECK_FALSE( who.is_wearing( itype_hat_hard ) );
    CHECK( owns_item_type( who, itype_hat_hard ) );
    CHECK( who.get_moves() < moves_before );
}

TEST_CASE( "npc_ai_equipment_stores_wielded_weapon_in_existing_storage",
           "[npc_ai][npc_ai_equipment]" )
{
    npc &who = prepare_equipment_npc();
    wear_backpack( who );
    item_location knife = who.i_add( item( itype_knife_hunting, calendar::turn ) );
    REQUIRE( knife );
    REQUIRE( who.wield( knife ) );
    item_location weapon = who.get_wielded_item();
    REQUIRE( weapon );

    const npc_ai::equipment_command_result result =
        npc_ai::execute_equipment_action( who, npc_ai::equipment_action::store, weapon );

    CHECK( result.success );
    CHECK_FALSE( who.get_wielded_item() );
    CHECK( owns_item_type( who, itype_knife_hunting ) );
    CHECK( map_item_of_type( who.pos_bub( get_map() ), itype_knife_hunting ) == nullptr );
}

TEST_CASE( "npc_ai_equipment_remembers_exact_emergency_drop_identity_and_location",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_memory]" )
{
    npc &who = prepare_equipment_npc();
    item_location backpack = wear_backpack( who, true );
    const tripoint_abs_ms expected_location = who.pos_abs();

    const npc_ai::equipment_command_result result = npc_ai::execute_equipment_action(
                who, npc_ai::equipment_action::drop, backpack, "combat_emergency", true );

    REQUIRE( result.success );
    CHECK_FALSE( result.equipment_uid.empty() );
    const std::vector<npc_ai::dropped_equipment_memory> records =
        npc_ai::get_dropped_equipment_memories( who );
    REQUIRE( records.size() == 1 );
    CHECK( records.front().item_uid == result.equipment_uid );
    CHECK( records.front().owner_id == who.getID().get_value() );
    CHECK( records.front().location == expected_location );
    CHECK( records.front().reason == "combat_emergency" );
    CHECK( records.front().retrieval_expected );
    CHECK( records.front().status == npc_ai::equipment_memory_status::dropped );

    item *dropped = map_item_of_type( who.pos_bub( get_map() ), itype_backpack );
    REQUIRE( dropped != nullptr );
    CHECK( dropped->get_var( "npc_ai_equipment_uid" ) == result.equipment_uid );
    CHECK( dropped->get_var( "npc_ai_equipment_owner", -1 ) == who.getID().get_value() );

    npc_ai::clear_runtime_equipment_memory_cache_for_test( who );
    const std::vector<npc_ai::dropped_equipment_memory> reloaded =
        npc_ai::get_dropped_equipment_memories( who );
    REQUIRE( reloaded.size() == 1 );
    CHECK( reloaded.front().item_uid == result.equipment_uid );
    CHECK( reloaded.front().location == expected_location );
    CHECK( reloaded.front().retrieval_expected );
}

TEST_CASE( "npc_ai_equipment_recovers_expected_drop_through_normal_pickup",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_memory]" )
{
    npc &who = prepare_equipment_npc();
    item_location backpack = wear_backpack( who, true );
    const npc_ai::equipment_command_result drop_result = npc_ai::execute_equipment_action(
                who, npc_ai::equipment_action::drop, backpack, "combat_emergency", true );
    REQUIRE( drop_result.success );
    REQUIRE( map_item_of_type( who.pos_bub( get_map() ), itype_backpack ) != nullptr );

    who.regen_ai_cache();
    who.set_moves( 1000 );
    CHECK( who.is_safe() );
    CHECK_FALSE( who.has_player_activity() );
    CHECK( who.sees( get_map(), who.pos_bub( get_map() ) ) );
    CHECK( get_map().could_see_items( who.pos_bub( get_map() ), who ) );
    CHECK( npc_ai::process_equipment_recovery( who ) );
    CHECK( map_item_of_type( who.pos_bub( get_map() ), itype_backpack ) == nullptr );
    CHECK( owns_item_type( who, itype_backpack ) );

    CHECK_FALSE( npc_ai::process_equipment_recovery( who ) );
    const std::vector<npc_ai::dropped_equipment_memory> records =
        npc_ai::get_dropped_equipment_memories( who );
    REQUIRE( records.size() == 1 );
    CHECK( records.front().status == npc_ai::equipment_memory_status::recovered );
    CHECK_FALSE( records.front().retrieval_expected );
}

TEST_CASE( "npc_ai_equipment_respects_ordinary_drop_and_detects_missing_expected_item",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_memory]" )
{
    SECTION( "ordinary player order is not automatically undone" ) {
        npc &who = prepare_equipment_npc();
        item_location backpack = wear_backpack( who );
        REQUIRE( npc_ai::execute_equipment_action( who, npc_ai::equipment_action::drop,
                 backpack ).success );

        who.set_moves( 1000 );
        CHECK_FALSE( npc_ai::process_equipment_recovery( who ) );
        CHECK( map_item_of_type( who.pos_bub( get_map() ), itype_backpack ) != nullptr );
    }

    SECTION( "visible expected item is no longer at its remembered tile" ) {
        npc &who = prepare_equipment_npc();
        item_location backpack = wear_backpack( who );
        REQUIRE( npc_ai::execute_equipment_action( who, npc_ai::equipment_action::drop,
                 backpack, "combat_emergency", true ).success );
        item *dropped = map_item_of_type( who.pos_bub( get_map() ), itype_backpack );
        REQUIRE( dropped != nullptr );
        get_map().i_rem( who.pos_bub( get_map() ), dropped );

        who.regen_ai_cache();
        CHECK_FALSE( npc_ai::process_equipment_recovery( who ) );
        const std::vector<npc_ai::dropped_equipment_memory> records =
            npc_ai::get_dropped_equipment_memories( who );
        REQUIRE( records.size() == 1 );
        CHECK( records.front().status == npc_ai::equipment_memory_status::missing );
        CHECK_FALSE( records.front().retrieval_expected );
    }
}

TEST_CASE( "npc_ai_equipment_explicitly_recovers_and_wears_same_ground_backpack",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_memory]" )
{
    npc &who = prepare_equipment_npc();
    item_location backpack = wear_backpack( who, true );
    const npc_ai::equipment_command_result drop = npc_ai::execute_equipment_action(
                who, npc_ai::equipment_action::drop, backpack, "player_order", false );
    REQUIRE( drop.success );
    REQUIRE_FALSE( drop.equipment_uid.empty() );
    const tripoint_bub_ms dropped_at = who.pos_bub( get_map() );

    who.setpos( get_map(), dropped_at + point( 5, 0 ) );
    const int initial_distance = rl_dist( who.pos_bub(), dropped_at );
    const npc_ai::equipment_command_result recover =
        npc_ai::try_handle_equipment_command( who, "Recoge tu mochila." );
    CAPTURE( recover.message );
    REQUIRE( recover.handled );
    REQUIRE( recover.success );
    CHECK( recover.action == npc_ai::equipment_action::recover );
    CHECK( recover.equipment_uid == drop.equipment_uid );

    who.regen_ai_cache();
    who.set_moves( 2000 );
    REQUIRE( npc_ai::process_equipment_recovery( who ) );
    CHECK( rl_dist( who.pos_bub(), dropped_at ) < initial_distance );
    run_equipment_recovery( who );

    REQUIRE( who.is_wearing( itype_backpack ) );
    item_location recovered = owned_item_with_uid( who, drop.equipment_uid );
    REQUIRE( recovered );
    CHECK( who.is_worn( *recovered ) );
    CHECK( recovered->get_var( "npc_ai_equipment_uid" ) == drop.equipment_uid );
    const std::list<item *> contents = recovered->all_items_top( pocket_type::CONTAINER );
    CHECK( std::any_of( contents.begin(), contents.end(), []( const item *it ) {
        return it != nullptr && it->typeId() == itype_test_rock;
    } ) );
    CHECK( map_item_of_type( dropped_at, itype_backpack ) == nullptr );
}

TEST_CASE( "npc_ai_equipment_wears_owned_carried_backpack_on_command",
           "[npc_ai][npc_ai_equipment]" )
{
    npc &who = prepare_equipment_npc();
    item storage( itype_debug_backpack, calendar::turn );
    REQUIRE( who.worn.wear_item( who, storage, false, false ).has_value() );
    item_location backpack = who.i_add( item( itype_backpack, calendar::turn ), true,
                                        nullptr, nullptr, true, false );
    REQUIRE( backpack );
    REQUIRE( backpack.where_recursive() == item_location::type::character );
    REQUIRE_FALSE( who.is_worn( *backpack ) );

    const npc_ai::equipment_command_result result =
        npc_ai::try_handle_equipment_command( who, "Ponte la mochila." );
    CAPTURE( result.message );
    REQUIRE( result.success );
    who.set_moves( 1000 );
    who.do_player_activity();
    CHECK( who.is_wearing( itype_backpack ) );
}

TEST_CASE( "npc_ai_equipment_explicit_recovery_reports_missing_remembered_backpack",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_memory]" )
{
    npc &who = prepare_equipment_npc();
    item_location backpack = wear_backpack( who );
    REQUIRE( npc_ai::execute_equipment_action( who, npc_ai::equipment_action::drop,
             backpack ).success );
    item *dropped = map_item_of_type( who.pos_bub( get_map() ), itype_backpack );
    REQUIRE( dropped != nullptr );
    get_map().i_rem( who.pos_bub( get_map() ), dropped );

    const npc_ai::equipment_command_result result =
        npc_ai::try_handle_equipment_command( who, "Vuelve a ponerte la mochila." );
    CHECK( result.handled );
    CHECK_FALSE( result.success );
    CHECK( result.message.find( "no longer" ) != std::string::npos );
}

TEST_CASE( "npc_ai_equipment_recovery_refuses_genuinely_ambiguous_backpacks",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_memory]" )
{
    npc &who = prepare_equipment_npc();
    item_location first = wear_backpack( who );
    REQUIRE( npc_ai::execute_equipment_action( who, npc_ai::equipment_action::drop,
             first ).success );
    who.setpos( get_map(), who.pos_bub() + point( 1, 0 ) );
    item_location second = wear_backpack( who );
    REQUIRE( npc_ai::execute_equipment_action( who, npc_ai::equipment_action::drop,
             second ).success );

    const npc_ai::equipment_command_result result =
        npc_ai::try_handle_equipment_command( who, "Recupera la mochila." );
    CHECK( result.handled );
    CHECK_FALSE( result.success );
    CHECK( result.message == _( "I can't determine which item you mean." ) );
}

TEST_CASE( "npc_ai_equipment_surfaces_native_wear_restriction",
           "[npc_ai][npc_ai_equipment]" )
{
    npc &who = prepare_equipment_npc();
    for( int count = 0; count < 2; ++count ) {
        item guard( itype_test_armguard, calendar::turn );
        REQUIRE( who.worn.wear_item( who, guard, false, false ).has_value() );
    }
    item_location third = who.i_add( item( itype_test_armguard, calendar::turn ), true,
                                     nullptr, nullptr, true, false );
    REQUIRE( third );
    REQUIRE_FALSE( who.can_wear( *third ).success() );

    const npc_ai::equipment_command_result result =
        npc_ai::try_handle_equipment_command( who, "Ponte el test_armguard." );
    CHECK( result.handled );
    CHECK_FALSE( result.success );
    CHECK_FALSE( result.message.empty() );
}

TEST_CASE( "npc_ai_equipment_combat_drop_can_be_commanded_back_on_after_danger",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_memory]" )
{
    npc &who = prepare_equipment_npc();
    item_location backpack = wear_backpack( who, true );
    const npc_ai::equipment_command_result drop =
        npc_ai::try_handle_equipment_command( who, "¡Suelta la mochila ahora!" );
    REQUIRE( drop.success );
    REQUIRE_FALSE( drop.equipment_uid.empty() );
    REQUIRE_FALSE( who.is_wearing( itype_backpack ) );

    const npc_ai::equipment_command_result recover =
        npc_ai::try_handle_equipment_command( who, "Busca tu mochila y póntela." );
    REQUIRE( recover.success );
    run_equipment_recovery( who );

    item_location recovered = owned_item_with_uid( who, drop.equipment_uid );
    REQUIRE( recovered );
    CHECK( who.is_worn( *recovered ) );
    CHECK( recovered->all_items_top( pocket_type::CONTAINER ).size() == 1 );
}

TEST_CASE( "npc_ai_equipment_text_order_drops_exact_wielded_axe",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_regression]" )
{
    npc &who = prepare_equipment_npc();
    wear_backpack( who );
    item axe( itype_fire_ax, calendar::turn );
    axe.set_var( "npc_ai_test_identity", "ordered-axe" );
    item_location axe_location = who.i_add( axe );
    REQUIRE( axe_location );
    REQUIRE( who.wield( axe_location ) );
    item_location wielded_before_drop = who.get_wielded_item();
    REQUIRE( wielded_before_drop );
    const int native_drop_cost = wielded_before_drop.obtain_cost(
                                     who, wielded_before_drop->count() );
    const int moves_before = who.get_moves();

    const npc_ai::equipment_command_result result =
        npc_ai::try_handle_equipment_command( who, "Liam, drop your axe." );

    CAPTURE( result.message );
    REQUIRE( result.success );
    CHECK_FALSE( who.get_wielded_item() );
    item *dropped = map_item_of_type( who.pos_bub( get_map() ), itype_fire_ax );
    REQUIRE( dropped != nullptr );
    CHECK( dropped->get_var( "npc_ai_test_identity" ) == "ordered-axe" );
    CHECK( who.get_moves() == moves_before - native_drop_cost );
}

TEST_CASE( "npc_ai_equipment_text_order_drops_empty_worn_backpack",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_regression]" )
{
    npc &who = prepare_equipment_npc();
    item_location backpack = wear_backpack( who );
    backpack->set_var( "npc_ai_test_identity", "empty-backpack" );

    const npc_ai::equipment_command_result result =
        npc_ai::try_handle_equipment_command( who, "Remove your backpack." );

    CAPTURE( result.message );
    REQUIRE( result.success );
    CHECK_FALSE( who.is_wearing( itype_backpack ) );
    item *dropped = map_item_of_type( who.pos_bub( get_map() ), itype_backpack );
    REQUIRE( dropped != nullptr );
    CHECK( dropped->get_var( "npc_ai_test_identity" ) == "empty-backpack" );
}

TEST_CASE( "npc_ai_equipment_text_order_drops_loaded_backpack_without_spilling",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_regression]" )
{
    npc &who = prepare_equipment_npc();
    item_location backpack = wear_backpack( who, true );
    backpack->set_var( "npc_ai_test_identity", "loaded-backpack" );

    const npc_ai::equipment_command_result result =
        npc_ai::try_handle_equipment_command( who, "Liam, suelta la mochila." );

    CAPTURE( result.message );
    REQUIRE( result.success );
    item *dropped = map_item_of_type( who.pos_bub( get_map() ), itype_backpack );
    REQUIRE( dropped != nullptr );
    CHECK( dropped->get_var( "npc_ai_test_identity" ) == "loaded-backpack" );
    const std::list<item *> contents = dropped->all_items_top( pocket_type::CONTAINER );
    REQUIRE( contents.size() == 1 );
    CHECK( contents.front()->typeId() == itype_test_rock );
    CHECK( map_item_of_type( who.pos_bub( get_map() ), itype_test_rock ) == nullptr );
}

TEST_CASE( "npc_ai_equipment_wears_loaded_backpack_directly_from_ground_without_pickup_capacity",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_regression]" )
{
    npc &who = prepare_equipment_npc();
    item backpack( itype_backpack, calendar::turn );
    backpack.set_var( "npc_ai_test_identity", "ground-loaded-backpack" );
    REQUIRE( backpack.put_in( item( itype_test_rock, calendar::turn ),
                              pocket_type::CONTAINER ).success() );
    item &ground_backpack = get_map().add_item( who.pos_bub( get_map() ), backpack );
    REQUIRE_FALSE( ground_backpack.is_null() );
    REQUIRE_FALSE( who.can_take_that( ground_backpack ) );
    REQUIRE( npc_ai::get_dropped_equipment_memories( who ).empty() );

    const npc_ai::equipment_command_result result =
        npc_ai::try_handle_equipment_command( who, "Put on the backpack." );

    CAPTURE( result.message );
    REQUIRE( result.success );
    CHECK( result.action == npc_ai::equipment_action::wear );
    who.set_moves( 2000 );
    who.do_player_activity();
    item_location worn = owned_item_with_test_identity( who, "ground-loaded-backpack" );
    REQUIRE( worn );
    CHECK( who.is_worn( *worn ) );
    CHECK( worn->all_items_top( pocket_type::CONTAINER ).size() == 1 );
    CHECK( map_item_of_type( who.pos_bub( get_map() ), itype_backpack ) == nullptr );
}

TEST_CASE( "npc_ai_equipment_drop_then_explicit_ground_wear_preserves_identity_and_contents",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_regression]" )
{
    npc &who = prepare_equipment_npc();
    item_location backpack = wear_backpack( who, true );
    backpack->set_var( "npc_ai_test_identity", "round-trip-backpack" );

    REQUIRE( npc_ai::try_handle_equipment_command( who, "Suelta la mochila." ).success );
    REQUIRE_FALSE( who.is_wearing( itype_backpack ) );
    const npc_ai::equipment_command_result wear =
        npc_ai::try_handle_equipment_command( who, "Ponte la mochila." );
    CAPTURE( wear.message );
    REQUIRE( wear.success );
    who.set_moves( 2000 );
    who.do_player_activity();

    item_location recovered = owned_item_with_test_identity( who, "round-trip-backpack" );
    REQUIRE( recovered );
    CHECK( who.is_worn( *recovered ) );
    const std::list<item *> contents = recovered->all_items_top( pocket_type::CONTAINER );
    REQUIRE( contents.size() == 1 );
    CHECK( contents.front()->typeId() == itype_test_rock );
}

TEST_CASE( "npc_ai_equipment_dropped_axe_can_be_rewielded_from_ground_without_ollama",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_regression]" )
{
    npc &who = prepare_equipment_npc();
    wear_backpack( who );
    item axe( itype_fire_ax, calendar::turn );
    axe.set_damage( 1000 );
    axe.set_var( "npc_ai_test_identity", "round-trip-axe" );
    item_location axe_location = who.i_add( axe );
    REQUIRE( axe_location );
    REQUIRE( who.wield( axe_location ) );
    REQUIRE( npc_ai::try_handle_equipment_command( who, "Suelta el hacha." ).success );

    const npc_ai::wield_command_result wield =
        npc_ai::try_handle_wield_command( who, "Empuña el hacha." );

    CAPTURE( wield.message );
    REQUIRE( wield.handled );
    REQUIRE_FALSE( wield.pending );
    REQUIRE( wield.success );
    CHECK( wield.message.find( "<color" ) == std::string::npos );
    REQUIRE( who.ai_directed_pickup );
    CHECK( who.ai_directed_pickup_intent == npc_ai::acquisition_intent::wield );
    who.pick_up_item();
    item_location wielded = who.get_wielded_item();
    REQUIRE( wielded );
    CHECK( wielded->get_var( "npc_ai_test_identity" ) == "round-trip-axe" );
    CHECK( map_item_of_type( who.pos_bub( get_map() ), itype_fire_ax ) == nullptr );
}

#if defined( LOCALIZE )
TEST_CASE( "npc_ai_new_wield_and_recovery_messages_have_runtime_spanish_fallbacks",
           "[npc_ai][npc_ai_equipment][npc_ai_language]" )
{
    restore_equipment_test_language restore_language;
    set_language( "es_ES" );

    SECTION( "ordinary wield result" ) {
        npc &who = prepare_equipment_npc();
        wear_backpack( who );
        item_location knife = who.i_add( item( itype_knife_hunting, calendar::turn ) );
        REQUIRE( knife );
        const npc_ai::wield_target_result result = npc_ai::wield_target( who, knife, true );
        REQUIRE( result.success );
        CAPTURE( result.message );
        CHECK( result.message.find( "Estoy empuñando" ) != std::string::npos );
        CHECK( result.message.find( "I'm wielding" ) == std::string::npos );
    }

    SECTION( "unstowable weapon swap result" ) {
        npc &who = prepare_equipment_npc();
        const tripoint_bub_ms position = who.pos_bub( get_map() );
        item &axe = get_map().add_item( position, item( itype_fire_ax, calendar::turn ) );
        REQUIRE( who.wield( item_location( map_cursor( &get_map(), position ), &axe ) ) );
        REQUIRE_FALSE( who.can_stash( *who.get_wielded_item() ) );
        item &sword = get_map().add_item(
                          position, item( itype_sword_sheets_welded_large, calendar::turn ) );
        item_location target( map_cursor( &get_map(), position ), &sword );

        const npc_ai::wield_target_result result = npc_ai::wield_target( who, target, true );
        REQUIRE( result.success );
        REQUIRE( result.drops_previous );
        CHECK( result.message.find( "No pude guardar" ) != std::string::npos );
        CHECK( result.message.find( "I couldn't put away" ) == std::string::npos );
    }

    SECTION( "remembered recovery acknowledgement" ) {
        npc &who = prepare_equipment_npc();
        wear_backpack( who );
        REQUIRE( npc_ai::try_handle_equipment_command( who, "Suelta la mochila." ).success );
        const npc_ai::equipment_command_result result =
            npc_ai::try_handle_equipment_command( who, "Recupera tu mochila." );
        REQUIRE( result.success );
        CHECK( result.message.find( "Recuperaré" ) != std::string::npos );
        CHECK( result.message.find( "I'll recover" ) == std::string::npos );
    }

    CHECK( npc_ai::localized_ai_message(
               _( "I need to put away %s before wielding something from it." ),
               "Necesito guardar %s antes de empuñar algo que contiene." ).find(
                   "I need to put away" ) == std::string::npos );
}
#endif

TEST_CASE( "npc_ai_equipment_explicit_ground_wear_does_not_require_recovery_metadata",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_regression]" )
{
    npc &who = prepare_equipment_npc();
    item backpack( itype_backpack, calendar::turn );
    backpack.set_var( "npc_ai_test_identity", "manually-placed-backpack" );
    get_map().add_item( who.pos_bub( get_map() ), backpack );
    REQUIRE( npc_ai::get_dropped_equipment_memories( who ).empty() );

    const npc_ai::equipment_command_result result =
        npc_ai::try_handle_equipment_command( who, "Recoge y ponte la mochila." );

    CAPTURE( result.message );
    REQUIRE( result.success );
    REQUIRE( result.action == npc_ai::equipment_action::wear );
    who.set_moves( 2000 );
    who.do_player_activity();
    item_location worn = owned_item_with_test_identity( who, "manually-placed-backpack" );
    REQUIRE( worn );
    CHECK( who.is_worn( *worn ) );
}

TEST_CASE( "npc_ai_equipment_backpack_order_never_drops_unrelated_clothing",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_regression]" )
{
    npc &who = prepare_equipment_npc();
    item hat( itype_hat_hard, calendar::turn );
    REQUIRE( who.worn.wear_item( who, hat, false, false ).has_value() );

    const npc_ai::equipment_command_result result =
        npc_ai::try_handle_equipment_command( who, "Suelta la mochila." );

    CHECK( result.handled );
    CHECK_FALSE( result.success );
    CHECK( who.is_wearing( itype_hat_hard ) );
    CHECK( map_item_of_type( who.pos_bub( get_map() ), itype_hat_hard ) == nullptr );
}

TEST_CASE( "npc_ai_group_equipment_orders_resolve_each_npcs_own_weapon",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_memory][npc_ai_phase6]" )
{
    reset_ai_requests reset;
    npc_ai::reset_ai_request_system_for_test();
    npc &liam = prepare_equipment_npc();
    liam.name = "Liam";
    wear_backpack( liam );

    npc &kim = spawn_npc( point_bub_ms{ 68, 60 }, "test_talker" );
    clear_character( kim );
    kim.name = "Kim";
    kim.set_moves( 1000 );
    npc_ai::clear_dropped_equipment_memories_for_test( kim );
    wear_backpack( kim );

    item_location liam_weapon = liam.i_add( item( itype_knife_hunting, calendar::turn ) );
    item_location kim_weapon = kim.i_add( item( itype_fire_ax, calendar::turn ) );
    REQUIRE( liam_weapon );
    REQUIRE( kim_weapon );
    REQUIRE( liam.wield( liam_weapon ) );
    REQUIRE( kim.wield( kim_weapon ) );

    const npc_ai::group_equipment_command_result drop =
        npc_ai::execute_group_equipment_command( { &liam, &kim },
                "Todos suelten sus armas." );
    REQUIRE( drop.handled );
    CHECK( drop.affected.size() == 2 );
    CHECK( drop.failed == 0 );
    CHECK_FALSE( liam.is_armed() );
    CHECK_FALSE( kim.is_armed() );
    CHECK( npc_ai::get_ai_request_queue().pending_count() == 0 );

    const std::vector<npc_ai::dropped_equipment_memory> liam_records =
        npc_ai::get_dropped_equipment_memories( liam );
    const std::vector<npc_ai::dropped_equipment_memory> kim_records =
        npc_ai::get_dropped_equipment_memories( kim );
    REQUIRE( liam_records.size() == 1 );
    REQUIRE( kim_records.size() == 1 );
    CHECK( liam_records.front().owner_id == liam.getID().get_value() );
    CHECK( kim_records.front().owner_id == kim.getID().get_value() );
    CHECK( liam_records.front().item_type == itype_knife_hunting.str() );
    CHECK( kim_records.front().item_type == itype_fire_ax.str() );
    CHECK( liam_records.front().item_uid != kim_records.front().item_uid );

    item *liam_dropped = map_item_of_type( liam.pos_bub( get_map() ), itype_knife_hunting );
    item *kim_dropped = map_item_of_type( kim.pos_bub( get_map() ), itype_fire_ax );
    REQUIRE( liam_dropped != nullptr );
    REQUIRE( kim_dropped != nullptr );
    CHECK( liam_dropped->get_var( "npc_ai_equipment_owner", -1 ) == liam.getID().get_value() );
    CHECK( kim_dropped->get_var( "npc_ai_equipment_owner", -1 ) == kim.getID().get_value() );

    const npc_ai::group_equipment_command_result recover =
        npc_ai::execute_group_equipment_command( { &liam, &kim },
                "Todos recojan sus armas." );
    REQUIRE( recover.handled );
    CHECK( recover.affected.size() == 2 );
    CHECK( recover.failed == 0 );
    CHECK( npc_ai::get_ai_request_queue().pending_count() == 0 );

    run_equipment_recovery( liam );
    run_equipment_recovery( kim );
    item_location recovered_by_liam = owned_item_with_uid( liam, liam_records.front().item_uid );
    item_location recovered_by_kim = owned_item_with_uid( kim, kim_records.front().item_uid );
    REQUIRE( recovered_by_liam );
    REQUIRE( recovered_by_kim );
    CHECK( recovered_by_liam->typeId() == itype_knife_hunting );
    CHECK( recovered_by_kim->typeId() == itype_fire_ax );
    CHECK_FALSE( owned_item_with_uid( liam, kim_records.front().item_uid ) );
    CHECK_FALSE( owned_item_with_uid( kim, liam_records.front().item_uid ) );
}

TEST_CASE( "npc_ai_acquisition_command_classification_is_explicit_ambiguous_and_question_safe",
           "[npc_ai][npc_ai_pickup][npc_ai_acquisition][npc_ai_command_ownership]" )
{
    using intent = npc_ai::acquisition_intent;
    CHECK( npc_ai::classify_acquisition_intent( "empuña tu arma" ).intent == intent::wield );
    CHECK( npc_ai::classify_acquisition_intent( "empuñen sus armas" ).intent == intent::wield );
    CHECK( npc_ai::classify_acquisition_intent( "equipaos el rifle" ).intent == intent::wield );
    CHECK( npc_ai::classify_acquisition_intent( "recoge la botella" ).intent == intent::store );
    CHECK( npc_ai::classify_acquisition_intent( "recojan las botellas" ).intent == intent::store );
    CHECK( npc_ai::classify_acquisition_intent( "recoged las botellas" ).intent == intent::store );
    CHECK( npc_ai::classify_acquisition_intent( "toma tu arma" ).intent == intent::automatic );
    CHECK( npc_ai::classify_acquisition_intent( "tomen sus armas" ).intent == intent::automatic );
    CHECK( npc_ai::classify_acquisition_intent( "tomad sus armas" ).intent == intent::automatic );
    CHECK_FALSE( npc_ai::classify_acquisition_intent(
                     "¿Crees que deberíamos recoger esas armas?" ).command );
    CHECK_FALSE( npc_ai::classify_acquisition_intent(
                     "¿Crees que tomen algo de ahí?" ).command );
}

TEST_CASE( "npc_ai_acquisition_intent_controls_real_destination_without_losing_target",
           "[npc_ai][npc_ai_pickup][npc_ai_wield][npc_ai_acquisition]" )
{
    reset_ai_requests reset;

    SECTION( "explicit wield uses the directed acquisition engine" ) {
        npc &who = prepare_equipment_npc();
        const tripoint_bub_ms position = who.pos_bub( get_map() );
        get_map().add_item( position, item( itype_fire_ax, calendar::turn ) );

        const npc_ai::pickup_command_result request =
            resolve_pickup_command( who, "empuña tu arma" );
        REQUIRE( request.pending );
        REQUIRE( who.ai_directed_pickup );
        CHECK( who.ai_directed_pickup_intent == npc_ai::acquisition_intent::wield );
        finish_directed_pickup( who );

        REQUIRE( who.get_wielded_item() );
        CHECK( who.get_wielded_item()->typeId() == itype_fire_ax );
        CHECK( map_item_count_of_type( position, itype_fire_ax ) == 0 );
        CHECK( owned_item_count_of_type( who, itype_fire_ax ) == 1 );
        CHECK_FALSE( who.has_new_items );
    }

    SECTION( "ambiguous take walks to a resolved weapon and wields that target" ) {
        npc &who = prepare_equipment_npc();
        const tripoint_bub_ms start = who.pos_bub( get_map() );
        const tripoint_bub_ms position = start + point( 3, 0 );
        get_map().add_item( position, item( itype_fire_ax, calendar::turn ) );

        const npc_ai::pickup_command_result request =
            resolve_pickup_command( who, "toma tu arma" );
        REQUIRE( request.pending );
        REQUIRE( who.ai_directed_pickup );
        CHECK( who.ai_directed_pickup_intent == npc_ai::acquisition_intent::wield );
        finish_directed_pickup( who );

        CHECK( who.pos_bub( get_map() ) != start );
        REQUIRE( who.get_wielded_item() );
        CHECK( who.get_wielded_item()->typeId() == itype_fire_ax );
        CHECK( map_item_count_of_type( position, itype_fire_ax ) == 0 );
        CHECK( owned_item_count_of_type( who, itype_fire_ax ) == 1 );
    }

    SECTION( "explicit store keeps an ordinary bottle out of the hands" ) {
        npc &who = prepare_equipment_npc();
        wear_backpack( who );
        const tripoint_bub_ms position = who.pos_bub( get_map() );
        get_map().add_item( position, item( itype_bottle_plastic, calendar::turn ) );

        const npc_ai::pickup_command_result request =
            resolve_pickup_command( who, "recoge la botella" );
        REQUIRE( request.pending );
        CHECK( who.ai_directed_pickup_intent == npc_ai::acquisition_intent::store );
        finish_directed_pickup( who );

        CHECK_FALSE( who.get_wielded_item() );
        CHECK( map_item_count_of_type( position, itype_bottle_plastic ) == 0 );
        CHECK( owned_item_count_of_type( who, itype_bottle_plastic ) == 1 );
    }

    SECTION( "store falls back to wield when the item cannot fit" ) {
        npc &who = prepare_equipment_npc();
        const tripoint_bub_ms position = who.pos_bub( get_map() );
        get_map().add_item( position,
                            item( itype_sword_sheets_welded_large, calendar::turn ) );

        const npc_ai::pickup_command_result request =
            resolve_pickup_command( who, "recoge la espada" );
        REQUIRE( request.pending );
        CHECK( who.ai_directed_pickup_intent == npc_ai::acquisition_intent::store );
        finish_directed_pickup( who );

        REQUIRE( who.get_wielded_item() );
        CHECK( who.get_wielded_item()->typeId() == itype_sword_sheets_welded_large );
        CHECK( map_item_count_of_type( position, itype_sword_sheets_welded_large ) == 0 );
        CHECK( owned_item_count_of_type( who, itype_sword_sheets_welded_large ) == 1 );
    }

    SECTION( "wield intent stores the weapon when injuries prevent wielding" ) {
        npc &who = prepare_equipment_npc();
        wear_backpack( who );
        who.set_part_hp_cur( body_part_arm_l, 0 );
        who.set_part_hp_cur( body_part_arm_r, 0 );
        const tripoint_bub_ms position = who.pos_bub( get_map() );
        get_map().add_item( position, item( itype_fire_ax, calendar::turn ) );

        const npc_ai::pickup_command_result request =
            resolve_pickup_command( who, "toma tu hacha" );
        REQUIRE( request.pending );
        CHECK( who.ai_directed_pickup_intent == npc_ai::acquisition_intent::wield );
        finish_directed_pickup( who );

        CHECK_FALSE( who.get_wielded_item() );
        CHECK( map_item_count_of_type( position, itype_fire_ax ) == 0 );
        CHECK( owned_item_count_of_type( who, itype_fire_ax ) == 1 );
    }

    SECTION( "ambiguous Spanish and English bottle orders remain store intent after resolution" ) {
        for( const std::string line : { std::string( "toma la botella" ),
                                       std::string( "take the bottle" ) } ) {
            npc_ai::reset_ai_request_system_for_test();
            npc &who = prepare_equipment_npc();
            wear_backpack( who );
            const tripoint_bub_ms position = who.pos_bub( get_map() );
            get_map().add_item( position, item( itype_bottle_plastic, calendar::turn ) );

            const npc_ai::pickup_command_result request = resolve_pickup_command( who, line );
            // "take the bottle" names the only visible candidate and resolves
            // deterministically; the Spanish line still needs the resolver.
            REQUIRE( ( request.pending || request.started ) );
            CHECK( who.ai_directed_pickup_intent == npc_ai::acquisition_intent::store );
            finish_directed_pickup( who );
            CHECK_FALSE( who.get_wielded_item() );
            CHECK( owned_item_count_of_type( who, itype_bottle_plastic ) == 1 );
            CHECK( map_item_count_of_type( position, itype_bottle_plastic ) == 0 );
        }
    }

    SECTION( "directed wield is not replaced by a better carried weapon scan" ) {
        npc &who = prepare_equipment_npc();
        item debug_pack( itype_debug_backpack, calendar::turn );
        const std::optional<std::list<item>::iterator> worn =
            who.worn.wear_item( who, debug_pack, false, false );
        REQUIRE( worn.has_value() );
        item better_axe( itype_fire_ax, calendar::turn );
        better_axe.set_var( "npc_ai_test_identity", "better-carried-axe" );
        item_location better = who.i_add( better_axe );
        REQUIRE( better );
        REQUIRE_FALSE( who.get_wielded_item() );

        const tripoint_bub_ms position = who.pos_bub( get_map() );
        item directed_axe( itype_fire_ax, calendar::turn );
        directed_axe.set_damage( 3000 );
        directed_axe.set_var( "npc_ai_test_identity", "directed-target-axe" );
        get_map().add_item( position, directed_axe );
        item *axe = map_item_of_type( position, itype_fire_ax );
        REQUIRE( axe != nullptr );
        CAPTURE( who.weapon_value( *better ), who.weapon_value( *axe ) );
        REQUIRE( who.weapon_value( *better ) > who.weapon_value( *axe ) );

        const npc_ai::pickup_command_result request =
            resolve_pickup_command( who, "toma tu hacha" );
        REQUIRE( request.pending );
        finish_directed_pickup( who );

        REQUIRE( who.get_wielded_item() );
        CHECK( who.get_wielded_item()->typeId() == itype_fire_ax );
        CHECK( who.get_wielded_item()->get_var( "npc_ai_test_identity" ) ==
               "directed-target-axe" );
        CHECK( better );
        CHECK( better->typeId() == itype_fire_ax );
        CHECK( better->get_var( "npc_ai_test_identity" ) == "better-carried-axe" );
        CHECK_FALSE( who.has_new_items );
        CHECK( map_item_count_of_type( position, itype_fire_ax ) == 0 );
        const std::vector<item_location> owned = who.all_items_loc();
        CHECK( std::count_if( owned.begin(), owned.end(), []( const item_location &candidate ) {
            return candidate && candidate->get_var( "npc_ai_test_identity" ) ==
                   "directed-target-axe";
        } ) == 1 );
        CHECK( owned_item_count_of_type( who, itype_fire_ax ) == 2 );
    }
}

TEST_CASE( "npc_ai_group_plural_acquisition_resolves_each_npcs_own_target",
           "[npc_ai][npc_ai_pickup][npc_ai_acquisition][npc_ai_command_ownership]" )
{
    reset_ai_requests reset;
    npc_ai::reset_ai_request_system_for_test();
    npc &liam = prepare_equipment_npc();
    liam.name = "Liam";
    npc &kim = spawn_npc( point_bub_ms{ 75, 60 }, "test_talker" );
    clear_character( kim );
    kim.name = "Kim";
    kim.set_moves( 1000 );

    npc_ai::set_ai_request_executor_for_test( []( const std::string & ) {
        return npc_ai::ai_response{ true, "PICKUP_INDEX=1", "" };
    }, false );
    npc_ai::begin_ai_session();

    const auto run_pending_group = [&]( const std::string &line ) {
        const npc_ai::group_acquisition_command_result group =
            npc_ai::execute_group_acquisition_command( { &liam, &kim }, line );
        REQUIRE( group.handled );
        CHECK( group.affected.size() == 2 );
        CHECK( group.pending == 2 );
        CHECK( group.failed == 0 );
        CHECK( npc_ai::get_ai_request_queue().pending_count() == 2 );
        npc_ai::get_ai_request_queue().start();
        REQUIRE( npc_ai::get_ai_request_queue().wait_until_idle_for_test(
                     std::chrono::seconds( 1 ) ) );
        const std::vector<npc_ai::ai_request_completion> completions =
            npc_ai::get_ai_request_queue().take_completions(
                npc_ai::get_ai_request_queue().ready_completion_count() );
        REQUIRE( completions.size() == 2 );
        for( const npc_ai::ai_request_completion &completion : completions ) {
            CHECK( completion.request.type == npc_ai::ai_request_type::pickup_resolution );
            npc *target = completion.request.npc_id == liam.getID().get_value() ? &liam :
                          completion.request.npc_id == kim.getID().get_value() ? &kim : nullptr;
            REQUIRE( target != nullptr );
            npc_ai::apply_pickup_ai_completion( *target, completion );
        }
        REQUIRE( liam.ai_directed_pickup );
        REQUIRE( kim.ai_directed_pickup );
        finish_directed_pickup( liam );
        finish_directed_pickup( kim );
    };

    SECTION( "ambiguous tomen resolves each associated weapon then wields it" ) {
        item liam_axe( itype_fire_ax, calendar::turn );
        liam_axe.set_var( "npc_ai_equipment_owner", liam.getID().get_value() );
        liam_axe.set_var( "npc_ai_test_identity", "liam-target" );
        get_map().add_item( liam.pos_bub( get_map() ), liam_axe );
        item kim_knife( itype_knife_hunting, calendar::turn );
        kim_knife.set_var( "npc_ai_equipment_owner", kim.getID().get_value() );
        kim_knife.set_var( "npc_ai_test_identity", "kim-target" );
        get_map().add_item( kim.pos_bub( get_map() ), kim_knife );

        run_pending_group( "tomen sus armas" );

        REQUIRE( liam.get_wielded_item() );
        REQUIRE( kim.get_wielded_item() );
        CHECK( liam.get_wielded_item()->get_var( "npc_ai_test_identity" ) == "liam-target" );
        CHECK( kim.get_wielded_item()->get_var( "npc_ai_test_identity" ) == "kim-target" );
    }

    SECTION( "explicit plural wield uses the same acquisition pipeline" ) {
        item liam_axe( itype_fire_ax, calendar::turn );
        liam_axe.set_var( "npc_ai_equipment_owner", liam.getID().get_value() );
        liam_axe.set_var( "npc_ai_test_identity", "liam-wield-target" );
        get_map().add_item( liam.pos_bub( get_map() ), liam_axe );
        item kim_knife( itype_knife_hunting, calendar::turn );
        kim_knife.set_var( "npc_ai_equipment_owner", kim.getID().get_value() );
        kim_knife.set_var( "npc_ai_test_identity", "kim-wield-target" );
        get_map().add_item( kim.pos_bub( get_map() ), kim_knife );

        run_pending_group( "empuñen sus armas" );

        REQUIRE( liam.get_wielded_item() );
        REQUIRE( kim.get_wielded_item() );
        CHECK( liam.get_wielded_item()->get_var( "npc_ai_test_identity" ) ==
               "liam-wield-target" );
        CHECK( kim.get_wielded_item()->get_var( "npc_ai_test_identity" ) ==
               "kim-wield-target" );
    }

    SECTION( "explicit plural store keeps each bottle in storage" ) {
        wear_backpack( liam );
        wear_backpack( kim );
        item liam_bottle( itype_bottle_plastic, calendar::turn );
        liam_bottle.set_var( "npc_ai_equipment_owner", liam.getID().get_value() );
        liam_bottle.set_var( "npc_ai_test_identity", "liam-bottle" );
        get_map().add_item( liam.pos_bub( get_map() ), liam_bottle );
        item kim_bottle( itype_bottle_plastic, calendar::turn );
        kim_bottle.set_var( "npc_ai_equipment_owner", kim.getID().get_value() );
        kim_bottle.set_var( "npc_ai_test_identity", "kim-bottle" );
        get_map().add_item( kim.pos_bub( get_map() ), kim_bottle );

        run_pending_group( "recojan las botellas" );

        CHECK_FALSE( liam.get_wielded_item() );
        CHECK_FALSE( kim.get_wielded_item() );
        CHECK( owned_item_with_test_identity( liam, "liam-bottle" ) );
        CHECK( owned_item_with_test_identity( kim, "kim-bottle" ) );
    }
}

TEST_CASE( "npc_ai_acquisition_command_owns_pending_and_rejected_player_lines",
           "[npc_ai][npc_ai_pickup][npc_ai_command_ownership]" )
{
    reset_ai_requests reset;

    SECTION( "pending command enqueues only target resolution and confirms its real result" ) {
        npc_ai::reset_ai_request_system_for_test();
        npc &who = prepare_equipment_npc();
        get_map().add_item( who.pos_bub( get_map() ), item( itype_fire_ax, calendar::turn ) );
        npc_ai::set_ai_request_executor_for_test( []( const std::string & ) {
            return npc_ai::ai_response{ true, "PICKUP_INDEX=1", "" };
        }, false );
        npc_ai::begin_ai_session();

        const npc_ai::pickup_command_result command =
            npc_ai::try_handle_pickup_command( who, "toma tu hacha" );
        REQUIRE( command.handled );
        REQUIRE( command.pending );
        CHECK( npc_ai::get_ai_request_queue().pending_count() == 1 );
        npc_ai::get_ai_request_queue().start();
        REQUIRE( npc_ai::get_ai_request_queue().wait_until_idle_for_test(
                     std::chrono::seconds( 1 ) ) );
        const std::vector<npc_ai::ai_request_completion> completions =
            npc_ai::get_ai_request_queue().take_completions(
                npc_ai::get_ai_request_queue().ready_completion_count() );
        REQUIRE( completions.size() == 1 );
        CHECK( completions.front().request.type == npc_ai::ai_request_type::pickup_resolution );
        CHECK( completions.front().request.player_line == "toma tu hacha" );

        npc_ai::apply_pickup_ai_completion( who, completions.front() );
        finish_directed_pickup( who );
        REQUIRE( who.get_wielded_item() );
        CHECK( who.get_wielded_item()->typeId() == itype_fire_ax );
    }

    SECTION( "rejected command remains owned and creates no dialogue request" ) {
        npc_ai::reset_ai_request_system_for_test();
        npc &who = prepare_equipment_npc();
        npc_ai::set_ai_request_executor_for_test( []( const std::string & ) {
            return npc_ai::ai_response{ true, "PICKUP_INDEX=999", "" };
        }, false );
        npc_ai::begin_ai_session();

        const npc_ai::pickup_command_result command =
            npc_ai::try_handle_pickup_command( who, "toma tu hacha" );
        REQUIRE( command.handled );
        REQUIRE( command.pending );
        CHECK_FALSE( command.started );
        CHECK( npc_ai::get_ai_request_queue().pending_count() == 1 );
        npc_ai::get_ai_request_queue().start();
        REQUIRE( npc_ai::get_ai_request_queue().wait_until_idle_for_test(
                     std::chrono::seconds( 1 ) ) );
        const std::vector<npc_ai::ai_request_completion> completions =
            npc_ai::get_ai_request_queue().take_completions(
                npc_ai::get_ai_request_queue().ready_completion_count() );
        REQUIRE( completions.size() == 1 );
        CHECK( completions.front().request.type == npc_ai::ai_request_type::pickup_resolution );

        who.setpos( get_map(), tripoint_bub_ms{ 61, 60, 0 } );
        Messages::clear_messages();
        sounds::reset_sounds();
        npc_ai::apply_pickup_ai_completion( who, completions.front() );
        CHECK_FALSE( who.ai_directed_pickup );
        CHECK_FALSE( who.get_wielded_item() );
        const auto messages = Messages::recent_messages( 0 );
        CHECK( std::any_of( messages.begin(), messages.end(), [&]( const auto &message ) {
            return message.second.find( who.get_name() ) != std::string::npos &&
                   ( message.second.find( "safely" ) != std::string::npos ||
                     message.second.find( "seguridad" ) != std::string::npos );
        } ) );
        CHECK( npc_ai::get_ai_request_queue().pending_count() == 0 );
    }
}

TEST_CASE( "npc_ai_directed_pickup_wields_item_that_cannot_be_stashed",
           "[npc_ai][npc_ai_pickup][npc_ai_equipment_regression]" )
{
    npc &who = prepare_equipment_npc();
    const tripoint_bub_ms position = who.pos_bub( get_map() );
    get_map().add_item( position, item( itype_sword_sheets_welded_large, calendar::turn ) );
    item *sword = map_item_of_type( position, itype_sword_sheets_welded_large );
    REQUIRE( sword != nullptr );
    item_location target( map_cursor( &get_map(), position ), sword );
    REQUIRE_FALSE( who.can_take_that( *target ) );
    REQUIRE( who.can_wield( *target ).success() );

    std::string error;
    REQUIRE( who.ai_request_pickup( target, position, error ) );
    who.pick_up_item();

    item_location wielded = who.get_wielded_item();
    REQUIRE( wielded );
    CHECK( wielded->typeId() == itype_sword_sheets_welded_large );
    CHECK( map_item_of_type( position, itype_sword_sheets_welded_large ) == nullptr );
}

TEST_CASE( "npc_ai_directed_pickup_sets_down_unstowable_previous_weapon",
           "[npc_ai][npc_ai_pickup][npc_ai_wield][npc_ai_equipment_regression]" )
{
    npc &who = prepare_equipment_npc();
    const tripoint_bub_ms position = who.pos_bub( get_map() );
    get_map().add_item( position, item( itype_fire_ax, calendar::turn ) );
    item *axe = map_item_of_type( position, itype_fire_ax );
    REQUIRE( axe != nullptr );
    REQUIRE( who.wield( item_location( map_cursor( &get_map(), position ), axe ) ) );
    REQUIRE_FALSE( who.can_stash( *who.get_wielded_item() ) );

    get_map().add_item( position, item( itype_sword_sheets_welded_large, calendar::turn ) );
    item *sword = map_item_of_type( position, itype_sword_sheets_welded_large );
    REQUIRE( sword != nullptr );
    item_location target( map_cursor( &get_map(), position ), sword );
    REQUIRE_FALSE( who.can_take_that( *target ) );

    std::string error;
    REQUIRE( who.ai_request_pickup( target, position, error ) );
    who.pick_up_item();

    item_location wielded = who.get_wielded_item();
    REQUIRE( wielded );
    CHECK( wielded->typeId() == itype_sword_sheets_welded_large );
    CHECK( map_item_of_type( position, itype_fire_ax ) != nullptr );
}

TEST_CASE( "npc_ai_impossible_pickup_reply_is_audible_immediately",
           "[npc_ai][npc_ai_pickup][npc_ai_messages][npc_ai_equipment_regression]" )
{
    npc &who = prepare_equipment_npc();
    who.setpos( get_map(), tripoint_bub_ms{ 61, 60, 0 } );
    who.set_part_hp_cur( body_part_arm_l, 0 );
    who.set_part_hp_cur( body_part_arm_r, 0 );
    const tripoint_bub_ms position = who.pos_bub( get_map() );
    item sword( itype_sword_sheets_welded_large, calendar::turn );
    sword.set_var( "npc_ai_async_target_uid", "audible-impossible-sword" );
    get_map().add_item( position, sword );
    item *ground_sword = map_item_of_type( position, itype_sword_sheets_welded_large );
    REQUIRE( ground_sword != nullptr );
    REQUIRE_FALSE( who.can_take_that( *ground_sword ) );
    REQUIRE_FALSE( who.can_wield( *ground_sword ).success() );

    const tripoint_abs_ms absolute = get_map().get_abs( position );
    npc_ai::ai_request_completion completion;
    completion.response = npc_ai::ai_response{ true, "PICKUP_INDEX=1", "" };
    completion.request.player_line = "recoge la espada";
    completion.request.targets.push_back( { "audible-impossible-sword",
            itype_sword_sheets_welded_large.str(), ground_sword->tname(),
            absolute.x(), absolute.y(), absolute.z() } );

    sounds::reset_sounds();
    Messages::clear_messages();
    npc_ai::apply_pickup_ai_completion( who, completion );

    const auto messages = Messages::recent_messages( 0 );
    REQUIRE( std::any_of( messages.begin(), messages.end(), [&]( const auto &message ) {
        const std::string &text = message.second;
        const bool concrete_reason = text.find( "arm" ) != std::string::npos ||
                                     text.find( "brazo" ) != std::string::npos ||
                                     text.find( "mano" ) != std::string::npos;
        return text.find( who.get_name() ) != std::string::npos &&
               text.find( ground_sword->tname() ) != std::string::npos && concrete_reason;
    } ) );
    CHECK( map_item_of_type( position, itype_sword_sheets_welded_large ) != nullptr );
    CHECK_FALSE( who.is_armed() );
}

TEST_CASE( "npc_ai_directed_pickup_prevalidation_preserves_unstorable_unwieldable_target",
           "[npc_ai][npc_ai_pickup][npc_ai_wield][npc_ai_equipment_regression]" )
{
    npc &who = prepare_equipment_npc();
    who.set_part_hp_cur( body_part_arm_l, 0 );
    who.set_part_hp_cur( body_part_arm_r, 0 );
    const tripoint_bub_ms position = who.pos_bub( get_map() );
    item damaged_sword( itype_sword_sheets_welded_large, calendar::turn );
    damaged_sword.set_damage( 1000 );
    damaged_sword.set_var( "npc_ai_test_identity", "impossible-directed-target" );
    get_map().add_item( position, damaged_sword );
    item *ground_sword = map_item_of_type( position, itype_sword_sheets_welded_large );
    REQUIRE( ground_sword != nullptr );
    item_location target( map_cursor( &get_map(), position ), ground_sword );
    REQUIRE_FALSE( who.can_take_that( *target ) );

    const npc_ai::wield_target_result validation =
        npc_ai::validate_wield_target( who, target, true );
    REQUIRE_FALSE( validation.success );
    CAPTURE( validation.message );
    CHECK( validation.message.find( target->tname() ) != std::string::npos );

    std::string error;
    CHECK_FALSE( who.ai_request_pickup( target, position, error, true,
                                       "recoge la espada danada" ) );

    REQUIRE( target );
    CHECK( target.get_item() == ground_sword );
    CHECK( map_item_count_of_type( position, itype_sword_sheets_welded_large ) == 1 );
    CHECK( owned_item_count_of_type( who, itype_sword_sheets_welded_large ) == 0 );
    CHECK( target->damage() == 1000 );
    CHECK( target->get_var( "npc_ai_test_identity" ) == "impossible-directed-target" );
    CHECK( target->get_var( "npc_ai_directed_transfer_uid" ).empty() );
}

TEST_CASE( "npc_ai_spanish_pickup_name_selects_the_model_chosen_real_candidate",
           "[npc_ai][npc_ai_pickup][npc_ai_async][npc_ai_equipment_regression]" )
{
    using namespace std::chrono_literals;
    reset_ai_requests reset;
    npc_ai::reset_ai_request_system_for_test();
    npc &who = prepare_equipment_npc();
    who.setpos( get_map(), tripoint_bub_ms{ 61, 60, 0 } );
    // clear_avatar() runs after clear_map() in prepare_equipment_npc() and can
    // spill the previous test's inventory back onto the map.  Remove that
    // residue before constructing the exact candidate set for this test.
    clear_items( 0 );
    const tripoint_bub_ms position = who.pos_bub( get_map() );
    get_map().add_item( position, item( itype_twig, calendar::turn ) );
    get_map().add_item( position, item( itype_sword_sheets_welded_large, calendar::turn ) );

    std::string resolver_prompt;
    int resolver_index = 0;
    npc_ai::set_ai_request_executor_for_test( [&]( const std::string &prompt ) {
        resolver_prompt = prompt;
        for( int index = 1; index <= 30; ++index ) {
            const std::string candidate_line = "\n" + std::to_string( index ) +
                                               " | id=sword_sheets_welded_large |";
            if( prompt.find( candidate_line ) != std::string::npos ) {
                resolver_index = index;
                return npc_ai::ai_response{ true,
                                           "PICKUP_INDEX=" + std::to_string( index ), "" };
            }
        }
        return npc_ai::ai_response{ true, "PICKUP_INDEX=0", "" };
    }, false );
    npc_ai::begin_ai_session();

    const npc_ai::pickup_command_result request =
        npc_ai::try_handle_pickup_command( who, "recoge la espada" );
    REQUIRE( request.handled );
    REQUIRE( request.pending );
    sounds::reset_sounds();
    npc_ai::get_ai_request_queue().start();
    REQUIRE( npc_ai::get_ai_request_queue().wait_until_idle_for_test( 1s ) );
    INFO( resolver_prompt );
    REQUIRE( resolver_index > 0 );
    npc_ai::process_ai_completions();
    who.pick_up_item();

    CHECK( resolver_prompt.find( "recoge la espada" ) != std::string::npos );
    CHECK( resolver_prompt.find( "id=twig" ) != std::string::npos );
    CHECK( resolver_prompt.find( "id=sword_sheets_welded_large" ) != std::string::npos );
    item_location wielded = who.get_wielded_item();
    REQUIRE( wielded );
    CHECK( wielded->typeId() == itype_sword_sheets_welded_large );
    CHECK( map_item_of_type( position, itype_twig ) != nullptr );
}

TEST_CASE( "npc_ai_pickup_prioritizes_requested_weapon_before_repeated_junk_limit",
           "[npc_ai][npc_ai_pickup][npc_ai_equipment_regression]" )
{
    using namespace std::chrono_literals;
    reset_ai_requests reset;
    npc_ai::reset_ai_request_system_for_test();
    npc &who = prepare_equipment_npc();
    clear_items( 0 );
    const tripoint_bub_ms junk_position = who.pos_bub( get_map() );
    const tripoint_bub_ms axe_position = junk_position + point::east;
    for( int count = 0; count < 35; ++count ) {
        get_map().add_item( junk_position, item( itype_glass_shard, calendar::turn ) );
    }
    item owned_axe( itype_fire_ax, calendar::turn );
    owned_axe.set_var( "npc_ai_equipment_owner", who.getID().get_value() );
    get_map().add_item( axe_position, owned_axe );

    std::string resolver_prompt;
    npc_ai::set_ai_request_executor_for_test( [&]( const std::string &prompt ) {
        resolver_prompt = prompt;
        return npc_ai::ai_response{ true, "PICKUP_INDEX=1", "" };
    }, false );
    npc_ai::begin_ai_session();

    const npc_ai::pickup_command_result request =
        npc_ai::try_handle_pickup_command( who, "recoge tu hacha" );
    REQUIRE( request.handled );
    REQUIRE( request.pending );
    npc_ai::get_ai_request_queue().start();
    REQUIRE( npc_ai::get_ai_request_queue().wait_until_idle_for_test( 1s ) );

    INFO( resolver_prompt );
    CHECK( resolver_prompt.find( "id=fire_ax" ) != std::string::npos );
    CHECK( substring_count( resolver_prompt, "id=glass_shard" ) == 1 );
    CHECK( resolver_prompt.find( "<color" ) == std::string::npos );
    CHECK( map_item_count_of_type( junk_position, itype_glass_shard ) == 35 );
    CHECK( map_item_count_of_type( axe_position, itype_fire_ax ) == 1 );
}

TEST_CASE( "npc_ai_pickup_uses_clean_names_but_preserves_structured_item_state",
           "[npc_ai][npc_ai_pickup][npc_ai_equipment_regression]" )
{
    using namespace std::chrono_literals;
    reset_ai_requests reset;
    npc_ai::reset_ai_request_system_for_test();
    npc &who = prepare_equipment_npc();
    clear_items( 0 );
    const tripoint_bub_ms position = who.pos_bub( get_map() );

    item pristine( itype_fire_ax, calendar::turn );
    pristine.charges = 7;
    item damaged( itype_fire_ax, calendar::turn );
    damaged.set_damage( 1000 );
    damaged.charges = 11;
    get_map().add_item( position, pristine );
    get_map().add_item( position, damaged );

    std::string resolver_prompt;
    npc_ai::set_ai_request_executor_for_test( [&]( const std::string &prompt ) {
        resolver_prompt = prompt;
        return npc_ai::ai_response{ true, "PICKUP_INDEX=0", "" };
    }, false );
    npc_ai::begin_ai_session();

    const npc_ai::pickup_command_result request =
        npc_ai::try_handle_pickup_command( who, "recoge el hacha menos dañada" );
    REQUIRE( request.handled );
    REQUIRE( request.pending );
    npc_ai::get_ai_request_queue().start();
    REQUIRE( npc_ai::get_ai_request_queue().wait_until_idle_for_test( 1s ) );

    INFO( resolver_prompt );
    CHECK( resolver_prompt.find( "<color" ) == std::string::npos );
    CHECK( substring_count( resolver_prompt, "id=fire_ax" ) == 2 );
    CHECK( resolver_prompt.find( "damage=0" ) != std::string::npos );
    CHECK( resolver_prompt.find( "damage=1000" ) != std::string::npos );
    CHECK( resolver_prompt.find( "charges=7" ) != std::string::npos );
    CHECK( resolver_prompt.find( "charges=11" ) != std::string::npos );
    CHECK( resolver_prompt.find( "posicion=" ) != std::string::npos );
    CHECK( map_item_count_of_type( position, itype_fire_ax ) == 2 );
}

TEST_CASE( "npc_ai_directed_pickup_never_equips_an_unrelated_inventory_item",
           "[npc_ai][npc_ai_pickup][npc_ai_wield][npc_ai_equipment_regression]" )
{
    npc &who = prepare_equipment_npc();
    wear_backpack( who );
    item_location unrelated = who.i_add( item( itype_knife_hunting, calendar::turn ) );
    REQUIRE( unrelated );
    REQUIRE_FALSE( who.get_wielded_item() );
    who.set_part_hp_cur( body_part_arm_l, 0 );

    const tripoint_bub_ms position = who.pos_bub( get_map() );
    item damaged_axe( itype_fire_ax, calendar::turn );
    damaged_axe.set_damage( 1000 );
    damaged_axe.set_var( "npc_ai_test_identity", "stored-directed-target" );
    get_map().add_item( position, damaged_axe );
    item *axe = map_item_of_type( position, itype_fire_ax );
    REQUIRE( axe != nullptr );
    item_location target( map_cursor( &get_map(), position ), axe );
    REQUIRE( who.can_take_that( *target ) );
    REQUIRE_FALSE( who.can_wield( *target ).success() );

    std::string error;
    REQUIRE( who.ai_request_pickup( target, position, error ) );
    who.pick_up_item();

    const item_location wielded = who.get_wielded_item();
    CHECK_FALSE( wielded );
    CHECK( owns_item_type( who, itype_fire_ax ) );
    CHECK( map_item_count_of_type( position, itype_fire_ax ) == 0 );
    CHECK( owned_item_count_of_type( who, itype_fire_ax ) == 1 );
    item_location stored = owned_item_with_test_identity( who, "stored-directed-target" );
    REQUIRE( stored );
    CHECK( stored->typeId() == itype_fire_ax );
    CHECK( stored->damage() == 1000 );
    CHECK( stored->get_var( "npc_ai_directed_transfer_uid" ).empty() );
    CHECK( unrelated->typeId() == itype_knife_hunting );
}

TEST_CASE( "npc_ai_directed_pickup_preserves_identity_across_two_wield_cycles",
           "[npc_ai][npc_ai_pickup][npc_ai_wield][npc_ai_equipment_regression]" )
{
    npc &who = prepare_equipment_npc();
    const tripoint_bub_ms position = who.pos_bub( get_map() );
    item sword( itype_sword_sheets_welded_large, calendar::turn );
    sword.set_damage( 1000 );
    sword.set_var( "npc_ai_test_identity", "two-cycle-directed-target" );
    get_map().add_item( position, sword );

    for( int cycle = 0; cycle < 2; ++cycle ) {
        item *ground_sword = map_item_of_type( position, itype_sword_sheets_welded_large );
        REQUIRE( ground_sword != nullptr );
        item_location target( map_cursor( &get_map(), position ), ground_sword );
        REQUIRE_FALSE( who.can_take_that( *target ) );
        REQUIRE( npc_ai::validate_wield_target( who, target, true ).success );

        std::string error;
        REQUIRE( who.ai_request_pickup( target, position, error, true,
                                        "recoge otra vez la espada" ) );
        who.set_moves( 2000 );
        who.pick_up_item();

        item_location wielded = who.get_wielded_item();
        REQUIRE( wielded );
        CHECK( wielded->typeId() == itype_sword_sheets_welded_large );
        CHECK( wielded->damage() == 1000 );
        CHECK( wielded->get_var( "npc_ai_test_identity" ) == "two-cycle-directed-target" );
        CHECK( map_item_count_of_type( position, itype_sword_sheets_welded_large ) == 0 );
        CHECK( owned_item_count_of_type( who, itype_sword_sheets_welded_large ) == 1 );

        if( cycle == 0 ) {
            const drop_locations to_drop = { { wielded, wielded->count() } };
            who.set_moves( 2000 );
            who.drop( to_drop, position, false );
            CHECK_FALSE( who.get_wielded_item() );
            CHECK( map_item_count_of_type( position, itype_sword_sheets_welded_large ) == 1 );
            CHECK( owned_item_count_of_type( who, itype_sword_sheets_welded_large ) == 0 );
        }
    }
}

TEST_CASE( "npc_ai_group_recovery_wields_large_weapon_without_storage",
           "[npc_ai][npc_ai_pickup][npc_ai_equipment_memory][npc_ai_phase6]" )
{
    npc &who = prepare_equipment_npc();
    const tripoint_bub_ms position = who.pos_bub( get_map() );
    get_map().add_item( position, item( itype_fire_ax, calendar::turn ) );
    item *axe = map_item_of_type( position, itype_fire_ax );
    REQUIRE( axe != nullptr );
    REQUIRE( who.wield( item_location( map_cursor( &get_map(), position ), axe ) ) );
    REQUIRE( npc_ai::try_handle_equipment_command( who, "Suelta el hacha." ).success );
    axe = map_item_of_type( position, itype_fire_ax );
    REQUIRE( axe != nullptr );
    REQUIRE_FALSE( who.can_take_that( *axe ) );

    const npc_ai::group_equipment_command_result recover =
        npc_ai::execute_group_equipment_command( { &who }, "Todos recojan sus hachas." );
    REQUIRE( recover.handled );
    REQUIRE( recover.affected.size() == 1 );
    CHECK( recover.failed == 0 );
    run_equipment_recovery( who );

    item_location wielded = who.get_wielded_item();
    REQUIRE( wielded );
    CHECK( wielded->typeId() == itype_fire_ax );
    const auto records = npc_ai::get_dropped_equipment_memories( who );
    REQUIRE( records.size() == 1 );
    CHECK( records.front().status == npc_ai::equipment_memory_status::recovered );
}

TEST_CASE( "npc_ai_recovery_without_matching_memory_falls_through_to_general_pickup",
           "[npc_ai][npc_ai_pickup][npc_ai_equipment]" )
{
    npc &who = prepare_equipment_npc();
    const npc_ai::equipment_command_result result =
        npc_ai::try_handle_equipment_command( who, "Recoge tu espada." );
    CHECK_FALSE( result.handled );
}

TEST_CASE( "npc_ai_pickup_unique_name_match_skips_the_model",
           "[npc_ai][npc_ai_pickup][npc_ai_orders]" )
{
    using namespace std::chrono_literals;
    reset_ai_requests reset;
    npc_ai::reset_ai_request_system_for_test();
    npc &who = prepare_equipment_npc();
    clear_items( 0 );
    const tripoint_bub_ms position = who.pos_bub( get_map() ) + point::east;

    bool resolver_called = false;
    npc_ai::set_ai_request_executor_for_test( [&]( const std::string & ) {
        resolver_called = true;
        return npc_ai::ai_response{ false, "", "resolver must not run" };
    }, false );
    npc_ai::begin_ai_session();

    SECTION( "one visible item whose name is in the order starts at once" ) {
        get_map().add_item( position, item( itype_fire_ax, calendar::turn ) );
        get_map().add_item( position, item( itype_bottle_plastic, calendar::turn ) );

        // "toma" is the ambiguous verb: the weapon target resolves it to wield.
        const npc_ai::pickup_command_result request =
            npc_ai::try_handle_pickup_command( who, "Toma el fire axe." );
        REQUIRE( request.handled );
        CHECK_FALSE( request.pending );
        CHECK( request.started );
        CHECK_FALSE( resolver_called );
        CHECK( npc_ai::get_ai_request_queue().pending_count() == 0 );
        CHECK( who.ai_directed_pickup );
        CHECK( who.ai_directed_pickup_intent == npc_ai::acquisition_intent::wield );
        finish_directed_pickup( who );
        REQUIRE( who.get_wielded_item() );
        CHECK( who.get_wielded_item()->typeId() == itype_fire_ax );
        CHECK( map_item_count_of_type( position, itype_bottle_plastic ) == 1 );
    }

    SECTION( "two items matching the same words still go to the resolver" ) {
        get_map().add_item( position, item( itype_fire_ax, calendar::turn ) );
        get_map().add_item( position + point::east, item( itype_fire_ax, calendar::turn ) );

        const npc_ai::pickup_command_result request =
            npc_ai::try_handle_pickup_command( who, "Recoge el fire axe." );
        REQUIRE( request.handled );
        CHECK( request.pending );
        CHECK_FALSE( request.started );
        CHECK( npc_ai::get_ai_request_queue().pending_count() == 1 );
    }

    SECTION( "no name match at all still goes to the resolver" ) {
        get_map().add_item( position, item( itype_fire_ax, calendar::turn ) );

        const npc_ai::pickup_command_result request =
            npc_ai::try_handle_pickup_command( who, "Recoge la linterna." );
        REQUIRE( request.handled );
        CHECK( request.pending );
        CHECK_FALSE( request.started );
    }
}

TEST_CASE( "npc_ai_equipment_drop_matches_partial_and_accented_item_names",
           "[npc_ai][npc_ai_equipment][npc_ai_orders]" )
{
    static const itype_id itype_sponge( "sponge" );
    npc &who = prepare_equipment_npc();
    wear_backpack( who );
    const item_location sponge = who.i_add( item( itype_sponge, calendar::turn ) );
    const item_location bottle = who.i_add( item( itype_bottle_plastic, calendar::turn ) );
    REQUIRE( sponge );
    REQUIRE( bottle );
    const tripoint_bub_ms position = who.pos_bub( get_map() );

    SECTION( "a word of a multi-word name is enough when it is unambiguous" ) {
        // The order only says "bottle"; the item is a "plastic bottle".
        const npc_ai::equipment_command_result result =
            npc_ai::try_handle_equipment_command( who, "Drop the bottle." );
        REQUIRE( result.handled );
        CHECK( result.success );
        CHECK( map_item_count_of_type( position, itype_bottle_plastic ) == 1 );
        CHECK( owned_item_count_of_type( who, itype_sponge ) == 1 );
    }

    SECTION( "uppercase, accents and trailing filler do not matter" ) {
        const npc_ai::equipment_command_result result =
            npc_ai::try_handle_equipment_command( who, "Suelta la SPÓNGE que tienes, ahora." );
        REQUIRE( result.handled );
        CHECK( result.success );
        CHECK( map_item_count_of_type( position, itype_sponge ) == 1 );
        CHECK( owned_item_count_of_type( who, itype_bottle_plastic ) == 1 );
    }

    SECTION( "a word shared by several carried items stays ambiguous" ) {
        who.i_add( item( itype_id( "bottle_glass" ), calendar::turn ) );
        const npc_ai::equipment_command_result result =
            npc_ai::try_handle_equipment_command( who, "Drop the bottle." );
        REQUIRE( result.handled );
        CHECK_FALSE( result.success );
        CHECK( result.message.find( "determin" ) != std::string::npos );
        CHECK( map_item_count_of_type( position, itype_bottle_plastic ) == 0 );
    }

    SECTION( "a word that matches nothing carried is refused, not guessed" ) {
        const npc_ai::equipment_command_result result =
            npc_ai::try_handle_equipment_command( who, "Suelta la linterna." );
        REQUIRE( result.handled );
        CHECK_FALSE( result.success );
        CHECK( owned_item_count_of_type( who, itype_sponge ) == 1 );
        CHECK( owned_item_count_of_type( who, itype_bottle_plastic ) == 1 );
    }
}

TEST_CASE( "npc_ai_pickup_name_match_ignores_case_and_accents",
           "[npc_ai][npc_ai_pickup][npc_ai_orders]" )
{
    using namespace std::chrono_literals;
    reset_ai_requests reset;
    npc_ai::reset_ai_request_system_for_test();
    npc &who = prepare_equipment_npc();
    clear_items( 0 );
    const tripoint_bub_ms position = who.pos_bub( get_map() ) + point::east;
    get_map().add_item( position, item( itype_fire_ax, calendar::turn ) );
    get_map().add_item( position, item( itype_bottle_plastic, calendar::turn ) );

    bool resolver_called = false;
    npc_ai::set_ai_request_executor_for_test( [&]( const std::string & ) {
        resolver_called = true;
        return npc_ai::ai_response{ false, "", "resolver must not run" };
    }, false );
    npc_ai::begin_ai_session();

    // Accented capitals and a trailing filler still name exactly one item.
    const npc_ai::pickup_command_result request =
        npc_ai::try_handle_pickup_command( who, "Toma el FÍRE ÁXE, por favor." );
    REQUIRE( request.handled );
    CHECK( request.started );
    CHECK_FALSE( request.pending );
    CHECK_FALSE( resolver_called );
    finish_directed_pickup( who );
    CHECK( owned_item_count_of_type( who, itype_fire_ax ) == 1 );
    CHECK( map_item_count_of_type( position, itype_bottle_plastic ) == 1 );
}

TEST_CASE( "npc_ai_equipment_recovery_narrows_by_item_word_inside_a_category",
           "[npc_ai][npc_ai_equipment][npc_ai_equipment_memory][npc_ai_orders]" )
{
    npc &who = prepare_equipment_npc();
    // Two remembered weapons dropped in an emergency: an axe and a knife.
    item_location axe = who.i_add( item( itype_fire_ax, calendar::turn ) );
    REQUIRE( axe );
    REQUIRE( npc_ai::execute_equipment_action( who, npc_ai::equipment_action::drop, axe,
             "combat_emergency", true ).success );
    who.setpos( get_map(), who.pos_bub() + point( 1, 0 ) );
    item_location knife = who.i_add( item( itype_knife_hunting, calendar::turn ) );
    REQUIRE( knife );
    REQUIRE( npc_ai::execute_equipment_action( who, npc_ai::equipment_action::drop, knife,
             "combat_emergency", true ).success );
    REQUIRE( npc_ai::get_dropped_equipment_memories( who ).size() == 2 );

    // The category alone ("weapon") is ambiguous...
    const npc_ai::equipment_command_result ambiguous =
        npc_ai::try_handle_equipment_command( who, "Recover your weapon." );
    CHECK( ambiguous.handled );
    CHECK_FALSE( ambiguous.success );

    // ...but one word of the item's name settles it, accents and case aside.
    const npc_ai::equipment_command_result result =
        npc_ai::try_handle_equipment_command( who, "Recover your weapon, the ÁXE." );
    REQUIRE( result.handled );
    CHECK( result.success );
    CHECK( result.action == npc_ai::equipment_action::recover );
}
