#include "npc_ai_action_parser.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <filesystem>
#include <fstream>
#include <map>

#include "cata_utility.h"
#include "catacharset.h"
#include "npc.h"
#include "path_info.h"
#include "npc_ai_async.h"
#include "npc_ai_context.h"
#include "npc_ai_item_catalog.h"
#include "npc_ai_debug.h"
#include "npc_ai_memory.h"
#include "npc_ai_watchlist.h"
#include "translations.h"
#include "unicode.h"

namespace
{

std::string trim_copy(
    const std::string &text
)
{
    const std::size_t first =
        text.find_first_not_of(
            " \t\r\n"
        );

    if( first == std::string::npos ) {
        return "";
    }

    const std::size_t last =
        text.find_last_not_of(
            " \t\r\n"
        );

    return text.substr(
               first,
               last - first + 1
           );
}

std::string lower_ascii(
    std::string text
)
{
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        []( unsigned char c ) {
            return static_cast<char>(
                       std::tolower( c )
                   );
        }
    );

    return text;
}

std::string upper_ascii(
    std::string text
)
{
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        []( unsigned char c ) {
            return static_cast<char>(
                       std::toupper( c )
                   );
        }
    );

    return text;
}

std::vector<std::string> split(
    const std::string &text,
    char separator
)
{
    std::vector<std::string> result;

    std::stringstream stream( text );
    std::string token;

    while( std::getline(
               stream,
               token,
               separator
           ) ) {

        token = trim_copy( token );

        if( !token.empty() ) {
            result.push_back(
                token
            );
        }
    }

    return result;
}

bool looks_like_watch_request(
    const std::string &line
)
{
    const std::string text =
        lower_ascii( line );

    return
        text.find( "avisa" ) != std::string::npos ||
        text.find( "avís" ) != std::string::npos ||
        text.find( "dime si" ) != std::string::npos ||
        text.find( "cuando ve" ) != std::string::npos ||
        text.find( "si ves" ) != std::string::npos ||
        text.find( "si veas" ) != std::string::npos ||
        text.find( "si encuent" ) != std::string::npos ||
        text.find( "cuando encuent" ) != std::string::npos ||
        text.find( "let me know" ) != std::string::npos ||
        text.find( "tell me if" ) != std::string::npos ||
        text.find( "notify me" ) != std::string::npos;
}

std::string sanitize_control_term(
    std::string text
)
{
    for( char &c : text ) {

        if( c == '|' ||
            c == '[' ||
            c == ']' ||
            c == '\r' ||
            c == '\n' ) {

            c = ' ';
        }
    }

    return trim_copy( text );
}

void debug_log(
    const npc_ai::watch_action_result &result,
    const std::string &player_line
)
{
    npc_ai::debug_stream output( "npc_ai_watch_debug.log" );

    if( !output ) {
        return;
    }

    output
        << "\n========================================\n"
        << "PLAYER: "
        << player_line
        << "\n"
        << "PARSER_RAW: "
        << result.raw_output
        << "\n"
        << "ATTEMPTED: "
        << ( result.attempted ? "YES" : "NO" )
        << "\n"
        << "SUCCESS: "
        << ( result.success ? "YES" : "NO" )
        << "\n"
        << "WATCH: "
        << ( result.is_watch ? "YES" : "NO" )
        << "\n"
        << "KIND: "
        << result.kind
        << "\n"
        << "CATALOG_TYPES: "
        << result.catalog_size
        << "\n";

    output << "TERMS:";

    for( const std::string &term :
         result.terms ) {

        output
            << " ["
            << term
            << "]";
    }

    output << "\nCANDIDATES:";

    for( const std::string &candidate :
         result.candidates ) {

        output
            << "\n  - "
            << candidate;
    }

    output
        << "\nCONTROL: "
        << result.control_marker
        << "\n";
}

} // namespace

