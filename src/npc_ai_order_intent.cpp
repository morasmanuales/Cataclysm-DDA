#include "npc_ai_order_intent.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

#include "debug.h"
#include "messages.h"
#include "npc.h"
#include "npc_ai_async.h"
#include "npc_ai_context.h"
#include "npc_ai_equipment.h"
#include "npc_ai_interior.h"
#include "npc_ai_memory.h"
#include "npc_ai_pickup.h"
#include "npc_ai_rescue.h"
#include "npc_ai_tactical.h"
#include "npc_ai_wield.h"
#include "npctalk.h"
#include "output.h"
#include "string_formatter.h"
#include "translations.h"

namespace npc_ai
{

namespace
{

std::string lowercase_ascii( std::string text )
{
    for( char &c : text ) {
        c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
    }
    return text;
}

std::string trim( const std::string &text )
{
    const std::size_t first = text.find_first_not_of( " \t\r\n\"'" );
    if( first == std::string::npos ) {
        return std::string();
    }
    const std::size_t last = text.find_last_not_of( " \t\r\n\"'." );
    return last < first ? std::string() : text.substr( first, last - first + 1 );
}

std::size_t word_count( const std::string &text )
{
    std::istringstream stream( text );
    std::string word;
    std::size_t count = 0;
    while( stream >> word ) {
        ++count;
    }
    return count;
}

order_intent intent_from_id( const std::string &id )
{
    static const std::vector<std::pair<const char *, order_intent>> ids = {
        { "none", order_intent::none },
        { "follow", order_intent::follow },
        { "guard", order_intent::guard },
        { "enter_interior", order_intent::enter_interior },
        { "pickup", order_intent::pickup },
        { "wield", order_intent::wield },
        { "drop", order_intent::drop },
        { "wear", order_intent::wear },
    };
    for( const auto &entry : ids ) {
        if( id == entry.first ) {
            return entry.second;
        }
    }
    return order_intent::none;
}

bool known_intent_id( const std::string &id )
{
    return id == "none" || id == "follow" || id == "guard" || id == "enter_interior" ||
           id == "pickup" || id == "wield" || id == "drop" || id == "wear";
}

void forward_to_dialogue( npc &who, const std::vector<npc *> &targets, const bool everyone,
                          const std::string &player_line )
{
    if( everyone && targets.size() > 1 ) {
        enqueue_group_ai_dialogue( targets, player_line, next_conversation_turn_id() );
        return;
    }
    enqueue_direct_dialogue( who, player_line, build_npc_prompt( who, player_line ) );
}

void speak_and_remember( npc &who, const std::string &player_line, const std::string &reply )
{
    say_command_reply( who, reply );
    remember_exchange( who, player_line, reply );
}

} // namespace

const char *order_intent_name( const order_intent intent )
{
    switch( intent ) {
        case order_intent::none:
            return "NONE";
        case order_intent::follow:
            return "FOLLOW";
        case order_intent::guard:
            return "GUARD";
        case order_intent::enter_interior:
            return "ENTER_INTERIOR";
        case order_intent::pickup:
            return "PICKUP";
        case order_intent::wield:
            return "WIELD";
        case order_intent::drop:
            return "DROP";
        case order_intent::wear:
            return "WEAR";
    }
    return "NONE";
}

bool order_intent_candidate( const std::string &player_line )
{
    if( player_line.empty() || player_line.size() > 120 ) {
        return false;
    }
    // Questions are never orders; the context router already serves them.
    if( player_line.find( '?' ) != std::string::npos ||
        player_line.find( "\xC2\xBF" ) != std::string::npos ) {
        return false;
    }
    const std::size_t words = word_count( player_line );
    if( words == 0 || words > 10 ) {
        return false;
    }
    // Health, perception, memory, greetings and watch orders have their own
    // routes with real context; only the unrouted remainder is worth a
    // classifier round trip.
    return classify_context_intent( player_line ) == context_intent::general;
}

std::string build_order_intent_prompt( const std::string &player_line, const bool everyone,
                                       const std::size_t ally_count )
{
    const bool spanish = current_dialogue_language_is_spanish();
    std::ostringstream prompt;
    if( spanish ) {
        prompt << "FRASE DEL JUGADOR: \"" << player_line << "\"\n"
               << "DESTINATARIOS: " << ( everyone ? "todos los companeros (" +
                                        std::to_string( ally_count ) + ")" : "un companero" ) << "\n\n"
               << "CATALOGO DE ORDENES EJECUTABLES:\n"
               << "FOLLOW = venir con el jugador, seguirlo, no quedarse atras, moverse con el.\n"
               << "GUARD = quedarse quieto, esperar, vigilar o mantener la posicion actual.\n"
               << "ENTER_INTERIOR = entrar o refugiarse dentro de un edificio cercano.\n"
               << "PICKUP = recoger o coger un objeto del suelo. TARGET=nombre del objeto.\n"
               << "WIELD = empunar, sacar o preparar un arma u objeto. TARGET=nombre.\n"
               << "DROP = soltar, dejar o tirar algo que lleva. TARGET=nombre.\n"
               << "WEAR = ponerse una prenda, mochila o equipo. TARGET=nombre.\n"
               << "NONE = no es una orden ejecutable ahora: pregunta, charla, opinion, "
               "comentario, promesa, plan futuro, broma o peticion de informacion.\n\n"
               << "REGLAS: solo un imperativo claro dirigido al companero cuenta como orden. "
               "Ante duda, NONE. No inventes objetos: TARGET copia las palabras del jugador.\n"
               << "Devuelve exactamente dos lineas y nada mas:\n"
               << "ORDER=<FOLLOW|GUARD|ENTER_INTERIOR|PICKUP|WIELD|DROP|WEAR|NONE>\n"
               << "TARGET=<palabras del objeto o vacio>\n";
    } else {
        prompt << "PLAYER LINE: \"" << player_line << "\"\n"
               << "ADDRESSEES: " << ( everyone ? "all companions (" + std::to_string( ally_count ) + ")" :
                                     "one companion" ) << "\n\n"
               << "EXECUTABLE ORDER CATALOGUE:\n"
               << "FOLLOW = come with the player, follow, keep up, move with them.\n"
               << "GUARD = stay put, wait, watch or hold the current position.\n"
               << "ENTER_INTERIOR = get inside or shelter in a nearby building.\n"
               << "PICKUP = pick up or grab an item from the ground. TARGET=item words.\n"
               << "WIELD = wield, draw or ready a weapon or item. TARGET=item words.\n"
               << "DROP = drop, leave or throw away something carried. TARGET=item words.\n"
               << "WEAR = put on clothing, a backpack or gear. TARGET=item words.\n"
               << "NONE = not an executable order right now: question, chat, opinion, "
               "comment, promise, future plan, joke or request for information.\n\n"
               << "RULES: only a clear imperative aimed at the companion counts as an order. "
               "When in doubt, NONE. Never invent items: TARGET copies the player's words.\n"
               << "Return exactly two lines and nothing else:\n"
               << "ORDER=<FOLLOW|GUARD|ENTER_INTERIOR|PICKUP|WIELD|DROP|WEAR|NONE>\n"
               << "TARGET=<item words or empty>\n";
    }
    return prompt.str();
}

order_intent_parse parse_order_intent_response( const std::string &text )
{
    order_intent_parse result;
    const std::string lower = lowercase_ascii( text );
    const std::size_t order_at = lower.find( "order=" );
    if( order_at == std::string::npos ) {
        return result;
    }
    std::size_t end = order_at + 6;
    while( end < lower.size() && ( std::isalnum( static_cast<unsigned char>( lower[end] ) ) ||
                                   lower[end] == '_' ) ) {
        ++end;
    }
    const std::string id = lower.substr( order_at + 6, end - order_at - 6 );
    if( !known_intent_id( id ) ) {
        return result;
    }
    result.valid = true;
    result.intent = intent_from_id( id );

    const std::size_t target_at = lower.find( "target=" );
    if( target_at != std::string::npos ) {
        std::size_t line_end = text.find_first_of( "\r\n", target_at );
        if( line_end == std::string::npos ) {
            line_end = text.size();
        }
        result.target = trim( text.substr( target_at + 7, line_end - target_at - 7 ) );
        const std::string lower_target = lowercase_ascii( result.target );
        if( lower_target == "vacio" || lower_target == "empty" || lower_target == "none" ||
            lower_target == "ninguno" || lower_target == "-" ) {
            result.target.clear();
        }
    }
    return result;
}

bool enqueue_order_intent_resolution( const npc &who, const std::string &player_line,
                                      const bool everyone, const std::size_t ally_count )
{
    const ai_enqueue_result queued = enqueue_command_resolution(
                                         who, ai_request_type::order_resolution, player_line,
                                         build_order_intent_prompt( player_line, everyone, ally_count ),
                                         {}, acquisition_intent::automatic, "",
                                         everyone ? "everyone" : "single" );
    return queued.accepted;
}

void apply_order_intent_completion( npc &who, const ai_request_completion &completion )
{
    const std::string &player_line = completion.request.player_line;
    const bool everyone = completion.request.event_detail == "everyone";
    std::vector<npc *> targets = everyone ? get_nearby_ai_talkers( true ) : std::vector<npc *>{};
    if( targets.empty() ) {
        targets.push_back( &who );
    }

    order_intent_parse parsed;
    if( completion.response.success ) {
        parsed = parse_order_intent_response( completion.response.text );
    }
    add_msg_debug( debugmode::DF_NPC_ITEMAI,
                   "%s ORDER_INTENT=%s VALID=%s TARGET=\"%s\" LINE=\"%s\"",
                   who.get_name(), order_intent_name( parsed.intent ), parsed.valid ? "yes" : "no",
                   parsed.target, player_line );

    if( !parsed.valid || parsed.intent == order_intent::none ) {
        forward_to_dialogue( who, targets, everyone, player_line );
        return;
    }

    switch( parsed.intent ) {
        case order_intent::follow:
        case order_intent::guard: {
            cancel_rescues_for_new_order( targets, "superseded by a tactical order" );
            const tactical_order order = parsed.intent == order_intent::guard ?
                                         tactical_order::guard : tactical_order::follow;
            const tactical_order_result result = execute_tactical_order( targets, order );
            if( result.affected.empty() ) {
                add_msg( m_info, _( "No eligible ally received that order." ) );
                return;
            }
            npc *speaker = result.affected.front();
            speak_and_remember( *speaker, player_line,
                                order == tactical_order::guard ? _( "I'll guard this position." ) :
                                _( "I'll follow you." ) );
            return;
        }
        case order_intent::enter_interior: {
            const interior_order_result interior =
                execute_enter_nearest_reachable_safe_interior( targets );
            if( interior.success ) {
                speak_and_remember( who, player_line, interior.message );
            } else {
                add_msg( m_info, interior.message );
            }
            return;
        }
        case order_intent::pickup: {
            if( parsed.target.empty() ) {
                break;
            }
            const pickup_command_result result =
                try_handle_pickup_command( who, "Recoge " + parsed.target + "." );
            if( !result.handled ) {
                break;
            }
            if( !result.pending && !result.message.empty() ) {
                speak_and_remember( who, player_line, result.message );
            }
            return;
        }
        case order_intent::wield: {
            if( parsed.target.empty() ) {
                break;
            }
            const wield_command_result result =
                try_handle_wield_command( who, "Empuna " + parsed.target + "." );
            if( !result.handled ) {
                break;
            }
            if( !result.pending && !result.message.empty() ) {
                speak_and_remember( who, player_line, result.message );
            }
            return;
        }
        case order_intent::drop:
        case order_intent::wear: {
            if( parsed.target.empty() ) {
                break;
            }
            const std::string canonical = ( parsed.intent == order_intent::drop ? "Suelta " : "Ponte " ) +
                                          parsed.target + ".";
            const equipment_command_result result = try_handle_equipment_command( who, canonical );
            if( !result.handled ) {
                break;
            }
            if( !result.message.empty() ) {
                speak_and_remember( who, player_line, result.message );
            }
            return;
        }
        case order_intent::none:
            break;
    }
    // The classifier named an order the deterministic handlers could not map
    // to anything real (no such object, unsupported phrasing).  Never guess:
    // the line becomes ordinary dialogue and the NPC can say so in character.
    forward_to_dialogue( who, targets, everyone, player_line );
}

} // namespace npc_ai
