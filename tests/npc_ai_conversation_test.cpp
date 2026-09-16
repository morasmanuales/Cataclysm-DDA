#include "cata_catch.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "action.h"
#include "avatar.h"
#include "bodypart.h"
#include "faction.h"
#include "game.h"
#include "input_context.h"
#include "input_enums.h"
#include "map.h"
#include "map_helpers.h"
#include "messages.h"
#include "move_mode.h"
#include "npc.h"
#include "npc_ai_async.h"
#include "npc_ai_client.h"
#include "npc_ai_context.h"
#include "npc_ai_interior.h"
#include "npc_ai_memory.h"
#include "npc_ai_tactical.h"
#include "npc_ai_spontaneous.h"
#include "npctalk.h"
#include "player_helpers.h"
#include "point.h"
#include "string_formatter.h"
#include "sounds.h"
#include "translations.h"
#if defined( LOCALIZE )
#include "translation_manager.h"
#endif

namespace
{

static const faction_id faction_your_followers( "your_followers" );
static const move_mode_id move_mode_run( "run" );
static const move_mode_id move_mode_walk( "walk" );

struct reset_conversation_ai_queue {
    ~reset_conversation_ai_queue() {
        npc_ai::reset_ai_request_system_for_test();
    }
};

void prepare_ai_talk_test_map()
{
    g->faction_manager_ptr->create_if_needed();
    clear_map();
    clear_avatar();
    sounds::reset_sounds();
    set_time_to_day();
    g->place_player( tripoint_bub_ms{ 60, 60, 0 } );

    map &here = get_map();
    here.invalidate_map_cache( 0 );
    here.build_map_cache( 0, true );
    here.invalidate_visibility_cache();
    here.update_visibility_cache( 0 );
}

npc &spawn_follower( const point &offset, const std::string &name )
{
    npc &follower = spawn_npc( get_avatar().pos_bub().xy() + offset, "test_talker" );
    follower.name = name;
    follower.set_fac( faction_your_followers );
    follower.set_attitude( NPCATT_FOLLOW );
    REQUIRE( follower.is_active() );
    REQUIRE( follower.is_player_ally() );
    REQUIRE( follower.is_following() );
    return follower;
}

} // namespace

TEST_CASE( "ai_talk_action_is_registered_and_bound_to_q", "[npc_ai][npc_ai_conversation]" )
{
    CHECK( action_ident( ACTION_AI_TALK ) == "ai_talk" );
    CHECK( look_up_action( "ai_talk" ) == ACTION_AI_TALK );

    input_context context = get_default_mode_input_context();
    REQUIRE( context.is_registered_action( "ai_talk" ) );
    const std::vector<input_event> bindings = context.keys_bound_to( "ai_talk", 0, true );
    const auto q_binding = std::find_if( bindings.begin(),
    bindings.end(), []( const input_event & event ) {
        return event.get_first_input() == 'q' && event.modifiers.empty();
    } );

    REQUIRE( q_binding != bindings.end() );
    CHECK( context.input_to_action( *q_binding ) == "ai_talk" );

    CHECK( action_ident( ACTION_NPC_MOVE ) == "npc_move" );
    CHECK( look_up_action( "npc_move" ) == ACTION_NPC_MOVE );
    REQUIRE( context.is_registered_action( "npc_move" ) );
    const std::vector<input_event> move_bindings = context.keys_bound_to( "npc_move", 1, true );
    const auto uppercase_q = std::find_if( move_bindings.begin(), move_bindings.end(),
    []( const input_event & event ) {
        return event.get_first_input() == 'Q' ||
               ( event.get_first_input() == 'q' && !event.modifiers.empty() );
    } );
    REQUIRE( uppercase_q != move_bindings.end() );
    CHECK( context.input_to_action( *uppercase_q ) == "npc_move" );
}

TEST_CASE( "structured_interior_order_parser_is_small_deterministic_and_accent_tolerant",
           "[npc_ai][npc_ai_conversation][npc_ai_interior]" )
{
    using order = npc_ai::structured_voice_order;
    CHECK( npc_ai::parse_structured_voice_order( "¡ENTREN A LA CASA!" ) ==
           order::enter_nearest_reachable_safe_interior );
    CHECK( npc_ai::parse_structured_voice_order( "Todos adentro" ) ==
           order::enter_nearest_reachable_safe_interior );
    CHECK( npc_ai::parse_structured_voice_order( "Refúgiense dentro" ) ==
           order::enter_nearest_reachable_safe_interior );
    CHECK( npc_ai::parse_structured_voice_order( "Liam, entra a la casa" ) ==
           order::enter_nearest_reachable_safe_interior );
    CHECK( npc_ai::parse_structured_voice_order( "Kim Rosas, métete dentro" ) ==
           order::enter_nearest_reachable_safe_interior );
    CHECK( npc_ai::parse_structured_voice_order( "¿Cómo están?" ) == order::none );
    CHECK( npc_ai::parse_structured_voice_order( "No entren" ) == order::none );
}

TEST_CASE( "npc_presentation_replaces_npcname_without_changing_dodge_or_stamina_rules",
           "[npc_ai][npc_ai_messages]" )
{
    npc liam;
    liam.name = "Liam";
    const std::string rendered = liam.replace_with_npc_name(
                                     "<npcname> no tiene suficiente vigor para intentar esquivar." );
    CHECK( rendered.find( "Liam" ) != std::string::npos );
    CHECK( rendered.find( "<npcname>" ) == std::string::npos );
}

