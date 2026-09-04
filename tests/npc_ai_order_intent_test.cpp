#include "cata_catch.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "avatar.h"
#include "faction.h"
#include "game.h"
#include "map_helpers.h"
#include "messages.h"
#include "npc.h"
#include "npc_ai_async.h"
#include "npc_ai_client.h"
#include "npc_ai_context.h"
#include "npc_ai_order_intent.h"
#include "npc_ai_tactical.h"
#include "npctalk.h"
#include "player_helpers.h"
#include "point.h"
#include "sounds.h"
#include "translation_manager.h"
#include "translations.h"

using namespace std::chrono_literals;

namespace
{

static const faction_id faction_your_followers( "your_followers" );

struct reset_order_executor {
    ~reset_order_executor() {
        npc_ai::reset_ai_request_system_for_test();
    }
};

npc &prepare_order_follower()
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
    npc_ai::begin_ai_session();
    return who;
}

std::vector<std::string> settle_and_messages()
{
    npc_ai::ai_request_queue &queue = npc_ai::get_ai_request_queue();
    queue.start();
    for( int round = 0; round < 4; ++round ) {
        REQUIRE( queue.wait_until_idle_for_test( 5s ) );
        npc_ai::process_ai_completions();
        if( queue.pending_count() == 0 && queue.ready_completion_count() == 0 ) {
            break;
        }
    }
    sounds::process_sound_markers( &get_avatar() );
    std::vector<std::string> out;
    for( const auto &message : Messages::recent_messages( 0 ) ) {
        out.push_back( message.second );
    }
    return out;
}

