#include "npc_ai_pickup.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cata_utility.h"
#include "catacharset.h"
#include "item.h"
#include "item_location.h"
#include "map.h"
#include "map_selector.h"
#include "messages.h"
#include "npc.h"
#include "npc_ai_async.h"
#include "npc_ai_context.h"
#include "npc_ai_debug.h"
#include "npc_ai_memory.h"
#include "npc_ai_wield.h"
#include "output.h"
#include "point.h"
#include "rng.h"
#include "string_formatter.h"
#include "translations.h"
#include "unicode.h"

namespace
{

struct pickup_candidate {
    item_location location;
    tripoint_bub_ms position;
    std::string item_id;
    std::string name;
    int distance = 0;
    std::size_t physical_count = 1;
    int relevance_tier = 4;
    std::size_t match_strength = 0;
    std::size_t discovery_order = 0;
    int damage = 0;
    int charges = 0;
    bool associated_with_npc = false;
};

// Lowercase, accents removed, punctuation turned into spaces: the same
// normalisation the equipment and search orders use, so every "Objetos"
// order reads the player's words the same way.
std::string normalize_pickup_text( const std::string &text )
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
            ( codepoint >= U'0' && codepoint <= U'9' ) || codepoint == U'_' ) {
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

std::size_t pickup_name_match_strength( const std::string &player_line,
                                        const pickup_candidate &candidate )
{
    const std::string line = normalize_pickup_text( player_line );
    const std::string plain_name = normalize_pickup_text( candidate.name );
    const std::string item_id = normalize_pickup_text( candidate.item_id );
    if( !plain_name.empty() && line.find( plain_name ) != std::string::npos ) {
        return plain_name.size() + 1000;
    }

    static const std::unordered_set<std::string> ignored_words = {
        "agarra", "agarren", "coge", "coger", "cogela", "cogelo", "cojan", "el", "ella", "grab",
        "la", "las", "levanta", "levantar", "los", "mi", "mis", "pick", "pickup",
        "recoge", "recoger", "recogela", "recogelo", "recojan", "take", "toma", "tomar", "tu",
        "tus", "un", "una", "unos", "unas", "up", "your", "the", "del", "de", "que", "hay",
        "ahi", "alli", "aqui", "suelo", "por", "favor", "ahora", "please", "now", "there"
    };
    std::size_t strongest = 0;
    std::istringstream stream( line );
    std::string word;
    while( stream >> word ) {
        if( word.size() < 3 || ignored_words.count( word ) > 0 ) {
            continue;
        }
        if( plain_name.find( word ) != std::string::npos ||
            item_id.find( word ) != std::string::npos ) {
            strongest = std::max( strongest, word.size() );
        }
    }
    return strongest;
}

void rank_pickup_candidate( const npc &who, const std::string &player_line,
                            pickup_candidate &candidate )
{
    const item &physical = *candidate.location;
    if( physical.get_var( "npc_ai_equipment_owner", -1 ) == who.getID().get_value() ) {
        candidate.relevance_tier = 0;
        candidate.match_strength = 10000;
        return;
    }
    candidate.match_strength = pickup_name_match_strength( player_line, candidate );
    if( candidate.match_strength >= 1000 ) {
        candidate.relevance_tier = 1;
    } else if( candidate.match_strength > 0 ) {
        candidate.relevance_tier = 2;
    }
}

bool pickup_candidate_precedes( const pickup_candidate &lhs, const pickup_candidate &rhs )
{
    if( lhs.relevance_tier != rhs.relevance_tier ) {
        return lhs.relevance_tier < rhs.relevance_tier;
    }
    if( lhs.match_strength != rhs.match_strength ) {
        return lhs.match_strength > rhs.match_strength;
    }
    return std::make_tuple( lhs.distance, lhs.position.z(), lhs.position.y(), lhs.position.x(),
                            lhs.item_id, lhs.name, lhs.discovery_order ) <
           std::make_tuple( rhs.distance, rhs.position.z(), rhs.position.y(), rhs.position.x(),
                            rhs.item_id, rhs.name, rhs.discovery_order );
}

std::string normalized_command_text( std::string text )
{
    for( char &ch : text ) {
        const unsigned char uch = static_cast<unsigned char>( ch );
        if( ch >= 'A' && ch <= 'Z' ) {
            ch = static_cast<char>( ch - 'A' + 'a' );
        } else if( uch < 128 && std::ispunct( uch ) ) {
            ch = ' ';
        }
    }
    for( const std::string marker : { std::string( "¡" ), std::string( "¿" ) } ) {
        for( std::size_t pos = text.find( marker ); pos != std::string::npos;
             pos = text.find( marker, pos ) ) {
            text.replace( pos, marker.size(), " " );
        }
    }

    std::string collapsed;
    collapsed.reserve( text.size() );
    bool previous_space = true;
    for( const char ch : text ) {
        const bool space = std::isspace( static_cast<unsigned char>( ch ) );
        if( !space ) {
            collapsed.push_back( ch );
        } else if( !previous_space ) {
            collapsed.push_back( ' ' );
        }
        previous_space = space;
    }
    if( !collapsed.empty() && collapsed.back() == ' ' ) {
        collapsed.pop_back();
    }
    return collapsed;
}

std::string matched_command_phrase( const std::string &normalized,
                                    const std::vector<std::string> &phrases )
{
    const std::string padded = " " + normalized + " ";
    for( const std::string &phrase : phrases ) {
        if( padded.find( " " + phrase + " " ) != std::string::npos ) {
            return phrase;
        }
    }
    return {};
}

npc_ai::acquisition_intent resolve_ambiguous_intent_for_target(
    const npc_ai::acquisition_intent classified, const item &target,
    std::string &source )
{
    if( classified != npc_ai::acquisition_intent::automatic ) {
        return classified;
    }
    if( target.is_gun() || target.is_maybe_melee_weapon() ) {
        source += "+weapon_target";
        return npc_ai::acquisition_intent::wield;
    }
    source += "+nonweapon_target";
    return npc_ai::acquisition_intent::store;
}

std::string build_resolver_prompt(
    const std::string &player_line,
    const std::vector<pickup_candidate> &candidates
)
{
    std::ostringstream prompt;

    prompt
        << "ORDEN DEL JUGADOR:\n"
        << player_line
        << "\n\n"
        << "CANDIDATOS REALES VISIBLES:\n";

    for( std::size_t i = 0; i < candidates.size(); ++i ) {

        const pickup_candidate &candidate = candidates[i];

        prompt
            << ( i + 1 )
            << " | id="
            << candidate.item_id
            << " | nombre="
            << candidate.name
            << " | posicion="
            << candidate.position.to_string_writable()
            << " | distancia="
            << candidate.distance
            << " | cantidad="
            << candidate.physical_count
            << " | damage="
            << candidate.damage
            << " | charges="
            << candidate.charges
            << " | association="
            << ( candidate.associated_with_npc ? "owned_equipment" : "none" )
            << "\n";
    }

    return prompt.str();
}

int parse_pickup_index(
    const std::string &response
)
{
    const std::string marker = "PICKUP_INDEX=";

    const std::size_t marker_pos =
        response.find( marker );

    if( marker_pos == std::string::npos ) {
        return -1;
    }

    std::size_t pos =
        marker_pos + marker.size();

    if( pos >= response.size() ) {
        return -1;
    }

    int value = 0;
    bool found_digit = false;

    while( pos < response.size() ) {

        const char c = response[pos];

        if( c < '0' || c > '9' ) {
            break;
        }

        found_digit = true;

        value =
            value * 10 +
            static_cast<int>( c - '0' );

        ++pos;
    }

    return found_digit ? value : -1;
}

} // namespace

