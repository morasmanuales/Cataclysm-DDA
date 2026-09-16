#include "npc_ai_equipment.h"

#include <algorithm>
#include <cstddef>
#include <list>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "activity_actor_definitions.h"
#include "bodypart.h"
#include "cata_utility.h"
#include "catacharset.h"
#include "item.h"
#include "map.h"
#include "map_selector.h"
#include "messages.h"
#include "npc.h"
#include "npc_ai_context.h"
#include "npc_ai_equipment_memory.h"
#include "npc_ai_goal.h"
#include "output.h"
#include "translations.h"
#include "unicode.h"
#include "units.h"

namespace
{

struct equipment_candidate {
    item_location location;
    std::string id;
    std::string name;
    std::string location_description;
    bool ground = false;
    bool wielded = false;
    bool worn = false;
    bool weapon = false;
    bool storage = false;
    bool pack = false;
    bool headwear = false;
};

bool contains_any( const std::string &text, const std::vector<std::string> &terms )
{
    return std::any_of( terms.begin(), terms.end(), [&]( const std::string &term ) {
        return lcmatch( text, term );
    } );
}

bool asks_for_pack( const std::string &text )
{
    return contains_any( text, { "mochila", "backpack", "rucksack", "bolso", "macuto" } );
}

bool asks_for_weapon( const std::string &text )
{
    return contains_any( text, { "arma", "rifle", "fusil", "escopeta", "pistola",
                                "revolver", "hacha", "machete", "cuchillo", "espada",
                                "lanza", "bate", "martillo", "palanca", "maza", "garrote",
                                "weapon", "gun", "axe", "knife", "sword", "spear", "bat",
                                "hammer", "crowbar", "mace", "club" } );
}

bool asks_for_headwear( const std::string &text )
{
    return contains_any( text, { "casco", "helmet", "sombrero", "hat" } );
}

bool asks_to_wear( const std::string &text )
{
    return contains_any( text, { "ponte", "póntela", "pontela", "ponértela", "ponertela",
                                "ponerte", "vístete", "vistete", "wear ", "put on" } );
}

bool is_weapon( const item &it )
{
    return it.is_gun() || it.is_maybe_melee_weapon();
}

bool is_storage_garment( const item &it )
{
    return it.is_armor() && it.get_total_capacity() > 0_ml;
}

bool is_pack( const item &it )
{
    if( !is_storage_garment( it ) ) {
        return false;
    }
    const std::string id = it.typeId().str();
    const std::string name = remove_color_tags( it.tname() );
    return contains_any( id, { "backpack", "rucksack", "knapsack", "macuto", "duffel" } ) ||
           contains_any( name, { "backpack", "rucksack", "knapsack", "mochila", "macuto",
                                "duffel bag", "bolso" } );
}

bool is_headwear( const item &it )
{
    static const bodypart_id head( "head" );
    return it.is_armor() && it.covers( head );
}

void add_candidate( npc &who, std::vector<equipment_candidate> &result,
                    const item_location &location )
{
    if( !location ) {
        return;
    }
    if( std::any_of( result.begin(), result.end(), [&]( const equipment_candidate &candidate ) {
    return candidate.location.get_item() == location.get_item();
    } ) ) {
        return;
    }

    const item &it = *location;
    equipment_candidate candidate;
    candidate.location = location;
    candidate.id = it.typeId().str();
    candidate.name = remove_color_tags( it.tname() );
    candidate.ground = location.where_recursive() == item_location::type::map;
    candidate.wielded = who.get_wielded_item().get_item() == location.get_item();
    candidate.worn = who.is_worn( it );
    if( candidate.wielded ) {
        candidate.location_description = "empuñado";
    } else if( candidate.worn ) {
        candidate.location_description = "puesto";
    } else if( candidate.ground ) {
        candidate.location_description = "suelo";
    } else if( location.has_parent() ) {
        candidate.location_description = "dentro de un contenedor propio";
    } else {
        candidate.location_description = "inventario";
    }
    candidate.weapon = is_weapon( it );
    candidate.storage = is_storage_garment( it );
    candidate.pack = is_pack( it );
    candidate.headwear = is_headwear( it );
    result.push_back( std::move( candidate ) );
}

std::vector<equipment_candidate> collect_candidates( npc &who,
        npc_ai::equipment_action action )
{
    std::vector<equipment_candidate> result;

    if( action == npc_ai::equipment_action::store ) {
        add_candidate( who, result, who.get_wielded_item() );
        return result;
    }

    if( action == npc_ai::equipment_action::take_off ) {
        for( const item_location &location : who.top_items_loc() ) {
            if( location && who.can_takeoff( *location ).success() ) {
                add_candidate( who, result, location );
            }
        }
        return result;
    }

    for( const item_location &location : who.all_items_loc() ) {
        if( !location ) {
            continue;
        }
        if( action == npc_ai::equipment_action::drop && who.can_drop( *location ).success() ) {
            add_candidate( who, result, location );
        } else if( action == npc_ai::equipment_action::wear && !who.is_worn( *location ) &&
                   location->is_armor() ) {
            add_candidate( who, result, location );
        }
    }

    if( action == npc_ai::equipment_action::wear ) {
        map &here = get_map();
        const tripoint_bub_ms origin = who.pos_bub( here );
        for( const tripoint_bub_ms &position : here.points_in_radius( origin, 1 ) ) {
            if( position.z() != origin.z() || !who.sees( here, position ) ||
                !here.could_see_items( position, who ) ||
                !here.clear_path( origin, position, 1, 1, 100 ) ) {
                continue;
            }
            for( item &it : here.i_at( position ) ) {
                if( it.is_armor() ) {
                    add_candidate( who, result, item_location( map_cursor( &here, position ), &it ) );
                }
            }
        }
    }

    return result;
}

// Lowercase, accents removed, punctuation turned into spaces.
std::string normalize_equipment_text( const std::string &text )
{
    std::u32string codepoints = utf8_to_utf32( text );
    for( char32_t &codepoint : codepoints ) {
        u32_to_lowercase( codepoint );
        remove_accent( codepoint );
    }
    std::string normalized;
    bool previous_space = true;
    for( const char32_t codepoint : codepoints ) {
        if( ( codepoint >= U'a' && codepoint <= U'z' ) ||
            ( codepoint >= U'0' && codepoint <= U'9' ) ) {
            normalized.push_back( static_cast<char>( codepoint ) );
            previous_space = false;
        } else if( codepoint > 127 ) {
            normalized += utf32_to_utf8( codepoint );
            previous_space = false;
        } else if( !previous_space ) {
            normalized.push_back( ' ' );
            previous_space = true;
        }
    }
    while( !normalized.empty() && normalized.back() == ' ' ) {
        normalized.pop_back();
    }
    return normalized;
}

// The object words of an equipment order: everything that is not the verb,
// an article or filler.  "Suelta la esponja ahora" -> { "esponja" }.
std::vector<std::string> equipment_request_words( const std::string &player_line )
{
    static const std::vector<std::string> filler = {
        // verbs of every equipment order
        "suelta", "suelten", "soltar", "soltad", "sueltala", "sueltalo", "deja", "dejen", "dejad",
        "dejate", "tira", "tiren", "tirad", "drop", "ponte", "pontela", "pontelo", "ponertela",
        "ponerte", "ponganse", "poneos", "vistete", "wear", "put", "on", "quitate", "quitense",
        "quitaos", "sacate", "take", "off", "remove", "guarda", "guardar", "guarden", "guardad",
        "enfunda", "holster", "away", "recoge", "recupera", "busca", "retrieve", "recover", "pick",
        "up", "empuna", "empunar",
        // articles, pronouns, filler
        "el", "la", "los", "las", "un", "una", "unos", "unas", "de", "del", "al", "tu", "tus",
        "su", "sus", "mi", "mis", "ese", "esa", "eso", "esos", "esas", "este", "esta", "esto",
        "que", "tienes", "llevas", "ahora", "ya", "por", "favor", "urgente", "the", "a", "an",
        "your", "my", "that", "this", "those", "these", "you", "have", "are", "carrying", "now",
        "please", "immediately"
    };
    std::vector<std::string> words;
    std::istringstream stream( normalize_equipment_text( player_line ) );
    std::string word;
    while( stream >> word ) {
        if( word.size() < 3 || std::find( filler.begin(), filler.end(), word ) != filler.end() ) {
            continue;
        }
        if( std::find( words.begin(), words.end(), word ) == words.end() ) {
            words.push_back( word );
        }
    }
    return words;
}

bool candidate_matches_words( const equipment_candidate &candidate,
                              const std::vector<std::string> &words )
{
    if( words.empty() ) {
        return false;
    }
    const std::string name = normalize_equipment_text( candidate.name );
    const std::string id = normalize_equipment_text( candidate.id );
    for( const std::string &word : words ) {
        if( name.find( word ) == std::string::npos && id.find( word ) == std::string::npos ) {
            return false;
        }
    }
    return true;
}

bool memory_matches_words( const npc_ai::dropped_equipment_memory &record,
                           const std::vector<std::string> &words )
{
    if( words.empty() ) {
        return false;
    }
    const std::string name = normalize_equipment_text( record.item_name );
    const std::string id = normalize_equipment_text( record.item_type );
    for( const std::string &word : words ) {
        if( name.find( word ) == std::string::npos && id.find( word ) == std::string::npos ) {
            return false;
        }
    }
    return true;
}

std::vector<std::size_t> deterministic_matches( const std::string &player_line,
        const std::vector<equipment_candidate> &candidates )
{
    // Three tiers: the whole item name quoted in the order; every object word
    // of the order found in the item name or id (so "suelta la esponja" finds
    // "esponja (limpia)" and "drop the bottle" finds "plastic bottle");
    // finally the coarse categories (pack / weapon / headwear).
    std::vector<std::size_t> exact;
    std::vector<std::size_t> by_words;
    std::vector<std::size_t> category;
    const std::vector<std::string> words = equipment_request_words( player_line );
    for( std::size_t index = 0; index < candidates.size(); ++index ) {
        const equipment_candidate &candidate = candidates[index];
        if( lcmatch( player_line, candidate.name ) || lcmatch( player_line, candidate.id ) ) {
            exact.push_back( index );
        }
        if( candidate_matches_words( candidate, words ) ) {
            by_words.push_back( index );
        }
        if( ( asks_for_pack( player_line ) && candidate.pack ) ||
            ( asks_for_weapon( player_line ) && candidate.weapon ) ||
            ( asks_for_headwear( player_line ) && candidate.headwear ) ) {
            category.push_back( index );
        }
    }
    if( !exact.empty() ) {
        return exact;
    }
    if( !by_words.empty() ) {
        return by_words;
    }
    return category;
}

std::vector<std::size_t> prefer_equipped_target( const npc_ai::equipment_action action,
        const std::string &player_line, const std::vector<equipment_candidate> &candidates,
        const std::vector<std::size_t> &matches )
{
    std::vector<std::size_t> preferred;
    for( const std::size_t index : matches ) {
        const equipment_candidate &candidate = candidates[index];
        if( action == npc_ai::equipment_action::drop && asks_for_weapon( player_line ) &&
            candidate.weapon && candidate.wielded ) {
            preferred.push_back( index );
        } else if( ( action == npc_ai::equipment_action::drop ||
                     action == npc_ai::equipment_action::take_off ) &&
                   asks_for_pack( player_line ) && candidate.pack && candidate.worn ) {
            preferred.push_back( index );
        }
    }
    return preferred.empty() ? matches : preferred;
}

const char *action_name( const npc_ai::equipment_action action )
{
    switch( action ) {
        case npc_ai::equipment_action::drop:
            return "DROP";
        case npc_ai::equipment_action::recover:
            return "RECOVER";
        case npc_ai::equipment_action::wear:
            return "WEAR";
        case npc_ai::equipment_action::take_off:
            return "TAKE_OFF";
        case npc_ai::equipment_action::store:
            return "STORE";
        case npc_ai::equipment_action::none:
            return "NONE";
    }
    return "NONE";
}

std::vector<npc_ai::dropped_equipment_memory> matching_dropped_memories(
    npc &who, const std::string &player_line )
{
    std::vector<npc_ai::dropped_equipment_memory> exact;
    std::vector<npc_ai::dropped_equipment_memory> by_words;
    std::vector<npc_ai::dropped_equipment_memory> category;
    // The generic category words select the family, not a name; drop them so
    // "recupera tu arma, el hacha" narrows by "hacha" alone.
    std::vector<std::string> words = equipment_request_words( player_line );
    words.erase( std::remove_if( words.begin(), words.end(), []( const std::string & word ) {
        static const std::vector<std::string> generic = {
            "arma", "weapon", "gun", "mochila", "backpack", "rucksack", "bolso", "macuto",
            "casco", "helmet", "sombrero", "hat", "equipo", "gear"
        };
        return std::find( generic.begin(), generic.end(), word ) != generic.end();
    } ), words.end() );
    for( const npc_ai::dropped_equipment_memory &record :
         npc_ai::get_dropped_equipment_memories( who ) ) {
        if( record.owner_id != who.getID().get_value() ||
            record.status == npc_ai::equipment_memory_status::recovered ) {
            continue;
        }
        item prototype( itype_id( record.item_type ), calendar::turn );
        if( lcmatch( player_line, record.item_name ) ||
            lcmatch( player_line, record.item_type ) ) {
            exact.push_back( record );
        }
        if( memory_matches_words( record, words ) ) {
            by_words.push_back( record );
        }
        if( ( asks_for_pack( player_line ) && is_pack( prototype ) ) ||
            ( asks_for_weapon( player_line ) && is_weapon( prototype ) ) ||
            ( asks_for_headwear( player_line ) && is_headwear( prototype ) ) ) {
            category.push_back( record );
        }
    }
    if( !exact.empty() ) {
        return exact;
    }
    if( !by_words.empty() ) {
        return by_words;
    }
    return category;
}

npc_ai::equipment_command_result request_remembered_equipment(
    npc &who, const std::string &player_line, const bool wear_after_recovery )
{
    npc_ai::equipment_command_result result;
    result.handled = true;
    result.action = npc_ai::equipment_action::recover;

    std::vector<npc_ai::dropped_equipment_memory> matches =
        matching_dropped_memories( who, player_line );
    if( matches.empty() ) {
        result.message = _( "I don't have or remember a suitable item for that action." );
        return result;
    }

    if( matches.size() > 1 ) {
        std::vector<npc_ai::dropped_equipment_memory> expected;
        std::copy_if( matches.begin(), matches.end(), std::back_inserter( expected ),
        []( const npc_ai::dropped_equipment_memory & record ) {
            return record.retrieval_expected;
        } );
        if( expected.size() == 1 ) {
            matches = std::move( expected );
        }
    }
    if( matches.size() != 1 ) {
        result.message = npc_ai::localized_ai_message(
                             _( "I can't determine which item you mean." ),
                             "No puedo determinar a qué objeto te refieres." );
        return result;
    }

    const npc_ai::dropped_equipment_memory &selected = matches.front();
    std::string error;
    if( !npc_ai::request_equipment_recovery( who, selected.item_uid,
            wear_after_recovery, error ) ) {
        result.message = error;
        return result;
    }

    result.success = true;
    result.action_started = true;
    result.equipment_uid = selected.item_uid;
    result.message = wear_after_recovery ?
                     string_format( npc_ai::localized_ai_message(
                                        _( "I'll recover %s and put it on." ),
                                        "Recuperaré %s y me lo pondré." ), selected.item_name ) :
                     string_format( npc_ai::localized_ai_message(
                                        _( "I'll recover %s." ),
                                        "Recuperaré %s." ), selected.item_name );
    return result;
}

} // namespace