#if defined( LOCALIZE )
TEST_CASE( "spanish_runtime_catalog_contains_npc_chatter_and_ai_equipment",
           "[npc_ai][npc_ai_language][translations]" )
{
    TranslationManager &manager = TranslationManager::GetInstance();
    const std::string previous_language = manager.GetCurrentLanguage();
    set_language( "es_ES" );

    const std::vector<std::string> visible_english = {
        "C'mon, bandages!",
        "Move over a bit, <name_g>.",
        "<general_noise_warning>  Could be a bloodbath in the making.",
        "Seems like the coast is clear… for now.",
        "I don't have or remember a suitable item for that action.",
        "I dropped %s.",
        "I'm putting on %s."
    };
    for( const std::string &english : visible_english ) {
        const std::string spanish = _( english );
        CHECK_FALSE( spanish.empty() );
        CHECK( spanish != english );
        CHECK( npc_ai::generated_text_matches_dialogue_language( spanish, "es_ES" ) );
    }

    set_language( previous_language );
}
#endif

TEST_CASE( "interior_group_order_uses_reachable_unique_destinations",
           "[npc_ai][npc_ai_conversation][npc_ai_interior]" )
{
    prepare_ai_talk_test_map();
    npc &liam = spawn_follower( point::east, "Liam" );
    npc &kim = spawn_follower( point::south, "Kim" );
    const tripoint_abs_ms liam_start = liam.pos_abs();
    const tripoint_abs_ms kim_start = kim.pos_abs();
    map &here = get_map();
    const tripoint_bub_ms anchor{ 65, 60, 0 };
    for( const tripoint_bub_ms &tile : here.points_in_radius( anchor, 2, 0 ) ) {
        here.ter_set( tile, ter_str_id( "t_floor" ) );
        here.ter_set( tile + tripoint::above, ter_str_id( "t_flat_roof" ) );
    }
    here.invalidate_map_cache( 0 );
    here.build_map_cache( 0, true );

    const npc_ai::interior_order_result result =
        npc_ai::execute_enter_nearest_reachable_safe_interior( { &liam, &kim } );
    REQUIRE( result.success );
    REQUIRE( result.assignments.size() == 2 );
    CHECK( result.assignments[0].second != result.assignments[1].second );
    CHECK( liam.goto_to_this_pos == result.assignments[0].second );
    CHECK( kim.goto_to_this_pos == result.assignments[1].second );
    CHECK( liam.ai_interior_hold_pos == result.assignments[0].second );
    CHECK( kim.ai_interior_hold_pos == result.assignments[1].second );
    CHECK( liam.pos_abs() == liam_start );
    CHECK( kim.pos_abs() == kim_start );
    CHECK_FALSE( here.is_outside( here.get_bub( result.assignments[0].second ) ) );
    CHECK_FALSE( here.is_outside( here.get_bub( result.assignments[1].second ) ) );
}

TEST_CASE( "interior_order_turns_into_a_guard_post_on_arrival",
           "[npc_ai][npc_ai_conversation][npc_ai_interior][npc_ai_orders]" )
{
    prepare_ai_talk_test_map();
    npc &liam = spawn_follower( point::east, "Liam" );
    npc &kim = spawn_follower( point::south, "Kim" );
    map &here = get_map();
    const tripoint_bub_ms anchor{ 65, 60, 0 };
    for( const tripoint_bub_ms &tile : here.points_in_radius( anchor, 2, 0 ) ) {
        here.ter_set( tile, ter_str_id( "t_floor" ) );
        here.ter_set( tile + tripoint::above, ter_str_id( "t_flat_roof" ) );
    }
    here.invalidate_map_cache( 0 );
    here.build_map_cache( 0, true );

    const npc_ai::interior_order_result result =
        npc_ai::execute_enter_nearest_reachable_safe_interior( { &liam, &kim } );
    REQUIRE( result.success );
    REQUIRE( result.assignments.size() == 2 );
    const tripoint_abs_ms liam_dest = result.assignments[0].second;
    REQUIRE( liam.is_following() );

    SECTION( "reaching the assigned interior tile stops the follow and guards it" ) {
        liam.setpos( here, here.get_bub( liam_dest ) );
        liam.goto_to_this_pos = std::nullopt;
        CHECK( npc_ai::on_move_destination_reached( liam, liam_dest ) );
        CHECK( liam.mission == NPC_MISSION_GUARD_ALLY );
        CHECK_FALSE( liam.is_following() );
        CHECK_FALSE( liam.ai_interior_hold_pos.has_value() );
        // Kim has not arrived: still following, still bound for her tile.
        CHECK( kim.is_following() );
        CHECK( kim.ai_interior_hold_pos == result.assignments[1].second );
    }

    SECTION( "reaching any other tile never turns into a guard post" ) {
        CHECK_FALSE( npc_ai::on_move_destination_reached( liam, liam.pos_abs() ) );
        CHECK( liam.is_following() );
        CHECK( liam.ai_interior_hold_pos == liam_dest );
    }

    SECTION( "a newer follow order cancels the pending walk and the hold" ) {
        const npc_ai::tactical_order_result follow =
            npc_ai::execute_tactical_order( { &liam }, npc_ai::tactical_order::follow );
        REQUIRE( follow.affected.size() == 1 );
        CHECK_FALSE( liam.ai_interior_hold_pos.has_value() );
        CHECK_FALSE( liam.goto_to_this_pos.has_value() );
        CHECK( liam.is_following() );
        // Arriving there later by chance changes nothing.
        CHECK_FALSE( npc_ai::on_move_destination_reached( liam, liam_dest ) );
        CHECK( liam.is_following() );
    }

    SECTION( "the guard post is released by the ordinary follow order" ) {
        liam.setpos( here, here.get_bub( liam_dest ) );
        REQUIRE( npc_ai::on_move_destination_reached( liam, liam_dest ) );
        REQUIRE( liam.mission == NPC_MISSION_GUARD_ALLY );
        npc_ai::execute_tactical_order( { &liam }, npc_ai::tactical_order::follow );
        CHECK( liam.is_following() );
        CHECK( liam.mission != NPC_MISSION_GUARD_ALLY );
    }
}