namespace npc_ai
{

// The object words of the player's own watch request, lowercased and
// without accents: "Avísame si ves unos guantes" -> { "guantes" }.  They are
// added to the selector next to the model's English terms, so translated
// item names match even when neither the id nor the name contains the
// English word (brigandine "hands" gloves, for instance).
static std::vector<std::string> player_watch_words( const std::string &player_line )
{
    static const std::vector<std::string> filler = {
        "avisame", "avisa", "avisen", "avisadme", "dime", "diganme", "decidme", "cuando", "veas",
        "ves", "vean", "vea", "encuentres", "encuentras", "encuentren", "encontres", "vez", "por",
        "favor", "algo", "algun", "alguna", "algunos", "algunas", "unos", "unas", "una", "los",
        "las", "del", "que", "para", "con", "sin", "mis", "tus", "sus", "let", "know", "tell",
        "notify", "warn", "you", "see", "find", "spot", "any", "some", "the", "when", "whenever",
        "please", "there", "around", "nearby"
    };
    std::u32string codepoints = utf8_to_utf32( player_line );
    for( char32_t &codepoint : codepoints ) {
        u32_to_lowercase( codepoint );
        remove_accent( codepoint );
    }
    std::string normalized;
    for( const char32_t codepoint : codepoints ) {
        if( ( codepoint >= U'a' && codepoint <= U'z' ) ||
            ( codepoint >= U'0' && codepoint <= U'9' ) ) {
            normalized.push_back( static_cast<char>( codepoint ) );
        } else if( codepoint > 127 ) {
            normalized += utf32_to_utf8( codepoint );
        } else {
            normalized.push_back( ' ' );
        }
    }
    std::vector<std::string> words;
    std::istringstream stream( normalized );
    std::string word;
    while( stream >> word ) {
        if( word.size() < 3 ||
            std::find( filler.begin(), filler.end(), word ) != filler.end() ||
            std::find( words.begin(), words.end(), word ) != words.end() ) {
            continue;
        }
        words.push_back( word );
    }
    return words;
}

// Synonym groups from data/npc_ai/watch_synonyms.txt: "guantes, gloves, glove".
static const std::map<std::string, std::vector<std::string>> &watch_synonym_table()
{
    static std::map<std::string, std::vector<std::string>> table;
    static bool loaded = false;
    if( loaded ) {
        return table;
    }
    loaded = true;
    const std::filesystem::path path =
        std::filesystem::u8path( PATH_INFO::datadir() ) / "npc_ai" / "watch_synonyms.txt";
    std::ifstream input( path, std::ios::binary );
    if( !input.is_open() ) {
        return table;
    }
    std::string line;
    while( std::getline( input, line ) ) {
        const std::size_t hash = line.find( '#' );
        if( hash != std::string::npos ) {
            line.erase( hash );
        }
        std::vector<std::string> group;
        std::string entry;
        std::istringstream fields( line );
        while( std::getline( fields, entry, ',' ) ) {
            // Each entry is normalised with the same rules as the request.
            const std::vector<std::string> words = player_watch_words( entry );
            std::string joined;
            for( const std::string &word : words ) {
                joined += ( joined.empty() ? "" : " " ) + word;
            }
            if( !joined.empty() ) {
                group.push_back( joined );
            }
        }
        for( const std::string &word : group ) {
            std::vector<std::string> &expansions = table[word];
            for( const std::string &other : group ) {
                if( other != word &&
                    std::find( expansions.begin(), expansions.end(), other ) == expansions.end() ) {
                    expansions.push_back( other );
                }
            }
        }
    }
    return table;
}

// (Already inside namespace npc_ai: this whole section of the file is.)
std::vector<std::string> watch_synonyms_for( const std::string &word )
{
    const auto &table = watch_synonym_table();
    const auto found = table.find( word );
    return found == table.end() ? std::vector<std::string>() : found->second;
}

std::string build_local_watch_selector( const std::string &player_line )
{
    const std::vector<std::string> words = player_watch_words( player_line );
    if( words.empty() ) {
        return std::string();
    }
    std::string flat;
    for( const std::string &word : words ) {
        flat += " " + word;
    }
    flat += " ";
    // Fixed categories, as the rest of the system understands them.
    static const std::vector<std::string> magazine_words = { " cargador ", " cargadores ", " magazine " };
    static const std::vector<std::string> ammo_words = {
        " municion ", " municiones ", " balas ", " bala ", " cartuchos ", " cartucho ", " ammo ", " ammunition "
    };
    static const std::vector<std::string> gun_words = {
        // "de" is dropped by the normaliser, so "arma de fuego" arrives as "arma fuego".
        " arma fuego ", " armas fuego ", " pistola ", " pistolas ", " rifle ", " rifles ", " fusil ",
        " escopeta ", " escopetas ", " revolver ", " gun ", " guns ", " firearm ", " handgun "
    };
    const auto has_any = [&]( const std::vector<std::string> &needles ) {
        return std::any_of( needles.begin(), needles.end(), [&]( const std::string & needle ) {
            return flat.find( needle ) != std::string::npos;
        } );
    };
    if( has_any( magazine_words ) ) {
        return "@MAGAZINE";
    }
    if( has_any( ammo_words ) ) {
        return "@AMMO";
    }
    if( has_any( gun_words ) ) {
        return "@GUN";
    }
    // The whole request is ONE fragment: every word must name the object
    // ("panel solar" never fires on a "panel de madera").  Synonyms of the
    // phrase follow, and for a single word its synonyms too.  Individual
    // words of a multi-word request are never added on their own.
    std::vector<std::string> fragments;
    const auto add = [&]( const std::string & fragment ) {
        const std::string clean = sanitize_control_term( fragment );
        if( clean.size() >= 3 &&
            std::find( fragments.begin(), fragments.end(), clean ) == fragments.end() ) {
            fragments.push_back( clean );
        }
    };
    std::string phrase;
    for( const std::string &word : words ) {
        phrase += ( phrase.empty() ? "" : " " ) + word;
    }
    add( phrase );
    for( const std::string &synonym : watch_synonyms_for( phrase ) ) {
        add( synonym );
    }
    if( fragments.empty() ) {
        return std::string();
    }
    std::string selector = "@idlike:";
    for( std::size_t i = 0; i < fragments.size(); ++i ) {
        selector += ( i == 0 ? "" : "|" ) + fragments[i];
    }
    return selector;
}

static watch_action_result parse_watch_action_impl( npc *who, const std::string &player_line,
        const std::string *model_output )
{
    watch_action_result result;

    if( !looks_like_watch_request(
            player_line
        ) ) {

        return result;
    }

    result.attempted = true;

    std::ostringstream prompt;

    prompt
        << "FRASE DEL JUGADOR:\n"
        << player_line;

    npc_ai::ai_response ai;
    if( model_output != nullptr ) {
        ai = { true, *model_output, "" };
    } else {
        // Live path: no model.  The selector comes from the player's words,
        // the synonym file and the fixed categories, and the watch is
        // registered on the companion right here.
        const std::string selector = build_local_watch_selector( player_line );
        result.kind = "LOCAL";
        result.terms = player_watch_words( player_line );
        if( selector.empty() ) {
            result.raw_output = "LOCAL: nothing to watch for";
            debug_log( result, player_line );
            return result;
        }
        result.control_marker = "[[WATCH_ITEM:" + selector + "]]";
        result.raw_output = "LOCAL: " + selector;
        if( who != nullptr ) {
            std::string control = result.control_marker;
            apply_watch_control( *who, control );
        }
        result.success = true;
        result.is_watch = true;
        debug_log( result, player_line );
        return result;
    }

    if( !ai.success ) {

        result.raw_output =
            "ERROR: " + ai.error;

        debug_log(
            result,
            player_line
        );

        return result;
    }

    result.raw_output =
        trim_copy( ai.text );

    std::string machine_line;

    std::stringstream lines(
        result.raw_output
    );

    std::string line;

    while( std::getline(
               lines,
               line
           ) ) {

        const std::string trimmed =
            trim_copy( line );

        if( trimmed == "NONE" ||
            trimmed.find( "WATCH|" ) == 0 ||
            trimmed.find( "WATCH;" ) == 0 ) {

            machine_line = trimmed;
            break;
        }
    }

    if( machine_line.empty() ) {

        debug_log(
            result,
            player_line
        );

        return result;
    }

    if( machine_line == "NONE" ) {

        // CDDA-AI:
        // El detector local ya decidio que esta frase parece una
        // solicitud de vigilancia. Si contiene una orden explicita
        // de "avisame", damos al parser un segundo intento.
        //
        // Esto evita que una variacion aleatoria de Qwen convierta:
        //
        //   "avisame si ves una camiseta"
        //
        // unas veces en WATCH y otras en NONE.

        const std::string lower_player =
            lower_ascii( player_line );

        const bool explicit_watch_order =
            lower_player.find( "avisame" ) != std::string::npos ||
            lower_player.find( "avísame" ) != std::string::npos ||
            lower_player.find( "avisa si" ) != std::string::npos ||
            lower_player.find( "avisa cuando" ) != std::string::npos;

        if( explicit_watch_order ) {

            std::ostringstream retry_prompt;

            retry_prompt
                << "Eres un parser de ordenes para "
                << "Cataclysm: Dark Days Ahead.\n"
                << "NO converses.\n"
                << "NO expliques nada.\n"
                << "La frase siguiente YA fue identificada como "
                << "una orden explicita de vigilancia futura.\n"
                << "NO puedes responder NONE.\n\n"

                << "Devuelve EXACTAMENTE uno de estos formatos:\n"
                << "WATCH|MAGAZINE|magazine\n"
                << "WATCH|GUN|gun\n"
                << "WATCH|AMMO|ammo\n"
                << "WATCH|SPECIFIC|term1;term2;term3\n\n"

                << "MAGAZINE = cargador fisico de un arma.\n"
                << "GUN = arma de fuego.\n"
                << "AMMO = municion, balas o cartuchos.\n"
                << "SPECIFIC = cualquier otro objeto concreto.\n"
                << "Para SPECIFIC usa de 1 a 5 terminos utiles "
                << "en ingles.\n\n"

                << "FRASE:\n"
                << player_line;

            // A retry would be a second background request and could reorder
            // direct conversation turns.  Treat NONE as a valid no-action
            // result; the main-thread completion handler falls back to normal
            // asynchronous conversation instead.
            const npc_ai::ai_response retry = { false, "", "" };

            if( retry.success ) {

                const std::string retry_raw =
                    trim_copy( retry.text );

                result.raw_output +=
                    "\nRETRY: " + retry_raw;

                std::stringstream retry_lines(
                    retry_raw
                );

                std::string retry_line;

                while( std::getline(
                           retry_lines,
                           retry_line
                       ) ) {

                    const std::string trimmed =
                        trim_copy( retry_line );

                    if( trimmed.find( "WATCH|" ) == 0 ||
                        trimmed.find( "WATCH;" ) == 0 ) {

                        machine_line = trimmed;
                        break;
                    }
                }
            }
        }

        // Si incluso el segundo intento no pudo producir una
        // orden WATCH valida, mantenemos NONE.
        if( machine_line == "NONE" ) {

            result.success = true;
            result.is_watch = false;

            debug_log(
                result,
                player_line
            );

            return result;
        }
    }

    // CDDA-AI: normaliza errores comunes del formato WATCH.
    //
    // Correcto:
    // WATCH|SPECIFIC|shirt;t-shirt
    //
    // Variantes toleradas:
    // WATCH|SPECIFIC;shirt;t-shirt
    // WATCH;SPECIFIC|shirt;t-shirt

    if( machine_line.rfind( "WATCH;", 0 ) == 0 ) {
        machine_line[5] = '|';
    }

    if( machine_line.rfind( "WATCH|", 0 ) == 0 ) {

        const std::size_t first_separator =
            machine_line.find( '|' );

        const std::size_t second_separator =
            machine_line.find(
                '|',
                first_separator + 1
            );

        // Si falta el segundo |, Qwen probablemente escribio ;
        // entre KIND y la lista de terminos.
        if( second_separator == std::string::npos ) {

            const std::size_t wrong_separator =
                machine_line.find(
                    ';',
                    first_separator + 1
                );

            if( wrong_separator != std::string::npos ) {
                machine_line[wrong_separator] = '|';
            }
        }
    }
    const std::vector<std::string> fields =
        split(
            machine_line,
            '|'
        );

    if( fields.size() < 3 ) {

        debug_log(
            result,
            player_line
        );

        return result;
    }

    if( upper_ascii(
            fields[0]
        ) != "WATCH" ) {

        debug_log(
            result,
            player_line
        );

        return result;
    }

    result.kind =
        upper_ascii(
            fields[1]
        );

    const std::string allowed_kind =
        result.kind;

    if( allowed_kind != "MAGAZINE" &&
        allowed_kind != "GUN" &&
        allowed_kind != "AMMO" &&
        allowed_kind != "SPECIFIC" ) {

        debug_log(
            result,
            player_line
        );

        return result;
    }

    result.terms =
        split(
            fields[2],
            ';'
        );

    const npc_ai::item_catalog_result catalog =
        npc_ai::resolve_item_candidates(
            result.kind,
            result.terms,
            40
        );

    result.catalog_size =
        catalog.total_types;

    result.candidates =
        catalog.candidates;

    if( result.kind == "MAGAZINE" ) {

        result.control_marker =
            "[[WATCH_ITEM:@MAGAZINE]]";

    } else if( result.kind == "GUN" ) {

        result.control_marker =
            "[[WATCH_ITEM:@GUN]]";

    } else if( result.kind == "AMMO" ) {

        result.control_marker =
            "[[WATCH_ITEM:@AMMO]]";

    } else {

        // Resolver V3:
        //
        // SPECIFIC guarda los terminos ingleses del modelo
        // como fragmentos de ID / nombre (@idlike), no una
        // lista cerrada de IDs.  La lista enumerada tenia un
        // tope (40 candidatos, 24 guardados) y con familias
        // grandes ("gloves" son 118 tipos) dejaba fuera
        // justo el objeto que el jugador tenia delante.
        //
        // Ejemplo:
        //
        // @idlike:gloves|glove
        //
        // El catalogo se conserva solo para el listado de
        // depuracion (result.candidates).

        std::ostringstream target;

        bool first = true;

        for( std::string term :
             result.terms ) {

            term =
                lower_ascii(
                    sanitize_control_term(
                        term
                    )
                );
            const std::size_t first_char =
                term.find_first_not_of( " \t\r\n" );
            if( first_char == std::string::npos ) {
                continue;
            }
            term = term.substr( first_char,
                                term.find_last_not_of( " \t\r\n" ) - first_char + 1 );

            // Los espacios internos se vuelven "_" como en
            // los IDs ("work gloves" -> "work_gloves") y
            // ademas se conserva la forma con espacio para
            // el nombre visible.
            std::string as_id = term;
            std::replace( as_id.begin(), as_id.end(), ' ', '_' );

            for( const std::string &fragment : { term, as_id } ) {
                if( fragment.size() < 3 ) {
                    continue;
                }
                if( first ) {
                    target << "@idlike:";
                } else {
                    target << "|";
                }
                target << fragment;
                first = false;
                if( as_id == term ) {
                    break;
                }
            }
        }
        // The player's own words in their language come last, as ONE
        // all-words fragment (same rule as the local selector): they match
        // the translated visible name, which the English terms cannot.
        std::string player_phrase;
        for( const std::string &word : player_watch_words( player_line ) ) {
            player_phrase += ( player_phrase.empty() ? "" : " " ) + word;
        }
        const std::string fragment = sanitize_control_term( player_phrase );
        if( fragment.size() >= 3 ) {
            if( first ) {
                target << "@idlike:";
            } else {
                target << "|";
            }
            target << fragment;
            first = false;
        }
        if( first ) {

            // Fallback temporal.
            //
            // Si el catalogo no pudo resolver ningun ID,
            // conservamos los terminos del parser para no
            // perder completamente la funcionalidad.
            //
            // Mas adelante podremos eliminar este fallback
            // cuando el resolver semantico este completo.

            for( std::string term :
                 result.terms ) {

                term =
                    sanitize_control_term(
                        term
                    );

                if( term.empty() ) {
                    continue;
                }

                if( !first ) {
                    target << "|";
                }

                target << term;
                first = false;
            }
        }

        if( first ) {

            debug_log(
                result,
                player_line
            );

            return result;
        }

        result.control_marker =
            "[[WATCH_ITEM:" +
            target.str() +
            "]]";
    }

    result.success = true;
    result.is_watch = true;

    debug_log(
        result,
        player_line
    );

    return result;
}

watch_action_result parse_watch_action( npc &who, const std::string &player_line )
{
    return parse_watch_action_impl( &who, player_line, nullptr );
}

watch_action_result parse_watch_action_response( const std::string &player_line,
        const std::string &model_output )
{
    return parse_watch_action_impl( nullptr, player_line, &model_output );
}

void apply_watch_ai_completion( npc &who, const ai_request_completion &completion )
{
    if( !completion.response.success ) {
        who.say( _( "I couldn't understand what you wanted me to watch for." ) );
        return;
    }

    const watch_action_result result = parse_watch_action_response(
                                           completion.request.player_line, completion.response.text );
    if( result.success && result.is_watch && !result.control_marker.empty() ) {
        std::string control = result.control_marker;
        apply_watch_control( who, control );
        const std::string reply = _( "I'll let you know if I see it." );
        who.say( reply );
        remember_exchange( who, completion.request.player_line, reply );
        return;
    }

    enqueue_direct_dialogue( who, completion.request.player_line,
                             build_npc_prompt( who, completion.request.player_line ),
                             completion.request.conversation_id );
}

void strip_watch_markers(
    std::string &text
)
{
    const std::string prefix =
        "[[WATCH_ITEM:";

    while( true ) {

        const std::size_t begin =
            text.find( prefix );

        if( begin == std::string::npos ) {
            break;
        }

        const std::size_t end =
            text.find(
                "]]",
                begin
            );

        if( end == std::string::npos ) {
            text.erase( begin );
            break;
        }

        text.erase(
            begin,
            end - begin + 2
        );
    }

    text = trim_copy( text );
}

} // namespace npc_ai
