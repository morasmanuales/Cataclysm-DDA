#include "npc_ai_watchlist.h"

#include "catacharset.h"
#include "unicode.h"
#include "npc_ai_world_memory.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "cata_path.h"
#include "filesystem.h"
#include "item.h"
#include "map.h"
#include "mapdata.h"
#include "npc.h"
#include "output.h"
#include "npc_ai_profiler.h"
#include "npc_ai_context.h"
#include "messages.h"
#include "path_info.h"
#include "translations.h"
#include "sounds.h"
#include "string_formatter.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "vpart_range.h"
#include "worldfactory.h"

namespace
{

using watch_targets = std::vector<std::string>;

std::unordered_map<std::string, watch_targets> watch_cache;

std::string trim_copy( std::string text )
{
    const auto not_space = []( const unsigned char c ) {
        return !std::isspace( c );
    };

    text.erase(
        text.begin(),
        std::find_if( text.begin(), text.end(), not_space )
    );

    text.erase(
        std::find_if( text.rbegin(), text.rend(), not_space ).base(),
        text.end()
    );

    return text;
}

std::string lower_ascii( std::string text )
{
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        []( const unsigned char c ) {
            return static_cast<char>( std::tolower( c ) );
        }
    );

    return text;
}

std::filesystem::path watch_directory()
{
    if( world_generator &&
        world_generator->active_world != nullptr ) {

        const cata_path dir =
            world_generator->active_world->folder_path() /
            "npc_ai_memory";

        return dir.get_unrelative_path();
    }

    return std::filesystem::u8path(
               PATH_INFO::user_dir()
           ) / "npc_ai_memory";
}

std::filesystem::path watch_file( const npc &who )
{
    const int npc_id = who.getID().get_value();

    std::string filename;

    if( npc_id > 0 ) {
        filename =
            "npc_" +
            std::to_string( npc_id ) +
            ".watch";
    } else {
        filename =
            "npc_" +
            ensure_valid_file_name( who.get_name() ) +
            ".watch";
    }

    return watch_directory() / filename;
}

watch_targets load_targets( const npc &who )
{
    watch_targets result;

    const std::filesystem::path path =
        watch_file( who );

    if( !file_exist( path ) ) {
        return result;
    }

    std::ifstream input(
        path,
        std::ios::binary
    );

    if( !input.is_open() ) {
        return result;
    }

    std::string line;

    while( std::getline( input, line ) ) {
        line = lower_ascii(
                   trim_copy( line )
               );

        if( !line.empty() ) {
            result.push_back( line );
        }
    }

    return result;
}

watch_targets &targets_for( const npc &who )
{
    const std::string key =
        watch_file( who ).generic_u8string();

    const auto found =
        watch_cache.find( key );

    if( found != watch_cache.end() ) {
        return found->second;
    }

    watch_cache.emplace(
        key,
        load_targets( who )
    );

    return watch_cache.at( key );
}

void save_targets(
    const npc &who,
    const watch_targets &targets
)
{
    const std::filesystem::path directory =
        watch_directory();

    if( !assure_dir_exist( directory ) ) {
        return;
    }

    std::ofstream output(
        watch_file( who ),
        std::ios::binary |
        std::ios::trunc
    );

    if( !output.is_open() ) {
        return;
    }

    for( const std::string &target : targets ) {
        output << target << '\n';
    }
}

bool add_target(
    const npc &who,
    std::string target
)
{
    target = lower_ascii(
                 trim_copy( target )
             );

    // Temporalmente permitimos selectores largos.
    // El Item Resolver V2 los reemplazara por IDs compactos.
    if( target.empty() ||
        target.size() > 4096 ) {
        return false;
    }

    std::replace(
        target.begin(),
        target.end(),
        '\n',
        ' '
    );

    std::replace(
        target.begin(),
        target.end(),
        '\r',
        ' '
    );

    watch_targets &targets =
        targets_for( who );

    if( std::find(
            targets.begin(),
            targets.end(),
            target
        ) != targets.end() ) {

        return true;
    }

    targets.push_back( target );

    save_targets(
        who,
        targets
    );

    return true;
}

// Lowercase, accents removed, split on anything that is not a letter or a
// digit: "guantes brigantina" -> {guantes, brigantina}; "gloves_work" ->
// {gloves, work}.
std::vector<std::string> watch_tokens( const std::string &text )
{
    std::u32string codepoints = utf8_to_utf32( text );
    for( char32_t &codepoint : codepoints ) {
        u32_to_lowercase( codepoint );
        remove_accent( codepoint );
    }
    std::vector<std::string> tokens;
    std::string current;
    for( const char32_t codepoint : codepoints ) {
        if( ( codepoint >= U'a' && codepoint <= U'z' ) ||
            ( codepoint >= U'0' && codepoint <= U'9' ) ) {
            current.push_back( static_cast<char>( codepoint ) );
        } else if( codepoint > 127 ) {
            current += utf32_to_utf8( codepoint );
        } else if( !current.empty() ) {
            tokens.push_back( current );
            current.clear();
        }
    }
    if( !current.empty() ) {
        tokens.push_back( current );
    }
    return tokens;
}