TEST_CASE( "interior_order_fails_cleanly_when_no_reachable_interior_exists",
           "[npc_ai][npc_ai_conversation][npc_ai_interior]" )
{
    prepare_ai_talk_test_map();
    npc &liam = spawn_follower( point::east, "Liam" );
    const tripoint_abs_ms start = liam.pos_abs();
    const std::optional<tripoint_abs_ms> old_destination = liam.goto_to_this_pos;

    const npc_ai::interior_order_result result =
        npc_ai::execute_enter_nearest_reachable_safe_interior( { &liam } );
    CHECK_FALSE( result.success );
    CHECK( result.assignments.empty() );
    CHECK( liam.pos_abs() == start );
    CHECK( liam.goto_to_this_pos == old_destination );
}

TEST_CASE( "ai_conversation_target_selection_handles_zero_one_and_multiple_npcs",
           "[npc_ai][npc_ai_conversation]" )
{
    npc pedro;
    npc ana;
    int selector_calls = 0;
    const npc_ai::ai_talker_selector select_ana = [&]( const std::vector<npc *> &talkers ) {
        ++selector_calls;
        CHECK( talkers.size() == 2 );
        return 2;
    };

    SECTION( "no eligible NPC returns cleanly" ) {
        CHECK( npc_ai::select_ai_conversation_target( {}, select_ana ) == nullptr );
        CHECK( selector_calls == 0 );
    }

    SECTION( "one eligible NPC is selected directly" ) {
        CHECK( npc_ai::select_ai_conversation_target( { &pedro }, select_ana ) == &pedro );
        CHECK( selector_calls == 0 );
    }

    SECTION( "multiple eligible NPCs invoke the selector" ) {
        CHECK( npc_ai::select_ai_conversation_target( { &pedro, &ana }, select_ana ) == &ana );
        CHECK( selector_calls == 1 );
    }
}

TEST_CASE( "ai_conversation_selector_places_talk_to_everyone_first_with_consecutive_hotkeys",
           "[npc_ai][npc_ai_conversation]" )
{
    npc juan;
    npc pedro;
    npc maria;
    juan.name = "Juan";
    pedro.name = "Pedro";
    maria.name = "Maria";
    const std::vector<npc *> talkers = { &juan, &pedro, &maria };

    const std::vector<std::string> entries = npc_ai::ai_conversation_menu_entries( talkers );
    REQUIRE( entries.size() == 4 );
    CHECK( entries[0] == string_format( _( "Talk to %s" ),
                                        npc_ai::ai_conversation_group_target_name(
                                            npc_ai::current_dialogue_language_code() ) ) );
    CHECK( npc_ai::ai_conversation_group_target_name( "es_ES" ) == "todos" );
    CHECK( npc_ai::ai_conversation_group_target_name( "es_AR" ) == "todos" );
    CHECK( npc_ai::ai_conversation_group_target_name( "en" ) == _( "everyone" ) );
    CHECK( entries[1] == string_format( _( "Talk to %s" ), "Juan" ) );
    CHECK( entries[2] == string_format( _( "Talk to %s" ), "Pedro" ) );
    CHECK( entries[3] == string_format( _( "Talk to %s" ), "Maria" ) );
    for( std::size_t index = 0; index < entries.size(); ++index ) {
        CHECK( npc_ai::ai_conversation_menu_hotkey( index ) == static_cast<int>( '1' + index ) );
    }

    const npc_ai::ai_conversation_selection individual =
        npc_ai::select_ai_conversation_targets( talkers,
    []( const std::vector<npc *> & ) {
        return 2;
    } );
    REQUIRE_FALSE( individual.everyone );
    REQUIRE( individual.targets.size() == 1 );
    CHECK( individual.targets.front() == &pedro );

    const npc_ai::ai_conversation_selection everyone =
        npc_ai::select_ai_conversation_targets( talkers,
    []( const std::vector<npc *> &candidates ) {
        return 0;
    } );
    REQUIRE( everyone.everyone );
    CHECK( everyone.targets == talkers );
}