namespace npc_ai
{

acquisition_intent_classification classify_acquisition_intent(
    const std::string &player_line )
{
    acquisition_intent_classification result;
    if( player_line.find( '?' ) != std::string::npos ||
        player_line.find( "¿" ) != std::string::npos ) {
        return result;
    }

    static const std::vector<std::string> wield_phrases = {
        "empuña", "empuna", "empuñar", "empunar", "empúñala", "empunala",
        "empúñalo", "empunalo", "empuñen", "empunen", "empuñad", "empunad",
        "equipa", "equipar", "equípate", "equipate", "equípense", "equipense",
        "equipaos", "wield", "equip"
    };
    static const std::vector<std::string> store_phrases = {
        "recoge", "recoger", "recógelo", "recogelo", "recógela", "recogela",
        "recojan", "recoged", "coge", "coger", "cógelo", "cogelo", "cógela",
        "cogela", "cojan", "coged", "agarra", "agarrar", "agarren", "agarrad",
        "levanta", "levantar", "levanten", "levantad", "pick up", "pickup", "grab"
    };
    static const std::vector<std::string> ambiguous_phrases = {
        "toma", "tomar", "tómalo", "tomalo", "tómala", "tomala", "tomen",
        "tomad", "take"
    };

    const std::string normalized = normalized_command_text( player_line );
    if( const std::string phrase = matched_command_phrase( normalized, wield_phrases );
        !phrase.empty() ) {
        result.command = true;
        result.intent = acquisition_intent::wield;
        result.source = "explicit_wield:" + phrase;
        return result;
    }
    if( const std::string phrase = matched_command_phrase( normalized, store_phrases );
        !phrase.empty() ) {
        result.command = true;
        result.intent = acquisition_intent::store;
        result.source = "explicit_store:" + phrase;
        return result;
    }
    if( const std::string phrase = matched_command_phrase( normalized, ambiguous_phrases );
        !phrase.empty() ) {
        result.command = true;
        result.intent = acquisition_intent::automatic;
        result.source = "ambiguous:" + phrase;
    }
    return result;
}

const char *acquisition_intent_name( const acquisition_intent intent )
{
    switch( intent ) {
        case acquisition_intent::automatic:
            return "AUTO";
        case acquisition_intent::wield:
            return "WIELD";
        case acquisition_intent::store:
            return "STORE";
    }
    return "AUTO";
}

namespace
{

struct directed_pickup_start {
    bool started = false;
    std::string message;
};

// Shared tail of both resolution paths (deterministic shortcut and model
// completion): resolve the final destination for the target and hand the
// physical pickup to the vanilla activity.
directed_pickup_start begin_directed_pickup( npc &who, const item_location &target,
        const tripoint_bub_ms &position, const std::string &name,
        const std::string &player_line, const acquisition_intent classified,
        std::string intent_source, const char *selection_reason )
{
    directed_pickup_start start;
    if( intent_source.empty() ) {
        intent_source = "automatic";
    }
    const acquisition_intent resolved_intent = resolve_ambiguous_intent_for_target(
            classified, *target, intent_source );

    const bool generic_capacity = who.can_take_that( *target );
    add_msg_debug( debugmode::DF_NPC_ITEMAI,
                   "%s EQUIP_INTENT=PICKUP TARGET_ITEM=%s TARGET_LOCATION=%s SOURCE=ground "
                   "GENERIC_PICKUP_CAPACITY_CHECK=%s WHY_PICKUP_WAS_SELECTED=%s "
                   "ACQUISITION_INTENT=%s INTENT_SOURCE=%s",
                   who.get_name(), name, position.to_string_writable(),
                   generic_capacity ? "success" : "failure", selection_reason,
                   acquisition_intent_name( resolved_intent ), intent_source );
    std::string error;
    if( !who.ai_request_pickup( target, position, error, true, player_line, resolved_intent,
                                intent_source ) ) {
        start.message = string_format( _( "I can't pick up %1$s: %2$s" ), name, error );
        return start;
    }
    start.started = true;
    start.message = resolved_intent == acquisition_intent::wield ?
                    string_format( npc_ai::localized_ai_message(
                                       _( "I'm going to pick up and wield %s." ),
                                       "Voy a recoger y empuñar %s." ), name ) :
                    string_format( npc_ai::localized_ai_message(
                                       _( "I'm going to pick up %s." ),
                                       "Voy a recoger %s." ), name );
    return start;
}

} // namespace

pickup_command_result try_handle_pickup_command(
    npc &who,
    const std::string &player_line
)
{
    pickup_command_result result;

    const acquisition_intent_classification classification =
        classify_acquisition_intent( player_line );
    if( !classification.command ) {
        return result;
    }

    result.handled = true;
    result.intent = classification.intent;
    result.intent_source = classification.source;

    npc_ai::debug_stream debug( "npc_ai_pickup_v1_runtime.txt", true );

    if( debug ) {
        debug
            << "CDDA-AI PICKUP V1 RUNTIME\n"
            << "NPC="
            << who.get_name()
            << "\n"
            << "REQUEST="
            << player_line
            << "\nACQUISITION_INTENT="
            << acquisition_intent_name( classification.intent )
            << "\nINTENT_SOURCE="
            << classification.source
            << "\n\n";
    }

    map &here = get_map();

    const tripoint_bub_ms origin =
        who.pos_bub( here );

    std::vector<pickup_candidate> candidates;
    std::unordered_map<std::string, std::vector<std::size_t>> logical_groups;
    std::size_t raw_physical_candidates = 0;

    for( const tripoint_bub_ms &p :
         here.points_in_radius( origin, 6 ) ) {

        if( p.z() != origin.z() ) {
            continue;
        }

        if( !who.sees( here, p ) ) {
            continue;
        }

        if( !here.could_see_items( p, who ) ) {
            continue;
        }

        map_stack items =
            here.i_at( p );

        for( item &it : items ) {

            if( classification.intent == acquisition_intent::wield &&
                !it.is_gun() && !it.is_maybe_melee_weapon() ) {
                continue;
            }

            ++raw_physical_candidates;

            const std::string clean_name = remove_color_tags( it.tname() );
            const std::string grouping_key = p.to_string_writable() + "|" +
                                             it.typeId().str() + "|" + clean_name;
            bool grouped = false;
            for( const std::size_t index : logical_groups[grouping_key] ) {
                if( candidates[index].location->display_stacked_with( it, true ) ) {
                    ++candidates[index].physical_count;
                    grouped = true;
                    break;
                }
            }
            if( grouped ) {
                continue;
            }

            pickup_candidate candidate;

            candidate.location =
                item_location(
                    map_cursor( &here, p ),
                    &it
                );

            candidate.position = p;
            candidate.item_id = it.typeId().str();
            candidate.name = clean_name;
            candidate.distance = rl_dist( origin, p );
            candidate.discovery_order = candidates.size();
            candidate.damage = it.damage();
            candidate.charges = it.charges;
            candidate.associated_with_npc =
                it.get_var( "npc_ai_equipment_owner", -1 ) == who.getID().get_value();
            rank_pickup_candidate( who, player_line, candidate );

            candidates.push_back( candidate );
            logical_groups[grouping_key].push_back( candidates.size() - 1 );
        }
    }

    const std::size_t logical_candidate_count = candidates.size();
    std::sort( candidates.begin(), candidates.end(), pickup_candidate_precedes );
    constexpr std::size_t resolver_candidate_limit = 30;
    if( candidates.size() > resolver_candidate_limit ) {
        candidates.resize( resolver_candidate_limit );
    }

    if( debug ) {

        debug
            << "RAW_PHYSICAL_CANDIDATES="
            << raw_physical_candidates
            << "\n"
            << "LOGICAL_CANDIDATES="
            << logical_candidate_count
            << "\n"
            << "PRIORITIZED_CANDIDATES="
            << logical_candidate_count
            << "\n"
            << "FINAL_CANDIDATES="
            << candidates.size()
            << "\n";

        for( std::size_t i = 0; i < candidates.size(); ++i ) {

            debug
                << ( i + 1 )
                << " | id="
                << candidates[i].item_id
                << " | name="
                << candidates[i].name
                << " | distance="
                << candidates[i].distance
                << " | pos="
                << candidates[i].position.to_string_writable()
                << " | count="
                << candidates[i].physical_count
                << " | priority="
                << candidates[i].relevance_tier
                << "\n";
        }

        debug << "\n";
    }

    if( candidates.empty() ) {

        if( classification.intent == acquisition_intent::wield ) {
            // No ground acquisition target exists.  Let the existing wield
            // route resolve an item that is already carried.
            result.handled = false;
            return result;
        }

        result.message =
            "No veo ningun objeto que pueda recoger ahora.";

        if( debug ) {
            debug << "RESULT=NO_CANDIDATES\n";
        }

        return result;
    }

    // Deterministic shortcut.  When the player's words name exactly one of
    // the visible candidates (full item name, or a word of the order found in
    // the item name or id) there is nothing for the model to disambiguate, so
    // the order executes at once without a request.  Zero or several name
    // matches still go to the resolver, exactly as before.  Candidates that
    // only carry this NPC's equipment tag (tier 0) are deliberately excluded:
    // that flow belongs to equipment recovery and keeps its resolver prompt.
    std::size_t matching = 0;
    std::size_t matching_index = 0;
    for( std::size_t i = 0; i < candidates.size(); ++i ) {
        if( candidates[i].relevance_tier == 1 || candidates[i].relevance_tier == 2 ) {
            ++matching;
            matching_index = i;
        }
    }
    if( matching == 1 ) {
        pickup_candidate &selected = candidates[matching_index];
        if( debug ) {
            debug << "RESOLVER=deterministic_unique_match\n"
                  << "SELECTED=" << selected.name << "\n";
        }
        const directed_pickup_start start = begin_directed_pickup(
                                                who, selected.location, selected.position, selected.name,
                                                player_line, classification.intent, classification.source,
                                                "pickup_unique_name_match" );
        result.started = start.started;
        result.message = start.message;
        if( debug ) {
            debug << ( start.started ? "RESULT=STARTED\n" : "RESULT=ACTION_REJECTED\n" );
        }
        return result;
    }

    const std::string resolver_prompt =
        build_resolver_prompt(
            player_line,
            candidates
        );

    if( debug ) {
        debug
            << "=== RESOLVER PROMPT ===\n"
            << resolver_prompt
            << "\n";
    }

    std::vector<ai_target_snapshot> targets;
    targets.reserve( candidates.size() );
    for( pickup_candidate &candidate : candidates ) {
        std::string uid = candidate.location->get_var( "npc_ai_async_target_uid" );
        if( uid.empty() ) {
            uid = "pickup-" + random_string( 16 );
            candidate.location->set_var( "npc_ai_async_target_uid", uid );
        }
        const tripoint_abs_ms absolute = here.get_abs( candidate.position );
        targets.push_back( { uid, candidate.item_id, candidate.name, absolute.x(), absolute.y(),
                             absolute.z() } );
    }

    const ai_enqueue_result queued = enqueue_command_resolution(
                                         who, ai_request_type::pickup_resolution, player_line,
                                         resolver_prompt, std::move( targets ),
                                         classification.intent, classification.source );
    result.pending = queued.accepted;
    if( !queued.accepted ) {
        result.message = _( "I am still considering your previous request." );
    }
    return result;
}

group_acquisition_command_result execute_group_acquisition_command(
    const std::vector<npc *> &targets, const std::string &player_line )
{
    group_acquisition_command_result group;
    if( !classify_acquisition_intent( player_line ).command ) {
        return group;
    }

    for( npc *target : targets ) {
        if( target == nullptr ) {
            continue;
        }
        const pickup_command_result pickup = try_handle_pickup_command( *target, player_line );
        if( pickup.handled ) {
            group.handled = true;
            if( pickup.pending || pickup.started ) {
                group.affected.push_back( target );
                group.pending += pickup.pending;
            } else {
                ++group.failed;
                if( group.failure_speaker == nullptr ) {
                    group.failure_speaker = target;
                    group.failure_reply = pickup.message;
                }
            }
            continue;
        }

        const wield_command_result wield = try_handle_wield_command( *target, player_line );
        if( !wield.handled ) {
            continue;
        }
        group.handled = true;
        if( wield.pending || wield.success ) {
            group.affected.push_back( target );
            group.pending += wield.pending;
            if( group.reply.empty() && !wield.message.empty() ) {
                group.reply = wield.message;
            }
        } else {
            ++group.failed;
            if( group.failure_speaker == nullptr ) {
                group.failure_speaker = target;
                group.failure_reply = wield.message;
            }
        }
    }
    return group;
}

void apply_pickup_ai_completion( npc &who, const ai_request_completion &completion )
{
    const auto speak = [&]( const std::string &message ) {
        say_command_reply( who, message );
        remember_exchange( who, completion.request.player_line, message );
    };

    if( !completion.response.success ) {
        speak( _( "I couldn't determine which object you wanted me to pick up." ) );
        return;
    }

    const int selected_index = parse_pickup_index( completion.response.text );
    if( selected_index <= 0 ||
        selected_index > static_cast<int>( completion.request.targets.size() ) ) {
        speak( _( "I can't safely identify the object you mean." ) );
        return;
    }

    const ai_target_snapshot &selected = completion.request.targets[
            static_cast<std::size_t>( selected_index - 1 )];
    map &here = get_map();
    const tripoint_bub_ms position = here.get_bub( tripoint_abs_ms{
        selected.x, selected.y, selected.z
    } );
    if( !here.inbounds( position ) || !who.sees( here, position ) ||
        !here.could_see_items( position, who ) ) {
        speak( _( "I can no longer see that object." ) );
        return;
    }

    item_location target = item_location::nowhere;
    for( item &it : here.i_at( position ) ) {
        if( it.get_var( "npc_ai_async_target_uid" ) == selected.uid ) {
            target = item_location( map_cursor( &here, position ), &it );
            break;
        }
    }
    if( !target ) {
        speak( _( "That object is no longer there." ) );
        return;
    }

    const directed_pickup_start start = begin_directed_pickup(
                                            who, target, position, selected.name,
                                            completion.request.player_line, completion.request.acquisition,
                                            completion.request.acquisition_intent_source, "pickup_router" );
    speak( start.message );
}

} // namespace npc_ai
