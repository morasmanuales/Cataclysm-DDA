#include "npc_ai_context.h"
#include "npc_ai_client.h"
#include "npc_ai_perception.h"
#include "npc_ai_memory.h"
#include "npc_ai_profiler.h"
#include "npc_ai_self.h"
#include "npc_ai_world_memory.h"
#include "npc_ai_watchlist.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

#include "calendar.h"
#include "cata_utility.h"
#include "character.h"
#include "game.h"
#include "item_location.h"
#include "line.h"
#include "map.h"
#include "npc.h"
#include "weather.h"
#include "output.h"
#if defined( LOCALIZE )
#include "translation_manager.h"
#endif

namespace
{

std::string context_ascii_lower( std::string text )
{
    for( char &c : text ) {
        if( c >= 'A' && c <= 'Z' ) {
            c = static_cast<char>( c - 'A' + 'a' );
        }
    }

    return text;
}

bool spanish_language_code( const std::string &language_code )
{
    return language_code == "es" || language_code.rfind( "es_", 0 ) == 0 ||
           language_code.rfind( "es-", 0 ) == 0;
}

std::string one_line_ascii_lower( std::string text )
{
    for( char &c : text ) {
        if( c == '\r' || c == '\n' || c == '\t' ) {
            c = ' ';
        } else if( c >= 'A' && c <= 'Z' ) {
            c = static_cast<char>( c - 'A' + 'a' );
        }
    }
    return " " + text + " ";
}

bool watch_context_relevant(
    const std::string &player_line
)
{
    const std::string line =
        context_ascii_lower( player_line );

    // Orden nueva relacionada con vigilancia.
    if( line.find( "avisame" ) != std::string::npos ||
        line.find( "avísame" ) != std::string::npos ||
        line.find( "avisa si" ) != std::string::npos ||
        line.find( "avisa cuando" ) != std::string::npos ) {

        return true;
    }

    // Preguntas sobre encargos ya existentes.
    if( line.find( "que te pedi" ) != std::string::npos ||
        line.find( "qué te pedí" ) != std::string::npos ||
        line.find( "que buscas" ) != std::string::npos ||
        line.find( "qué buscas" ) != std::string::npos ||
        line.find( "que estas buscando" ) != std::string::npos ||
        line.find( "qué estás buscando" ) != std::string::npos ||
        line.find( "encargo" ) != std::string::npos ||
        line.find( "busqueda" ) != std::string::npos ||
        line.find( "búsqueda" ) != std::string::npos ) {

        return true;
    }

    return false;
}

std::string build_relevant_watch_context(
    const npc &who,
    const std::string &player_line
)
{
    if( !watch_context_relevant( player_line ) ) {
        return "";
    }

    std::ostringstream output;

    output
            << "=== ENCARGOS ACTIVOS - SOLO CONTEXTO ===\n"
            << "La frase actual esta relacionada con tus encargos.\n"
            << "Usa esta informacion solo si ayuda a responderla.\n"
            << "No repitas automaticamente confirmaciones anteriores.\n"
            << npc_ai::build_watchlist_context( who )
            << "\n";

    return output.str();
}

bool world_memory_context_relevant(
    const std::string &player_line
)
{
    const std::string line =
        context_ascii_lower(
            player_line
        );


    // La memoria historica solo entra cuando el jugador
    // pregunta por recuerdos o por hechos anteriores.
    if(
        line.find( "recuerd" ) != std::string::npos ||
        line.find( "recorda" ) != std::string::npos ||
        line.find( "antes" ) != std::string::npos ||
        line.find( "pasado" ) != std::string::npos ||
        line.find( "viste" ) != std::string::npos ||
        line.find( "vimos" ) != std::string::npos ||
        line.find( "habias visto" ) != std::string::npos ||
        line.find( "habías visto" ) != std::string::npos ||
        line.find( "has visto" ) != std::string::npos ||
        line.find( "donde viste" ) != std::string::npos ||
        line.find( "dónde viste" ) != std::string::npos
    ) {

        return true;
    }


    return false;
}


std::string build_relevant_world_memory_context(
    const npc &who,
    const std::string &player_line
)
{
    if(
        !world_memory_context_relevant(
            player_line
        )
    ) {

        return "";
    }


    std::ostringstream output;


    output
            << "=== RECUERDOS HISTORICOS RELEVANTES ===\n"
            << "La pregunta actual esta relacionada con cosas "
            << "que observaste anteriormente.\n"
            << "Los siguientes datos son recuerdos reales del pasado.\n"
            << "Puedes decir que recuerdas haber visto esos objetos.\n"
            << "NO significa que sigan actualmente en ese lugar.\n"
            << "Para afirmar que algo esta visible AHORA usa "
            << "solamente tu percepcion actual.\n\n"
            << npc_ai::build_world_memory_context(
                who,
                12
            )
            << "\n";


    return output.str();
}

bool present_perception_query(
    const std::string &player_line
)
{
    // Una pregunta que habla explicitamente del pasado
    // no debe tratarse como una consulta del presente.
    if(
        lcmatch( player_line, "recuerd" ) ||
        lcmatch( player_line, "recorda" ) ||
        lcmatch( player_line, "antes" ) ||
        lcmatch( player_line, "pasado" ) ||
        lcmatch( player_line, "viste" ) ||
        lcmatch( player_line, "habias visto" )
    ) {

        return false;
    }


    // Consultas sobre hechos observables que existen AHORA. lcmatch
    // compara UTF-8 sin distinguir mayusculas y acepta consultas ASCII
    // contra texto con acentos, por ejemplo "qué" y "está".
    if(
        lcmatch( player_line, "que ves" ) ||
        lcmatch( player_line, "que objetos ves" ) ||
        lcmatch( player_line, "que puedes ver" ) ||
        lcmatch( player_line, "ves una" ) ||
        lcmatch( player_line, "ves un " ) ||
        lcmatch( player_line, "ves algun" ) ||
        lcmatch( player_line, "ves algo" ) ||
        lcmatch( player_line, "ves zomb" ) ||
        // Live scenario run 2026-09-03: "¿Hay zombis cerca?" got no
        // perception block and the model invented noises.  Anything asking
        // whether hostiles or people are around is a present-tense scan.
        lcmatch( player_line, "zombis cerca" ) ||
        lcmatch( player_line, "zombi cerca" ) ||
        lcmatch( player_line, "zombies cerca" ) ||
        lcmatch( player_line, "enemigos cerca" ) ||
        lcmatch( player_line, "alguien cerca" ) ||
        lcmatch( player_line, "algo cerca" ) ||
        lcmatch( player_line, "cerca hay" ) ||
        lcmatch( player_line, "hay zomb" ) ||
        lcmatch( player_line, "hay peligro" ) ||
        lcmatch( player_line, "hay alguien" ) ||
        lcmatch( player_line, "estamos solos" ) ||
        lcmatch( player_line, "nos siguen" ) ||
        lcmatch( player_line, "any zombies" ) ||
        lcmatch( player_line, "zombies near" ) ||
        lcmatch( player_line, "zombies around" ) ||
        lcmatch( player_line, "anyone around" ) ||
        lcmatch( player_line, "anything near" ) ||
        lcmatch( player_line, "are we alone" ) ||
        lcmatch( player_line, "hay una" ) ||
        lcmatch( player_line, "hay un " ) ||
        lcmatch( player_line, "hay algun" ) ||
        lcmatch( player_line, "hay algo" ) ||
        lcmatch( player_line, "hay enemig" ) ||
        lcmatch( player_line, "algo peligroso" ) ||
        lcmatch( player_line, "ves peligro" ) ||
        lcmatch( player_line, "esta aqui" ) ||
        lcmatch( player_line, "puedes ver" ) ||
        lcmatch( player_line, "puede ver" ) ||
        lcmatch( player_line, "encendid" ) ||
        lcmatch( player_line, "apagad" ) ||
        lcmatch( player_line, "ardiendo" ) ||
        lcmatch( player_line, "hay fuego" ) ||
        lcmatch( player_line, "abiert" ) ||
        lcmatch( player_line, "cerrad" ) ||
        lcmatch( player_line, "donde esta" )
    ) {

        return true;
    }


    // "ahora" and "actualmente" only mark the tense of a sentence.  On their
    // own they say nothing about perception, so "vámonos ahora" must not pay
    // for a full sensory scan; they count only next to a perception verb.
    if(
        lcmatch( player_line, "ahora" ) ||
        lcmatch( player_line, "actualmente" )
    ) {

        return
            lcmatch( player_line, "ves" ) ||
            lcmatch( player_line, "ver" ) ||
            lcmatch( player_line, "hay" ) ||
            lcmatch( player_line, "oyes" ) ||
            lcmatch( player_line, "escuchas" ) ||
            lcmatch( player_line, "hueles" );
    }


    return false;
}

bool present_self_query( const std::string &player_line )
{
    if( lcmatch( player_line, "recuerd" ) || lcmatch( player_line, "antes" ) ||
        lcmatch( player_line, "ayer" ) || lcmatch( player_line, "pasado" ) ) {
        return false;
    }
    return lcmatch( player_line, "como te sientes" ) ||
           lcmatch( player_line, "como estas" ) ||
           lcmatch( player_line, "como estan" ) ||
           lcmatch( player_line, "estas bien" ) ||
           lcmatch( player_line, "tienes hambre" ) ||
           lcmatch( player_line, "tienes sed" ) ||
           lcmatch( player_line, "estas cansad" ) ||
           lcmatch( player_line, "te duele" ) ||
           lcmatch( player_line, "tienes dolor" ) ||
           lcmatch( player_line, "donde te duele" ) ||
           lcmatch( player_line, "estas herid" ) ||
           lcmatch( player_line, "estas sangrando" ) ||
           lcmatch( player_line, "sangras" ) ||
           // Live scenario run 2026-09-03: these phrasings reached the model
           // with no health block at all, so a bleeding NPC answered "estoy
           // bien".  They are questions about the NPC's own body or needs.
           lcmatch( player_line, "necesitas ayuda" ) ||
           lcmatch( player_line, "necesitas algo" ) ||
           lcmatch( player_line, "necesitas descansar" ) ||
           lcmatch( player_line, "necesitas vendas" ) ||
           lcmatch( player_line, "necesitas agua" ) ||
           lcmatch( player_line, "necesitas comer" ) ||
           lcmatch( player_line, "puedes caminar" ) ||
           lcmatch( player_line, "puedes andar" ) ||
           lcmatch( player_line, "puedes correr" ) ||
           lcmatch( player_line, "puedes moverte" ) ||
           lcmatch( player_line, "puedes seguir" ) ||
           lcmatch( player_line, "puedes pelear" ) ||
           lcmatch( player_line, "aguantas" ) ||
           lcmatch( player_line, "estas mal" ) ||
           lcmatch( player_line, "te encuentras" ) ||
           lcmatch( player_line, "te sientes" ) ||
           lcmatch( player_line, "te han mordido" ) ||
           lcmatch( player_line, "te mordi" ) ||
           lcmatch( player_line, "tienes algo roto" ) ||
           lcmatch( player_line, "algo roto" ) ||
           lcmatch( player_line, "need help" ) ||
           lcmatch( player_line, "need anything" ) ||
           lcmatch( player_line, "can you walk" ) ||
           lcmatch( player_line, "can you move" ) ||
           lcmatch( player_line, "can you fight" ) ||
           lcmatch( player_line, "are you bleeding" ) ||
           lcmatch( player_line, "holding up" ) ||
           lcmatch( player_line, "how are you" ) ||
           lcmatch( player_line, "you okay" ) ||
           lcmatch( player_line, "you alright" ) ||
           lcmatch( player_line, "como tienes el brazo" ) ||
           lcmatch( player_line, "como tienes la pierna" ) ||
           lcmatch( player_line, "como tienes la cabeza" ) ||
           lcmatch( player_line, "como tienes el torso" ) ||
           lcmatch( player_line, "que te paso" ) ||
           lcmatch( player_line, "llevas comida" ) ||
           lcmatch( player_line, "llevas agua" ) ||
           lcmatch( player_line, "que llevas" ) ||
           lcmatch( player_line, "que tienes" ) ||
           lcmatch( player_line, "en las manos" ) ||
           lcmatch( player_line, "tienes municion" ) ||
           lcmatch( player_line, "que arma" ) ||
           lcmatch( player_line, "que equipo" ) ||
           lcmatch( player_line, "tu inventario" ) ||
           lcmatch( player_line, "how do you feel" ) ||
           lcmatch( player_line, "are you okay" ) ||
           lcmatch( player_line, "are you hurt" ) ||
           lcmatch( player_line, "where does it hurt" ) ||
           lcmatch( player_line, "are you hungry" ) ||
           lcmatch( player_line, "are you thirsty" ) ||
           lcmatch( player_line, "what are you carrying" ) ||
           lcmatch( player_line, "what do you have" ) ||
           lcmatch( player_line, "in your hands" ) ||
           lcmatch( player_line, "do you have ammunition" ) ||
           lcmatch( player_line, "what weapon" );
}

bool self_query_needs_inventory( const std::string &player_line )
{
    return lcmatch( player_line, "llevas comida" ) ||
           lcmatch( player_line, "llevas agua" ) ||
           lcmatch( player_line, "que llevas" ) ||
           lcmatch( player_line, "que tienes" ) ||
           lcmatch( player_line, "en las manos" ) ||
           lcmatch( player_line, "tienes municion" ) ||
           lcmatch( player_line, "que arma" ) ||
           lcmatch( player_line, "que equipo" ) ||
           lcmatch( player_line, "tu inventario" ) ||
           lcmatch( player_line, "what are you carrying" ) ||
           lcmatch( player_line, "what do you have" ) ||
           lcmatch( player_line, "in your hands" ) ||
           lcmatch( player_line, "do you have ammunition" ) ||
           lcmatch( player_line, "what weapon" );
}

bool current_situation_query( const std::string &player_line )
{
    return lcmatch( player_line, "que esta pasando" ) ||
           lcmatch( player_line, "que pasa aqui" ) ||
           lcmatch( player_line, "como esta la cosa" ) ||
           lcmatch( player_line, "estamos seguros" ) ||
           lcmatch( player_line, "que hay aqui" ) ||
           // Live scenario run 2026-09-03: "¿Es de noche?" three hours after
           // sunset was answered "es de dia" because no environment block was
           // routed.  Time, light and weather questions are situation queries.
           lcmatch( player_line, "es de noche" ) ||
           lcmatch( player_line, "es de dia" ) ||
           lcmatch( player_line, "esta oscuro" ) ||
           lcmatch( player_line, "se ve algo" ) ||
           lcmatch( player_line, "se ve bien" ) ||
           lcmatch( player_line, "que hora es" ) ||
           lcmatch( player_line, "hace frio" ) ||
           lcmatch( player_line, "hace calor" ) ||
           lcmatch( player_line, "esta lloviendo" ) ||
           lcmatch( player_line, "va a llover" ) ||
           lcmatch( player_line, "como esta el clima" ) ||
           lcmatch( player_line, "como esta el tiempo" ) ||
           lcmatch( player_line, "que tiempo hace" ) ||
           lcmatch( player_line, "donde estamos" ) ||
           lcmatch( player_line, "is it dark" ) ||
           lcmatch( player_line, "is it night" ) ||
           lcmatch( player_line, "what time is it" ) ||
           lcmatch( player_line, "is it cold" ) ||
           lcmatch( player_line, "is it raining" ) ||
           lcmatch( player_line, "where are we" ) ||
           lcmatch( player_line, "what is happening" ) ||
           lcmatch( player_line, "what is going on" ) ||
           lcmatch( player_line, "are we safe" ) ||
           lcmatch( player_line, "what is here" );
}

bool greeting_query( const std::string &player_line )
{
    return lcmatch( player_line, "hola" ) ||
           lcmatch( player_line, "buenos dias" ) ||
           lcmatch( player_line, "buenas tardes" ) ||
           lcmatch( player_line, "buenas noches" ) ||
           lcmatch( player_line, "hello" ) ||
           lcmatch( player_line, "hi " ) || player_line == "Hi" || player_line == "Hi.";
}

bool detailed_scene_query( const std::string &player_line )
{
    // A detailed scene drops the tile radius limit entirely, so it must be
    // reserved for phrasings that really ask for an exhaustive sweep.  A plain
    // "¿qué ves?" is a brief question and is served by the ordinary radius.
    return lcmatch( player_line, "dime todo lo que ves" ) ||
           lcmatch( player_line, "absolutamente todo" ) ||
           lcmatch( player_line, "que hay aqui" ) ||
           lcmatch( player_line, "mira bien alrededor" ) ||
           lcmatch( player_line, "describe todo lo que ves" ) ||
           lcmatch( player_line, "describe esta habitacion" ) ||
           lcmatch( player_line, "examina la habitacion" ) ||
           lcmatch( player_line, "analiza la escena" ) ||
           lcmatch( player_line, "describe everything" );
}

// Player and allied NPCs the speaker can actually see right now, with
// distance, so "no veo a nadie" can never be said with a friend adjacent.
std::string visible_allies_line( const npc &who )
{
    const map &here = get_map();
    std::ostringstream line;
    int listed = 0;
    const Character &player = get_player_character();
    if( who.sees( here, player.pos_bub( here ) ) ) {
        line << "el jugador (distancia " << rl_dist( who.pos_bub( here ), player.pos_bub( here ) ) << ")";
        ++listed;
    }
    for( const npc *other : g->get_npcs_if( [&]( const npc & candidate ) {
    return &candidate != &who && candidate.is_player_ally() && !candidate.is_dead() &&
           who.sees( here, candidate.pos_bub( here ) );
    } ) ) {
        if( listed >= 6 ) {
            line << ", y otros";
            break;
        }
        // Visible condition only: what a person sees at a glance, never HP
        // numbers the speaker could not know.  Round 4 had Liam announce
        // "no veo problemas graves" beside a bleeding Kim.
        static const efftype_id effect_bleed_id( "bleed" );
        // Judge by the worst limb, not the body total: one arm at 10 % is a
        // visible wound even when the rest is untouched.
        int worst_percent = 100;
        for( const bodypart_id &part : other->get_all_body_parts( get_body_part_flags::only_main ) ) {
            const int part_max = other->get_part_hp_max( part );
            if( part_max > 0 ) {
                worst_percent = std::min( worst_percent, other->get_part_hp_cur( part ) * 100 / part_max );
            }
        }
        const char *condition = worst_percent < 35 ? "malherido" : worst_percent < 75 ? "herido" :
                                "sin heridas visibles";
        line << ( listed > 0 ? ", " : "" ) << other->get_name() << " (distancia "
             << rl_dist( who.pos_bub( here ), other->pos_bub( here ) ) << ", " << condition
             << ( other->has_effect( effect_bleed_id ) ? ", sangrando" : "" ) << ")";
        ++listed;
    }
    return listed > 0 ? line.str() : "ninguno";
}

// Time of day, light and weather at the NPC's own tile.  The sensory context
// lists creatures and tiles but never says whether it is night; the live
// scenario run of 2026-09-03 had a follower claim daylight three hours after
// sunset.  Same vanilla sources the spontaneous module already reads.
std::string build_environment_context( const npc &who )
{
    const map &here = get_map();
    const tripoint_bub_ms pos = who.pos_bub( here );
    const float ambient = here.ambient_light_at( pos );
    weather_manager &weather = get_weather();
    std::ostringstream output;
    output << "\n=== ENTORNO ACTUAL (HECHOS DE CDDA) ===\n"
           << "es_de_noche=" << ( is_night( calendar::turn ) ? "si" : "no" ) << "\n"
           << "luz_ambiente=" << static_cast<int>( ambient ) << "\n"
           << "oscuro=" << ( ambient < 5.0f ? "si" : "no" ) << "\n"
           << "exterior=" << ( here.is_outside( pos ) ? "si" : "no" ) << "\n"
           << "clima_id=" << weather.weather_id.str() << "\n"
           << "temperatura=" << print_temperature( weather.get_temperature( pos ), 1 ) << "\n"
           << "- Usa estos valores para hablar de hora, luz o tiempo; no los deduzcas.\n";
    return output.str();
}

std::string build_context_for_intent( const npc &who, const std::string &player_line,
                                      const npc_ai::context_intent intent )
{
    switch( intent ) {
        case npc_ai::context_intent::greeting:
            return npc_ai::build_recent_speech_context( who, 2 );
        case npc_ai::context_intent::self_state:
            return npc_ai::render_self_snapshot( npc_ai::build_self_snapshot(
                    who, npc_ai::self_snapshot_scope::physical_state ) );
        case npc_ai::context_intent::self_inventory:
            return npc_ai::render_self_snapshot( npc_ai::build_self_snapshot(
                    who, npc_ai::self_snapshot_scope::full_inventory ) );
        case npc_ai::context_intent::perception_brief:
            return npc_ai::build_sensory_context( who, false );
        case npc_ai::context_intent::perception_detailed:
            return npc_ai::build_sensory_context( who, true );
        case npc_ai::context_intent::current_situation:
            return build_environment_context( who ) +
                   npc_ai::build_sensory_context( who, false ) +
                   npc_ai::render_self_snapshot( npc_ai::build_self_snapshot(
                           who, npc_ai::self_snapshot_scope::physical_state ) );
        case npc_ai::context_intent::memory:
            return npc_ai::build_memory_context( who, 10 ) +
                   build_relevant_world_memory_context( who, player_line );
        case npc_ai::context_intent::watch:
            return build_relevant_watch_context( who, player_line );
        case npc_ai::context_intent::spontaneous:
            return npc_ai::build_memory_context( who, 6 ) +
                   npc_ai::build_recent_speech_context( who, 4 ) +
                   npc_ai::build_sensory_context( who, false );
        case npc_ai::context_intent::npc_social:
            // Live scenario run 2026-09-03: Sarah, herself grabbed, replied to
            // "Sarah esta atrapada" with "¿donde estas atrapada?".  A reply
            // needs the speaker's own physical state, not only the scene.
            return npc_ai::build_memory_context( who, 4 ) +
                   npc_ai::build_recent_speech_context( who, 4 ) +
                   npc_ai::render_self_snapshot( npc_ai::build_self_snapshot(
                           who, npc_ai::self_snapshot_scope::physical_state ) ) +
                   npc_ai::build_sensory_context( who, false );
        case npc_ai::context_intent::general:
            return npc_ai::build_recent_speech_context( who, 4 );
    }
    return {};
}

std::string truncate_utf8( const std::string &text, const std::size_t max_bytes )
{
    if( text.size() <= max_bytes ) {
        return text;
    }
    std::size_t end = max_bytes;
    while( end > 0 && ( static_cast<unsigned char>( text[end] ) & 0xc0U ) == 0x80U ) {
        --end;
    }
    return text.substr( 0, end );
}

std::string bounded_context( const std::string &context, const std::size_t max_bytes )
{
    static const std::string marker = "\n[CONTEXTO OPCIONAL TRUNCADO POR PRESUPUESTO]\n";
    if( context.size() <= max_bytes ) {
        return context;
    }
    if( max_bytes <= marker.size() ) {
        return truncate_utf8( marker, max_bytes );
    }
    const std::size_t content_limit = max_bytes - marker.size();
    std::string prefix = truncate_utf8( context, content_limit );
    const std::size_t last_newline = prefix.rfind( '\n' );
    if( last_newline != std::string::npos ) {
        prefix.resize( last_newline + 1 );
    }
    return prefix + marker;
}


} // namespace