TEST_CASE( "group_ai_conversation_queues_independent_context_for_every_npc",
           "[npc_ai][npc_ai_conversation][npc_ai_async]" )
{
    using namespace std::chrono_literals;
    reset_conversation_ai_queue reset;
    // Reproduce the process-global sound left behind by unrelated full-suite tests.
    sounds::sound( tripoint_bub_ms{ 60, 60, 0 }, 120, sounds::sound_t::combat,
                   "stale test sound" );
    prepare_ai_talk_test_map();
    npc &liam = spawn_follower( point::east, "Liam" );
    npc &kim = spawn_follower( point::south, "Kim" );
    liam.personality.aggression = -7;
    kim.personality.aggression = 8;
    liam.set_part_hp_cur( body_part_arm_r,
                          std::max( 1, liam.get_part_hp_max( body_part_arm_r ) / 10 ) );
    std::vector<std::pair<std::string, std::string>> requests;
    npc_ai::set_ai_request_executor_for_test(
    [&]( const std::string & prompt, const std::string & system_prompt ) {
        requests.emplace_back( prompt, system_prompt );
        return npc_ai::ai_response{true,
                                   system_prompt.find( "aggression=-7" ) !=
                                   std::string::npos
                                   ? "RESPUESTA_LIAM"
                                   : "RESPUESTA_KIM",
                                   ""};
    },
    false );
    npc_ai::begin_ai_session();

    CHECK( npc_ai::enqueue_group_ai_dialogue(
    {&liam, &kim}, "¿Cómo están?" ) == 2 );
    CHECK( npc_ai::get_ai_request_queue().pending_count() == 2 );
    npc_ai::get_ai_request_queue().start();
    REQUIRE( npc_ai::get_ai_request_queue().wait_until_idle_for_test(
                 std::chrono::seconds( 1 ) ) );
    REQUIRE( requests.size() == 2 );
    CHECK( std::any_of( requests.begin(), requests.end(), []( const auto & request ) {
        return request.second.find( "aggression=-7" ) != std::string::npos &&
               request.first.find( "respuesta es independiente" ) !=
               std::string::npos &&
               request.first.find( "id=arm_r" ) != std::string::npos &&
               request.first.find( "severidad_dano=grave" ) != std::string::npos;
    } ) );
    CHECK( std::any_of( requests.begin(), requests.end(), []( const auto & request ) {
        return request.second.find( "aggression=8" ) != std::string::npos &&
               request.first.find( "respuesta es independiente" ) !=
               std::string::npos &&
               request.first.find( "id=arm_r" ) == std::string::npos;
    } ) );

    Messages::clear_messages();
    while( npc_ai::get_ai_request_queue().ready_completion_count() > 0 ) {
        npc_ai::process_ai_completions();
    }
    sounds::process_sound_markers( &get_avatar() );
    const auto messages = Messages::recent_messages( 0 );
    CHECK( std::any_of( messages.begin(), messages.end(), []( const auto & message ) {
        return message.second.find( "Liam" ) != std::string::npos &&
               message.second.find( "RESPUESTA_LIAM" ) != std::string::npos;
    } ) );
    CHECK( std::any_of( messages.begin(), messages.end(), []( const auto & message ) {
        return message.second.find( "Kim" ) != std::string::npos &&
               message.second.find( "RESPUESTA_KIM" ) != std::string::npos;
    } ) );
}

TEST_CASE( "ai_talk_player_and_npc_lines_remain_in_the_normal_message_log",
           "[npc_ai][npc_ai_conversation][npc_ai_messages]" )
{
    prepare_ai_talk_test_map();
    npc &liam = spawn_follower( point::east, "Liam" );
    npc &kim = spawn_follower( point::south, "Kim Rosas" );

    Messages::clear_messages();
    npc_ai::log_ai_player_speech( true, liam.get_name(), "Tenemos que volver a la base." );
    std::vector<std::pair<std::string, std::string>> messages = Messages::recent_messages( 0 );
    REQUIRE( messages.size() == 1 );
    CHECK( messages.front().second == npc_ai::ai_player_speech_log_line(
               true, liam.get_name(), "Tenemos que volver a la base.",
               npc_ai::current_dialogue_language_code() ) );
    CHECK( npc_ai::ai_player_speech_log_line(
               true, liam.get_name(), "Volvamos.", "es_ES" ) ==
           "Dices al grupo: \"Volvamos.\"" );
    CHECK( npc_ai::ai_player_speech_log_line(
               false, liam.get_name(), "¿Estás bien?", "es_ES" ) ==
           "Dices a Liam: \"¿Estás bien?\"" );
    CHECK( npc_ai::ai_talking_status_line( true, liam.get_name(), "es_ES" ).empty() );
    CHECK( npc_ai::ai_talking_status_line( false, liam.get_name(), "es_ES" ) ==
           "Hablando con Liam..." );

    Messages::clear_messages();
    liam.say( "RESPUESTA_LIAM" );
    kim.say( "RESPUESTA_KIM" );
    sounds::process_sound_markers( &get_avatar() );
    messages = Messages::recent_messages( 0 );
    CHECK( std::any_of( messages.begin(), messages.end(), []( const auto & message ) {
        return message.second.find( "Liam" ) != std::string::npos &&
               message.second.find( "RESPUESTA_LIAM" ) != std::string::npos;
    } ) );
    CHECK( std::any_of( messages.begin(), messages.end(), []( const auto & message ) {
        return message.second.find( "Kim Rosas" ) != std::string::npos &&
               message.second.find( "RESPUESTA_KIM" ) != std::string::npos;
    } ) );
}