// A fragment names a token when they are equal or differ only by a short
// plural/gender ending (guante/guantes, bota/botas, glove/gloves).  Plain
// substring matching was too loose: "rope" inside "europea" or "sirope",
// "water" inside "waterfowl".
bool fragment_names_token( const std::string &fragment, const std::string &token )
{
    if( fragment == token ) {
        return true;
    }
    if( token.size() > fragment.size() ) {
        return token.size() - fragment.size() <= 1 &&
               token.compare( 0, fragment.size(), fragment ) == 0;
    }
    return fragment.size() - token.size() <= 1 &&
           fragment.compare( 0, token.size(), token ) == 0 && token.size() >= 4;
}

// A multi-word fragment ("panel solar", "solar_panel", "work gloves") names
// the object only when EVERY word names one of its tokens, in any order.  A
// single word matches any token.  "panel solar" therefore never fires on a
// "panel de madera", while "solar_panel" and "panel solar" both fire on the
// id solar_panel and on the Spanish name "panel solar".
bool fragment_names_all_tokens( const std::string &fragment, const std::vector<std::string> &tokens )
{
    const std::vector<std::string> fragment_tokens = watch_tokens( fragment );
    if( fragment_tokens.empty() ) {
        return false;
    }
    for( const std::string &wanted : fragment_tokens ) {
        bool found = false;
        for( const std::string &token : tokens ) {
            if( fragment_names_token( wanted, token ) ) {
                found = true;
                break;
            }
        }
        if( !found ) {
            return false;
        }
    }
    return true;
}

// Installed things (vehicle parts, furniture) only have an id and a name:
// the @idlike selector and plain text terms apply to them; the item-only
// categories (@magazine, @gun, @ammo) and exact @id lists do not.
bool installed_matches_target( const std::string &id, const std::string &name,
                               const std::string &target )
{
    const std::string normalized = lower_ascii( target );
    if( normalized.rfind( "@idlike:", 0 ) == 0 ) {
        std::vector<std::string> tokens = watch_tokens( id );
        const std::vector<std::string> name_tokens = watch_tokens( name );
        tokens.insert( tokens.end(), name_tokens.begin(), name_tokens.end() );
        std::size_t begin = 8;
        while( begin <= normalized.size() ) {
            const std::size_t end = normalized.find( '|', begin );
            const std::string fragment = end == std::string::npos ? normalized.substr( begin ) :
                                         normalized.substr( begin, end - begin );
            if( fragment.size() >= 3 && fragment_names_all_tokens( fragment, tokens ) ) {
                return true;
            }
            if( end == std::string::npos ) {
                break;
            }
            begin = end + 1;
        }
        return false;
    }
    if( normalized.empty() || normalized.front() == '@' ) {
        return false;
    }
    // Legacy plain text terms.
    const std::string lower_name = lower_ascii( name );
    std::size_t begin = 0;
    while( begin <= normalized.size() ) {
        const std::size_t end = normalized.find( '|', begin );
        const std::string term = trim_copy( end == std::string::npos ? normalized.substr( begin ) :
                                            normalized.substr( begin, end - begin ) );
        if( !term.empty() && lower_name.find( term ) != std::string::npos ) {
            return true;
        }
        if( end == std::string::npos ) {
            break;
        }
        begin = end + 1;
    }
    return false;
}

