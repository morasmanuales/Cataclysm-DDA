#include "cata_catch.h"

#include <string>

#include "avatar.h"
#include "calendar.h"
#include "faction.h"
#include "game.h"
#include "item.h"
#include "map.h"
#include "map_helpers.h"
#include "messages.h"
#include "npc.h"
#include "npc_ai_action_parser.h"
#include "npc_ai_watchlist.h"
#include "player_helpers.h"
#include "point.h"
#include "sounds.h"
#include "type_id.h"
#include "units.h"
#include "vehicle.h"

namespace
{

static const faction_id faction_your_followers( "your_followers" );
static const itype_id itype_gloves_work( "gloves_work" );
static const itype_id itype_bottle_plastic( "bottle_plastic" );

npc &prepare_watcher()
{
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
    npc_ai::reset_watch_cache();
    Messages::clear_messages();
    return who;
}

bool said_something_containing( const std::string &needle )
{
    sounds::process_sound_markers( &get_avatar() );
    for( const auto &message : Messages::recent_messages( 0 ) ) {
        if( message.second.find( needle ) != std::string::npos ) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE( "watch_specific_terms_cover_the_whole_item_family",
           "[npc_ai][npc_ai_watch][npc_ai_orders]" )
{
    npc &who = prepare_watcher();
    map &here = get_map();

    // The model answers with one English family word for "guantes".
    const npc_ai::watch_action_result parsed = npc_ai::parse_watch_action_response(
                "Avisame si ves guantes.", "WATCH|SPECIFIC|gloves" );
    REQUIRE( parsed.success );
    REQUIRE( parsed.is_watch );
    // The stored selector is a fragment match, not an enumerated id list,
    // and it carries the player's own word next to the model's term.
    CHECK( parsed.control_marker.find( "@idlike:gloves" ) != std::string::npos );
    CHECK( parsed.control_marker.find( "|guantes" ) != std::string::npos );
    CHECK( parsed.control_marker.find( "@id:" ) == std::string::npos );

    std::string speech = "Vale. " + parsed.control_marker;
    REQUIRE( npc_ai::apply_watch_control( who, speech ) );
    CHECK( speech.find( "WATCH_ITEM" ) == std::string::npos );

    // A bottle in sight does not trigger the watch.
    const tripoint_bub_ms spot = who.pos_bub( here ) + point::east;
    here.add_item_or_charges( spot, item( itype_bottle_plastic, calendar::turn ) );
    npc_ai::check_item_watchlist( who );
    CHECK_FALSE( said_something_containing( "gloves" ) );

    // Work gloves (gloves_work) are far down any catalogue listing of the 118
    // glove types; the fragment selector still recognises them.
    here.add_item_or_charges( spot, item( itype_gloves_work, calendar::turn ) );
    npc_ai::check_item_watchlist( who );
    CHECK( said_something_containing( "gloves" ) );

    // One notification: the task is fulfilled and removed.
    Messages::clear_messages();
    npc_ai::check_item_watchlist( who );
    CHECK_FALSE( said_something_containing( "gloves" ) );
}

TEST_CASE( "watch_specific_multiword_terms_match_ids_and_names",
           "[npc_ai][npc_ai_watch][npc_ai_orders]" )
{
    const npc_ai::watch_action_result parsed = npc_ai::parse_watch_action_response(
                "Avisame si ves guantes de trabajo.", "WATCH|SPECIFIC|work gloves;gloves" );
    REQUIRE( parsed.is_watch );
    // "work gloves" is kept for the visible name and also as "work_gloves"
    // for the id form; "gloves" follows.
    CHECK( parsed.control_marker.find( "work gloves" ) != std::string::npos );
    CHECK( parsed.control_marker.find( "work_gloves" ) != std::string::npos );
    CHECK( parsed.control_marker.find( "|gloves" ) != std::string::npos );
    // Player words: accents dropped, filler ("avisame", "si", "ves", "de")
    // removed, and kept together as one all-words fragment.
    CHECK( parsed.control_marker.find( "|guantes trabajo" ) != std::string::npos );
    CHECK( parsed.control_marker.find( "avisame" ) == std::string::npos );
}

TEST_CASE( "watch_player_words_reach_items_the_english_term_cannot",
           "[npc_ai][npc_ai_watch][npc_ai_orders]" )
{
    npc &who = prepare_watcher();
    map &here = get_map();
    // Brigandine gloves: id "*_brigandine_hands", no "glove" anywhere in the
    // id; only the player's word can reach them through the visible name
    // when the game runs in Spanish.  In the English test locale the visible
    // name does contain "gloves", so this checks the selector composition and
    // the end-to-end alert with the combined fragments.
    const npc_ai::watch_action_result parsed = npc_ai::parse_watch_action_response(
                "Avísame si ves unos guantes.", "WATCH|SPECIFIC|gloves" );
    REQUIRE( parsed.is_watch );
    std::string speech = parsed.control_marker;
    REQUIRE( npc_ai::apply_watch_control( who, speech ) );

    here.add_item_or_charges( who.pos_bub( here ) + point::east,
                              item( itype_id( "mc_brigandine_hands" ), calendar::turn ) );
    npc_ai::check_item_watchlist( who );
    CHECK( said_something_containing( "brigandine" ) );
}

TEST_CASE( "watch_order_is_registered_without_the_model", "[npc_ai][npc_ai_watch][npc_ai_orders]" )
{
    npc &who = prepare_watcher();
    map &here = get_map();

    // The live order: no request is queued, the watch is registered at once.
    const npc_ai::watch_action_result result =
        npc_ai::parse_watch_action( who, "Avísame si ves unos guantes." );
    REQUIRE( result.attempted );
    REQUIRE( result.success );
    CHECK_FALSE( result.pending );
    CHECK( result.kind == "LOCAL" );
    CHECK( result.control_marker.find( "@idlike:guantes" ) != std::string::npos );
    // Synonyms from data/npc_ai/watch_synonyms.txt: the English id form too.
    CHECK( result.control_marker.find( "|gloves" ) != std::string::npos );
    CHECK( npc_ai::build_watchlist_context( who ).find( "@idlike:" ) != std::string::npos );

    // The alert arrives, in the mixed (magenta) message channel.
    here.add_item_or_charges( who.pos_bub( here ) + point::east,
                              item( itype_gloves_work, calendar::turn ) );
    Messages::clear_messages();
    npc_ai::check_item_watchlist( who );
    bool magenta_alert = false;
    for( const auto &message : Messages::recent_messages( 0 ) ) {
        if( message.second.find( "gloves" ) != std::string::npos ) {
            magenta_alert = true;
        }
    }
    CHECK( magenta_alert );
}

TEST_CASE( "local_watch_selector_categories_and_synonyms", "[npc_ai][npc_ai_watch][npc_ai_orders]" )
{
    using npc_ai::build_local_watch_selector;
    CHECK( build_local_watch_selector( "Avísame si ves un cargador." ) == "@MAGAZINE" );
    CHECK( build_local_watch_selector( "Dime si encuentras munición." ) == "@AMMO" );
    CHECK( build_local_watch_selector( "Avísame cuando veas un arma de fuego." ) == "@GUN" );
    CHECK( build_local_watch_selector( "Avísame si ves una pistola." ) == "@GUN" );
    // Plain words: the player's word first, synonyms after.
    const std::string pilas = build_local_watch_selector( "Avísame si ves pilas." );
    CHECK( pilas.rfind( "@idlike:pilas", 0 ) == 0 );
    CHECK( pilas.find( "battery" ) != std::string::npos );
    CHECK( pilas.find( "bateria" ) != std::string::npos );
    // Filler only: nothing to watch for.
    CHECK( build_local_watch_selector( "Avísame si ves algo." ).empty() );
    // A word without synonyms still works on its own.
    CHECK( build_local_watch_selector( "Avísame si ves un destornillador." ).find( "destornillador" ) !=
           std::string::npos );
}

TEST_CASE( "multi_word_watch_requires_every_word", "[npc_ai][npc_ai_watch][npc_ai_orders]" )
{
    npc &who = prepare_watcher();
    map &here = get_map();
    const npc_ai::watch_action_result result =
        npc_ai::parse_watch_action( who, "Avísame si ves un panel solar." );
    REQUIRE( result.success );
    // One all-words fragment plus its synonyms; never "panel" on its own.
    CHECK( result.control_marker.find( "@idlike:panel solar" ) != std::string::npos );
    CHECK( result.control_marker.find( "|panel|" ) == std::string::npos );
    CHECK( result.control_marker.find( "|solar panel" ) != std::string::npos );

    // A wooden panel in sight is not a solar panel.
    here.add_item_or_charges( who.pos_bub( here ) + point::east,
                              item( itype_id( "wood_panel" ), calendar::turn ) );
    Messages::clear_messages();
    npc_ai::check_item_watchlist( who );
    CHECK_FALSE( said_something_containing( "panel" ) );
    CHECK( npc_ai::build_watchlist_context( who ).find( "@idlike:" ) != std::string::npos );

    // A solar panel installed on a vehicle is.
    vehicle *car = here.add_vehicle( vproto_id( "electric_car" ), who.pos_bub( here ) +
                                     tripoint_rel_ms{ 3, 0, 0 }, 0_degrees, 100, 0 );
    REQUIRE( car != nullptr );
    Messages::clear_messages();
    npc_ai::check_item_watchlist( who );
    CHECK( said_something_containing( "solar panel" ) );
    CHECK( said_something_containing( "installed on" ) );
    CHECK( npc_ai::build_watchlist_context( who ).find( "@idlike:" ) == std::string::npos );
}

TEST_CASE( "watch_sees_installed_furniture", "[npc_ai][npc_ai_watch][npc_ai_orders]" )
{
    npc &who = prepare_watcher();
    map &here = get_map();
    REQUIRE( npc_ai::parse_watch_action( who, "Avísame si ves una nevera." ).success );
    here.furn_set( who.pos_bub( here ) + tripoint_rel_ms{ 2, 1, 0 }, furn_str_id( "f_fridge" ) );
    Messages::clear_messages();
    npc_ai::check_item_watchlist( who );
    CHECK( said_something_containing( "refrigerator" ) );
}