TEST_CASE( "individual_ai_conversation_queues_only_the_selected_npc",
           "[npc_ai][npc_ai_conversation][npc_ai_async]" )
{
    reset_conversation_ai_queue reset;
    prepare_ai_talk_test_map();
    npc &liam = spawn_follower( point::east, "Liam" );
    npc &kim = spawn_follower( point::south, "Kim" );
    npc_ai::set_ai_request_executor_for_test( []( const std::string & ) {
        return npc_ai::ai_response{ true, "Entendido.", "" };
    }, false );
    npc_ai::begin_ai_session();

    REQUIRE( npc_ai::enqueue_direct_dialogue(
                 liam, "¿Estás bien?", npc_ai::build_npc_prompt( liam, "¿Estás bien?" ) ).accepted );
    CHECK( npc_ai::get_ai_request_queue().pending_count() == 1 );
    CHECK( npc_ai::get_ai_request_queue().pending_direct_count(
               liam.getID().get_value() ) == 1 );
    CHECK( npc_ai::get_ai_request_queue().pending_direct_count(
               kim.getID().get_value() ) == 0 );
    npc_ai::get_ai_request_queue().start();
    REQUIRE( npc_ai::get_ai_request_queue().wait_until_idle_for_test(
                 std::chrono::seconds( 1 ) ) );
    const std::vector<npc_ai::ai_request_completion> completions =
        npc_ai::get_ai_request_queue().take_completions(
            npc_ai::get_ai_request_queue().ready_completion_count() );
    REQUIRE( completions.size() == 1 );
    CHECK( completions.front().request.npc_id == liam.getID().get_value() );
    CHECK( completions.front().request.type == npc_ai::ai_request_type::direct_dialogue );
    CHECK( completions.front().request.origin ==
           npc_ai::conversation_origin::direct_player_dialogue );
    CHECK( completions.front().request.context_categories == "HEALTH" );
    CHECK_FALSE( npc_ai::conversation_origin_allows_npc_reply(
                     completions.front().request.origin ) );
    CHECK_FALSE( npc_ai::maybe_enqueue_npc_reply(
                     liam, "Entendido.", completions.front().request.origin,
                     completions.front().request.conversation_id, 0 ) );
    CHECK( npc_ai::get_ai_request_queue().pending_count() == 0 );
}

TEST_CASE( "direct_ai_completion_is_delivered_only_by_the_selected_npc",
           "[npc_ai][npc_ai_conversation][npc_ai_async]" )
{
    reset_conversation_ai_queue reset;
    prepare_ai_talk_test_map();
    npc &liam = spawn_follower( point::east, "Liam" );
    spawn_follower( point::south, "Kim" );
    int executor_calls = 0;
    npc_ai::set_ai_request_executor_for_test( [&]( const std::string & ) {
        ++executor_calls;
        return npc_ai::ai_response{ true, "TARGET_ONLY_REPLY", "" };
    }, false );
    npc_ai::begin_ai_session();

    REQUIRE( npc_ai::enqueue_direct_dialogue(
                 liam, "¿Te duele algo?",
                 npc_ai::build_npc_prompt( liam, "¿Te duele algo?" ) ).accepted );
    npc_ai::get_ai_request_queue().start();
    REQUIRE( npc_ai::get_ai_request_queue().wait_until_idle_for_test(
                 std::chrono::seconds( 1 ) ) );
    Messages::clear_messages();
    while( npc_ai::get_ai_request_queue().ready_completion_count() > 0 ) {
        npc_ai::process_ai_completions();
    }
    sounds::process_sound_markers( &get_avatar() );

    CHECK( executor_calls == 1 );
    const auto messages = Messages::recent_messages( 0 );
    CHECK( std::count_if( messages.begin(), messages.end(), []( const auto &message ) {
        return message.second.find( "TARGET_ONLY_REPLY" ) != std::string::npos &&
               message.second.find( "Liam" ) != std::string::npos;
    } ) == 1 );
    CHECK( npc_ai::get_ai_request_queue().pending_count() == 0 );
}

TEST_CASE( "older_npc_reply_chain_cannot_contaminate_new_direct_player_dialogue",
           "[npc_ai][npc_ai_conversation][npc_ai_async][npc_ai_dedup]" )
{
    reset_conversation_ai_queue reset;
    prepare_ai_talk_test_map();
    npc &liam = spawn_follower( point::east, "Liam" );
    npc &kim = spawn_follower( point::south, "Kim" );
    npc_ai::set_ai_request_executor_for_test(
    []( const std::string &prompt, const std::string & ) {
        if( prompt.find( "[CONVERSACION NPC A NPC]" ) != std::string::npos ) {
            return npc_ai::ai_response{ true,
                                       "DECISION=TALK\nTEXT=STALE_NPC_CHAIN_REPLY", "" };
        }
        return npc_ai::ai_response{ true, "DIRECT_TARGET_REPLY", "" };
    }, false );
    npc_ai::begin_ai_session();

    REQUIRE( npc_ai::maybe_enqueue_npc_reply(
                 liam, "Comentario anterior.",
                 npc_ai::conversation_origin::npc_initiated_social, 7001, 0 ) );
    REQUIRE( npc_ai::enqueue_direct_dialogue(
                 liam, "¿Qué llevas?",
                 npc_ai::build_npc_prompt( liam, "¿Qué llevas?" ) ).accepted );
    npc_ai::get_ai_request_queue().start();
    REQUIRE( npc_ai::get_ai_request_queue().wait_until_idle_for_test(
                 std::chrono::seconds( 1 ) ) );
    Messages::clear_messages();
    while( npc_ai::get_ai_request_queue().ready_completion_count() > 0 ) {
        npc_ai::process_ai_completions();
    }
    sounds::process_sound_markers( &get_avatar() );

    const auto messages = Messages::recent_messages( 0 );
    CHECK( std::any_of( messages.begin(), messages.end(), []( const auto &message ) {
        return message.second.find( "DIRECT_TARGET_REPLY" ) != std::string::npos &&
               message.second.find( "Liam" ) != std::string::npos;
    } ) );
    CHECK_FALSE( std::any_of( messages.begin(), messages.end(), []( const auto &message ) {
        return message.second.find( "STALE_NPC_CHAIN_REPLY" ) != std::string::npos;
    } ) );
    CHECK( npc_ai::build_memory_context( kim ).find( "STALE_NPC_CHAIN_REPLY" ) ==
           std::string::npos );
    CHECK( npc_ai::get_ai_request_queue().pending_count() == 0 );
}