namespace npc_ai
{

context_intent classify_context_intent( const std::string &player_line )
{
    if( lcmatch( player_line, "[evaluacion interna de habla espontanea]" ) ) {
        return context_intent::spontaneous;
    }
    if( lcmatch( player_line, "[conversacion npc a npc]" ) ) {
        return context_intent::npc_social;
    }
    if( current_situation_query( player_line ) ) {
        return context_intent::current_situation;
    }
    if( detailed_scene_query( player_line ) ) {
        return context_intent::perception_detailed;
    }
    if( present_perception_query( player_line ) ) {
        return context_intent::perception_brief;
    }
    if( present_self_query( player_line ) ) {
        return self_query_needs_inventory( player_line ) ? context_intent::self_inventory :
               context_intent::self_state;
    }
    if( watch_context_relevant( player_line ) ) {
        return context_intent::watch;
    }
    if( world_memory_context_relevant( player_line ) ||
        lcmatch( player_line, "donde dejamos" ) ||
        lcmatch( player_line, "donde deje" ) ||
        lcmatch( player_line, "donde dejaste" ) ||
        lcmatch( player_line, "where did we leave" ) ||
        lcmatch( player_line, "where did i leave" ) ) {
        return context_intent::memory;
    }
    if( greeting_query( player_line ) ) {
        return context_intent::greeting;
    }
    return context_intent::general;
}

const char *context_intent_name( const context_intent intent )
{
    switch( intent ) {
        case context_intent::greeting:
            return "GREETING";
        case context_intent::self_state:
            return "HEALTH";
        case context_intent::self_inventory:
            return "INVENTORY_EQUIPMENT";
        case context_intent::perception_brief:
        case context_intent::perception_detailed:
            return "PERCEPTION";
        case context_intent::current_situation:
            return "CURRENT_SITUATION";
        case context_intent::memory:
            return "MEMORY";
        case context_intent::watch:
            return "WATCH";
        case context_intent::spontaneous:
            return "SPONTANEOUS";
        case context_intent::npc_social:
            return "NPC_SOCIAL";
        case context_intent::general:
            return "GENERAL";
    }
    return "UNKNOWN";
}

std::size_t context_prompt_budget_bytes( const context_intent intent )
{
    std::size_t routed_budget = 10U * 1024U;
    switch( intent ) {
        case context_intent::greeting:
        case context_intent::self_state:
            routed_budget = 8U * 1024U;
            break;
        case context_intent::self_inventory:
        case context_intent::perception_brief:
            routed_budget = 14U * 1024U;
            break;
        case context_intent::perception_detailed:
        case context_intent::spontaneous:
            routed_budget = 24U * 1024U;
            break;
        case context_intent::current_situation:
            routed_budget = 18U * 1024U;
            break;
        case context_intent::memory:
            routed_budget = 12U * 1024U;
            break;
        case context_intent::watch:
        case context_intent::general:
            routed_budget = 10U * 1024U;
            break;
        case context_intent::npc_social:
            routed_budget = 18U * 1024U;
            break;
    }
    // Reuse the existing router, but cap every route by the transport's hard
    // byte-safe context budget.  This keeps degradation before enqueue and
    // prevents Ollama from silently removing the authoritative prefix.
    return std::min( routed_budget, ollama_hard_input_budget_bytes() );
}

bool is_current_sensory_query( const std::string &player_line )
{
    return present_perception_query( player_line );
}

bool is_current_self_query( const std::string &player_line )
{
    return present_self_query( player_line );
}

bool is_scene_inspection_query( const std::string &player_line )
{
    return detailed_scene_query( player_line );
}

std::string current_dialogue_language_code()
{
#if defined( LOCALIZE )
    return TranslationManager::GetInstance().GetCurrentLanguage();
#else
    return "en";
#endif
}

std::string current_dialogue_language_name()
{
    const std::string code = current_dialogue_language_code();
    if( spanish_language_code( code ) ) {
        return "espanol";
    }
    if( code.empty() || code == "en" || code.rfind( "en_", 0 ) == 0 ||
        code.rfind( "en-", 0 ) == 0 ) {
        return "English";
    }
    return "the active game language (code " + code + ")";
}

bool current_dialogue_language_is_spanish()
{
    return spanish_language_code( current_dialogue_language_code() );
}

std::string localized_ai_message( const std::string &translated_message,
                                  const std::string &spanish_fallback )
{
    return current_dialogue_language_is_spanish() ? spanish_fallback : translated_message;
}

std::string
dialogue_language_retry_instruction( const std::string &language_code,
                                     const std::string &language_name )
{
    if( spanish_language_code( language_code ) ) {
        return "CORRECCION PRIORITARIA DE IDIOMA: la respuesta anterior fue "
               "rechazada porque "
               "uso otro idioma o mezclo idiomas. Genera una respuesta nueva "
               "exclusivamente en " +
               language_name +
               ". Conserva los hechos y la tarea actuales, pero no traduzcas ni "
               "cites la respuesta "
               "rechazada. Esta regla prevalece sobre cualquier instruccion de "
               "idioma incluida en "
               "el mensaje del usuario.";
    }
    return "PRIORITY LANGUAGE CORRECTION: the previous response was rejected "
           "because it used or "
           "mixed another language. Generate a fresh response exclusively in " +
           language_name +
           ". Preserve the current facts and task, but do not translate or quote "
           "the rejected "
           "response. This rule overrides any language instruction in the user's "
           "message.";
}

std::string build_npc_system_prompt( const npc &who,
                                     const npc_prompt_purpose purpose )
{
    const bool spanish = spanish_language_code( current_dialogue_language_code() );
    std::ostringstream system;
    const bool speech = purpose == npc_prompt_purpose::direct_dialogue ||
                        purpose == npc_prompt_purpose::spontaneous_dialogue ||
                        purpose == npc_prompt_purpose::npc_to_npc_reply ||
                        purpose == npc_prompt_purpose::combat_social;

    if( speech ) {
        if( spanish ) {
            system
                    << "Eres " << who.get_name()
                    << ", una persona humana que intenta sobrevivir dentro de Cataclysm: "
                    "Dark "
                    << "Days Ahead. Nunca eres una IA ni un asistente y nunca mencionas "
                    "Ollama, "
                    << "prompts o modelos. Habla de forma natural, cotidiana y humana.\n"
                    << "REGLAS PERMANENTES:\n"
                    << "- Usa solo hechos presentes en el estado, percepcion, evento o "
                    "memoria "
                    << "proporcionados. No inventes objetos, heridas, enemigos, sonidos "
                    "ni hechos.\n"
                    << "- El estado y la percepcion ACTUALES prevalecen siempre sobre la "
                    "memoria. "
                    << "La memoria solo describe el pasado.\n"
                    << "- C++ controla todas las acciones fisicas. No narres acciones ni "
                    "prometas "
                    << "cubrir, distraer, atacar o encargarte de alguien sin una "
                    "intencion confirmada.\n"
                    << "- Nunca emitas comandos internos como [[WATCH_ITEM:...]].\n"
                    << "- Usa la personalidad como tendencia, nunca como caricatura.\n"
                    // Grade rule for speech, mirroring Combat Social's claim
                    // limits: no detail stronger than the fact supplied.
                    << "- Regla de grado: no anadas a un hecho detalles que no esten en "
                    "los datos. Sin origen ni historia de un objeto, sin lugar para un "
                    "dolor generico, sin sonidos u olores que no esten listados, sin "
                    "pasado que no este en tu memoria.\n"
                    << "- Si tu estado lista una parte grave, critica, rota, mordida, "
                    "infectada o sangrando, nunca digas que estas bien ni la minimices.\n";
        } else {
            system
                    << "You are " << who.get_name()
                    << ", a human survivor inside Cataclysm: Dark Days Ahead. You are "
                    "never an AI "
                    << "or assistant and never mention Ollama, prompts, or language "
                    "models. Speak "
                    << "naturally as a person in this world.\n"
                    << "PERMANENT RULES:\n"
                    << "- Use only facts in the supplied state, perception, event, or "
                    "memory. Never "
                    << "invent items, injuries, enemies, sounds, or events.\n"
                    << "- CURRENT state and perception always override memory. Memory "
                    "describes only "
                    << "the past.\n"
                    << "- C++ controls every physical action. Do not narrate actions or "
                    "promise to "
                    << "cover, distract, attack, or handle someone without confirmed "
                    "intent.\n"
                    << "- Never emit internal commands such as [[WATCH_ITEM:...]].\n"
                    << "- Treat personality values as tendencies, never a caricature.\n"
                    << "- Grade rule: add no detail a fact does not carry. No origin or "
                    "history for an item, no location for generic pain, no sounds or "
                    "smells not listed, no past that is not in your memory.\n"
                    << "- If your state lists a part that is severe, critical, broken, "
                    "bitten, infected, or bleeding, never say you are fine and never "
                    "downplay it.\n";
        }

        switch( purpose ) {
            case npc_prompt_purpose::direct_dialogue:
                system
                        << ( spanish
                             ? "TAREA: responde directamente a la FRASE ACTUAL del "
                             "jugador en una a tres "
                             "frases cortas. No repitas la pregunta ni respuestas o "
                             "encargos anteriores "
                             "salvo que sean relevantes. Si no sabes algo, admítelo. "
                             "Devuelve solo las "
                             "palabras del NPC, sin nombre, comillas, etiquetas ni "
                             "explicaciones.\n"
                             : "TASK: answer the player's CURRENT LINE directly in one to "
                             "three short "
                             "sentences. Do not repeat the question or prior answers or "
                             "assignments "
                             "unless relevant. Admit when you do not know. Return only "
                             "the NPC's spoken "
                             "words, without a name, quotes, labels, or explanation.\n" );
                break;
            case npc_prompt_purpose::spontaneous_dialogue:
                system << ( spanish ? "TAREA: decide si dirias algo espontaneamente "
                            "AHORA. El silencio es normal; "
                            "no repitas clima, necesidades o ideas recientes. "
                            "Si hablas, usa una o dos "
                            "frases cortas (maximo tres). Devuelve solo "
                            "DECISION=SILENT o DECISION=TALK, "
                            "TYPE=COMMENT|JOKE|FEELING|WARNING|QUESTION|MEMORY, "
                            "QUESTION=yes|no, TEXT=<palabras dichas>. "
                            "TEXT debe ser el ultimo campo.\n"
                            : "TASK: decide whether you would naturally say "
                            "something aloud NOW. Silence "
                            "is normal; do not repeat weather, needs, or recent "
                            "ideas. If speaking, use "
                            "one or two short sentences (three maximum). Return "
                            "only DECISION=SILENT or "
                            "DECISION=TALK, "
                            "TYPE=COMMENT|JOKE|FEELING|WARNING|QUESTION|MEMORY, "
                            "QUESTION=yes|no, TEXT=<spoken words>. "
                            "TEXT must be the final field.\n" );
                break;
            case npc_prompt_purpose::npc_to_npc_reply:
                system << ( spanish ? "TAREA: otro NPC visible acaba de hablar. Decide si "
                            "respondes una sola vez. "
                            "Puedes devolver DECISION=SILENT; si respondes, "
                            "devuelve solo DECISION=TALK "
                            "y TEXT=<respuesta breve>. No estas respondiendo al "
                            "jugador. Si la frase habla de ti o de tu situacion, "
                            "responde desde tu propio estado real, no preguntes por "
                            "algo que te esta pasando a ti. No des instrucciones "
                            "sobre acciones que el otro no ha hecho.\n"
                            : "TASK: another visible NPC has just spoken. Decide "
                            "whether to reply once. "
                            "You may return DECISION=SILENT; when replying, "
                            "return only DECISION=TALK "
                            "and TEXT=<short reply>. You are not answering the "
                            "player. If the line is about you or your situation, "
                            "answer from your own real state instead of asking "
                            "about something happening to you. Give no instructions "
                            "about actions the other has not taken.\n" );
                break;
            case npc_prompt_purpose::combat_social:
                system << ( spanish ? "TAREA: decide solo si dices algo durante el "
                            "combate. El silencio es normal. "
                            "No controlas movimiento, combate, blancos ni "
                            "acciones. Usa los nombres "
                            "traducidos y entity_type exactos; nunca conviertas "
                            "un monstruo en bandido, "
                            "soldado o persona. Puedes pedir ayuda si estas "
                            "agarrado, sangrando o rodeado, "
                            "pero pedirla no confirma una accion. Devuelve solo "
                            "DECISION=SILENT o "
                            "DECISION=TALK seguido de TEXT=<una frase corta, "
                            "como maximo dos>.\n"
                            : "TASK: decide only whether to speak during combat. "
                            "Silence is normal. You do "
                            "not control movement, combat, targets, or actions. "
                            "Use exact translated "
                            "names and entity_type. Never reinterpret a monster "
                            "as a bandit, soldier, or "
                            "person. You may ask for help when grabbed, "
                            "bleeding, or surrounded, but a "
                            "request does not confirm an action. Return only "
                            "DECISION=SILENT or "
                            "DECISION=TALK followed by TEXT=<one short "
                            "sentence, two maximum>.\n" );
                break;
            case npc_prompt_purpose::watch_resolution:
            case npc_prompt_purpose::pickup_resolution:
            case npc_prompt_purpose::wield_resolution:
            case npc_prompt_purpose::order_resolution:
                break;
        }

        if( purpose == npc_prompt_purpose::combat_social ) {
            system << ( spanish ?
                        "REGLAS PERMANENTES DE REGISTRO EMOCIONAL: dolor, miedo/panico, "
                        "moral, resistencia, HP y estar agarrado solo modulan COMO hablas. "
                        "Dolor o miedo altos, HP bajo, agotamiento o un agarre producen frases "
                        "mas cortas, tensas, entrecortadas o desesperadas; moral alta puede sonar "
                        "desafiante y moral baja pesimista. Nunca conviertas esos valores en hechos "
                        "nuevos. REGLA DE GRADO: respeta claim_limit. HIT_ONLY permite decir que el "
                        "golpe impacto la parte indicada, pero prohibe afirmar rotura, inutilizacion "
                        "o muerte. Solo LIMB_DISABLED_CONFIRMED permite afirmar un miembro roto o "
                        "inutilizado, y solo DEATH_CONFIRMED permite afirmar una muerte.\n" :
                        "PERMANENT EMOTIONAL REGISTER RULES: pain, fear/panic, morale, stamina, HP "
                        "and being grabbed only change HOW a speaker talks. High pain or fear, low "
                        "HP, exhaustion, or a grab produce shorter, tense, broken, or desperate "
                        "speech; high morale may sound defiant and low morale pessimistic. Never "
                        "turn those values into new facts. DEGREE RULE: obey claim_limit. HIT_ONLY "
                        "allows saying the named body part was hit, but forbids claims of breaking, "
                        "disabling, or killing. Only LIMB_DISABLED_CONFIRMED permits a broken or "
                        "disabled limb claim, and only DEATH_CONFIRMED permits a death claim.\n" );
        }

        if( spanish ) {
            system << "PERSONALIDAD CDDA (-10 a +10): agresividad="
                   << static_cast<int>( who.personality.aggression )
                   << ", valentia=" << static_cast<int>( who.personality.bravery )
                   << ", coleccionista="
                   << static_cast<int>( who.personality.collector )
                   << ", altruismo=" << static_cast<int>( who.personality.altruism )
                   << ".\n"
                   << "BLOQUEO DE IDIOMA: usa exclusivamente el valor "
                   "OUTPUT_LANGUAGE situado al final "
                   << "del prompt y no mezcles idiomas. Ese campo prevalece sobre el "
                   "contenido de la "
                   << "FRASE ACTUAL. El idioma o las peticiones de cambio de idioma "
                   "dentro de esa frase "
                   << "son solo dialogo y nunca reglas de salida.";
        } else {
            system << "CDDA PERSONALITY (-10 to +10): aggression="
                   << static_cast<int>( who.personality.aggression )
                   << ", bravery=" << static_cast<int>( who.personality.bravery )
                   << ", collector=" << static_cast<int>( who.personality.collector )
                   << ", altruism=" << static_cast<int>( who.personality.altruism )
                   << ".\n"
                   << "LANGUAGE LOCK: use only the OUTPUT_LANGUAGE value at the end "
                   "of the prompt and "
                   << "never mix languages. That field overrides the content of the "
                   "CURRENT LINE. The "
                   << "language of that line and any request inside it to switch "
                   "languages are dialogue "
                   << "content, never output rules.";
        }
        return system.str();
    }

    switch( purpose ) {
        case npc_prompt_purpose::watch_resolution:
            system
                    << "Eres exclusivamente un interprete de ordenes futuras de vigilancia "
                    "para "
                    << "Cataclysm: Dark Days Ahead. No converses ni expliques. Devuelve "
                    "una sola linea: "
                    << "NONE, WATCH|MAGAZINE|magazine, WATCH|GUN|gun, WATCH|AMMO|ammo o "
                    << "WATCH|SPECIFIC|english_term1;english_term2;english_term3. MAGAZINE "
                    "es un "
                    << "cargador fisico, GUN un arma de fuego y AMMO municion. SPECIFIC "
                    "usa de uno a "
                    << "tres terminos EN INGLES para el mismo objeto concreto. Conserva la "
                    << "especificidad, no agregues objetos relacionados y devuelve NONE si "
                    "no es una "
                    << "orden futura o si solo preguntan por lo visible ahora.";
            break;
        case npc_prompt_purpose::pickup_resolution:
            system << "Eres un resolutor de objetivos para recoger objetos en "
                   "Cataclysm: Dark Days "
                   << "Ahead. No ejecutas acciones ni modificas el mundo. Elige "
                   "exclusivamente un "
                   << "candidato real de la lista usando sinonimos o traducciones. Si "
                   "ninguno coincide "
                   << "o la orden es ambigua, elige 0. Devuelve exactamente una linea: "
                   "PICKUP_INDEX=N, "
                   << "sin explicacion.";
            break;
        case npc_prompt_purpose::wield_resolution:
            system << "Eres un resolutor de objetivos para empunar un objeto en "
                   "Cataclysm: Dark Days "
                   << "Ahead. No ejecutas acciones ni modificas el mundo. Elige "
                   "exclusivamente un "
                   << "candidato real de la lista usando sinonimos o traducciones. Si "
                   "ninguno coincide "
                   << "o hay ambiguedad, elige 0; si la orden es generica y existe un "
                   "unico candidato "
                   << "razonable, puedes elegirlo. Devuelve exactamente una linea: "
                   "WIELD_INDEX=N, sin "
                   << "explicacion.";
            break;
        case npc_prompt_purpose::order_resolution:
            system << ( spanish ?
                        "Eres exclusivamente un clasificador de ordenes habladas para companeros "
                        "de Cataclysm: Dark Days Ahead. No conversas, no explicas, no ejecutas nada. "
                        "Recibes una frase del jugador y un catalogo cerrado; eliges una entrada "
                        "del catalogo o NONE. Una pregunta, una charla, una opinion, una promesa o "
                        "un plan para mas tarde son NONE. Solo un imperativo claro y presente "
                        "dirigido al companero es una orden. Ante cualquier duda, NONE. Nunca "
                        "inventes objetos ni anadas texto fuera de las dos lineas pedidas." :
                        "You are strictly a spoken-order classifier for Cataclysm: Dark Days Ahead "
                        "companions. You do not converse, explain, or execute anything. You get one "
                        "player line and a closed catalogue; pick one catalogue entry or NONE. A "
                        "question, chat, opinion, promise or plan for later is NONE. Only a clear, "
                        "present-tense imperative aimed at the companion is an order. When in any "
                        "doubt, NONE. Never invent items or add text beyond the two requested lines." );
            break;
        case npc_prompt_purpose::direct_dialogue:
        case npc_prompt_purpose::spontaneous_dialogue:
        case npc_prompt_purpose::npc_to_npc_reply:
        case npc_prompt_purpose::combat_social:
            break;
    }
    return system.str();
}

bool generated_text_matches_dialogue_language(
    const std::string &text, const std::string &language_code )
{
    if( !spanish_language_code( language_code ) ) {
        return true;
    }

    // Monster/item names may legitimately remain untranslated.  Detect grammar
    // through common function words instead of treating every foreign token as
    // English ("boomer", item names and proper names remain valid).
    static constexpr std::array<std::string_view, 27> english_clauses = {
        " what in ",      " this is ",   " it's ",       " it is ",
        " there is ",     " there are ", " we need ",    " we have ",
        " let's ",        " i am ",      " i'm ",        " i'll ",
        " i will ",       " watch out ", " get out ",    " goddamn ",
        " horror movie ", " the hell ",  " seems like ", " coast is clear ",
        " c'mon ",        " got it",     " sounds good", " all right",
        " be careful",    " thank you",  " of course"
    };
    const std::string lower = one_line_ascii_lower( text );
    if( std::any_of( english_clauses.begin(), english_clauses.end(),
    [&]( const std::string_view clause ) {
    return lower.find( clause ) != std::string::npos;
    } ) ) {
        return false;
    }

    static const std::unordered_set<std::string> english_words = {
        "a",         "an",      "and",      "are",       "as",    "at",
        "back",      "bandage", "bandages", "bandaging", "be",    "better",
        "bloodbath", "can",     "clear",    "colleague", "could", "damn",
        "do",        "for",     "from",     "get",       "go",    "good",
        "got",       "have",    "hey",      "hope",      "i",     "if",
        "in",        "is",      "it",       "looks",     "make",  "move",
        "my",        "of",      "on",       "over",      "pass",  "seems",
        "someone",   "sounds",  "stay",     "that",      "the",   "their",
        "there",     "this",    "through",  "to",        "we",    "what",
        "with",      "you",     "your",     "c'mon",     "watch", "out"
    };
    static const std::unordered_set<std::string> strong_english_words = {
        "agreed", "careful", "fine",   "hello",      "okay", "ready",
        "sorry",  "sure",    "thanks", "understood", "yeah", "yep"
    };
    static const std::unordered_set<std::string> spanish_words = {
        "a",    "al",  "algo",  "con",  "contra", "de",  "del", "el",
        "ella", "en",  "ese",   "esta", "esto",   "hay", "la",  "las",
        "lo",   "los", "me",    "mi",   "mierda", "no",  "nos", "para",
        "pero", "por", "que",   "se",   "si",     "su",  "te",  "tenemos",
        "un",   "una", "vamos", "ya",   "yo"
    };
    int english_count = 0;
    int spanish_count = 0;
    int strong_english_count = 0;
    std::string word;
    const auto count_word = [&]() {
        if( word.empty() ) {
            return;
        }
        english_count += english_words.count( word ) != 0 ? 1 : 0;
        spanish_count += spanish_words.count( word ) != 0 ? 1 : 0;
        strong_english_count += strong_english_words.count( word ) != 0 ? 1 : 0;
        word.clear();
    };
    for( const unsigned char c : lower ) {
        if( ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '\'' ) {
            word.push_back( static_cast<char>( c ) );
        } else {
            count_word();
        }
    }
    count_word();
    return strong_english_count == 0 && english_count < 3 &&
           !( english_count >= 2 && english_count > spanish_count );
}

std::string dialogue_language_fallback( const std::string &event_kind,
                                        const bool combat_active )
{
    const bool spanish = spanish_language_code( current_dialogue_language_code() );
    if( event_kind == "COMBAT_END" ) {
        return spanish ? "Parece que ya no hay amenazas a la vista."
               : "It looks like there are no threats in sight.";
    }
    if( event_kind == "ENEMY_KILLED" ) {
        return spanish ? "Cayó." : "It's down.";
    }
    if( event_kind.find( "HIT" ) != std::string::npos ||
        event_kind.find( "HURT" ) != std::string::npos ||
        event_kind.find( "BLEED" ) != std::string::npos ||
        event_kind.find( "GRABBED" ) != std::string::npos ||
        event_kind.find( "SURROUNDED" ) != std::string::npos ) {
        return spanish ? "¡Cuidado!" : "Watch out!";
    }
    if( combat_active ) {
        return spanish ? "¡Atentos!" : "Stay alert!";
    }
    if( event_kind == "NPC_TO_NPC_REPLY" ) {
        return spanish ? "Te escucho." : "I'm listening.";
    }
    return spanish ? "No sé cómo decirlo." : "I don't know how to put it.";
}

std::string build_npc_prompt( const npc &who, const std::string &player_line,
                              const npc_prompt_purpose purpose,
                              const std::size_t reserved_suffix_bytes )
{
    scoped_profile profile( profile_subsystem::context );
    std::ostringstream prompt;
    const context_intent intent = classify_context_intent( player_line );
    std::string bounded_player_line = truncate_utf8( player_line, 2048 );
    if( bounded_player_line.size() != player_line.size() ) {
        bounded_player_line += " [FRASE TRUNCADA POR PRESUPUESTO]";
    }

    const item_location wielded = who.get_wielded_item();

    const std::string wielded_name = wielded ? remove_color_tags( wielded->tname() ) : "nada";

    prompt << "=== ESTADO REAL DEL NPC ===\n"
           << "Nombre: " << who.get_name() << "\n"
           << "Estado actual: " << who.get_current_status() << "\n"
           << "Actividad actual: " << who.get_current_activity() << "\n"
           << "Aliado del jugador: " << ( who.is_player_ally() ? "si" : "no" )
           << "\n"
           << "Objeto o arma empunada: " << wielded_name << "\n"
           // Live scenario run 2026-09-03: with only this header routed, the
           // model invented where the bat came from.  State the gap outright.
           << "Origen de tus objetos: desconocido (no inventes donde los conseguiste)\n"
           // Live scenario run 2026-09-03: on a HEALTH-only route Liam said
           // "no veo a los demas" with Kim adjacent.  Allies in sight are cheap
           // to list and belong in every prompt, whatever the route.
           << "Companeros visibles ahora: " << visible_allies_line( who ) << "\n"
           << "Relacion con jugador: confianza=" << who.op_of_u.trust
           << "; miedo=" << who.op_of_u.fear << "; valor=" << who.op_of_u.value
           << "; ira=" << who.op_of_u.anger << "\n\n";

    const std::string routed_context =
        build_context_for_intent( who, player_line, intent );
    std::ostringstream suffix;
    suffix << "\n";
    if( purpose == npc_prompt_purpose::spontaneous_dialogue ) {
        suffix << "=== DISPARADOR INTERNO ACTUAL ===\n";
    } else if( purpose == npc_prompt_purpose::npc_to_npc_reply ) {
        suffix << "=== MENSAJE OIDO ACTUAL ===\n";
    } else {
        suffix << "=== CONVERSACION ACTUAL ===\nEl jugador dice:\n";
    }
    suffix << bounded_player_line;
    if( purpose != npc_prompt_purpose::spontaneous_dialogue ) {
        suffix << "\n\nOUTPUT_LANGUAGE=" << current_dialogue_language_name();
    }

    const std::string fixed = prompt.str();
    const std::string ending = suffix.str();
    const std::size_t total_budget = context_prompt_budget_bytes( intent );
    const std::size_t system_bytes = build_npc_system_prompt( who, purpose ).size();
    const std::size_t reserved = system_bytes + reserved_suffix_bytes;
    const std::size_t budget = total_budget > reserved ? total_budget - reserved : 0;
    if( budget == 0 ) {
        return {};
    }
    if( fixed.size() + ending.size() >= budget ) {
        return bounded_context( fixed, budget - std::min( budget, ending.size() ) ) +
               truncate_utf8( ending, budget );
    }
    return fixed +
           bounded_context( routed_context,
                            budget - fixed.size() - ending.size() ) +
           ending;
}

} // namespace npc_ai