namespace npc_ai
{

equipment_action detect_equipment_action( const std::string &player_line )
{
    if( contains_any( player_line, { "guarda", "guardar", "enfunda", "holster",
                                    "put away", "deja de empunar" } ) ) {
        return equipment_action::store;
    }
    if( contains_any( player_line, { "quitate", "quítate", "sacate", "sácate",
                                    "take off", "remove your" } ) ) {
        return equipment_action::take_off;
    }
    if( contains_any( player_line, { "recoge tu", "recupera", "busca tu", "busca la mochila",
                                    "recojan", "recoged", "pick up your", "retrieve", "recover" } ) &&
        ( asks_for_pack( player_line ) || asks_for_weapon( player_line ) ||
          asks_for_headwear( player_line ) ) ) {
        return equipment_action::recover;
    }
    if( asks_to_wear( player_line ) ) {
        return equipment_action::wear;
    }
    if( contains_any( player_line, { "suelta", "suelten", "soltar", "soltad", "deja ",
                                    "dejate", "déjate", "tira ", "drop " } ) ) {
        return equipment_action::drop;
    }
    return equipment_action::none;
}

equipment_command_result execute_equipment_action( npc &who, equipment_action action,
        item_location target, const std::string &reason, const bool retrieval_expected )
{
    equipment_command_result result;
    result.handled = action != equipment_action::none;
    result.action = action;
    if( !result.handled ) {
        return result;
    }
    if( !target ) {
        result.message = npc_ai::localized_ai_message(
                             _( "That item is no longer available." ),
                             "Ese objeto ya no está disponible." );
        return result;
    }

    const std::string name = remove_color_tags( target->tname() );
    const std::string source = target.where_recursive() == item_location::type::map ?
                               "ground" : who.get_wielded_item().get_item() == target.get_item() ?
                               "wielded" : who.is_worn( *target ) ? "worn" : "inventory";
    add_msg_debug( debugmode::DF_NPC_ITEMAI,
                   "%s EQUIP_INTENT=%s TARGET_ITEM=%s TARGET_LOCATION=%s ACTION=%s SOURCE=%s",
                   who.get_name(), action_name( action ), name, target.describe( &who ),
                   action_name( action ), source );
    switch( action ) {
        case equipment_action::drop: {
            const ret_val<void> can_drop = who.can_drop( *target );
            add_msg_debug( debugmode::DF_NPC_ITEMAI, "%s DROP_INTENT=%s DROP_VALIDATION=%s",
                           who.get_name(), name, can_drop.success() ? "success" : can_drop.str() );
            if( !can_drop.success() ) {
                result.message = string_format( npc_ai::localized_ai_message(
                                                    _( "I can't drop %s: %s" ),
                                                    "No puedo dejar %s en el suelo: %s" ),
                                                name, can_drop.str() );
                return result;
            }
            result.equipment_uid = remember_dropped_equipment( who, *target, who.pos_abs(), reason,
                                   retrieval_expected );
            const drop_locations items = { { target, target->count() } };
            who.drop( items, who.pos_bub( get_map() ), false );
            // npc::drop processes the native drop actor immediately.  Do not query
            // target afterward: moving a containing item intentionally invalidates it.
            result.success = true;
            result.action_started = true;
            result.message = string_format( _( "I dropped %s." ), name );
            add_msg_debug( debugmode::DF_NPC_ITEMAI,
                           "%s DROP_RESULT=started GROUND_LOCATION=%s", who.get_name(),
                           who.pos_abs().to_string_writable() );
            return result;
        }
        case equipment_action::recover:
            result.message = npc_ai::localized_ai_message(
                                 _( "Recovery requires a remembered dropped item." ),
                                 "La recuperación requiere recordar un objeto que dejé en el suelo." );
            return result;
        case equipment_action::wear: {
            const ret_val<void> can_wear = who.can_wear( *target );
            add_msg_debug( debugmode::DF_NPC_ITEMAI, "%s WEAR_VALIDATION=%s",
                           who.get_name(), can_wear.success() ? "success" : can_wear.str() );
            if( !can_wear.success() ) {
                result.message = string_format( npc_ai::localized_ai_message(
                                                    _( "I can't wear %s: %s" ),
                                                    "No puedo ponerme %s: %s" ),
                                                name, can_wear.str() );
                return result;
            }
            who.assign_activity( wear_activity_actor( { target }, { target->count() } ) );
            result.success = true;
            result.action_started = true;
            result.message = string_format( _( "I'm putting on %s." ), name );
            add_msg_debug( debugmode::DF_NPC_ITEMAI,
                           "%s WEAR_RESULT=activity_started SOURCE=%s", who.get_name(), source );
            return result;
        }
        case equipment_action::take_off: {
            const ret_val<void> can_takeoff = who.can_takeoff( *target );
            if( !can_takeoff.success() ) {
                result.message = string_format( npc_ai::localized_ai_message(
                                                    _( "I can't take off %s: %s" ),
                                                    "No puedo quitarme %s: %s" ), name,
                                                can_takeoff.str() );
                return result;
            }
            const int move_cost = target.obtain_cost( who, target->count() );
            std::list<item> removed;
            if( !who.takeoff( target, &removed ) ) {
                result.message = string_format( npc_ai::localized_ai_message(
                                                    _( "I couldn't take off %s." ),
                                                    "No pude quitarme %s." ), name );
                return result;
            }
            bool placed_on_ground = false;
            for( item &it : removed ) {
                const item_location destination = who.i_add( std::move( it ), true, nullptr, nullptr,
                                                  true, false );
                placed_on_ground = placed_on_ground ||
                                   destination.where_recursive() == item_location::type::map;
            }
            who.mod_moves( -move_cost );
            result.success = true;
            result.action_started = true;
            result.message = placed_on_ground ?
                             string_format( npc_ai::localized_ai_message(
                                                _( "I took off %s and set it down because I couldn't store it." ),
                                                "Me quité %s y lo dejé en el suelo porque no pude guardarlo." ), name ) :
                             string_format( npc_ai::localized_ai_message(
                                                _( "I took off %s and put it away." ),
                                                "Me quité %s y lo guardé." ), name );
            return result;
        }
        case equipment_action::store:
            if( who.get_wielded_item().get_item() != target.get_item() ) {
                result.message = npc_ai::localized_ai_message(
                                     _( "I'm not wielding that item." ),
                                     "No estoy empuñando ese objeto." );
                return result;
            }
            result.success = who.unwield();
            result.action_started = result.success;
            result.message = result.success ? string_format( npc_ai::localized_ai_message(
                                 _( "I put away %s." ), "Guardé %s." ), name ) :
                             string_format( npc_ai::localized_ai_message(
                                                _( "I couldn't put away %s." ),
                                                "No pude guardar %s." ), name );
            return result;
        case equipment_action::none:
            break;
    }
    return result;
}

equipment_command_result try_handle_equipment_command( npc &who,
        const std::string &player_line )
{
    equipment_action action = detect_equipment_action( player_line );
    if( action == equipment_action::none ) {
        return {};
    }

    // Removing a backpack by explicit player order means taking it off and
    // setting that same container down, not trying to stuff it into itself.
    if( action == equipment_action::take_off && asks_for_pack( player_line ) ) {
        action = equipment_action::drop;
    }
    add_msg_debug( debugmode::DF_NPC_ITEMAI,
                   "%s EQUIP_INTENT=%s WHY_PICKUP_WAS_SELECTED=not_selected",
                   who.get_name(), action_name( action ) );

    if( action == equipment_action::recover ) {
        // A backpack is functional equipment, not generic cargo.  "Recoge tu
        // mochila" therefore retains the established recover-and-wear routing
        // even when the player does not repeat "póntela" explicitly.
        const bool wear_after_recovery = asks_to_wear( player_line ) ||
                                         asks_for_pack( player_line );
        equipment_command_result remembered = request_remembered_equipment(
                who, player_line, wear_after_recovery );
        if( remembered.success || remembered.message !=
            _( "I don't have or remember a suitable item for that action." ) ) {
            return remembered;
        }
        if( wear_after_recovery ) {
            action = equipment_action::wear;
        } else {
            // "Recoge tu espada" may refer to a real visible object that has
            // no lost-equipment UID.  Let the general pickup resolver see it
            // instead of consuming the order in the recovery-only route.
            return {};
        }
    }

    std::vector<equipment_candidate> candidates = collect_candidates( who, action );
    if( candidates.empty() ) {
        if( action == equipment_action::wear ) {
            return request_remembered_equipment( who, player_line, true );
        }
        equipment_command_result result;
        result.handled = true;
        result.action = action;
        result.message = _( "I don't have a suitable item for that action." );
        return result;
    }

    const std::vector<std::size_t> matches = prefer_equipped_target(
                action, player_line, candidates, deterministic_matches( player_line, candidates ) );
    std::size_t selected = candidates.size();
    if( matches.size() == 1 ) {
        selected = matches.front();
    } else if( matches.empty() && candidates.size() == 1 &&
               !asks_for_pack( player_line ) && !asks_for_weapon( player_line ) &&
               !asks_for_headwear( player_line ) ) {
        selected = 0;
    }

    if( selected >= candidates.size() ) {
        if( action == equipment_action::wear ) {
            equipment_command_result remembered = request_remembered_equipment(
                    who, player_line, true );
            if( remembered.success || remembered.message !=
                _( "I don't have or remember a suitable item for that action." ) ) {
                return remembered;
            }
        }
        equipment_command_result result;
        result.handled = true;
        result.action = action;
        result.message = npc_ai::localized_ai_message(
                             _( "I can't determine which item you mean." ),
                             "No puedo determinar a qué objeto te refieres." );
        return result;
    }
    item_location target = candidates[selected].location;
    add_msg_debug( debugmode::DF_NPC_ITEMAI,
                   "%s TARGET_ITEM=%s TARGET_LOCATION=%s SOURCE=%s ACTION=%s",
                   who.get_name(), candidates[selected].name,
                   candidates[selected].location_description,
                   candidates[selected].ground ? "ground" :
                   candidates[selected].wielded ? "wielded" :
                   candidates[selected].worn ? "worn" : "inventory",
                   action_name( action ) );
    candidates.clear();
    const bool urgent_drop = action == equipment_action::drop &&
                             ( !who.is_safe() || contains_any( player_line, {
        "ahora", "urgente", "emergencia", "combate", "zombi", "zombie", "immediately"
    } ) );
    const ai_goal_id goal_id = urgent_drop ?
                               begin_goal( who, ai_goal_kind::drop_equipment,
                                           ai_goal_priority::emergency, player_line ) : 0;
    equipment_command_result result = execute_equipment_action(
                                          who, action, std::move( target ),
                                          urgent_drop ? "combat_emergency" : "player_order",
                                          urgent_drop );
    if( goal_id != 0 ) {
        if( result.success ) {
            complete_goal( who, goal_id );
        } else {
            fail_goal( who, goal_id, result.message );
        }
    }
    return result;
}

group_equipment_command_result execute_group_equipment_command(
    const std::vector<npc *> &targets, const std::string &player_line )
{
    group_equipment_command_result group;
    if( detect_equipment_action( player_line ) == equipment_action::none ) {
        return group;
    }

    for( npc *target : targets ) {
        if( target == nullptr ) {
            ++group.failed;
            continue;
        }
        const equipment_command_result result = try_handle_equipment_command( *target, player_line );
        if( !result.handled ) {
            continue;
        }
        group.handled = true;
        if( result.success ) {
            group.affected.push_back( target );
            group.pending += result.action_started;
            if( group.reply.empty() ) {
                group.reply = result.message;
            }
        } else {
            ++group.failed;
            if( group.failure_speaker == nullptr ) {
                group.failure_speaker = target;
                group.failure_reply = result.message.empty() ?
                                      npc_ai::localized_ai_message(
                                          _( "I don't remember leaving a matching item anywhere." ),
                                          "No recuerdo haber dejado en ningún sitio un objeto que coincida." ) :
                                      result.message;
            }
            if( group.reply.empty() ) {
                group.reply = group.failure_reply;
            }
        }
    }
    return group;
}

} // namespace npc_ai