TEST_CASE( "group_ai_requests_keep_group_origin_and_allow_independent_targets",
           "[npc_ai][npc_ai_conversation][npc_ai_async]" )
{
    reset_conversation_ai_queue reset;
    prepare_ai_talk_test_map();
    npc &liam = spawn_follower( point::east, "Liam" );
    npc &kim = spawn_follower( point::south, "Kim" );
    npc_ai::set_ai_request_executor_for_test( []( const std::string & ) {
        return npc_ai::ai_response{ true, "GROUP_REPLY", "" };
    }, false );
    npc_ai::begin_ai_session();

    const std::uint64_t conversation_id = npc_ai::next_conversation_turn_id();
    REQUIRE( npc_ai::enqueue_group_ai_dialogue(
                 { &liam, &kim }, "¿Cómo están?", conversation_id ) == 2 );
    npc_ai::get_ai_request_queue().start();
    REQUIRE( npc_ai::get_ai_request_queue().wait_until_idle_for_test(
                 std::chrono::seconds( 1 ) ) );
    const std::vector<npc_ai::ai_request_completion> completions =
        npc_ai::get_ai_request_queue().take_completions(
            npc_ai::get_ai_request_queue().ready_completion_count() );
    REQUIRE( completions.size() == 2 );
    for( const npc_ai::ai_request_completion &completion : completions ) {
        CHECK( completion.request.type == npc_ai::ai_request_type::group_dialogue );
        CHECK( completion.request.origin == npc_ai::conversation_origin::group_player_dialogue );
        CHECK( completion.request.conversation_id == conversation_id );
        CHECK( completion.request.context_categories == "HEALTH" );
        CHECK_FALSE( npc_ai::conversation_origin_allows_npc_reply(
                         completion.request.origin ) );
    }
    CHECK( npc_ai::conversation_origin_allows_npc_reply(
               npc_ai::conversation_origin::spontaneous_world_event ) );
    CHECK( npc_ai::conversation_origin_allows_npc_reply(
               npc_ai::conversation_origin::npc_initiated_social ) );
}

TEST_CASE( "severe_health_fact_reaches_the_exact_final_ollama_payload",
           "[npc_ai][npc_ai_conversation][npc_ai_self][npc_ai_ollama]" )
{
    reset_conversation_ai_queue reset;
    prepare_ai_talk_test_map();
    npc &liam = spawn_follower( point::east, "Liam" );
    const int severe_hp = std::max( 1, liam.get_part_hp_max( body_part_arm_r ) / 10 );
    liam.set_part_hp_cur( body_part_arm_r, severe_hp );
    std::string executor_prompt;
    std::string executor_system;
    npc_ai::set_ai_request_executor_for_test(
    [&]( const std::string &prompt, const std::string &system_prompt ) {
        executor_prompt = prompt;
        executor_system = system_prompt;
        return npc_ai::ai_response{ true, "FINAL_PROMPT_CAPTURED", "" };
    }, false );
    npc_ai::begin_ai_session();

    REQUIRE( npc_ai::enqueue_direct_dialogue(
                 liam, "¿Te duele algo?",
                 npc_ai::build_npc_prompt( liam, "¿Te duele algo?" ) ).accepted );
    npc_ai::get_ai_request_queue().start();
    REQUIRE( npc_ai::get_ai_request_queue().wait_until_idle_for_test(
                 std::chrono::seconds( 1 ) ) );
    const std::vector<npc_ai::ai_request_completion> completions =
        npc_ai::get_ai_request_queue().take_completions(
            npc_ai::get_ai_request_queue().ready_completion_count() );
    REQUIRE( completions.size() == 1 );
    CHECK( completions.front().request.prompt == executor_prompt );
    CHECK( completions.front().request.system_prompt == executor_system );
    CHECK( completions.front().request.context_categories == "HEALTH" );
    CHECK( executor_prompt.find( "id=arm_r" ) != std::string::npos );
    CHECK( executor_prompt.find( "hp=" + std::to_string( severe_hp ) + "/" ) !=
           std::string::npos );
    CHECK( executor_prompt.find( "severidad_dano=grave" ) != std::string::npos );

    const std::string payload = npc_ai::build_ollama_request_json(
                                    executor_prompt, executor_system );
    CHECK( payload.find( "id=arm_r" ) != std::string::npos );
    CHECK( payload.find( "severidad_dano=grave" ) != std::string::npos );
}