bool matches_target( const item &it, const std::string &target )
{
    const std::string normalized = lower_ascii( target );

    // ----------------------------------------------------
    // Categorias semanticas reales de CDDA
    // ----------------------------------------------------

    if( normalized == "@magazine" ) {
        return it.is_magazine();
    }

    if( normalized == "@gun" ) {
        return it.is_gun();
    }

    if( normalized == "@ammo" ) {
        return it.is_ammo();
    }

    // ----------------------------------------------------
    // IDs internos reales de CDDA
    //
    // Formatos aceptados:
    //
    // @id:radio
    //
    // @id:combat_boots|winter_boots|rubber_boots
    //
    // Tambien tolera:
    //
    // @id:combat_boots|@id:winter_boots
    // ----------------------------------------------------

    // ----------------------------------------------------
    // Fragmentos de ID o de nombre visible
    //
    // @idlike:gloves|glove
    //
    // Casa con cualquier tipo cuyo ID interno (ingles) o
    // cuyo nombre visible (traducido) contenga alguno de
    // los fragmentos.  Cubre familias completas ("gloves"
    // son 118 tipos) sin enumerar IDs ni depender de un
    // tope del catalogo.
    // ----------------------------------------------------
    if( normalized.rfind( "@idlike:", 0 ) == 0 ) {
        std::vector<std::string> tokens = watch_tokens( it.typeId().str() );
        const std::vector<std::string> name_tokens =
            watch_tokens( remove_color_tags( it.tname() ) );
        tokens.insert( tokens.end(), name_tokens.begin(), name_tokens.end() );
        std::size_t begin = 8;
        while( begin <= normalized.size() ) {
            const std::size_t end =
                normalized.find( '|', begin );
            std::string fragment = end == std::string::npos ?
                                   normalized.substr( begin ) :
                                   normalized.substr( begin, end - begin );
            const std::size_t first =
                fragment.find_first_not_of( " \t\r\n" );
            if( first == std::string::npos ) {
                fragment.clear();
            } else {
                const std::size_t last =
                    fragment.find_last_not_of( " \t\r\n" );
                fragment = fragment.substr( first, last - first + 1 );
            }
            if( fragment.size() >= 3 && fragment_names_all_tokens( fragment, tokens ) ) {
                return true;
            }
            if( end == std::string::npos ) {
                break;
            }
            begin = end + 1;
        }
        return false;
    }

    if( normalized.rfind( "@id:", 0 ) == 0 ) {

        const std::string actual_id =
            lower_ascii( it.typeId().str() );

        std::size_t begin = 4;

        while( begin <= normalized.size() ) {

            const std::size_t end =
                normalized.find( '|', begin );

            std::string candidate;

            if( end == std::string::npos ) {
                candidate = normalized.substr( begin );
            } else {
                candidate =
                    normalized.substr( begin, end - begin );
            }

            // Permitir:
            // @id:a|@id:b
            if( candidate.rfind( "@id:", 0 ) == 0 ) {
                candidate = candidate.substr( 4 );
            }

            const std::size_t first =
                candidate.find_first_not_of( " \t\r\n" );

            if( first != std::string::npos ) {

                const std::size_t last =
                    candidate.find_last_not_of( " \t\r\n" );

                candidate =
                    candidate.substr(
                        first,
                        last - first + 1
                    );

            } else {
                candidate.clear();
            }

            if( !candidate.empty() &&
                actual_id == candidate ) {

                return true;
            }

            if( end == std::string::npos ) {
                break;
            }

            begin = end + 1;
        }

        return false;
    }

    // ----------------------------------------------------
    // Compatibilidad con el sistema antiguo.
    // Sigue permitiendo terminos de texto.
    // ----------------------------------------------------

    const std::string item_name =
        lower_ascii( remove_color_tags( it.tname() ) );

    std::size_t begin = 0;

    while( begin <= normalized.size() ) {

        const std::size_t end =
            normalized.find( '|', begin );

        std::string term;

        if( end == std::string::npos ) {
            term = normalized.substr( begin );
        } else {
            term =
                normalized.substr( begin, end - begin );
        }

        const std::size_t first =
            term.find_first_not_of( " \t\r\n" );

        if( first != std::string::npos ) {

            const std::size_t last =
                term.find_last_not_of( " \t\r\n" );

            term =
                term.substr(
                    first,
                    last - first + 1
                );

        } else {
            term.clear();
        }

        if( !term.empty() &&
            item_name.find( term ) != std::string::npos ) {

            return true;
        }

        if( end == std::string::npos ) {
            break;
        }

        begin = end + 1;
    }

    return false;
}

} // namespace