bool any_contains( const std::vector<std::string> &lines, const std::string &needle )
{
    for( const std::string &line : lines ) {
        if( line.find( needle ) != std::string::npos ) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE( "order_intent_gate_only_admits_short_unrouted_imperatives",
           "[npc_ai][npc_ai_orders]" )
{
    CHECK( npc_ai::order_intent_candidate( "Siganme" ) );
    CHECK( npc_ai::order_intent_candidate( "Muevanse conmigo, vamos" ) );
    CHECK( npc_ai::order_intent_candidate( "Deja eso en el suelo" ) );
    CHECK( npc_ai::order_intent_candidate( "Everyone on me" ) );

    // Questions and routed queries already have real context; never classify.
    CHECK_FALSE( npc_ai::order_intent_candidate( "¿Como estas?" ) );
    CHECK_FALSE( npc_ai::order_intent_candidate( "Estas herido" ) );
    CHECK_FALSE( npc_ai::order_intent_candidate( "Que ves" ) );
    CHECK_FALSE( npc_ai::order_intent_candidate( "Hola Liam" ) );
    CHECK_FALSE( npc_ai::order_intent_candidate( "Where did we leave the truck" ) );
    // Long speeches are conversation.
    CHECK_FALSE( npc_ai::order_intent_candidate(
                     "Cuando lleguemos a la ciudad tenemos que buscar un lugar seguro para pasar la noche y comer algo" ) );
    CHECK_FALSE( npc_ai::order_intent_candidate( "" ) );
}

TEST_CASE( "order_intent_parser_accepts_only_the_closed_catalogue",
           "[npc_ai][npc_ai_orders]" )
{
    npc_ai::order_intent_parse follow = npc_ai::parse_order_intent_response( "ORDER=FOLLOW\nTARGET=" );
    CHECK( follow.valid );
    CHECK( follow.intent == npc_ai::order_intent::follow );
    CHECK( follow.target.empty() );

    npc_ai::order_intent_parse pickup =
        npc_ai::parse_order_intent_response( "order=pickup\ntarget=\"la mochila\"\n" );
    CHECK( pickup.valid );
    CHECK( pickup.intent == npc_ai::order_intent::pickup );
    CHECK( pickup.target == "la mochila" );

    npc_ai::order_intent_parse none = npc_ai::parse_order_intent_response( "ORDER=NONE\nTARGET=vacio" );
    CHECK( none.valid );
    CHECK( none.intent == npc_ai::order_intent::none );
    CHECK( none.target.empty() );

    // Anything outside the catalogue is rejected, never mapped to "closest".
    CHECK_FALSE( npc_ai::parse_order_intent_response( "ORDER=DANCE" ).valid );
    CHECK_FALSE( npc_ai::parse_order_intent_response( "FOLLOW" ).valid );
    CHECK_FALSE( npc_ai::parse_order_intent_response( "Claro, te sigo." ).valid );
    CHECK_FALSE( npc_ai::parse_order_intent_response( "" ).valid );

    const std::string prompt = npc_ai::build_order_intent_prompt( "Siganme", true, 3 );
    CHECK( prompt.find( "Siganme" ) != std::string::npos );
    CHECK( prompt.find( "ORDER=" ) != std::string::npos );
    CHECK( prompt.find( "NONE" ) != std::string::npos );
}

TEST_CASE( "order_intent_follow_from_model_executes_the_same_tactical_path",
           "[npc_ai][npc_ai_orders][npc_ai_async]" )
{
    reset_order_executor reset;
    npc &who = prepare_order_follower();
    talk_function::assign_guard( who );
    REQUIRE( who.mission == NPC_MISSION_GUARD_ALLY );

    // Not in any keyword list: the fast path must not have caught it.
    const std::string line = "Muevanse conmigo, vamos";
    CHECK( npc_ai::parse_tactical_order( line ) == npc_ai::tactical_order::none );

    std::string seen_prompt;
    npc_ai::set_ai_request_executor_for_test( [&]( const std::string & prompt ) {
        seen_prompt = prompt;
        return npc_ai::ai_response{ true, "ORDER=FOLLOW\nTARGET=", "" };
    }, false );
    npc_ai::begin_ai_session();

    REQUIRE( npc_ai::enqueue_order_intent_resolution( who, line, false, 1 ) );
    const std::vector<std::string> messages = settle_and_messages();

    CHECK( seen_prompt.find( line ) != std::string::npos );
    CHECK( who.mission != NPC_MISSION_GUARD_ALLY );
    CHECK( any_contains( messages, "Liam" ) );
    CHECK( npc_ai::get_ai_request_queue().pending_count() == 0 );
}

TEST_CASE( "order_intent_none_forwards_the_line_to_ordinary_dialogue",
           "[npc_ai][npc_ai_orders][npc_ai_async]" )
{
    reset_order_executor reset;
    npc &who = prepare_order_follower();
    talk_function::assign_guard( who );

    int calls = 0;
    npc_ai::set_ai_request_executor_for_test( [&]( const std::string & ) {
        ++calls;
        return calls == 1 ? npc_ai::ai_response{ true, "ORDER=NONE\nTARGET=", "" }
               : npc_ai::ai_response{ true, "RESPUESTA_DE_CHARLA_ORDINARIA", "" };
    }, false );
    npc_ai::begin_ai_session();

    REQUIRE( npc_ai::enqueue_order_intent_resolution( who, "Vamos a ver que tal", false, 1 ) );
    const std::vector<std::string> messages = settle_and_messages();

    CHECK( calls == 2 );
    CHECK( who.mission == NPC_MISSION_GUARD_ALLY );
    CHECK( any_contains( messages, "RESPUESTA_DE_CHARLA_ORDINARIA" ) );
}

TEST_CASE( "order_intent_invalid_model_output_never_executes_anything",
           "[npc_ai][npc_ai_orders][npc_ai_async]" )
{
    reset_order_executor reset;
    npc &who = prepare_order_follower();
    talk_function::assign_guard( who );

    int calls = 0;
    npc_ai::set_ai_request_executor_for_test( [&]( const std::string & ) {
        ++calls;
        return calls == 1 ? npc_ai::ai_response{ true, "ORDER=TELEPORT\nTARGET=la luna", "" }
               : npc_ai::ai_response{ true, "CHARLA_TRAS_SALIDA_INVALIDA", "" };
    }, false );
    npc_ai::begin_ai_session();

    REQUIRE( npc_ai::enqueue_order_intent_resolution( who, "Teleportate a la luna", false, 1 ) );
    const std::vector<std::string> messages = settle_and_messages();

    CHECK( who.mission == NPC_MISSION_GUARD_ALLY );
    CHECK( any_contains( messages, "CHARLA_TRAS_SALIDA_INVALIDA" ) );
}

// Hidden: real classifier accuracy against the active remote provider.
TEST_CASE( "order_intent_live_classifier_accuracy", "[.npc_ai_live_orders]" )
{
    if( !npc_ai::openai_api_key_available() ) {
        WARN( "CDDA_NPC_AI_OPENAI_API_KEY not set; live order classification skipped" );
        return;
    }
#if defined( LOCALIZE )
    const std::string previous_language = TranslationManager::GetInstance().GetCurrentLanguage();
    set_language( "es_ES" );
#endif
    const std::vector<std::pair<std::string, npc_ai::order_intent>> cases = {
        { "Siganme", npc_ai::order_intent::follow },
        { "Muevanse conmigo, vamos", npc_ai::order_intent::follow },
        { "No os quedeis atras", npc_ai::order_intent::follow },
        { "Esperadme aqui un momento", npc_ai::order_intent::guard },
        { "Cubrid esta puerta y no os movais", npc_ai::order_intent::guard },
        { "Todo el mundo dentro de la casa, ya", npc_ai::order_intent::enter_interior },
        { "Coge esa mochila del suelo", npc_ai::order_intent::pickup },
        { "Saca el bate", npc_ai::order_intent::wield },
        { "Tira ese cuchillo oxidado", npc_ai::order_intent::drop },
        { "Ponte el chaleco", npc_ai::order_intent::wear },
        { "Vamos a ver que tal nos va", npc_ai::order_intent::none },
        { "Deberiamos descansar luego", npc_ai::order_intent::none },
        { "Que frio hace hoy", npc_ai::order_intent::none },
        { "Me alegra que estes bien", npc_ai::order_intent::none },
        { "Manana buscamos gasolina", npc_ai::order_intent::none },
        { "Gracias por cubrirme antes", npc_ai::order_intent::none },
    };
    int correct = 0;
    for( const auto &entry : cases ) {
        const std::string prompt = npc_ai::build_order_intent_prompt( entry.first, false, 1 );
        npc &dummy = prepare_order_follower();
        const std::string system = npc_ai::build_npc_system_prompt(
                                       dummy, npc_ai::npc_prompt_purpose::order_resolution );
        const npc_ai::ai_response response = npc_ai::ask_openai( prompt, system );
        const npc_ai::order_intent_parse parsed = npc_ai::parse_order_intent_response( response.text );
        const bool ok = response.success && parsed.valid && parsed.intent == entry.second;
        correct += ok ? 1 : 0;
        WARN( ( ok ? "OK   " : "MISS " ) << "\"" << entry.first << "\" expected="
              << npc_ai::order_intent_name( entry.second ) << " got="
              << ( response.success ? response.text : response.error ) );
    }
    WARN( "order intent accuracy: " << correct << "/" << cases.size() );
    CHECK( correct * 100 >= static_cast<int>( cases.size() ) * 80 );
#if defined( LOCALIZE )
    set_language( previous_language );
#endif
}