TEST_CASE( "group_dialogue_suppresses_an_identical_cross_speaker_candidate",
           "[npc_ai][npc_ai_conversation][npc_ai_dedup]" )
{
    reset_conversation_ai_queue reset;
    prepare_ai_talk_test_map();
    npc &liam = spawn_follower( point::east, "Liam" );
    npc &kim = spawn_follower( point::south, "Kim" );
    int executor_calls = 0;
    npc_ai::set_ai_request_executor_for_test( [&]( const std::string & ) {
        ++executor_calls;
        return npc_ai::ai_response{ true, "IDENTICAL_CROSS_SPEAKER_REPLY", "" };
    }, false );
    npc_ai::begin_ai_session();

    REQUIRE( npc_ai::enqueue_group_ai_dialogue(
                 { &liam, &kim }, "¿Cómo están?" ) == 2 );
    npc_ai::get_ai_request_queue().start();
    REQUIRE( npc_ai::get_ai_request_queue().wait_until_idle_for_test(
                 std::chrono::seconds( 1 ) ) );
    Messages::clear_messages();
    while( npc_ai::get_ai_request_queue().ready_completion_count() > 0 ) {
        npc_ai::process_ai_completions();
    }
    sounds::process_sound_markers( &get_avatar() );

    CHECK( executor_calls == 2 );
    const auto messages = Messages::recent_messages( 0 );
    CHECK( std::count_if( messages.begin(), messages.end(), []( const auto &message ) {
        return message.second.find( "IDENTICAL_CROSS_SPEAKER_REPLY" ) != std::string::npos;
    } ) == 1 );
}

TEST_CASE( "group_dialogue_keeps_distinct_phrasing_about_the_same_event",
           "[npc_ai][npc_ai_conversation][npc_ai_dedup]" )
{
    reset_conversation_ai_queue reset;
    prepare_ai_talk_test_map();
    npc &liam = spawn_follower( point::east, "Liam" );
    npc &kim = spawn_follower( point::south, "Kim" );
    npc_ai::set_ai_request_executor_for_test(
    []( const std::string &, const std::string &system_prompt ) {
        return npc_ai::ai_response{ true,
                                   system_prompt.find( "Liam" ) != std::string::npos ?
                                   "MAKE_ROOM_FIRST" : "MOVE_ASIDE_SECOND", "" };
    }, false );
    npc_ai::begin_ai_session();

    REQUIRE( npc_ai::enqueue_group_ai_dialogue(
                 { &liam, &kim }, "Háganme sitio." ) == 2 );
    npc_ai::get_ai_request_queue().start();
    REQUIRE( npc_ai::get_ai_request_queue().wait_until_idle_for_test(
                 std::chrono::seconds( 1 ) ) );
    Messages::clear_messages();
    while( npc_ai::get_ai_request_queue().ready_completion_count() > 0 ) {
        npc_ai::process_ai_completions();
    }
    sounds::process_sound_markers( &get_avatar() );

    const auto messages = Messages::recent_messages( 0 );
    CHECK( std::any_of( messages.begin(), messages.end(), []( const auto &message ) {
        return message.second.find( "MAKE_ROOM_FIRST" ) != std::string::npos;
    } ) );
    CHECK( std::any_of( messages.begin(), messages.end(), []( const auto &message ) {
        return message.second.find( "MOVE_ASIDE_SECOND" ) != std::string::npos;
    } ) );
}

TEST_CASE( "direct_ai_talk_candidates_are_visible_nearby_allies",
           "[npc_ai][npc_ai_conversation]" )
{
    prepare_ai_talk_test_map();

    SECTION( "non-ally is not eligible" ) {
        npc &neutral = spawn_npc( get_avatar().pos_bub().xy() + point::east, "test_talker" );
        neutral.name = "Neutral NPC";
        CHECK_FALSE( neutral.is_following() );
        CHECK( npc_ai::get_nearby_ai_talkers( true ).empty() );

        const std::vector<npc *> normal_chat_talkers = npc_ai::get_nearby_ai_talkers( false );
        REQUIRE( normal_chat_talkers.size() == 1 );
        CHECK( normal_chat_talkers.front() == &neutral );
    }

    SECTION( "one visible nearby follower is eligible" ) {
        npc &pedro = spawn_follower( point::east, "Pedro" );
        const std::vector<npc *> talkers = npc_ai::get_nearby_ai_talkers( true );

        REQUIRE( talkers.size() == 1 );
        CHECK( talkers.front() == &pedro );
    }

    SECTION( "multiple visible nearby followers are eligible" ) {
        npc &pedro = spawn_follower( point::east, "Pedro" );
        npc &ana = spawn_follower( point::south, "Ana" );
        const std::vector<npc *> talkers = npc_ai::get_nearby_ai_talkers( true );

        REQUIRE( talkers.size() == 2 );
        CHECK( std::find( talkers.begin(), talkers.end(), &pedro ) != talkers.end() );
        CHECK( std::find( talkers.begin(), talkers.end(), &ana ) != talkers.end() );
    }
}

TEST_CASE( "ai_tactical_order_parser_recognizes_spanish_and_english_follow_and_guard",
           "[npc_ai][npc_ai_conversation][npc_ai_tactical]" )
{
    for( const std::string &line : {
             "sígueme", "sigueme", "ven conmigo", "acompáñame",
             "todos síganme", "vengan conmigo", "follow me",
             "everyone follow me"
         } ) {
        CAPTURE( line );
        CHECK( npc_ai::parse_tactical_order( line ) == npc_ai::tactical_order::follow );
    }
    for( const std::string &line : {
             "quédate aquí", "quedate aqui", "quieto", "quietos",
             "no te muevas", "no se muevan", "protejan esta posición",
             "mantengan esta posición", "quédense aquí", "hold position",
             "stay here", "guard this position"
         } ) {
        CAPTURE( line );
        CHECK( npc_ai::parse_tactical_order( line ) == npc_ai::tactical_order::guard );
    }
    CHECK( npc_ai::parse_tactical_order( "How are you?" ) == npc_ai::tactical_order::none );
}