namespace npc_ai
{

bool apply_watch_control(
    const npc &who,
    std::string &speech
)
{
    const std::string marker =
        "[[WATCH_ITEM:";

    const std::size_t start =
        speech.find( marker );

    if( start == std::string::npos ) {
        return false;
    }

    const std::size_t end =
        speech.find(
            "]]",
            start + marker.size()
        );

    if( end == std::string::npos ) {
        return false;
    }

    std::string target =
        speech.substr(
            start + marker.size(),
            end - ( start + marker.size() )
        );

    target = trim_copy( target );

    const bool added =
        add_target(
            who,
            target
        );

    // El jugador nunca debe ver la instruccion interna.
    speech.erase(
        start,
        end + 2 - start
    );

    speech = trim_copy( speech );

    if( speech.empty() && added ) {
        speech =
            "Vale. Te aviso si lo veo.";
    }

    return added;
}

std::string build_watchlist_context(
    const npc &who
)
{
    const watch_targets &targets =
        targets_for( who );

    std::string result;

    result +=
        "=== ENCARGOS ACTIVOS DE VIGILANCIA ===\n";

    if( targets.empty() ) {
        result +=
            "No tienes busquedas de objetos activas.\n";

        return result;
    }

    result +=
        "El jugador te pidio que le avises si ves:\n";

    for( const std::string &target : targets ) {
        result += "- " + target + "\n";
    }

    return result;
}

void check_item_watchlist( npc &who )
{
    scoped_profile profile( profile_subsystem::watchlist );

    watch_targets &targets = targets_for( who );

    // Esta es la ruta normal: sin busquedas activas
    // no se escanea absolutamente nada.
    if( targets.empty() ) {
        return;
    }

    map &here = get_map();
    constexpr int watch_radius = 6;
    const tripoint_bub_ms origin = who.pos_bub( here );

    // One alert closes the task; every kind of find goes through here.
    const auto announce = [&]( const std::size_t target_index, const std::string &type_id,
    const std::string &name, const tripoint_bub_ms &where, const std::string &installed_on ) {
        const std::string alert = installed_on.empty() ?
                                  string_format( npc_ai::localized_ai_message(
                                          _( "Hey, I found %s.  You asked me to let you know." ),
                                          "¡Oye, encontré %s!  Me pediste que te avisara." ), name ) :
                                  string_format( npc_ai::localized_ai_message(
                                          _( "Hey, there is %1$s installed on %2$s.  You asked me to let you know." ),
                                          "¡Oye, hay %1$s instalado en %2$s!  Me pediste que te avisara." ),
                                          name, installed_on );
        const tripoint_abs_ms abs_pos = here.get_abs( where );
        npc_ai::remember_seen_item( who, type_id, name, abs_pos.x(), abs_pos.y(), abs_pos.z() );
        // Magenta, so the alert stands out from ordinary chatter.
        add_msg( m_mixed, "%s: %s", who.get_name(), alert );
        // Una sola notificacion: una vez encontrado, el encargo queda cumplido.
        targets.erase( targets.begin() + target_index );
        save_targets( who, targets );
    };

    // Installed things first: vehicle parts (a solar panel on a car, a
    // wheel), then furniture (a fridge, a workbench).  They are visible
    // whenever the tile is, containers or not.
    for( const wrapped_vehicle &wrapped : here.get_vehicles(
             origin - tripoint_rel_ms{ watch_radius, watch_radius, 0 },
             origin + tripoint_rel_ms{ watch_radius, watch_radius, 0 } ) ) {
        if( wrapped.v == nullptr ) {
            continue;
        }
        for( const vpart_reference &part : wrapped.v->get_all_parts() ) {
            const tripoint_bub_ms where = part.pos_bub( here );
            if( where.z() != origin.z() || rl_dist( origin, where ) > watch_radius ||
                part.part().is_broken() || !who.sees( here, where ) ) {
                continue;
            }
            const std::string part_id = part.info().id.str();
            const std::string part_name = remove_color_tags( part.part().name( false ) );
            for( std::size_t i = 0; i < targets.size(); ++i ) {
                if( installed_matches_target( part_id, part_name, targets[i] ) ) {
                    announce( i, part_id, part_name, where, wrapped.v->name );
                    return;
                }
            }
        }
    }
    for( const tripoint_bub_ms &p : here.points_in_radius( origin, watch_radius, 0 ) ) {
        if( !who.sees( here, p ) ) {
            continue;
        }
        const furn_id furniture = here.furn( p );
        if( furniture.id().str() == "f_null" ) {
            continue;
        }
        const std::string furn_id_str = furniture.id().str();
        const std::string furn_name = remove_color_tags( furniture.obj().name() );
        for( std::size_t i = 0; i < targets.size(); ++i ) {
            if( installed_matches_target( furn_id_str, furn_name, targets[i] ) ) {
                announce( i, furn_id_str, furn_name, p, std::string() );
                return;
            }
        }
    }

    // Loose items.
    for( const tripoint_bub_ms &p : here.points_in_radius( origin, watch_radius, 0 ) ) {
        if( !who.sees( here, p ) || !here.could_see_items( p, who ) ) {
            continue;
        }
        for( const item &it : here.i_at( p ) ) {
            for( std::size_t i = 0; i < targets.size(); ++i ) {
                if( !matches_target( it, targets[i] ) ) {
                    continue;
                }
                announce( i, it.typeId().str(), remove_color_tags( it.tname() ), p, std::string() );
                return;
            }
        }
    }
}


void reset_watch_cache()
{
    // The watchlists themselves stay on disk; only the in-memory mirror keyed
    // by the previous world's file paths is dropped.
    watch_cache.clear();
}

} // namespace npc_ai