TEST_CASE( "individual_ai_tactical_orders_only_change_the_selected_npc",
           "[npc_ai][npc_ai_conversation][npc_ai_tactical]" )
{
    prepare_ai_talk_test_map();
    npc &juan = spawn_follower( point::east, "Juan" );
    npc &pedro = spawn_follower( point::south, "Pedro" );

    const npc_ai::tactical_order_result hold =
        npc_ai::execute_tactical_order( { &juan }, "quédate aquí" );
    REQUIRE( hold.handled );
    REQUIRE( hold.affected.size() == 1 );
    CHECK( hold.affected.front() == &juan );
    CHECK( juan.is_guarding() );
    CHECK_FALSE( juan.is_following() );
    CHECK( juan.get_attitude() == NPCATT_NULL );
    CHECK( juan.mission == NPC_MISSION_GUARD_ALLY );
    CHECK( pedro.is_following() );
    CHECK_FALSE( pedro.is_guarding() );

    const npc_ai::tactical_order_result follow =
        npc_ai::execute_tactical_order( { &juan }, "follow me" );
    REQUIRE( follow.handled );
    REQUIRE( follow.affected.size() == 1 );
    CHECK( juan.is_following() );
    CHECK_FALSE( juan.is_guarding() );
    CHECK( juan.get_attitude() == NPCATT_FOLLOW );
    CHECK( juan.mission == NPC_MISSION_NULL );
    CHECK( pedro.is_following() );
}

TEST_CASE( "group_ai_tactical_orders_change_all_and_only_eligible_allies",
           "[npc_ai][npc_ai_conversation][npc_ai_tactical]" )
{
    prepare_ai_talk_test_map();
    npc &juan = spawn_follower( point::east, "Juan" );
    npc &pedro = spawn_follower( point::south, "Pedro" );
    npc &outside_radius = spawn_follower( point{ SEEX * 2 + 5, 0 }, "Outside radius" );

    const std::vector<npc *> eligible = npc_ai::get_nearby_ai_talkers( true );
    REQUIRE( eligible.size() == 2 );
    CHECK( std::find( eligible.begin(), eligible.end(), &outside_radius ) == eligible.end() );
    const std::size_t pending_before = npc_ai::get_ai_request_queue().pending_count();

    const npc_ai::tactical_order_result hold =
        npc_ai::execute_tactical_order( eligible, "quédense aquí" );
    REQUIRE( hold.handled );
    CHECK( hold.affected.size() == 2 );
    CHECK( juan.is_guarding() );
    CHECK( pedro.is_guarding() );
    CHECK_FALSE( juan.is_following() );
    CHECK_FALSE( pedro.is_following() );
    CHECK( outside_radius.is_following() );
    CHECK_FALSE( outside_radius.is_guarding() );
    CHECK( npc_ai::get_ai_request_queue().pending_count() == pending_before );

    // Guarding allies remain eligible for AI conversation, allowing the same
    // selector to issue the inverse vanilla order.
    const std::vector<npc *> eligible_guards = npc_ai::get_nearby_ai_talkers( true );
    REQUIRE( eligible_guards.size() == 2 );
    const npc_ai::tactical_order_result follow =
        npc_ai::execute_tactical_order( eligible_guards, "todos síganme" );
    REQUIRE( follow.handled );
    CHECK( follow.affected.size() == 2 );
    CHECK( juan.is_following() );
    CHECK( pedro.is_following() );
    CHECK_FALSE( juan.is_guarding() );
    CHECK_FALSE( pedro.is_guarding() );
    CHECK( outside_radius.is_following() );
    CHECK( npc_ai::get_ai_request_queue().pending_count() == pending_before );
}

TEST_CASE( "avatar_run_walk_mode_is_synchronized_only_to_active_followers",
           "[npc_ai][npc_ai_conversation][npc_movement]" )
{
    prepare_ai_talk_test_map();
    avatar &player = get_avatar();
    npc &follower = spawn_follower( point::east, "Follower" );
    npc &guard = spawn_follower( point::south, "Guard" );
    npc &non_follower = spawn_follower( point::west, "Independent ally" );
    talk_function::assign_guard( guard );
    non_follower.set_attitude( NPCATT_NULL );
    non_follower.set_mission( NPC_MISSION_NULL );

    player.set_movement_mode( move_mode_walk );
    follower.set_movement_mode( move_mode_walk );
    guard.set_movement_mode( move_mode_walk );
    non_follower.set_movement_mode( move_mode_walk );
    follower.set_stamina( follower.get_stamina_max() / 2 );
    const int follower_stamina = follower.get_stamina();

    player.set_movement_mode( move_mode_run );
    CHECK( follower.movement_mode_is( move_mode_run ) );
    CHECK( guard.movement_mode_is( move_mode_walk ) );
    CHECK( non_follower.movement_mode_is( move_mode_walk ) );
    CHECK( follower.get_stamina() == follower_stamina );

    player.set_movement_mode( move_mode_walk );
    CHECK( follower.movement_mode_is( move_mode_walk ) );
    CHECK( guard.movement_mode_is( move_mode_walk ) );
    CHECK( non_follower.movement_mode_is( move_mode_walk ) );
    CHECK( follower.get_stamina() == follower_stamina );
}
