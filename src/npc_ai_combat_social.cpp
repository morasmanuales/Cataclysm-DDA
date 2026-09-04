#include "npc_ai_combat_social.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <map>
#include <limits>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "calendar.h"
#include "cata_utility.h"
#include "character.h"
#include "character_id.h"
#include "creature_tracker.h"
#include "debug.h"
#include "game.h"
#include "item.h"
#include "item_location.h"
#include "json.h"
#include "map.h"
#include "messages.h"
#include "monster.h"
#include "mtype.h"
#include "npc.h"
#include "output.h"
#include "npc_ai_async.h"
#include "npc_ai_context.h"
#include "npc_ai_debug.h"
#include "npc_ai_event_stream.h"
#include "npc_ai_memory.h"
#include "npc_ai_profiler.h"
#include "npc_ai_spontaneous.h"
#include "point.h"
#include "sounds.h"
#include "weather.h"

namespace
{

constexpr std::size_t max_visible_creatures = 12;
// Vanilla has already classified nearby creatures in npc::regen_ai_cache().
// Combat Social may revalidate a bounded suffix for observer-specific LOS,
// but must not rescan the complete global creature range for every NPC.
constexpr std::size_t max_creature_visibility_checks = max_visible_creatures * 2;
constexpr std::size_t max_audible_events = 4;
constexpr int raw_ordinary_request_gap_turns = 30;
constexpr int raw_urgent_request_gap_turns = 5;
// Combat Social now batches physical facts into several scheduled lines.  The
// remaining per-NPC snapshot route is paced for readable bursts instead of
// the old request-minimisation target.
constexpr int ordinary_request_gap_turns = 8;
constexpr int urgent_request_gap_turns = 2;
constexpr int duplicate_event_gap_turns = 120;
constexpr int combat_result_max_age_turns = 45;
constexpr int post_combat_result_max_age_turns = 90;
constexpr std::size_t max_pending_candidates = 12;
constexpr int candidate_max_age_turns = 120;
constexpr std::size_t max_batch_facts = 5;
constexpr std::size_t max_batch_candidates = 4;
constexpr std::size_t max_batch_inferences_per_suffix = 2;
constexpr int batch_line_spacing_turns = 1;
constexpr int batch_line_expiry_turns = 12;
// Outside combat, vanilla's refreshed target/danger cache and physical event
// stream provide immediate wakeups.  This fallback scan catches non-combat
// visual transitions without rebuilding LOS on every NPC action.
constexpr int idle_snapshot_interval_turns = 5;

static const efftype_id effect_bleed( "bleed" );
static const efftype_id effect_npc_run_away( "npc_run_away" );
static const json_character_flag json_flag_GRAB( "GRAB" );

struct combat_social_state {
    bool initialized = false;
    npc_ai::combat_perception_snapshot last;
    std::uint64_t encounter_generation = 0;
    std::uint64_t event_generation = 0;
    std::uint64_t pending_request_id = 0;
    int pending_importance = 0;
    npc_ai::combat_social_event_type pending_type =
        npc_ai::combat_social_event_type::combat_end;
    int last_request_turn = -1000000000;
    int last_spoken_turn = -1000000000;
    npc_ai::combat_social_event_type last_requested_type =
        npc_ai::combat_social_event_type::combat_end;
    std::uint64_t last_seen_world_sequence = 0;
    int last_snapshot_turn = -1000000000;
    int next_idle_snapshot_turn = -1000000000;
    std::size_t last_cached_friend_count = 0;
    std::size_t last_active_npc_count = 0;
    std::size_t last_monster_count = 0;
    std::uint64_t last_group_state_fingerprint = 0;
    std::unordered_map<std::string, int> last_event_turn;
    struct candidate {
        npc_ai::combat_social_event event;
        int first_turn = 0;
        int last_turn = 0;
    };
    std::vector<candidate> candidates;
};

std::unordered_map<int, combat_social_state> combat_states;

npc_ai::combat_social_metrics social_metrics;
std::unordered_set<std::uint64_t> narrable_event_ids;
std::unordered_set<std::uint64_t> verbalized_event_ids;
std::unordered_set<std::uint64_t> batched_event_ids;
std::vector<std::size_t> observed_queue_depths;
std::unordered_map<int, std::uint64_t> emitted_by_speaker;
bool batching_enabled = true;

struct active_first_sight_fact {
    std::uint64_t sequence_id = 0;
    bool request_reserved = false;
};

// This is a bounded lifecycle index over facts in the existing world event
// stream, not a permanent identity blacklist.  An entry is removed as soon as
// no living allied observer's latest snapshot still contains the hostile.
std::unordered_map<std::string, active_first_sight_fact> active_first_sight_facts;

struct pending_combat_line {
    int speaker_id = -1;
    std::string text;
    std::vector<std::uint64_t> event_ids;
    int speak_priority = 0;
    int earliest_turn = 0;
    int expiry_turn = 0;
    int combat_event_type = -1;
    int target_id = -1;
};

std::vector<pending_combat_line> pending_combat_lines;
int last_group_line_turn = -1000000000;

struct group_state_cache {
    int turn = -1000000000;
    std::uint64_t fingerprint = 0;
};

group_state_cache cached_group_state;

int current_turn_number()
{
    return to_turn<int>( calendar::turn );
}

int npc_key( const npc &who )
{
    return who.getID().get_value();
}

std::uint64_t group_state_fingerprint()
{
    const int now = current_turn_number();
    if( cached_group_state.turn == now ) {
        return cached_group_state.fingerprint;
    }

    std::uint64_t fingerprint = 1469598103934665603ULL;
    const auto mix = [&]( const std::uint64_t value ) {
        fingerprint ^= value;
        fingerprint *= 1099511628211ULL;
    };
    const auto mix_character = [&]( const Character &character ) {
        mix( static_cast<std::uint64_t>( character.getID().get_value() ) );
        mix( static_cast<std::uint64_t>( character.hp_percentage() ) );
        mix( character.has_effect( effect_bleed ) ? 1ULL : 0ULL );
        mix( character.has_effect_with_flag( json_flag_GRAB ) ? 1ULL : 0ULL );
        if( const npc *other_npc = dynamic_cast<const npc *>( &character ) ) {
            mix( other_npc->has_effect( effect_npc_run_away ) ? 1ULL : 0ULL );
        }
    };

    mix_character( get_player_character() );
    for( const npc &other : g->all_npcs() ) {
        if( !other.is_dead_state() && !other.is_hallucination() ) {
            mix_character( other );
        }
    }
    cached_group_state.turn = now;
    cached_group_state.fingerprint = fingerprint;
    return fingerprint;
}

std::string lower_ascii( std::string text )
{
    for( char &c : text ) {
        if( c >= 'A' && c <= 'Z' ) {
            c = static_cast<char>( c - 'A' + 'a' );
        }
    }
    return text;
}

std::string trim_copy( std::string text )
{
    const auto not_space = []( const unsigned char c ) { return !std::isspace( c ); };
    text.erase( text.begin(), std::find_if( text.begin(), text.end(), not_space ) );
    text.erase( std::find_if( text.rbegin(), text.rend(), not_space ).base(), text.end() );
    return text;
}

std::string one_line( std::string text )
{
    for( char &c : text ) {
        if( c == '\r' || c == '\n' || c == '\t' ) {
            c = ' ';
        }
    }
    while( text.find( "  " ) != std::string::npos ) {
        text.replace( text.find( "  " ), 2, " " );
    }
    return trim_copy( std::move( text ) );
}

std::string visible_condition( const Creature &creature )
{
    const int hp = creature.hp_percentage();
    if( hp >= 95 ) {
        return "sin heridas visibles";
    }
    if( hp >= 75 ) {
        return "ligeramente herido";
    }
    if( hp >= 45 ) {
        return "herido";
    }
    if( hp >= 20 ) {
        return "gravemente herido";
    }
    return "al borde de la muerte";
}

std::string relative_direction( const int dx, const int dy, const int dz )
{
    if( dz > 0 ) {
        return "arriba";
    }
    if( dz < 0 ) {
        return "abajo";
    }
    std::string vertical;
    std::string horizontal;
    if( dy < 0 ) {
        vertical = "norte";
    } else if( dy > 0 ) {
        vertical = "sur";
    }
    if( dx < 0 ) {
        horizontal = "oeste";
    } else if( dx > 0 ) {
        horizontal = "este";
    }
    if( vertical.empty() ) {
        return horizontal.empty() ? "aqui" : horizontal;
    }
    return horizontal.empty() ? vertical : vertical + "-" + horizontal;
}

std::string sound_kind( const sounds::sound_t type )
{
    switch( type ) {
        case sounds::sound_t::combat:
            return "combate";
        case sounds::sound_t::alert:
            return "alerta";
        case sounds::sound_t::alarm:
            return "alarma";
        case sounds::sound_t::destructive_activity:
            return "destruccion";
        case sounds::sound_t::speech:
        case sounds::sound_t::electronic_speech:
            return "voz";
        default:
            return "ruido";
    }
}

const npc_ai::combat_visible_creature *
find_character( const npc_ai::combat_perception_snapshot &snapshot, const int id )
{
    const auto found =
        std::find_if( snapshot.visible_creatures.begin(), snapshot.visible_creatures.end(),
                      [id]( const npc_ai::combat_visible_creature &creature ) {
                          return creature.character_id == id;
                      } );
    return found == snapshot.visible_creatures.end() ? nullptr : &*found;
}

const npc_ai::combat_visible_creature *find_named_target(
    const npc_ai::combat_perception_snapshot &snapshot, const std::string &name )
{
    const auto found = std::find_if( snapshot.visible_creatures.begin(),
    snapshot.visible_creatures.end(), [&]( const npc_ai::combat_visible_creature &creature ) {
        return creature.name == name;
    } );
    return found == snapshot.visible_creatures.end() ? nullptr : &*found;
}

const npc_ai::combat_visible_creature *
find_player( const npc_ai::combat_perception_snapshot &snapshot )
{
    const auto found = std::find_if(
        snapshot.visible_creatures.begin(), snapshot.visible_creatures.end(),
        []( const npc_ai::combat_visible_creature &creature ) { return creature.player; } );
    return found == snapshot.visible_creatures.end() ? nullptr : &*found;
}

std::string visible_creature_identity( const npc_ai::combat_visible_creature &creature )
{
    if( creature.character_id >= 0 ) {
        return "character:" + std::to_string( creature.character_id );
    }
    return "runtime:" + std::to_string( creature.runtime_identity );
}

const npc_ai::combat_visible_creature *find_hostile_identity(
    const npc_ai::combat_perception_snapshot &snapshot, const std::string &identity )
{
    const auto found = std::find_if( snapshot.visible_creatures.begin(),
    snapshot.visible_creatures.end(), [&]( const npc_ai::combat_visible_creature &creature ) {
        return creature.hostile && visible_creature_identity( creature ) == identity;
    } );
    return found == snapshot.visible_creatures.end() ? nullptr : &*found;
}

bool allied_group_still_sees_hostile( const std::string &identity )
{
    if( g == nullptr ) {
        return false;
    }
    for( const auto &entry : combat_states ) {
        const combat_social_state &state = entry.second;
        if( !state.initialized || find_hostile_identity( state.last, identity ) == nullptr ) {
            continue;
        }
        npc *observer = g->find_npc( character_id( entry.first ) );
        if( observer != nullptr && observer->is_player_ally() && !observer->is_dead_state() &&
            !observer->is_hallucination() && !observer->in_sleep_state() ) {
            return true;
        }
    }
    return false;
}

void expire_inactive_first_sight_facts()
{
    for( auto iter = active_first_sight_facts.begin();
         iter != active_first_sight_facts.end(); ) {
        if( allied_group_still_sees_hostile( iter->first ) ) {
            ++iter;
        } else {
            iter = active_first_sight_facts.erase( iter );
        }
    }
}

void debug_first_sight_decision( const npc &observer,
                                 const npc_ai::combat_social_event &event,
                                 const std::string &decision );

void reserve_group_first_sight_request( const npc_ai::combat_social_event &selected )
{
    if( !selected.observer_first_sight || selected.target_identity.empty() ) {
        return;
    }
    active_first_sight_fact &active = active_first_sight_facts[selected.target_identity];
    active.sequence_id = selected.sequence_id;
    active.request_reserved = true;

    for( auto &state_entry : combat_states ) {
        npc *observer = g == nullptr ? nullptr :
                        g->find_npc( character_id( state_entry.first ) );
        std::vector<combat_social_state::candidate> &candidates = state_entry.second.candidates;
        candidates.erase( std::remove_if( candidates.begin(), candidates.end(),
        [&]( combat_social_state::candidate &candidate ) {
            if( !candidate.event.observer_first_sight ||
                candidate.event.target_identity != selected.target_identity ) {
                return false;
            }
            candidate.event.group_already_verbalized = true;
            if( observer != nullptr ) {
                debug_first_sight_decision( *observer, candidate.event, "known_by_only" );
            }
            return true;
        } ), candidates.end() );
    }
}

void debug_first_sight_decision( const npc &observer,
                                 const npc_ai::combat_social_event &event,
                                 const std::string &decision )
{
    if( !npc_ai::runtime_debug_enabled() || !event.observer_first_sight ) {
        return;
    }
    std::ostringstream line;
    line << "FIRST_SIGHT NPC=" << observer.get_name()
         << " identity=" << event.target_identity
         << " name=" << event.target_name
         << " prev_count=" << event.previous_visible_enemy_count
         << " now_count=" << event.current_visible_enemy_count
         << " observer_new=yes"
         << " group_already_verbalized="
         << ( event.group_already_verbalized ? "yes" : "no" )
         << " bypass=" << ( event.may_bypass_cooldown ? "yes" : "no" )
         << " EVENT=ENEMY_SPOTTED"
         << " coalesced=" << ( event.coalesced_sequences.empty() ? "no" : "yes" )
         << " SOCIAL_DECISION=" << decision;
    npc_ai::append_debug_line( "npc_ai_ollama_diagnostics.txt", line.str() );
}

std::string event_key( const npc_ai::combat_social_event &event )
{
    if( event.type == npc_ai::combat_social_event_type::dragged ||
        event.type == npc_ai::combat_social_event_type::ally_dragged ) {
        // Repeated tile-by-tile dragging is one continuing situation for the
        // victim even though the monster's snapshot position changes.
        return std::to_string( event.encounter_generation ) + ":" +
               npc_ai::combat_social_event_name( event.type ) + ":" + event.target_identity;
    }
    return std::to_string( event.encounter_generation ) + ":" +
           npc_ai::combat_social_event_name( event.type ) + ":" +
           event.actor_identity + ":" +
           std::to_string( event.target_id ) + ":" + event.target_identity + ":" +
           event.target_name;
}

std::string shared_social_event_family( const npc_ai::combat_social_event_type type )
{
    using event_type = npc_ai::combat_social_event_type;
    switch( type ) {
        case event_type::npc_hit:
        case event_type::player_hit:
        case event_type::ally_hit: return "character_hit";
        case event_type::npc_badly_hurt:
        case event_type::player_badly_hurt:
        case event_type::ally_badly_hurt: return "character_badly_hurt";
        case event_type::npc_bleeding:
        case event_type::player_bleeding:
        case event_type::ally_bleeding: return "character_bleeding";
        case event_type::npc_grabbed:
        case event_type::player_grabbed:
        case event_type::ally_grabbed: return "character_grabbed";
        case event_type::player_surrounded:
        case event_type::ally_surrounded: return "character_surrounded";
        case event_type::retreat_started:
        case event_type::ally_retreating: return "character_retreating";
        case event_type::dragged:
        case event_type::ally_dragged: return "character_dragged";
        case event_type::significant_critical:
        case event_type::ally_critical_hit:
        case event_type::player_critical_hit: return "significant_critical";
        default: return npc_ai::combat_social_event_name( type );
    }
}

std::string shared_social_event_key( const npc_ai::combat_social_event &event )
{
    using event_type = npc_ai::combat_social_event_type;
    std::string subject = event.target_identity;
    if( subject.empty() ) {
        switch( event.type ) {
            case event_type::combat_start:
            case event_type::combat_end:
            case event_type::enemy_group_detected:
            case event_type::dangerous_enemy_spotted:
                break;
            default:
                subject = event.actor_identity;
                break;
        }
    }
    return "combat:" + std::to_string( event.encounter_generation ) + ":" +
           shared_social_event_family( event.type ) + ":" + subject;
}

std::string entity_identity( const npc_ai::world_entity_snapshot &entity )
{
    if( entity.character_id >= 0 ) {
        return "character:" + std::to_string( entity.character_id );
    }
    return entity.kind + ":" + entity.type_id + ":" + std::to_string( entity.x ) + "," +
           std::to_string( entity.y ) + "," + std::to_string( entity.z );
}

void add_candidate( combat_social_state &state, npc_ai::combat_social_event event, const int now )
{
    const std::string key = event_key( event );
    auto existing = std::find_if( state.candidates.begin(), state.candidates.end(),
    [&]( const combat_social_state::candidate &candidate ) {
        return event_key( candidate.event ) == key;
    } );
    if( existing != state.candidates.end() ) {
        existing->last_turn = now;
        existing->event.importance = std::max( existing->event.importance, event.importance );
        existing->event.may_bypass_cooldown = existing->event.may_bypass_cooldown ||
                                              event.may_bypass_cooldown;
        if( event.observer_first_sight ) {
            existing->event.observer_first_sight = true;
            existing->event.previous_visible_enemy_count =
                event.previous_visible_enemy_count;
            existing->event.current_visible_enemy_count = event.current_visible_enemy_count;
            existing->event.group_already_verbalized = event.group_already_verbalized;
        }
        if( event.sequence_id != 0 &&
            std::find( existing->event.coalesced_sequences.begin(),
                       existing->event.coalesced_sequences.end(), event.sequence_id ) ==
            existing->event.coalesced_sequences.end() ) {
            existing->event.coalesced_sequences.push_back( event.sequence_id );
            npc_ai::annotate_world_event( event.sequence_id, "coalesced", false, 0,
                                          "merged_with_equivalent_candidate" );
        }
        if( !event.first_sight_count_companion &&
            existing->event.detail.find( event.detail ) == std::string::npos ) {
            existing->event.detail += " " + event.detail;
        }
        return;
    }
    combat_social_state::candidate candidate;
    candidate.event = std::move( event );
    candidate.first_turn = now;
    candidate.last_turn = now;
    state.candidates.push_back( std::move( candidate ) );
    if( state.candidates.size() > max_pending_candidates ) {
        const auto least = std::min_element( state.candidates.begin(), state.candidates.end(),
        []( const combat_social_state::candidate &lhs,
        const combat_social_state::candidate &rhs ) {
            return std::tie( lhs.event.importance, lhs.first_turn ) <
                   std::tie( rhs.event.importance, rhs.first_turn );
        } );
        npc_ai::annotate_world_event( least->event.sequence_id, "discarded", false, 0,
                                      "candidate_capacity" );
        state.candidates.erase( least );
    }
}

bool is_grab_sequence_event( const npc_ai::combat_social_event_type type )
{
    using type_t = npc_ai::combat_social_event_type;
    return type == type_t::npc_grabbed || type == type_t::ally_grabbed ||
           type == type_t::failed_escape || type == type_t::dragged ||
           type == type_t::ally_dragged || type == type_t::grab_broken;
}

int grab_subject_id( const combat_social_state::candidate &candidate, const int observer_id )
{
    using type_t = npc_ai::combat_social_event_type;
    if( candidate.event.type == type_t::failed_escape ||
        candidate.event.type == type_t::npc_grabbed ||
        candidate.event.type == type_t::dragged || candidate.event.type == type_t::grab_broken ) {
        return observer_id;
    }
    return candidate.event.target_id;
}

void coalesce_grab_sequence( combat_social_state &state, const int observer_id )
{
    for( std::size_t primary_index = 0; primary_index < state.candidates.size(); ++primary_index ) {
        combat_social_state::candidate &primary = state.candidates[primary_index];
        if( !is_grab_sequence_event( primary.event.type ) ) {
            continue;
        }
        for( std::size_t other_index = primary_index + 1; other_index < state.candidates.size(); ) {
            combat_social_state::candidate &other = state.candidates[other_index];
            if( !is_grab_sequence_event( other.event.type ) ||
                grab_subject_id( primary, observer_id ) != grab_subject_id( other, observer_id ) ||
                std::abs( primary.last_turn - other.last_turn ) > 8 ) {
                ++other_index;
                continue;
            }
            combat_social_state::candidate *destination = &primary;
            combat_social_state::candidate *source = &other;
            if( std::tie( other.event.importance, other.last_turn ) >
                std::tie( primary.event.importance, primary.last_turn ) ) {
                std::swap( primary.event, other.event );
                std::swap( primary.first_turn, other.first_turn );
                std::swap( primary.last_turn, other.last_turn );
            }
            destination = &primary;
            source = &other;
            if( destination->event.detail.find( source->event.detail ) == std::string::npos ) {
                destination->event.detail = source->event.detail + " " + destination->event.detail;
            }
            if( source->event.sequence_id != 0 ) {
                destination->event.coalesced_sequences.push_back( source->event.sequence_id );
                npc_ai::annotate_world_event( source->event.sequence_id, "coalesced", false, 0,
                                              "merged_into_grab_sequence" );
            }
            destination->event.coalesced_sequences.insert(
                destination->event.coalesced_sequences.end(),
                source->event.coalesced_sequences.begin(), source->event.coalesced_sequences.end() );
            destination->first_turn = std::min( destination->first_turn, source->first_turn );
            destination->last_turn = std::max( destination->last_turn, source->last_turn );
            state.candidates.erase( state.candidates.begin() + other_index );
        }
    }

    using type_t = npc_ai::combat_social_event_type;
    const auto is_grabbed_surrounded_pair = []( const type_t lhs, const type_t rhs ) {
        const bool lhs_player = lhs == type_t::player_grabbed ||
                                lhs == type_t::player_surrounded;
        const bool rhs_player = rhs == type_t::player_grabbed ||
                                rhs == type_t::player_surrounded;
        const bool lhs_ally = lhs == type_t::ally_grabbed ||
                              lhs == type_t::ally_surrounded;
        const bool rhs_ally = rhs == type_t::ally_grabbed ||
                              rhs == type_t::ally_surrounded;
        return ( lhs_player && rhs_player ) || ( lhs_ally && rhs_ally );
    };
    for( std::size_t primary_index = 0; primary_index < state.candidates.size(); ++primary_index ) {
        for( std::size_t other_index = primary_index + 1;
             other_index < state.candidates.size(); ) {
            combat_social_state::candidate &primary = state.candidates[primary_index];
            combat_social_state::candidate &other = state.candidates[other_index];
            if( !is_grabbed_surrounded_pair( primary.event.type, other.event.type ) ||
                primary.last_turn != other.last_turn ||
                primary.event.target_identity != other.event.target_identity ) {
                ++other_index;
                continue;
            }
            if( npc_ai::combat_social_speak_priority( other.event ) >
                npc_ai::combat_social_speak_priority( primary.event ) ) {
                std::swap( primary.event, other.event );
                std::swap( primary.first_turn, other.first_turn );
                std::swap( primary.last_turn, other.last_turn );
            }
            if( other.event.sequence_id != 0 ) {
                primary.event.coalesced_sequences.push_back( other.event.sequence_id );
                npc_ai::annotate_world_event( other.event.sequence_id, "coalesced", false, 0,
                                              "merged_into_simultaneous_grab_or_surrounded" );
            }
            primary.event.coalesced_sequences.insert(
                primary.event.coalesced_sequences.end(),
                other.event.coalesced_sequences.begin(), other.event.coalesced_sequences.end() );
            state.candidates.erase( state.candidates.begin() + other_index );
        }
    }
}

std::vector<npc_ai::ai_target_snapshot> capture_visible_targets(
    const npc_ai::combat_perception_snapshot &snapshot )
{
    std::vector<npc_ai::ai_target_snapshot> result;
    result.reserve( snapshot.visible_creatures.size() );
    for( const npc_ai::combat_visible_creature &creature : snapshot.visible_creatures ) {
        npc_ai::ai_target_snapshot target;
        target.uid = "visible_alive";
        target.item_id = creature.monster ? "monster" : creature.npc ? "npc" :
                         creature.player ? "player" : "creature";
        target.name = creature.name;
        target.x = snapshot.x + creature.dx;
        target.y = snapshot.y + creature.dy;
        target.z = snapshot.z + creature.dz;
        result.push_back( std::move( target ) );
    }
    return result;
}

bool response_mentions_entity_no_longer_visible( const std::string &spoken,
        const npc_ai::ai_request_snapshot &request,
        const npc_ai::combat_perception_snapshot &current )
{
    for( const npc_ai::ai_target_snapshot &target : request.targets ) {
        if( target.uid != "visible_alive" || target.name.empty() || !lcmatch( spoken, target.name ) ) {
            continue;
        }
        const bool still_visible = std::any_of(
            current.visible_creatures.begin(), current.visible_creatures.end(),
        [&]( const npc_ai::combat_visible_creature &creature ) {
            return creature.name == target.name;
        } );
        if( !still_visible ) {
            return true;
        }
    }
    return false;
}

void add_event( std::vector<npc_ai::combat_social_event> &events,
                const npc_ai::combat_social_event_type type, const int importance,
                const bool bypass, const std::string &detail,
                const npc_ai::combat_visible_creature *target = nullptr )
{
    npc_ai::combat_social_event event;
    event.type = type;
    event.importance = importance;
    event.may_bypass_cooldown = bypass;
    event.confirmed_outcome = true;
    event.detail = detail;
    switch( type ) {
        case npc_ai::combat_social_event_type::npc_hit:
        case npc_ai::combat_social_event_type::npc_badly_hurt:
        case npc_ai::combat_social_event_type::player_hit:
        case npc_ai::combat_social_event_type::player_badly_hurt:
        case npc_ai::combat_social_event_type::ally_hit:
        case npc_ai::combat_social_event_type::ally_badly_hurt:
        case npc_ai::combat_social_event_type::significant_critical:
        case npc_ai::combat_social_event_type::ally_critical_hit:
        case npc_ai::combat_social_event_type::player_critical_hit:
            event.claim_level = npc_ai::world_event_claim_level::hit_confirmed;
            break;
        case npc_ai::combat_social_event_type::enemy_killed:
            event.claim_level = npc_ai::world_event_claim_level::death_confirmed;
            break;
        default:
            break;
    }
    if( target != nullptr ) {
        event.target_id = target->character_id;
        event.target_name = target->name;
        event.target_identity = visible_creature_identity( *target );
    }
    events.push_back( std::move( event ) );
}

std::vector<npc_ai::combat_social_event>
detect_events( const npc_ai::combat_perception_snapshot &before,
               const npc_ai::combat_perception_snapshot &now )
{
    using event_type = npc_ai::combat_social_event_type;
    std::vector<npc_ai::combat_social_event> events;

    if( now.in_combat && !before.in_combat ) {
        add_event( events, event_type::combat_start, 82, false, "El combate acaba de comenzar." );
    } else if( !now.in_combat && before.in_combat ) {
        add_event( events, event_type::combat_end, 72, true,
                   "El encuentro acaba de terminar; ya no percibes enemigos inmediatos." );
    }

    const npc_ai::combat_visible_creature *old_target = nullptr;
    const npc_ai::combat_visible_creature *new_target = nullptr;
    for( const npc_ai::combat_visible_creature &creature : before.visible_creatures ) {
        if( creature.observer_target ) {
            old_target = &creature;
            break;
        }
    }
    for( const npc_ai::combat_visible_creature &creature : now.visible_creatures ) {
        if( creature.observer_target ) {
            new_target = &creature;
            break;
        }
    }
    if( new_target != nullptr &&
        find_hostile_identity( before, visible_creature_identity( *new_target ) ) != nullptr &&
        ( old_target == nullptr || old_target->name != new_target->name ||
          old_target->dx != new_target->dx || old_target->dy != new_target->dy ||
          old_target->dz != new_target->dz ) ) {
        add_event( events, event_type::npc_attack, 76, false,
                   "Tu objetivo de combate actual es " + new_target->name + ".", new_target );
    }

    std::unordered_set<std::string> previous_hostile_identities;
    for( const npc_ai::combat_visible_creature &creature : before.visible_creatures ) {
        if( creature.hostile ) {
            previous_hostile_identities.insert( visible_creature_identity( creature ) );
        }
    }
    std::vector<const npc_ai::combat_visible_creature *> newly_visible_hostiles;
    for( const npc_ai::combat_visible_creature &creature : now.visible_creatures ) {
        if( creature.hostile &&
            previous_hostile_identities.count( visible_creature_identity( creature ) ) == 0 ) {
            newly_visible_hostiles.push_back( &creature );
        }
    }
    if( !newly_visible_hostiles.empty() ) {
        const npc_ai::combat_visible_creature *mentioned = nullptr;
        if( new_target != nullptr ) {
            const std::string target_identity = visible_creature_identity( *new_target );
            const auto target_is_new = std::find_if(
                newly_visible_hostiles.begin(), newly_visible_hostiles.end(),
            [&]( const npc_ai::combat_visible_creature *candidate ) {
                return visible_creature_identity( *candidate ) == target_identity;
            } );
            if( target_is_new != newly_visible_hostiles.end() ) {
                mentioned = *target_is_new;
            }
        }
        if( mentioned == nullptr ) {
            mentioned = *std::min_element(
                newly_visible_hostiles.begin(), newly_visible_hostiles.end(),
            []( const npc_ai::combat_visible_creature *lhs,
            const npc_ai::combat_visible_creature *rhs ) {
                return lhs->distance < rhs->distance;
            } );
        }

        // tactical_danger is aggregate state.  No existing snapshot field is
        // an explicit per-creature threat metric, so an appearance is always
        // ENEMY_SPOTTED; the model may still use the grounded creature name.
        add_event( events, event_type::enemy_spotted, 74, true,
                   mentioned->name + " acaba de aparecer en tu linea de vision.", mentioned );
        npc_ai::combat_social_event &first_sight = events.back();
        first_sight.observer_first_sight = true;
        first_sight.previous_visible_enemy_count = before.visible_enemy_count;
        first_sight.current_visible_enemy_count = now.visible_enemy_count;

        if( now.visible_enemy_count > before.visible_enemy_count ) {
            // Preserve the old count transition as a lower companion fact,
            // then let add_candidate consume it into the identity fact.  It
            // can never name a hostile outside the typed now-before set.
            add_event( events, event_type::enemy_spotted, 72, false,
                       "El numero de enemigos visibles acaba de aumentar.", mentioned );
            events.back().first_sight_count_companion = true;
        }
    }

    const int now_friendly_group = now.visible_ally_count + 1;
    const int before_friendly_group = before.visible_ally_count + 1;
    const bool now_outnumbered = now.visible_enemy_count >= 4 &&
                                 now.visible_enemy_count >= now_friendly_group * 2;
    const bool before_outnumbered = before.visible_enemy_count >= 4 &&
                                    before.visible_enemy_count >= before_friendly_group * 2;
    if( now_outnumbered && !before_outnumbered ) {
        add_event( events, event_type::enemy_group_detected, 90, false,
                   "Ahora percibes " + std::to_string( now.visible_enemy_count ) +
                       " enemigos visibles frente a " + std::to_string( now_friendly_group ) +
                       " miembros visibles o presentes de tu grupo." );
    }
    if( now.hp_percent < before.hp_percent ) {
        const bool severe = now.hp_percent <= 35 || before.hp_percent - now.hp_percent >= 20;
        if( severe ) {
            add_event( events, event_type::npc_badly_hurt, 98, true,
                       "Tu salud visible paso de " + std::to_string( before.hp_percent ) + "% a " +
                           std::to_string( now.hp_percent ) + "%." );
        } else {
            add_event( events, event_type::npc_hit, 74, false,
                       "Acabas de recibir dano; tu salud paso de " +
                           std::to_string( before.hp_percent ) + "% a " +
                           std::to_string( now.hp_percent ) + "%." );
        }
    }
    if( now.bleeding && !before.bleeding ) {
        add_event( events, event_type::npc_bleeding, 99, true,
                   "Has empezado a sangrar de verdad." );
    }
    if( now.grabbed && !before.grabbed ) {
        add_event( events, event_type::npc_grabbed, 100, true,
                   "Acabas de quedar agarrado por una amenaza real." );
    }
    if( now.stamina_percent <= 25 && before.stamina_percent > 25 ) {
        add_event( events, event_type::low_stamina, 78, false,
                   "Tu resistencia bajo a " + std::to_string( now.stamina_percent ) + "%." );
    }
    if( now.ammo_remaining == 0 && before.ammo_remaining > 0 ) {
        add_event( events, event_type::out_of_ammo, 96, true,
                   "El arma de fuego que empunas se ha quedado sin municion." );
    } else if( now.ammo_capacity > 0 && now.ammo_remaining > 0 &&
               now.ammo_remaining * 4 <= now.ammo_capacity &&
               ( before.ammo_capacity <= 0 || before.ammo_remaining * 4 > before.ammo_capacity ) ) {
        add_event( events, event_type::low_ammo, 80, false,
                   "Queda poca municion en el arma empunada." );
    }
    if( now.retreating && !before.retreating ) {
        add_event( events, event_type::retreat_started, 96, true,
                   "La IA tactica de CDDA ha iniciado una retirada o reposicionamiento." );
    }
    const npc_ai::combat_visible_creature *old_player = find_player( before );
    const npc_ai::combat_visible_creature *new_player = find_player( now );
    if( new_player != nullptr ) {
        if( old_player != nullptr && new_player->hp_percent < old_player->hp_percent ) {
            const bool severe = new_player->hp_percent <= 35 ||
                                old_player->hp_percent - new_player->hp_percent >= 20;
            if( severe ) {
                add_event( events, event_type::player_badly_hurt, 100, true,
                           "El estado visible del jugador empeoro a: " + new_player->condition + ".",
                           new_player );
            } else {
                add_event( events, event_type::player_hit, 82, false,
                           "El jugador visible acaba de recibir dano.", new_player );
            }
        }
        if( new_player->bleeding && ( old_player == nullptr || !old_player->bleeding ) ) {
            add_event( events, event_type::player_bleeding, 100, true,
                       "El jugador visible acaba de empezar a sangrar.", new_player );
        }
        if( new_player->grabbed && ( old_player == nullptr || !old_player->grabbed ) ) {
            add_event( events, event_type::player_grabbed, 100, true,
                       "El jugador visible acaba de quedar agarrado.", new_player );
        }
        const bool player_surrounded_now = new_player->adjacent_hostiles >= 2 ||
                                           ( new_player->grabbed &&
                                             new_player->adjacent_hostiles >= 1 );
        const bool player_surrounded_before = old_player != nullptr &&
                ( old_player->adjacent_hostiles >= 2 ||
                  ( old_player->grabbed && old_player->adjacent_hostiles >= 1 ) );
        if( player_surrounded_now && !player_surrounded_before ) {
            add_event( events, event_type::player_surrounded, 99, true,
                       "El jugador tiene " +
                       std::to_string( new_player->adjacent_hostiles ) +
                       " enemigos visibles adyacentes.", new_player );
        }
    }

    for( const npc_ai::combat_visible_creature &ally : now.visible_creatures ) {
        if( !ally.npc || ally.hostile ) {
            continue;
        }
        const npc_ai::combat_visible_creature *old = find_character( before, ally.character_id );
        if( old != nullptr && ally.hp_percent < old->hp_percent ) {
            const bool severe = ally.hp_percent <= 35 || old->hp_percent - ally.hp_percent >= 20;
            if( severe ) {
                add_event( events, event_type::ally_badly_hurt, 98, true,
                           "El estado visible de " + ally.name + " empeoro a: " + ally.condition + ".",
                           &ally );
            } else {
                add_event( events, event_type::ally_hit, 80, false,
                           ally.name + " acaba de recibir dano y es visible.", &ally );
            }
        }
        if( ally.bleeding && ( old == nullptr || !old->bleeding ) ) {
            add_event( events, event_type::ally_bleeding, 99, true,
                       ally.name + " acaba de empezar a sangrar y es visible.", &ally );
        }
        if( ally.grabbed && ( old == nullptr || !old->grabbed ) ) {
            add_event( events, event_type::ally_grabbed, 99, true,
                       ally.name + " acaba de quedar agarrado y es visible.", &ally );
        }
        const bool ally_surrounded_now = ally.adjacent_hostiles >= 2 ||
                                         ( ally.grabbed && ally.adjacent_hostiles >= 1 );
        const bool ally_surrounded_before = old != nullptr &&
                ( old->adjacent_hostiles >= 2 ||
                  ( old->grabbed && old->adjacent_hostiles >= 1 ) );
        if( ally_surrounded_now && !ally_surrounded_before ) {
            add_event( events, event_type::ally_surrounded, 97, true,
                       ally.name + " tiene " + std::to_string( ally.adjacent_hostiles ) +
                       " enemigos visibles adyacentes.", &ally );
        }
        if( ally.retreating && ( old == nullptr || !old->retreating ) ) {
            add_event( events, event_type::ally_retreating, 84, false,
                       ally.name + " ha iniciado una retirada visible.", &ally );
        }
    }

    return events;
}

bool combat_event_from_world_event( const npc &observer, const npc_ai::world_event &source,
                                    npc_ai::combat_social_event &result )
{
    using combat_type = npc_ai::combat_social_event_type;
    using world_type = npc_ai::world_event_type;
    const int observer_id = observer.getID().get_value();
    switch( source.type ) {
        case world_type::combat_start:
            result.type = combat_type::combat_start;
            break;
        case world_type::combat_end:
            result.type = combat_type::combat_end;
            break;
        case world_type::enemy_spotted:
            result.type = combat_type::enemy_spotted;
            break;
        case world_type::dangerous_enemy_spotted:
            result.type = combat_type::dangerous_enemy_spotted;
            break;
        case world_type::enemy_group_detected:
            result.type = combat_type::enemy_group_detected;
            break;
        case world_type::npc_attack:
            result.type = source.target.character_id == observer_id ? combat_type::npc_hit :
                          source.target.kind == "player" ? combat_type::player_hit :
                          source.target.kind == "npc" ? combat_type::ally_hit :
                          combat_type::npc_attack;
            break;
        case world_type::npc_hit:
            result.type = combat_type::npc_hit;
            break;
        case world_type::npc_badly_hurt:
            result.type = combat_type::npc_badly_hurt;
            break;
        case world_type::npc_bleeding:
            result.type = combat_type::npc_bleeding;
            break;
        case world_type::npc_grabbed:
            result.type = source.target.character_id == observer_id ? combat_type::npc_grabbed :
                          source.target.kind == "player" ? combat_type::player_grabbed :
                          combat_type::ally_grabbed;
            break;
        case world_type::player_hit:
            result.type = combat_type::player_hit;
            break;
        case world_type::player_badly_hurt:
            result.type = combat_type::player_badly_hurt;
            break;
        case world_type::player_bleeding:
            result.type = combat_type::player_bleeding;
            break;
        case world_type::player_grabbed:
            result.type = combat_type::player_grabbed;
            break;
        case world_type::player_surrounded:
            result.type = combat_type::player_surrounded;
            break;
        case world_type::ally_hit:
            result.type = combat_type::ally_hit;
            break;
        case world_type::ally_badly_hurt:
            result.type = combat_type::ally_badly_hurt;
            break;
        case world_type::ally_bleeding:
            result.type = combat_type::ally_bleeding;
            break;
        case world_type::ally_grabbed:
            result.type = combat_type::ally_grabbed;
            break;
        case world_type::ally_surrounded:
            result.type = combat_type::ally_surrounded;
            break;
        case world_type::low_stamina:
            result.type = combat_type::low_stamina;
            break;
        case world_type::low_ammo:
            result.type = combat_type::low_ammo;
            break;
        case world_type::out_of_ammo:
            result.type = combat_type::out_of_ammo;
            break;
        case world_type::retreat_started:
            result.type = combat_type::retreat_started;
            break;
        case world_type::ally_retreating:
            result.type = combat_type::ally_retreating;
            break;
        case world_type::failed_escape:
            if( source.actor.character_id != observer_id ) {
                return false;
            }
            result.type = combat_type::failed_escape;
            break;
        case world_type::grab_broken:
            if( source.actor.character_id != observer_id && source.target.character_id != observer_id ) {
                return false;
            }
            result.type = combat_type::grab_broken;
            break;
        case world_type::dragged:
            result.type = source.target.character_id == observer_id ? combat_type::dragged :
                          combat_type::ally_dragged;
            break;
        case world_type::significant_critical:
            result.type = source.actor.character_id == observer_id ? combat_type::significant_critical :
                          source.actor.kind == "player" ? combat_type::player_critical_hit :
                          combat_type::ally_critical_hit;
            break;
        case world_type::ally_critical_hit:
            result.type = combat_type::ally_critical_hit;
            break;
        case world_type::player_critical_hit:
            result.type = combat_type::player_critical_hit;
            break;
        case world_type::attack_missed:
            result.type = combat_type::attack_missed;
            break;
        case world_type::dodge:
            result.type = combat_type::dodge;
            break;
        case world_type::ally_saved:
            result.type = combat_type::ally_saved;
            break;
        case world_type::weapon_jammed:
            result.type = combat_type::weapon_jammed;
            break;
        case world_type::heal_started:
            result.type = combat_type::heal_started;
            break;
        case world_type::heal_completed:
            result.type = combat_type::heal_completed;
            break;
        case world_type::enemy_killed:
            result.type = combat_type::enemy_killed;
            break;
        default:
            return false;
    }
    result.sequence_id = source.sequence_id;
    result.actor_id = source.actor.character_id;
    result.actor_name = source.actor.name;
    result.actor_identity = entity_identity( source.actor );
    result.target_id = source.target.character_id;
    result.target_name = source.target.name;
    result.target_identity = entity_identity( source.target );
    result.importance = source.importance;
    result.encounter_generation = source.encounter_generation;
    result.confirmed_outcome = source.confirmed_outcome;
    result.body_part = source.body_part;
    result.attack_mode = source.attack_mode;
    result.damage = source.damage;
    result.claim_level = source.claim_level;
    result.detail = source.detail;
    result.may_bypass_cooldown = npc_ai::combat_social_speak_priority( result ) >= 94;
    return true;
}

struct batch_speaker_context {
    npc *speaker = nullptr;
    npc_ai::combat_perception_snapshot snapshot;
    std::vector<std::uint64_t> event_ids;
};

struct physical_batch_result {
    bool queued = false;
    bool deferred = false;
    std::uint64_t request_id = 0;
    npc_ai::combat_social_event primary_event;
};

std::string event_id_signature( const std::vector<std::uint64_t> &ids )
{
    std::ostringstream signature;
    for( const std::uint64_t id : ids ) {
        if( signature.tellp() > 0 ) {
            signature << ',';
        }
        signature << id;
    }
    return signature.str();
}

std::string build_combat_batch_system_prompt(
    const std::vector<batch_speaker_context *> &speakers )
{
    const bool spanish = npc_ai::current_dialogue_language_code().rfind( "es", 0 ) == 0;
    std::ostringstream system;
    system << ( spanish ?
                "Escribes dialogo humano breve para NPC de Cataclysm: Dark Days Ahead. C++ ya "
                "eligio cada hablante, los hechos que conoce, prioridad, expiracion y audiencia. "
                "Tu unica autoridad es el texto expresivo. No inventes ni completes hechos. Cada "
                "slot solo puede usar sus event_ids permitidos. Devuelve JSON estricto: "
                "{\"candidates\":[{\"slot\":0,\"event_ids\":[1],\"claim_level\":\"FACT_ONLY\","
                "\"text\":\"frase\"}]}. Puedes omitir un slot para silencio. No devuelvas speaker, "
                "priority, expiry ni audience. Dolor o miedo altos, HP bajo, agotamiento o estar "
                "agarrado producen frases mas cortas, tensas o entrecortadas; moral alta puede ser "
                "desafiante y moral baja pesimista. Esos valores solo cambian COMO hablan. "
                "HIT_ONLY permite decir que hubo impacto en la parte indicada, pero prohibe rotura, "
                "inutilizacion o muerte. Solo LIMB_DISABLED_CONFIRMED permite afirmar un miembro "
                "roto/inutilizado y solo DEATH_CONFIRMED permite muerte. Si confirmed_outcome=no, "
                "limitate literalmente al hecho observado. No prometas acciones tacticas no "
                "confirmadas. attack_mode=melee es un golpe cuerpo a cuerpo: nunca lo llames "
                "tiro, disparo ni bala; sin attack_mode no nombres el arma ni el modo. Usa "
                "exclusivamente OUTPUT_LANGUAGE.\n" :
                "Write short human dialogue for Cataclysm: Dark Days Ahead NPCs. C++ already chose "
                "every speaker, known facts, priority, expiry, and audience. Your only authority is "
                "expressive text. Never invent or complete facts. Each slot may use only its allowed "
                "event_ids. Return strict JSON: {\"candidates\":[{\"slot\":0,\"event_ids\":[1],"
                "\"claim_level\":\"FACT_ONLY\",\"text\":\"line\"}]}. Omit a slot for silence. Do "
                "not return speaker, priority, expiry, or audience. High pain or fear, low HP, "
                "exhaustion, or being grabbed makes speech shorter, tense, or broken; high morale "
                "may be defiant and low morale pessimistic. These values only change HOW they speak. "
                "HIT_ONLY permits a claim that the named part was hit, never broken, disabled, or "
                "killed. Only LIMB_DISABLED_CONFIRMED permits a disabled limb and only "
                "DEATH_CONFIRMED permits death. When confirmed_outcome=no, state only the literal "
                "observation. Never promise unconfirmed tactical actions. attack_mode=melee is a "
                "hand-to-hand blow: never call it a shot or bullet; without attack_mode do not "
                "name the weapon or mode. Use only OUTPUT_LANGUAGE.\n" );
    system << "SPEAKER_PERSONALITIES\n";
    std::unordered_set<int> written;
    for( const batch_speaker_context *context : speakers ) {
        const npc &speaker = *context->speaker;
        if( !written.insert( speaker.getID().get_value() ).second ) {
            continue;
        }
        system << "speaker_id=" << speaker.getID().get_value()
               << ",name=" << speaker.get_name()
               << ",aggression=" << static_cast<int>( speaker.personality.aggression )
               << ",bravery=" << static_cast<int>( speaker.personality.bravery )
               << ",collector=" << static_cast<int>( speaker.personality.collector )
               << ",altruism=" << static_cast<int>( speaker.personality.altruism ) << '\n';
    }
    return system.str();
}

std::string build_combat_batch_prompt(
    const std::vector<npc_ai::world_event> &facts,
    const std::vector<batch_speaker_context *> &speakers,
    const std::vector<npc_ai::ai_combat_utterance_slot> &slots )
{
    std::ostringstream prompt;
    prompt << "FACTS_SHARED_BY_EVERY_SPEAKER_IN_THIS_INFERENCE\n";
    for( const npc_ai::world_event &fact : facts ) {
        prompt << "event_id=" << fact.sequence_id
               << ",type=" << npc_ai::world_event_type_name( fact.type )
               << ",turn=" << fact.game_turn
               << ",actor=" << ( fact.actor.name.empty() ? "unknown" : fact.actor.name )
               << ",target=" << ( fact.target.name.empty() ? "none" : fact.target.name )
               << ",confirmed_outcome=" << ( fact.confirmed_outcome ? "yes" : "no" )
               << ",claim_limit=" << npc_ai::world_event_claim_level_name( fact.claim_level );
        if( !fact.body_part.empty() ) {
            prompt << ",body_part=" << fact.body_part;
        }
        if( fact.damage > 0 ) {
            prompt << ",damage=" << fact.damage;
        }
        if( !fact.attack_mode.empty() ) {
            prompt << ",attack_mode=" << fact.attack_mode;
        }
        prompt << ",fact=" << one_line( fact.detail ) << '\n';
    }
    prompt << "CURRENT_SPEAKER_STATE\n";
    std::unordered_set<int> written;
    for( const batch_speaker_context *context : speakers ) {
        const int id = context->speaker->getID().get_value();
        if( !written.insert( id ).second ) {
            continue;
        }
        const npc_ai::combat_perception_snapshot &state = context->snapshot;
        prompt << "speaker_id=" << id << ",name=" << context->speaker->get_name()
               << ",pain=" << state.pain << ",morale=" << state.morale
               << ",fear=" << state.fear << ",stamina=" << state.stamina_percent
               << ",hp=" << state.hp_percent
               << ",grabbed=" << ( state.grabbed ? "yes" : "no" )
               << ",bleeding=" << ( state.bleeding ? "yes" : "no" ) << '\n';
    }
    prompt << "CPP_FIXED_SLOTS\n";
    for( const npc_ai::ai_combat_utterance_slot &slot : slots ) {
        prompt << "slot=" << slot.slot_id << ",speaker_id=" << slot.speaker_id
               << ",speaker_name=" << slot.speaker_name << ",allowed_event_ids="
               << event_id_signature( slot.allowed_event_ids ) << '\n';
    }
    prompt << "OUTPUT_LANGUAGE=" << npc_ai::current_dialogue_language_name() << '\n';
    return prompt.str();
}

physical_batch_result try_enqueue_physical_batch(
    npc &coordinator, const std::vector<npc_ai::world_event> &incoming,
    const std::uint64_t encounter_generation )
{
    physical_batch_result result;
    if( !batching_enabled || incoming.empty() ) {
        return result;
    }

    const std::uint64_t latest_sequence = npc_ai::latest_world_event_sequence();
    const std::uint64_t oldest_live_sequence = latest_sequence > npc_ai::world_event_ring_capacity ?
                                               latest_sequence - npc_ai::world_event_ring_capacity : 0;
    for( auto iter = batched_event_ids.begin(); iter != batched_event_ids.end(); ) {
        if( *iter <= oldest_live_sequence ) {
            iter = batched_event_ids.erase( iter );
        } else {
            ++iter;
        }
    }

    std::vector<npc_ai::world_event> eligible;
    eligible.reserve( std::min( incoming.size(), max_batch_facts ) );
    for( auto iter = incoming.rbegin(); iter != incoming.rend() &&
         eligible.size() < max_batch_facts; ++iter ) {
        if( iter->sequence_id == 0 || batched_event_ids.count( iter->sequence_id ) != 0 ) {
            continue;
        }
        npc_ai::combat_social_event mapped;
        if( !combat_event_from_world_event( coordinator, *iter, mapped ) ) {
            continue;
        }
        eligible.push_back( *iter );
        narrable_event_ids.insert( iter->sequence_id );
    }
    if( eligible.empty() ) {
        return result;
    }
    result.deferred = true;
    std::sort( eligible.begin(), eligible.end(), []( const npc_ai::world_event &lhs,
    const npc_ai::world_event &rhs ) {
        return lhs.sequence_id < rhs.sequence_id;
    } );

    bool all_grab_sequence = eligible.size() >= 3;
    int result_priority = -1;
    for( const npc_ai::world_event &fact : eligible ) {
        npc_ai::combat_social_event mapped;
        if( !combat_event_from_world_event( coordinator, fact, mapped ) ) {
            continue;
        }
        const int priority = npc_ai::combat_social_speak_priority( mapped );
        if( priority >= result_priority ) {
            result.primary_event = mapped;
            result_priority = priority;
        }
        all_grab_sequence = all_grab_sequence && is_grab_sequence_event( mapped.type );
    }
    if( result.primary_event.encounter_generation == 0 ) {
        result.primary_event.encounter_generation = encounter_generation;
    }
    if( all_grab_sequence ) {
        for( const npc_ai::world_event &fact : eligible ) {
            if( fact.sequence_id == result.primary_event.sequence_id ) {
                continue;
            }
            result.primary_event.coalesced_sequences.push_back( fact.sequence_id );
            if( result.primary_event.detail.find( fact.detail ) == std::string::npos ) {
                result.primary_event.detail = fact.detail + " " + result.primary_event.detail;
            }
        }
    }

    // One quiet turn is enough to close a short burst; a full five-fact batch
    // closes immediately.  This lets a four-second sequence coalesce without
    // delaying an isolated urgent fact by more than one game second.
    if( eligible.size() < max_batch_facts && !all_grab_sequence &&
        eligible.back().game_turn >= current_turn_number() ) {
        return result;
    }
    const std::uint64_t first_sequence = eligible.front().sequence_id;

    std::vector<batch_speaker_context> speaker_contexts;
    for( npc &speaker : g->all_npcs() ) {
        if( !speaker.is_player_ally() || speaker.is_dead_state() ||
            speaker.is_hallucination() || speaker.in_sleep_state() ) {
            continue;
        }
        combat_social_state &speaker_state = combat_states[npc_key( speaker )];
        if( speaker_state.pending_request_id != 0 ) {
            continue;
        }
        const std::vector<npc_ai::world_event> known = npc_ai::recent_world_events_for(
                    speaker, first_sequence - 1, npc_ai::world_event_ring_capacity,
                    candidate_max_age_turns );
        batch_speaker_context context;
        context.speaker = &speaker;
        for( const npc_ai::world_event &fact : eligible ) {
            if( std::any_of( known.begin(), known.end(), [&]( const npc_ai::world_event &seen ) {
                return seen.sequence_id == fact.sequence_id;
            } ) ) {
                context.event_ids.push_back( fact.sequence_id );
            }
        }
        if( !context.event_ids.empty() ) {
            context.snapshot = npc_ai::build_combat_perception_snapshot( speaker );
            speaker_contexts.push_back( std::move( context ) );
        }
    }
    if( speaker_contexts.empty() ) {
        return result;
    }

    std::map<std::string, std::vector<batch_speaker_context *>> cohorts;
    for( batch_speaker_context &context : speaker_contexts ) {
        cohorts[event_id_signature( context.event_ids )].push_back( &context );
    }
    std::vector<std::pair<std::string, std::vector<batch_speaker_context *>>> ordered_cohorts(
        cohorts.begin(), cohorts.end() );
    std::sort( ordered_cohorts.begin(), ordered_cohorts.end(), []( const auto &lhs,
    const auto &rhs ) {
        const std::size_t lhs_weight = lhs.first.size() * lhs.second.size();
        const std::size_t rhs_weight = rhs.first.size() * rhs.second.size();
        return std::tie( rhs_weight, rhs.first ) < std::tie( lhs_weight, lhs.first );
    } );

    std::size_t inference_count = 0;
    for( auto &cohort : ordered_cohorts ) {
        if( inference_count >= max_batch_inferences_per_suffix ) {
            break;
        }
        std::vector<npc_ai::world_event> facts;
        for( const std::uint64_t id : cohort.second.front()->event_ids ) {
            const auto found = std::find_if( eligible.begin(), eligible.end(),
            [id]( const npc_ai::world_event &fact ) {
                return fact.sequence_id == id;
            } );
            if( found != eligible.end() ) {
                facts.push_back( *found );
            }
        }
        if( facts.empty() ) {
            continue;
        }
        std::sort( cohort.second.begin(), cohort.second.end(), []( const auto *lhs,
        const auto *rhs ) {
            const combat_social_state &left_state = combat_states[npc_key( *lhs->speaker )];
            const combat_social_state &right_state = combat_states[npc_key( *rhs->speaker )];
            return std::make_tuple( left_state.last_spoken_turn,
                                    lhs->speaker->getID().get_value() ) <
                   std::make_tuple( right_state.last_spoken_turn,
                                    rhs->speaker->getID().get_value() );
        } );

        const std::size_t candidate_count = std::min<std::size_t>(
                    max_batch_candidates, std::max<std::size_t>( 1, facts.size() ) );
        std::vector<npc_ai::ai_combat_utterance_slot> slots;
        int batch_priority = 0;
        for( std::size_t index = 0; index < candidate_count; ++index ) {
            batch_speaker_context &speaker = *cohort.second[index % cohort.second.size()];
            npc_ai::combat_social_event primary;
            int primary_priority = -1;
            for( const npc_ai::world_event &fact : facts ) {
                npc_ai::combat_social_event mapped;
                if( !combat_event_from_world_event( *speaker.speaker, fact, mapped ) ) {
                    continue;
                }
                const int priority = npc_ai::combat_social_speak_priority( mapped );
                if( priority > primary_priority ) {
                    primary = std::move( mapped );
                    primary_priority = priority;
                }
            }
            if( primary_priority < 0 ) {
                continue;
            }
            npc_ai::ai_combat_utterance_slot slot;
            slot.slot_id = static_cast<int>( slots.size() );
            slot.speaker_id = speaker.speaker->getID().get_value();
            slot.speaker_name = speaker.speaker->get_name();
            slot.allowed_event_ids = speaker.event_ids;
            slot.combat_event_type = static_cast<int>( primary.type );
            slot.target_id = primary.target_id;
            slot.speak_priority = primary_priority;
            slot.expiry_turn = current_turn_number() + batch_line_expiry_turns;
            batch_priority = std::max( batch_priority, slot.speak_priority );
            slots.push_back( std::move( slot ) );
        }
        if( slots.empty() ) {
            continue;
        }

        std::vector<batch_speaker_context *> prompt_speakers;
        for( const npc_ai::ai_combat_utterance_slot &slot : slots ) {
            const auto found = std::find_if( cohort.second.begin(), cohort.second.end(),
            [&]( const batch_speaker_context *context ) {
                return context->speaker->getID().get_value() == slot.speaker_id;
            } );
            if( found != cohort.second.end() ) {
                prompt_speakers.push_back( *found );
            }
        }
        std::string system = build_combat_batch_system_prompt( prompt_speakers );
        std::string prompt = build_combat_batch_prompt( facts, prompt_speakers, slots );
        while( !npc_ai::ollama_prompt_fits_context( prompt, system ) && facts.size() > 1 ) {
            facts.pop_back();
            for( npc_ai::ai_combat_utterance_slot &slot : slots ) {
                slot.allowed_event_ids.erase( std::remove_if( slot.allowed_event_ids.begin(),
                slot.allowed_event_ids.end(), [&]( const std::uint64_t id ) {
                    return std::none_of( facts.begin(), facts.end(),
                    [id]( const npc_ai::world_event &fact ) {
                        return fact.sequence_id == id;
                    } );
                } ), slot.allowed_event_ids.end() );
            }
            prompt = build_combat_batch_prompt( facts, prompt_speakers, slots );
        }
        while( !npc_ai::ollama_prompt_fits_context( prompt, system ) && slots.size() > 1 ) {
            slots.pop_back();
            prompt = build_combat_batch_prompt( facts, prompt_speakers, slots );
        }
        if( !npc_ai::ollama_prompt_fits_context( prompt, system ) ) {
            ++social_metrics.discarded_validation;
            continue;
        }

        std::string batch_key = event_id_signature( slots.front().allowed_event_ids ) + ":";
        for( const npc_ai::ai_combat_utterance_slot &slot : slots ) {
            batch_key += std::to_string( slot.speaker_id ) + ",";
        }
        const npc_ai::ai_enqueue_result queued = npc_ai::enqueue_combat_social_batch_dialogue(
                    coordinator, prompt, system, batch_priority,
                    std::any_of( prompt_speakers.begin(), prompt_speakers.end(),
        []( const batch_speaker_context *speaker ) {
            return speaker->snapshot.in_combat;
        } ), encounter_generation, slots, batch_key );
        if( !queued.accepted ) {
            continue;
        }
        ++inference_count;
        ++social_metrics.inferences_queued;
        const std::size_t depth = npc_ai::get_ai_request_queue().pending_count();
        observed_queue_depths.push_back( depth );
        social_metrics.queue_depth_max = std::max( social_metrics.queue_depth_max, depth );
        for( const npc_ai::world_event &fact : facts ) {
            batched_event_ids.insert( fact.sequence_id );
            npc_ai::annotate_world_event( fact.sequence_id, "batched", true,
                                          queued.request_id );
        }
        for( const npc_ai::ai_combat_utterance_slot &slot : slots ) {
            combat_social_state &state = combat_states[slot.speaker_id];
            state.pending_request_id = queued.request_id;
            state.pending_importance = slot.speak_priority;
            state.pending_type = static_cast<npc_ai::combat_social_event_type>(
                                     slot.combat_event_type );
        }
        if( !result.queued ) {
            result.queued = true;
            result.request_id = queued.request_id;
        }
    }
    return result;
}

npc_ai::world_event_type world_type_from_combat_event(
    const npc_ai::combat_social_event_type type )
{
    using c = npc_ai::combat_social_event_type;
    using w = npc_ai::world_event_type;
    switch( type ) {
        case c::combat_start: return w::combat_start;
        case c::combat_end: return w::combat_end;
        case c::enemy_spotted: return w::enemy_spotted;
        case c::dangerous_enemy_spotted: return w::dangerous_enemy_spotted;
        case c::enemy_group_detected: return w::enemy_group_detected;
        case c::npc_attack: return w::npc_attack;
        case c::npc_hit: return w::npc_hit;
        case c::npc_badly_hurt: return w::npc_badly_hurt;
        case c::npc_bleeding: return w::npc_bleeding;
        case c::npc_grabbed: return w::npc_grabbed;
        case c::player_hit: return w::player_hit;
        case c::player_badly_hurt: return w::player_badly_hurt;
        case c::player_bleeding: return w::player_bleeding;
        case c::player_grabbed: return w::player_grabbed;
        case c::player_surrounded: return w::player_surrounded;
        case c::ally_hit: return w::ally_hit;
        case c::ally_badly_hurt: return w::ally_badly_hurt;
        case c::ally_bleeding: return w::ally_bleeding;
        case c::ally_grabbed: return w::ally_grabbed;
        case c::ally_surrounded: return w::ally_surrounded;
        case c::enemy_killed: return w::enemy_killed;
        case c::low_stamina: return w::low_stamina;
        case c::low_ammo: return w::low_ammo;
        case c::out_of_ammo: return w::out_of_ammo;
        case c::weapon_jammed: return w::weapon_jammed;
        case c::retreat_started: return w::retreat_started;
        case c::ally_retreating: return w::ally_retreating;
        case c::failed_escape: return w::failed_escape;
        case c::grab_broken: return w::grab_broken;
        case c::dragged: return w::dragged;
        case c::ally_dragged: return w::ally_dragged;
        case c::significant_critical: return w::significant_critical;
        case c::ally_critical_hit: return w::ally_critical_hit;
        case c::player_critical_hit: return w::player_critical_hit;
        case c::attack_missed: return w::attack_missed;
        case c::dodge: return w::dodge;
        case c::ally_saved: return w::ally_saved;
        case c::heal_started: return w::heal_started;
        case c::heal_completed: return w::heal_completed;
    }
    return w::combat_start;
}

bool event_still_relevant( const npc_ai::combat_social_event_type type,
                           const npc_ai::combat_perception_snapshot &snapshot, const int target_id )
{
    using event_type = npc_ai::combat_social_event_type;
    if( type == event_type::combat_end ) {
        return !snapshot.in_combat && snapshot.visible_enemy_count == 0 &&
               snapshot.tactical_danger <= NPC_DANGER_VERY_LOW;
    }
    if( type == event_type::enemy_killed ) {
        return true;
    }
    if( type == event_type::npc_attack || type == event_type::npc_hit ||
        type == event_type::player_hit || type == event_type::ally_hit ||
        type == event_type::significant_critical ||
        type == event_type::ally_critical_hit ||
        type == event_type::player_critical_hit ||
        type == event_type::attack_missed || type == event_type::dodge ) {
        // These are recent completed facts, not promises about the target's
        // present state.  Their C++ expiry is the relevance boundary.
        return snapshot.in_combat;
    }
    if( type == event_type::heal_started || type == event_type::heal_completed ||
        type == event_type::grab_broken ) {
        return type != event_type::grab_broken || !snapshot.grabbed;
    }
    if( !snapshot.in_combat ) {
        return false;
    }
    if( type == event_type::npc_bleeding ) {
        return snapshot.bleeding;
    }
    if( type == event_type::npc_grabbed || type == event_type::failed_escape ||
        type == event_type::dragged ) {
        return snapshot.grabbed;
    }
    if( type == event_type::ally_saved && target_id == snapshot.observer_id ) {
        return !snapshot.grabbed;
    }
    if( target_id < 0 ) {
        return true;
    }
    const npc_ai::combat_visible_creature *target = find_character( snapshot, target_id );
    if( target == nullptr ) {
        return false;
    }
    if( type == event_type::player_grabbed || type == event_type::ally_grabbed ||
        type == event_type::ally_dragged ) {
        return target->grabbed;
    }
    if( type == event_type::ally_saved ) {
        return !target->grabbed;
    }
    if( type == event_type::player_bleeding || type == event_type::ally_bleeding ) {
        return target->bleeding;
    }
    if( type == event_type::player_surrounded || type == event_type::ally_surrounded ) {
        return target->adjacent_hostiles >= 2 ||
               ( target->grabbed && target->adjacent_hostiles >= 1 );
    }
    if( type == event_type::player_badly_hurt || type == event_type::ally_badly_hurt ) {
        return target->condition == "gravemente herido" ||
               target->condition == "al borde de la muerte";
    }
    return true;
}

bool model_chose_silence( const std::string &raw )
{
    const std::string lower = lower_ascii( one_line( raw ) );
    return lower == "silent" || lower == "silencio" ||
           lower.find( "decision=silent" ) != std::string::npos ||
           lower.find( "\"decision\":\"silent\"" ) != std::string::npos ||
           lower.find( "\"decision\": \"silent\"" ) != std::string::npos;
}

std::string extract_spoken_text( const std::string &raw )
{
    std::istringstream lines( raw );
    std::string line;
    while( std::getline( lines, line ) ) {
        const std::string trimmed = trim_copy( line );
        const std::string lower = lower_ascii( trimmed );
        if( lower.rfind( "text=", 0 ) == 0 || lower.rfind( "text:", 0 ) == 0 ) {
            return trim_copy( trimmed.substr( 5 ) );
        }
    }

    const std::string lower = lower_ascii( raw );
    const std::size_t text_key = lower.find( "\"text\"" );
    if( text_key != std::string::npos ) {
        const std::size_t colon = raw.find( ':', text_key + 6 );
        const std::size_t quote =
            colon == std::string::npos ? std::string::npos : raw.find( '"', colon + 1 );
        if( quote != std::string::npos ) {
            std::string result;
            bool escaped = false;
            for( std::size_t i = quote + 1; i < raw.size(); ++i ) {
                const char c = raw[i];
                if( escaped ) {
                    result.push_back( c );
                    escaped = false;
                } else if( c == '\\' ) {
                    escaped = true;
                } else if( c == '"' ) {
                    return result;
                } else {
                    result.push_back( c );
                }
            }
        }
    }

    if( lower.find( "decision=talk" ) == std::string::npos &&
        lower.find( "\"decision\"" ) == std::string::npos ) {
        return one_line( raw );
    }
    return "";
}

std::string sanitize_speech( std::string text )
{
    text = one_line( std::move( text ) );
    if( text.size() >= 2 && text.front() == '"' && text.back() == '"' ) {
        text = text.substr( 1, text.size() - 2 );
    }
    if( text.size() > 300 ) {
        text.resize( 300 );
    }
    return trim_copy( std::move( text ) );
}

bool is_important_memory( const npc_ai::combat_social_event &event )
{
    return event.importance >= 94 || event.type == npc_ai::combat_social_event_type::combat_end;
}

struct parsed_batch_candidate {
    int slot_id = -1;
    std::vector<std::uint64_t> event_ids;
    int claim_level = -1;
    std::string text;
};

int claim_level_from_name( const std::string &name )
{
    if( name == "FACT_ONLY" ) {
        return static_cast<int>( npc_ai::world_event_claim_level::fact_only );
    }
    if( name == "HIT_ONLY" ) {
        return static_cast<int>( npc_ai::world_event_claim_level::hit_confirmed );
    }
    if( name == "LIMB_DISABLED_CONFIRMED" ) {
        return static_cast<int>( npc_ai::world_event_claim_level::limb_disabled );
    }
    if( name == "DEATH_CONFIRMED" ) {
        return static_cast<int>( npc_ai::world_event_claim_level::death_confirmed );
    }
    return -1;
}

std::vector<parsed_batch_candidate> parse_batch_candidates(
    const std::string &raw, const npc_ai::ai_request_snapshot &request,
    bool &structured, bool &parse_failed )
{
    structured = false;
    parse_failed = false;
    std::vector<parsed_batch_candidate> result;
    const std::size_t first = raw.find( '{' );
    const std::size_t last = raw.rfind( '}' );
    if( first != std::string::npos && last != std::string::npos && last >= first ) {
        try {
            std::istringstream stream( raw.substr( first, last - first + 1 ) );
            TextJsonIn input( stream );
            TextJsonObject root = input.get_object();
            TextJsonArray candidates = root.get_array( "candidates" );
            for( std::size_t index = 0; index < candidates.size() &&
                 result.size() < request.combat_slots.size(); ++index ) {
                TextJsonObject object = candidates.get_object( index );
                parsed_batch_candidate candidate;
                candidate.slot_id = object.get_int( "slot" );
                candidate.claim_level = claim_level_from_name(
                                            object.get_string( "claim_level" ) );
                candidate.text = object.get_string( "text" );
                TextJsonArray ids = object.get_array( "event_ids" );
                for( std::size_t id_index = 0; id_index < ids.size(); ++id_index ) {
                    const int id = ids.get_int( id_index );
                    if( id > 0 ) {
                        candidate.event_ids.push_back( static_cast<std::uint64_t>( id ) );
                    }
                }
                object.allow_omitted_members();
                result.push_back( std::move( candidate ) );
            }
            root.allow_omitted_members();
            structured = true;
            return result;
        } catch( const JsonError & ) {
            parse_failed = true;
        }
    }

    if( parse_failed ) {
        return result;
    }

    // Bounded compatibility degradation for the pre-batch contract.  It still
    // uses the first C++ slot and one C++-allowed event; model metadata never
    // becomes authoritative.
    const std::string legacy_text = sanitize_speech( extract_spoken_text( raw ) );
    if( !legacy_text.empty() && !request.combat_slots.empty() &&
        !request.combat_slots.front().allowed_event_ids.empty() ) {
        parsed_batch_candidate legacy;
        legacy.slot_id = request.combat_slots.front().slot_id;
        legacy.event_ids.push_back( request.combat_slots.front().allowed_event_ids.front() );
        legacy.claim_level = static_cast<int>( npc_ai::world_event_claim_level::fact_only );
        legacy.text = legacy_text;
        result.push_back( std::move( legacy ) );
    }
    return result;
}

bool speaker_knows_event( const int speaker_id, const npc_ai::world_event &event )
{
    return std::find( event.known_by_npc_ids.begin(), event.known_by_npc_ids.end(),
                      speaker_id ) != event.known_by_npc_ids.end();
}

template<typename Callable>
void measure_completion_phase( std::uint64_t *elapsed_us, Callable &&action )
{
    if( elapsed_us == nullptr ) {
        action();
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    action();
    *elapsed_us += static_cast<std::uint64_t>( std::max<std::int64_t>( 0,
                   std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - started ).count() ) );
}

bool emit_due_combat_line( npc &speaker,
                           npc_ai::ai_completion_apply_timings *timings = nullptr )
{
    const int now = current_turn_number();
    for( auto iter = pending_combat_lines.begin(); iter != pending_combat_lines.end(); ) {
        if( now > iter->expiry_turn ) {
            ++social_metrics.discarded_expiration;
            iter = pending_combat_lines.erase( iter );
        } else {
            ++iter;
        }
    }
    if( now <= last_group_line_turn ) {
        return false;
    }
    const int speaker_id = speaker.getID().get_value();
    auto selected = pending_combat_lines.end();
    for( auto iter = pending_combat_lines.begin(); iter != pending_combat_lines.end(); ++iter ) {
        if( iter->speaker_id != speaker_id || iter->earliest_turn > now ) {
            continue;
        }
        if( selected == pending_combat_lines.end() ||
            std::tie( iter->speak_priority, iter->expiry_turn ) >
            std::tie( selected->speak_priority, selected->expiry_turn ) ) {
            selected = iter;
        }
    }
    if( selected == pending_combat_lines.end() ) {
        return false;
    }

    const npc_ai::combat_perception_snapshot current =
        npc_ai::build_combat_perception_snapshot( speaker );
    const npc_ai::combat_social_event_type type =
        static_cast<npc_ai::combat_social_event_type>(
            selected->combat_event_type );
    bool valid = event_still_relevant( type, current, selected->target_id );
    bool rejection_counted = false;
    for( const std::uint64_t id : selected->event_ids ) {
        const std::optional<npc_ai::world_event> fact = npc_ai::world_event_by_sequence( id );
        if( !fact || now - fact->game_turn > batch_line_expiry_turns ) {
            valid = false;
            ++social_metrics.discarded_expiration;
            rejection_counted = true;
            break;
        }
        if( !speaker_knows_event( speaker_id, *fact ) ) {
            valid = false;
            ++social_metrics.discarded_knowledge;
            rejection_counted = true;
            break;
        }
    }
    if( !valid || npc_ai::recent_speech_is_duplicate( speaker, selected->text ) ) {
        if( valid ) {
            ++social_metrics.discarded_deduplication;
        } else if( !rejection_counted ) {
            ++social_metrics.discarded_validation;
        }
        pending_combat_lines.erase( selected );
        return false;
    }

    measure_completion_phase( timings == nullptr ? nullptr : &timings->say_us, [&]() {
        npc_ai::say_ai_line( speaker, selected->text, sounds::sound_t::alert );
    } );
    last_group_line_turn = now;
    combat_states[speaker_id].last_spoken_turn = now;
    ++social_metrics.lines_emitted;
    ++emitted_by_speaker[speaker_id];
    for( const std::uint64_t id : selected->event_ids ) {
        verbalized_event_ids.insert( id );
    }
    measure_completion_phase( timings == nullptr ? nullptr : &timings->memory_us, [&]() {
        npc_ai::remember_recent_speech( speaker, selected->text,
                                        npc_ai::combat_social_intent_name( type ) );
        npc_ai::remember_exchange( speaker, "[BATCH SOCIAL DE COMBATE]", selected->text );
    } );
    pending_combat_lines.erase( selected );
    return true;
}

bool enqueue_smaller_batch_retry( const npc_ai::ai_request_snapshot &request )
{
    if( request.combat_batch_retry_count != 0 || request.combat_slots.empty() ||
        npc_ai::get_ai_request_queue().pending_count() >= 3 ) {
        return false;
    }
    npc_ai::ai_combat_utterance_slot slot = request.combat_slots.front();
    npc *speaker = g->find_npc( character_id( slot.speaker_id ) );
    if( speaker == nullptr || speaker->is_dead_state() || slot.allowed_event_ids.empty() ) {
        return false;
    }
    const std::optional<npc_ai::world_event> fact = npc_ai::world_event_by_sequence(
                slot.allowed_event_ids.back() );
    if( !fact || !speaker_knows_event( slot.speaker_id, *fact ) ) {
        return false;
    }
    slot.allowed_event_ids = { fact->sequence_id };
    slot.slot_id = 0;
    batch_speaker_context context;
    context.speaker = speaker;
    context.snapshot = npc_ai::build_combat_perception_snapshot( *speaker );
    context.event_ids = slot.allowed_event_ids;
    std::vector<batch_speaker_context *> speakers{ &context };
    const std::vector<npc_ai::world_event> facts{ *fact };
    const std::vector<npc_ai::ai_combat_utterance_slot> slots{ slot };
    const std::string system = build_combat_batch_system_prompt( speakers );
    const std::string prompt = build_combat_batch_prompt( facts, speakers, slots );
    if( !npc_ai::ollama_prompt_fits_context( prompt, system ) ) {
        return false;
    }
    const npc_ai::ai_enqueue_result retry = npc_ai::enqueue_combat_social_batch_dialogue(
                *speaker, prompt, system, slot.speak_priority, context.snapshot.in_combat,
                request.encounter_generation, slots,
                event_id_signature( slot.allowed_event_ids ) + ":fallback:" +
                std::to_string( request.id ), 1 );
    if( !retry.accepted ) {
        return false;
    }
    combat_social_state &state = combat_states[slot.speaker_id];
    state.pending_request_id = retry.request_id;
    state.pending_importance = slot.speak_priority;
    state.pending_type = static_cast<npc_ai::combat_social_event_type>( slot.combat_event_type );
    ++social_metrics.fallback_activations;
    ++social_metrics.inferences_queued;
    const std::size_t depth = npc_ai::get_ai_request_queue().pending_count();
    observed_queue_depths.push_back( depth );
    social_metrics.queue_depth_max = std::max( social_metrics.queue_depth_max, depth );
    return true;
}

void clear_batch_pending_request( const npc_ai::ai_request_snapshot &request )
{
    for( const npc_ai::ai_combat_utterance_slot &slot : request.combat_slots ) {
        combat_social_state &state = combat_states[slot.speaker_id];
        if( state.pending_request_id == request.id ) {
            state.pending_request_id = 0;
            state.pending_importance = 0;
        }
    }
}

void retain_batch_pending_request( const npc_ai::ai_request_snapshot &request,
                                   const std::uint64_t retry_id )
{
    for( const npc_ai::ai_combat_utterance_slot &slot : request.combat_slots ) {
        npc *speaker = g->find_npc( character_id( slot.speaker_id ) );
        if( speaker == nullptr || speaker->is_dead_state() || !speaker->is_player_ally() ) {
            continue;
        }
        combat_social_state &state = combat_states[slot.speaker_id];
        state.pending_request_id = retry_id;
        state.pending_importance = slot.speak_priority;
        state.pending_type = static_cast<npc_ai::combat_social_event_type>(
                                 slot.combat_event_type );
    }
}

void apply_combat_batch_completion( const npc_ai::ai_request_completion &completion,
                                    npc_ai::ai_completion_apply_timings *timings )
{
    const npc_ai::ai_request_snapshot &request = completion.request;
    clear_batch_pending_request( request );

    const auto bounded_failure = [&]() {
        ++social_metrics.discarded_validation;
        enqueue_smaller_batch_retry( request );
    };
    if( completion.response.context_truncated || !completion.response.success ||
        completion.response.text.empty() ) {
        // context_truncated is an invariant violation, never usable output.
        bounded_failure();
        return;
    }

    bool structured = false;
    bool parse_failed = false;
    std::vector<parsed_batch_candidate> candidates = parse_batch_candidates(
                completion.response.text, request, structured, parse_failed );
    if( parse_failed ) {
        bounded_failure();
        return;
    }
    if( candidates.empty() ) {
        // An empty structured candidates array (or explicit legacy silence) is
        // a valid expressive choice, not a transport failure.
        if( !structured && !model_chose_silence( completion.response.text ) ) {
            bounded_failure();
        }
        return;
    }

    const std::string language_code = request.dialogue_language_code.empty() ?
                                      npc_ai::current_dialogue_language_code() :
                                      request.dialogue_language_code;
    const bool wrong_language = std::any_of( candidates.begin(), candidates.end(),
    [&]( const parsed_batch_candidate & candidate ) {
        const std::string text = sanitize_speech( candidate.text );
        return !text.empty() &&
               !npc_ai::generated_text_matches_dialogue_language( text, language_code );
    } );
    if( wrong_language ) {
        const npc_ai::ai_enqueue_result retry = npc_ai::enqueue_language_retry( request );
        if( retry.accepted ) {
            retain_batch_pending_request( request, retry.request_id );
            ++social_metrics.inferences_queued;
            const std::size_t depth = npc_ai::get_ai_request_queue().pending_count();
            observed_queue_depths.push_back( depth );
            social_metrics.queue_depth_max = std::max( social_metrics.queue_depth_max, depth );
            return;
        }
    }

    const int now = current_turn_number();
    std::unordered_set<int> used_slots;
    std::size_t accepted_count = 0;
    for( parsed_batch_candidate &candidate : candidates ) {
        const auto slot_iter = std::find_if( request.combat_slots.begin(),
        request.combat_slots.end(), [&]( const npc_ai::ai_combat_utterance_slot &slot ) {
            return slot.slot_id == candidate.slot_id;
        } );
        if( slot_iter == request.combat_slots.end() ||
            !used_slots.insert( candidate.slot_id ).second || candidate.event_ids.empty() ||
            candidate.claim_level < 0 || now > slot_iter->expiry_turn ) {
            ++social_metrics.discarded_validation;
            continue;
        }

        npc *speaker = g->find_npc( character_id( slot_iter->speaker_id ) );
        if( speaker == nullptr || speaker->is_dead_state() || speaker->is_hallucination() ||
            !speaker->is_player_ally() || speaker->in_sleep_state() ) {
            ++social_metrics.discarded_validation;
            continue;
        }

        bool mechanically_valid = true;
        bool knowledge_valid = true;
        bool expired = false;
        int maximum_claim = static_cast<int>( npc_ai::world_event_claim_level::death_confirmed );
        int derived_priority = -1;
        int derived_expiry = now + batch_line_expiry_turns;
        npc_ai::combat_social_event derived_event;
        std::unordered_set<std::uint64_t> unique_ids;
        for( const std::uint64_t event_id : candidate.event_ids ) {
            if( !unique_ids.insert( event_id ).second ||
                std::find( slot_iter->allowed_event_ids.begin(),
                           slot_iter->allowed_event_ids.end(), event_id ) ==
                slot_iter->allowed_event_ids.end() ) {
                mechanically_valid = false;
                break;
            }
            const std::optional<npc_ai::world_event> fact =
                npc_ai::world_event_by_sequence( event_id );
            if( !fact || now - fact->game_turn > batch_line_expiry_turns ) {
                mechanically_valid = false;
                expired = true;
                break;
            }
            if( !speaker_knows_event( slot_iter->speaker_id, *fact ) ) {
                knowledge_valid = false;
                break;
            }
            npc_ai::combat_social_event mapped;
            if( !combat_event_from_world_event( *speaker, *fact, mapped ) ) {
                mechanically_valid = false;
                break;
            }
            const int fact_priority = npc_ai::combat_social_speak_priority( mapped );
            if( fact_priority > derived_priority ) {
                derived_event = std::move( mapped );
                derived_priority = fact_priority;
            }
            derived_expiry = std::min( derived_expiry,
                                       fact->game_turn + batch_line_expiry_turns );
            maximum_claim = std::min( maximum_claim,
                                      static_cast<int>( fact->claim_level ) );
        }
        if( !knowledge_valid ) {
            ++social_metrics.discarded_knowledge;
            continue;
        }
        if( expired ) {
            ++social_metrics.discarded_expiration;
            continue;
        }
        if( !mechanically_valid || derived_priority < 0 || now > derived_expiry ||
            candidate.claim_level > maximum_claim ) {
            ++social_metrics.discarded_validation;
            continue;
        }

        std::string spoken = sanitize_speech( std::move( candidate.text ) );
        if( spoken.empty() ||
            !npc_ai::generated_text_matches_dialogue_language( spoken, language_code ) ) {
            ++social_metrics.discarded_validation;
            continue;
        }
        if( npc_ai::combat_social_text_has_unconfirmed_tactical_promise( spoken ) ) {
            ++social_metrics.discarded_tactical_promise;
            continue;
        }
        const npc_ai::combat_perception_snapshot current =
            npc_ai::build_combat_perception_snapshot( *speaker );
        if( !event_still_relevant( derived_event.type, current, derived_event.target_id ) ||
            ( npc_ai::combat_social_text_claims_no_threats( spoken ) &&
              ( current.in_combat || current.visible_enemy_count > 0 ||
                current.tactical_danger > NPC_DANGER_VERY_LOW ) ) ) {
            ++social_metrics.discarded_validation;
            continue;
        }
        if( npc_ai::recent_speech_is_duplicate( *speaker, spoken ) ) {
            ++social_metrics.discarded_deduplication;
            continue;
        }

        pending_combat_line line;
        line.speaker_id = slot_iter->speaker_id;
        line.text = std::move( spoken );
        line.event_ids = std::move( candidate.event_ids );
        line.speak_priority = derived_priority;
        line.earliest_turn = now + static_cast<int>( accepted_count );
        line.expiry_turn = derived_expiry;
        line.combat_event_type = static_cast<int>( derived_event.type );
        line.target_id = derived_event.target_id;
        pending_combat_lines.push_back( std::move( line ) );
        ++social_metrics.candidates_validated;
        ++accepted_count;
    }

    if( accepted_count == 0 ) {
        bounded_failure();
        return;
    }

    // The completion can arrive between NPC turns; emit at most the first due
    // line immediately and leave the rest to normal main-thread NPC updates.
    for( const npc_ai::ai_combat_utterance_slot &slot : request.combat_slots ) {
        npc *speaker = g->find_npc( character_id( slot.speaker_id ) );
        if( speaker != nullptr && emit_due_combat_line( *speaker, timings ) ) {
            break;
        }
    }
}

} // namespace

namespace npc_ai
{

std::string combat_social_event_name( const combat_social_event_type type )
{
    switch( type ) {
        case combat_social_event_type::combat_start:
            return "COMBAT_START";
        case combat_social_event_type::enemy_spotted:
            return "ENEMY_SPOTTED";
        case combat_social_event_type::dangerous_enemy_spotted:
            return "DANGEROUS_ENEMY_SPOTTED";
        case combat_social_event_type::npc_attack:
            return "NPC_ATTACK";
        case combat_social_event_type::npc_hit:
            return "NPC_HIT";
        case combat_social_event_type::npc_badly_hurt:
            return "NPC_BADLY_HURT";
        case combat_social_event_type::npc_bleeding:
            return "NPC_BLEEDING";
        case combat_social_event_type::npc_grabbed:
            return "NPC_GRABBED";
        case combat_social_event_type::player_hit:
            return "PLAYER_HIT";
        case combat_social_event_type::player_badly_hurt:
            return "PLAYER_BADLY_HURT";
        case combat_social_event_type::player_bleeding:
            return "PLAYER_BLEEDING";
        case combat_social_event_type::player_grabbed:
            return "PLAYER_GRABBED";
        case combat_social_event_type::player_surrounded:
            return "PLAYER_SURROUNDED";
        case combat_social_event_type::ally_hit:
            return "ALLY_HIT";
        case combat_social_event_type::ally_badly_hurt:
            return "ALLY_BADLY_HURT";
        case combat_social_event_type::ally_bleeding:
            return "ALLY_BLEEDING";
        case combat_social_event_type::ally_grabbed:
            return "ALLY_GRABBED";
        case combat_social_event_type::ally_surrounded:
            return "ALLY_SURROUNDED";
        case combat_social_event_type::enemy_killed:
            return "ENEMY_KILLED";
        case combat_social_event_type::enemy_group_detected:
            return "ENEMY_GROUP_DETECTED";
        case combat_social_event_type::low_stamina:
            return "LOW_STAMINA";
        case combat_social_event_type::low_ammo:
            return "LOW_AMMO";
        case combat_social_event_type::out_of_ammo:
            return "OUT_OF_AMMO";
        case combat_social_event_type::weapon_jammed:
            return "WEAPON_JAMMED";
        case combat_social_event_type::retreat_started:
            return "RETREAT_STARTED";
        case combat_social_event_type::ally_retreating:
            return "ALLY_RETREATING";
        case combat_social_event_type::failed_escape:
            return "FAILED_ESCAPE";
        case combat_social_event_type::grab_broken:
            return "GRAB_BROKEN";
        case combat_social_event_type::dragged:
            return "DRAGGED";
        case combat_social_event_type::ally_dragged:
            return "ALLY_DRAGGED";
        case combat_social_event_type::significant_critical:
            return "SIGNIFICANT_CRITICAL";
        case combat_social_event_type::ally_critical_hit:
            return "ALLY_CRITICAL_HIT";
        case combat_social_event_type::player_critical_hit:
            return "PLAYER_CRITICAL_HIT";
        case combat_social_event_type::attack_missed:
            return "ATTACK_MISSED";
        case combat_social_event_type::dodge:
            return "DODGE";
        case combat_social_event_type::ally_saved:
            return "ALLY_SAVED";
        case combat_social_event_type::heal_started:
            return "HEAL_STARTED";
        case combat_social_event_type::heal_completed:
            return "HEAL_COMPLETED";
        case combat_social_event_type::combat_end:
            return "COMBAT_END";
    }
    return "UNKNOWN";
}

std::string combat_social_intent_name( const combat_social_event_type type )
{
    switch( type ) {
        case combat_social_event_type::combat_start:
            return "inicio del combate";
        case combat_social_event_type::enemy_spotted:
        case combat_social_event_type::dangerous_enemy_spotted:
        case combat_social_event_type::enemy_group_detected:
            return "cambio importante de amenaza";
        case combat_social_event_type::npc_attack:
            return "intencion tactica actual";
        case combat_social_event_type::npc_hit:
        case combat_social_event_type::npc_badly_hurt:
        case combat_social_event_type::npc_bleeding:
        case combat_social_event_type::npc_grabbed:
        case combat_social_event_type::failed_escape:
        case combat_social_event_type::dragged:
            return "herida propia";
        case combat_social_event_type::player_hit:
        case combat_social_event_type::player_badly_hurt:
        case combat_social_event_type::player_bleeding:
        case combat_social_event_type::ally_hit:
        case combat_social_event_type::ally_badly_hurt:
        case combat_social_event_type::ally_bleeding:
            return "companero herido";
        case combat_social_event_type::player_grabbed:
        case combat_social_event_type::player_surrounded:
        case combat_social_event_type::ally_grabbed:
        case combat_social_event_type::ally_surrounded:
        case combat_social_event_type::ally_dragged:
            return "companero en peligro inmediato";
        case combat_social_event_type::significant_critical:
        case combat_social_event_type::ally_critical_hit:
        case combat_social_event_type::player_critical_hit:
            return "golpe critico significativo";
        case combat_social_event_type::attack_missed:
        case combat_social_event_type::dodge:
            return "ataque evitado";
        case combat_social_event_type::grab_broken:
        case combat_social_event_type::ally_saved:
            return "companero liberado del peligro";
        case combat_social_event_type::heal_started:
        case combat_social_event_type::heal_completed:
            return "atencion medica real";
        case combat_social_event_type::enemy_killed:
            return "enemigo acaba de morir";
        case combat_social_event_type::low_stamina:
        case combat_social_event_type::low_ammo:
        case combat_social_event_type::out_of_ammo:
        case combat_social_event_type::weapon_jammed:
            return "recurso propio critico";
        case combat_social_event_type::retreat_started:
        case combat_social_event_type::ally_retreating:
            return "necesidad real de retirarse o moverse";
        case combat_social_event_type::combat_end:
            return "fin del combate";
    }
    return "situacion de combate";
}

bool combat_social_text_has_unconfirmed_tactical_promise( const std::string &text )
{
    // Live scenario run 2026-09-03: "¡Voy a cubrirte!" passed while "te
    // cubro" was filtered.  Cover the common Spanish conjugations and the
    // "voy a por" variant the handoff already flagged as leaking.
    static constexpr std::array<std::string_view, 24> promises = {
        "te cubro", "yo me encargo", "me encargo yo", "voy a distraer", "voy por el",
        "voy a por", "quedate ahi", "voy a cubrir", "te cubrire", "cubrirte", "os cubro",
        "los cubro", "yo lo distraigo", "dejamelo a mi", "dejamelo", "yo voy por",
        "i'll cover", "i will cover", "i'll handle", "i'm going after", "i'll distract",
        "leave it to me", "i've got this", "i got him"
    };
    return std::any_of( promises.begin(), promises.end(), [&]( const std::string_view promise ) {
        return lcmatch( text, promise );
    } );
}

int combat_social_speak_priority( const combat_social_event &event )
{
    int priority = event.importance;
    switch( event.type ) {
        case combat_social_event_type::significant_critical:
        case combat_social_event_type::ally_critical_hit:
        case combat_social_event_type::player_critical_hit:
            // A confirmed critical is narratively urgent even though its
            // semantic importance remains 88 for memory prioritisation.
            priority = std::max( priority, 96 );
            break;
        case combat_social_event_type::enemy_killed:
            priority = std::max( priority, 94 );
            break;
        case combat_social_event_type::npc_grabbed:
        case combat_social_event_type::player_grabbed:
        case combat_social_event_type::ally_grabbed:
        case combat_social_event_type::failed_escape:
        case combat_social_event_type::dragged:
        case combat_social_event_type::ally_dragged:
            priority = std::max( priority, 98 );
            break;
        default:
            break;
    }
    return priority;
}

combat_social_metrics combat_social_metrics_snapshot()
{
    combat_social_metrics result = social_metrics;
    result.narrable_events_captured = narrable_event_ids.size();
    result.narrable_events_verbalized = verbalized_event_ids.size();
    result.lines_by_speaker.assign( emitted_by_speaker.begin(), emitted_by_speaker.end() );
    std::sort( result.lines_by_speaker.begin(), result.lines_by_speaker.end() );
    if( !observed_queue_depths.empty() ) {
        std::vector<std::size_t> depths = observed_queue_depths;
        std::sort( depths.begin(), depths.end() );
        const std::size_t rank = ( depths.size() * 95 + 99 ) / 100;
        result.queue_depth_p95 = depths[std::max<std::size_t>( 1, rank ) - 1];
    }
    return result;
}

void reset_combat_social_metrics()
{
    social_metrics = combat_social_metrics{};
    narrable_event_ids.clear();
    verbalized_event_ids.clear();
    observed_queue_depths.clear();
    emitted_by_speaker.clear();
}

void set_combat_social_batching_for_test( const bool enabled )
{
    batching_enabled = enabled;
}

combat_perception_snapshot build_combat_perception_snapshot_impl(
    const npc &who, const combat_perception_snapshot *reused_visible_creatures )
{
    scoped_profile profile( profile_subsystem::combat_snapshot );
    map &here = get_map();
    const tripoint_bub_ms origin = who.pos_bub( here );
    combat_perception_snapshot snapshot;
    snapshot.observer_id = who.getID().get_value();
    snapshot.turn = current_turn_number();
    snapshot.x = who.pos_abs().x();
    snapshot.y = who.pos_abs().y();
    snapshot.z = who.pos_abs().z();
    snapshot.stamina_percent =
        who.get_stamina_max() > 0 ? who.get_stamina() * 100 / who.get_stamina_max() : 100;
    snapshot.pain = who.get_perceived_pain();
    snapshot.morale = who.get_morale_level();
    snapshot.fear = who.mem_combat.panic;
    snapshot.hp_percent = who.hp_percentage();
    snapshot.bleeding = who.has_effect( effect_bleed );
    snapshot.grabbed = who.has_effect_with_flag( json_flag_GRAB );
    snapshot.retreating = who.has_effect( effect_npc_run_away );
    snapshot.outside = here.is_outside( origin );
    snapshot.ambient_light = here.ambient_light_at( origin );
    snapshot.tactical_danger = who.danger_assessment();
    snapshot.current_tile =
        here.has_furn( origin ) ? here.furnname( origin ) : here.tername( origin );
    snapshot.weather = get_weather().weather_id.str();

    const item_location wielded = who.get_wielded_item();
    if( wielded ) {
        snapshot.wielded_weapon = remove_color_tags( wielded->tname() );
        if( wielded->is_gun() ) {
            snapshot.ammo_remaining = wielded->ammo_remaining( &who );
            snapshot.ammo_capacity = snapshot.ammo_remaining + wielded->remaining_ammo_capacity();
        }
    } else {
        snapshot.wielded_weapon = "ninguna";
    }

    const Creature *observer_target = who.current_target();
    if( reused_visible_creatures != nullptr ) {
        snapshot.visible_enemy_count = reused_visible_creatures->visible_enemy_count;
        snapshot.visible_ally_count = reused_visible_creatures->visible_ally_count;
        snapshot.visible_creatures = reused_visible_creatures->visible_creatures;
    } else {
        struct visible_candidate {
            Creature *creature = nullptr;
            Creature::Attitude attitude = Creature::Attitude::NEUTRAL;
        };
        std::vector<visible_candidate> visible;
        visible.reserve( max_visible_creatures );

        const auto append_visible = [&]( Creature *other, const Creature::Attitude attitude ) {
            if( other == nullptr || other == &who || other->is_dead_state() ||
                visible.size() >= max_visible_creatures ||
                snapshot.creature_visibility_checks >= max_creature_visibility_checks ) {
                return;
            }
            if( std::any_of( visible.begin(), visible.end(),
                             [&]( const visible_candidate &candidate ) {
                                 return candidate.creature == other;
                             } ) ) {
                return;
            }
            ++snapshot.creature_visibility_checks;
            if( who.sees( here, *other ) ) {
                visible.push_back( { other, attitude } );
            }
        };
        const auto append_cached = [&]( const std::vector<weak_ptr_fast<Creature>> &cached,
                                        const Creature::Attitude attitude ) {
            for( const weak_ptr_fast<Creature> &entry : cached ) {
                if( visible.size() >= max_visible_creatures ||
                    snapshot.creature_visibility_checks >= max_creature_visibility_checks ) {
                    break;
                }
                if( const shared_ptr_fast<Creature> creature = entry.lock() ) {
                    append_visible( creature.get(), attitude );
                }
            }
        };

        // Combat Social currently runs for player allies, so keep the player and
        // the active target ahead of ambient allies when the 12-entry prompt limit
        // is reached.  Every entry is still revalidated through this observer's
        // own LOS before it can enter the snapshot.
        append_visible( &get_player_character(), Creature::Attitude::FRIENDLY );
        append_visible( const_cast<Creature *>( observer_target ), Creature::Attitude::HOSTILE );
        append_cached( who.get_cached_hostiles(), Creature::Attitude::HOSTILE );
        append_cached( who.get_cached_friends(), Creature::Attitude::FRIENDLY );
        append_cached( who.get_cached_neutrals(), Creature::Attitude::NEUTRAL );

        std::sort( visible.begin(), visible.end(),
                   [&]( const visible_candidate &lhs, const visible_candidate &rhs ) {
                       const bool lhs_hostile = lhs.attitude == Creature::Attitude::HOSTILE;
                       const bool rhs_hostile = rhs.attitude == Creature::Attitude::HOSTILE;
                       return std::make_tuple( !lhs_hostile,
                                               rl_dist( origin, lhs.creature->pos_bub( here ) ),
                                               lhs.creature->disp_name() ) <
                              std::make_tuple( !rhs_hostile,
                                               rl_dist( origin, rhs.creature->pos_bub( here ) ),
                                               rhs.creature->disp_name() );
                   } );

        snapshot.visible_creatures.reserve( visible.size() );
        for( const visible_candidate &candidate : visible ) {
            Creature *other = candidate.creature;
            const tripoint_bub_ms pos = other->pos_bub( here );
            combat_visible_creature observation;
            observation.runtime_identity = static_cast<std::uint64_t>(
                                               reinterpret_cast<std::uintptr_t>( other ) );
            observation.name =
                other->is_avatar() ? get_player_character().get_name() : other->disp_name();
            observation.player = other->is_avatar();
            observation.npc = other->is_npc();
            observation.monster = other->is_monster();
            if( const Character *character = other->as_character() ) {
                observation.character_id = character->getID().get_value();
                observation.grabbed = character->has_effect_with_flag( json_flag_GRAB );
                observation.bleeding = character->has_effect( effect_bleed );
                const item_location held = character->get_wielded_item();
                if( held ) {
                    observation.held_item = remove_color_tags( held->tname() );
                }
            }
            observation.dx = pos.x() - origin.x();
            observation.dy = pos.y() - origin.y();
            observation.dz = pos.z() - origin.z();
            observation.distance = rl_dist( origin, pos );
            observation.adjacent = observation.distance <= 1;
            const Creature::Attitude attitude = candidate.attitude;
            observation.attitude = Creature::attitude_raw_string( attitude );
            observation.hostile = attitude == Creature::Attitude::HOSTILE;
            observation.condition = visible_condition( *other );
            observation.hp_percent = other->hp_percentage();
            observation.observer_target = observer_target == other;
            if( const npc *other_npc = dynamic_cast<const npc *>( other ) ) {
                const Creature *target = other_npc->current_target();
                observation.targeting_observer = target == &who;
                if( target != nullptr && who.sees( here, *target ) ) {
                    observation.target_name = target->disp_name();
                }
                observation.retreating = other_npc->has_effect( effect_npc_run_away );
            } else if( monster *mon = dynamic_cast<monster *>( other ) ) {
                Creature *target = mon->attack_target();
                observation.targeting_observer = target == &who;
                if( target != nullptr && who.sees( here, *target ) ) {
                    observation.target_name = target->disp_name();
                }
            }
            if( observation.hostile ) {
                ++snapshot.visible_enemy_count;
            } else {
                ++snapshot.visible_ally_count;
            }
            snapshot.visible_creatures.push_back( std::move( observation ) );
        }

        for( combat_visible_creature &ally : snapshot.visible_creatures ) {
            if( ally.hostile ) {
                continue;
            }
            const tripoint_bub_ms ally_pos{ origin.x() + ally.dx, origin.y() + ally.dy,
                                            origin.z() + ally.dz };
            ally.adjacent_hostiles = static_cast<int>( std::count_if(
                snapshot.visible_creatures.begin(), snapshot.visible_creatures.end(),
                [&]( const combat_visible_creature &enemy ) {
                    if( !enemy.hostile ) {
                        return false;
                    }
                    const tripoint_bub_ms enemy_pos{ origin.x() + enemy.dx, origin.y() + enemy.dy,
                                                     origin.z() + enemy.dz };
                    return rl_dist( ally_pos, enemy_pos ) <= 1;
                } ) );
        }
    }

    for( const dangerous_sound &sound : who.get_cached_dangerous_sounds() ) {
        if( snapshot.audible_events.size() >= max_audible_events ) {
            break;
        }
        const tripoint_rel_ms delta = sound.abs_pos - who.pos_abs();
        combat_audible_event audible;
        audible.kind = sound_kind( sound.type );
        audible.dx = delta.x();
        audible.dy = delta.y();
        audible.dz = delta.z();
        audible.distance = rl_dist( who.pos_abs(), sound.abs_pos );
        audible.volume = sound.volume;
        snapshot.audible_events.push_back( std::move( audible ) );
    }

    snapshot.in_combat = snapshot.visible_enemy_count > 0 || observer_target != nullptr ||
                         snapshot.tactical_danger > NPC_DANGER_VERY_LOW;
    if( snapshot.retreating ) {
        snapshot.tactical_intent = "retirarse o reposicionarse";
    } else if( observer_target != nullptr && who.sees( here, *observer_target ) ) {
        snapshot.tactical_intent = "combatir contra " + observer_target->disp_name();
    }
    return snapshot;
}

combat_perception_snapshot build_combat_perception_snapshot( const npc &who )
{
    return build_combat_perception_snapshot_impl( who, nullptr );
}

std::vector<combat_social_event> detect_combat_social_events_for_test(
    const combat_perception_snapshot &before, const combat_perception_snapshot &now )
{
    return detect_events( before, now );
}

std::size_t combat_snapshot_visibility_check_limit()
{
    return max_creature_visibility_checks;
}

bool combat_social_situation_is_clear( const npc &who )
{
    const combat_perception_snapshot snapshot = build_combat_perception_snapshot( who );
    return !snapshot.in_combat && snapshot.visible_enemy_count == 0 &&
           snapshot.tactical_danger <= NPC_DANGER_VERY_LOW;
}

bool combat_social_text_claims_no_threats( const std::string &text )
{
    const std::string lower = " " + lower_ascii( one_line( text ) ) + " ";
    static constexpr std::array<std::string_view, 16> cleared_claims = {
        " coast is clear ", " area is clear ", " threat is gone ", " threats are gone ",
        " no enemies left ", " no more enemies ", " combat is over ",
        " zona esta despejada ", " zona está despejada ", " no quedan enemigos ",
        " ya no hay enemigos ", " se acabo el combate ", " se acabó el combate ",
        " todo esta despejado ", " todo está despejado ", " estamos a salvo "
    };
    return std::any_of( cleared_claims.begin(), cleared_claims.end(),
    [&]( const std::string_view claim ) {
        return lower.find( claim ) != std::string::npos;
    } );
}

std::string build_combat_social_prompt( const npc &who, const combat_perception_snapshot &snapshot,
                                        const combat_social_event &event )
{
    scoped_profile profile( profile_subsystem::async_preparation );
    std::ostringstream prompt;
    prompt << "EVENT\n"
           << "event_id=" << event.sequence_id << "\n"
           << "type=" << combat_social_event_name( event.type ) << "\n"
           << "importance=" << event.importance << "\n"
           << "target=" << ( event.target_name.empty() ? "ninguno" : event.target_name ) << "\n"
           << "target_alive="
           << ( event.type == combat_social_event_type::enemy_killed ? "no" : "yes_or_not_applicable" )
           << "\n"
           << "confirmed_outcome=" << ( event.confirmed_outcome ? "yes" : "no" ) << "\n"
           << "claim_limit=" << world_event_claim_level_name( event.claim_level ) << "\n";
    if( !event.body_part.empty() ) {
        prompt << "body_part=" << event.body_part << "\n";
    }
    if( event.damage > 0 ) {
        prompt << "damage=" << event.damage << "\n";
    }
    if( !event.attack_mode.empty() ) {
        prompt << "attack_mode=" << event.attack_mode << "\n";
    }
    if( !event.confirmed_outcome ) {
        prompt << "ASSERTION_LIMIT=Only state the literal observed fact; do not infer a stronger "
                  "physical result.\n";
    }
    prompt << "fact=" << event.detail << "\n\n"
           << "YOU\n"
           << "name=" << who.get_name() << "\n"
           << "position=x" << snapshot.x << ",y" << snapshot.y << ",z" << snapshot.z << "\n"
           << "weapon=" << snapshot.wielded_weapon << "\n"
           << "stamina=" << snapshot.stamina_percent << "%\n"
           << "pain=" << snapshot.pain << "\n"
           << "morale=" << snapshot.morale << "\n"
           << "fear=" << snapshot.fear << "\n"
           << "hp=" << snapshot.hp_percent << "%\n"
           << "visible_condition=" << visible_condition( who ) << "\n"
           << "bleeding=" << ( snapshot.bleeding ? "yes" : "no" ) << "\n"
           << "grabbed=" << ( snapshot.grabbed ? "yes" : "no" ) << "\n"
           << "ammo=" << snapshot.ammo_remaining << "/" << snapshot.ammo_capacity << "\n\n"
           << "CURRENT PERCEPTION\n"
           << "visible_enemies=" << snapshot.visible_enemy_count << "\n"
           << "visible_allies=" << snapshot.visible_ally_count << "\n";
    for( const combat_visible_creature &creature : snapshot.visible_creatures ) {
        prompt << "- exact_name=" << creature.name
               << ", entity_type=" << ( creature.monster ? "monster" : creature.npc ? "npc" :
                                         creature.player ? "player" : "creature" )
               << ", alive=yes, direction=" << relative_direction( creature.dx, creature.dy, creature.dz )
               << ", distance=" << creature.distance << ", attitude=" << creature.attitude
               << ", condition=" << creature.condition
               << ", bleeding=" << ( creature.bleeding ? "yes" : "no" )
               << ", grabbed=" << ( creature.grabbed ? "yes" : "no" )
               << ", adjacent_hostiles=" << creature.adjacent_hostiles;
        if( !creature.held_item.empty() ) {
            prompt << ", held_item=" << creature.held_item;
        }
        if( creature.observer_target ) {
            prompt << ", your_current_target=yes";
        }
        if( creature.targeting_observer ) {
            prompt << ", targeting_you=yes";
        }
        if( !creature.target_name.empty() ) {
            prompt << ", attacking_visible_target=" << creature.target_name;
        }
        prompt << "\n";
    }
    for( const combat_audible_event &audible : snapshot.audible_events ) {
        prompt << "- HEARD ONLY: " << audible.kind << " "
               << relative_direction( audible.dx, audible.dy, audible.dz )
               << ", distance=" << audible.distance << ", volume=" << audible.volume
               << ". The source identity is unknown.\n";
    }
    prompt << "environment=current_tile=" << snapshot.current_tile
           << ", outside=" << ( snapshot.outside ? "yes" : "no" )
           << ", weather=" << snapshot.weather << ", ambient_light=" << snapshot.ambient_light
           << "\ntactical_danger="
           << ( snapshot.tactical_danger >= 20.0f                ? "alta"
                : snapshot.tactical_danger > NPC_DANGER_VERY_LOW ? "moderada"
                                                                 : "baja" )
           << "\n\n"
           << "TACTICAL INTENT\n"
           << ( snapshot.tactical_intent.empty() ? "No promised assistance or special action."
                                                 : snapshot.tactical_intent )
           << "\n\n"
           << "RELATIONSHIP\n"
           << "player_trust=" << who.op_of_u.trust << "\n"
           << "player_value=" << who.op_of_u.value << "\n"
           << "player_fear=" << who.op_of_u.fear << "\n\n"
           << "RELEVANT MEMORY (lower authority than current perception)\n"
           << build_memory_context( who, 4 ) << "\n"
           << build_recent_speech_context( who, 4 ) << "\n"
           << build_recent_world_event_context( who, 10, 120 )
           << "CURRENT STATE above is authoritative if recent history conflicts with it.\n";
    std::vector<std::string> recently_used_names;
    for( const combat_visible_creature &creature : snapshot.visible_creatures ) {
        if( ( creature.player || creature.npc ) && recent_speech_mentions( who, creature.name ) ) {
            recently_used_names.push_back( creature.name );
        }
    }
    if( !recently_used_names.empty() ) {
        prompt << "NAMES USED RECENTLY (do not address them again unless this event urgently "
                  "requires it): ";
        for( std::size_t index = 0; index < recently_used_names.size(); ++index ) {
            if( index > 0 ) {
                prompt << ", ";
            }
            prompt << recently_used_names[index];
        }
        prompt << "\n";
    }
    prompt << "OUTPUT_LANGUAGE=" << current_dialogue_language_name() << "\n";
    return prompt.str();
}

combat_social_process_result process_combat_social( npc &who )
{
    scoped_profile profile( profile_subsystem::combat_social );
    combat_social_process_result result;
    if( !who.is_player_ally() || who.is_dead_state() || who.is_hallucination() ||
        who.in_sleep_state() ) {
        return result;
    }

    // Generated batch candidates are emitted only by the main-thread Social
    // Director, at most one group line per game turn.
    emit_due_combat_line( who );

    combat_social_state &state = combat_states[npc_key( who )];
    const int now = current_turn_number();
    const std::uint64_t latest_world_sequence = latest_world_event_sequence();
    const std::uint64_t current_group_state_fingerprint = group_state_fingerprint();
    const bool group_state_changed = current_group_state_fingerprint !=
                                     state.last_group_state_fingerprint;
    bool reuse_idle_visible_creatures = false;

    if( state.initialized ) {
        bool any_relevant_physical_event = false;
        bool immediate_physical_event = false;
        if( latest_world_sequence > state.last_seen_world_sequence ) {
            const std::vector<world_event> probe = recent_world_events_for(
                    who, state.last_seen_world_sequence, max_pending_candidates,
                    candidate_max_age_turns );
            any_relevant_physical_event = !probe.empty();
            // Ordinary misses/dodges remain candidates, but are coalesced on
            // the next normal combat/idle evaluation instead of forcing a full
            // LOS snapshot for every attack animation.  State-changing events
            // remain immediate.
            immediate_physical_event = std::any_of( probe.begin(), probe.end(),
            []( const world_event &event ) {
                return event.importance >= 80;
            } );
        }
        const bool self_state_changed = who.hp_percentage() != state.last.hp_percent ||
                                        who.has_effect( effect_bleed ) != state.last.bleeding ||
                                        who.has_effect_with_flag( json_flag_GRAB ) != state.last.grabbed ||
                                        who.has_effect( effect_npc_run_away ) != state.last.retreating;
        const bool vanilla_combat_wakeup = !state.last.in_combat &&
                                           ( who.current_target() != nullptr ||
                                             who.danger_assessment() > NPC_DANGER_VERY_LOW );
        const bool friend_set_changed = who.get_cached_friends().size() !=
                                        state.last_cached_friend_count;
        const creature_tracker &tracker = get_creature_tracker();
        const bool creature_set_changed = tracker.npc_size() != state.last_active_npc_count ||
                                          tracker.size() != state.last_monster_count;

        // npc::move() can run multiple times during one game second.  A second
        // full creature/LOS scan cannot reveal a new snapshot transition unless
        // a physical hook published a relevant event in between.
        if( state.last_snapshot_turn == now && !immediate_physical_event &&
            !self_state_changed && !vanilla_combat_wakeup && !friend_set_changed &&
            !group_state_changed ) {
            if( !creature_set_changed ) {
                return result;
            }
        }
        if( !state.last.in_combat && state.candidates.empty() && !immediate_physical_event &&
            !self_state_changed && !vanilla_combat_wakeup &&
            !friend_set_changed && !creature_set_changed && !group_state_changed ) {
            // No event in the global suffix belonged to this observer, so it is
            // safe to advance past it instead of rescanning the ring next turn.
            if( !any_relevant_physical_event ) {
                state.last_seen_world_sequence = latest_world_sequence;
            }
            if( now < state.next_idle_snapshot_turn ) {
                return result;
            }
            // Preserve the periodic self/environment refresh and profiler
            // lifecycle, but reuse the already validated idle creature list.
            // A shared per-turn fingerprint forces a full observer-specific
            // rebuild as soon as HP, bleeding, grabs or retreat state changes.
            reuse_idle_visible_creatures = true;
        }
    }

    const combat_perception_snapshot current = build_combat_perception_snapshot_impl(
                who, reuse_idle_visible_creatures ? &state.last : nullptr );
    state.last_snapshot_turn = current.turn;
    state.last_cached_friend_count = who.get_cached_friends().size();
    state.last_active_npc_count = get_creature_tracker().npc_size();
    state.last_monster_count = get_creature_tracker().size();
    state.last_group_state_fingerprint = current_group_state_fingerprint;
    state.next_idle_snapshot_turn = current.in_combat ? current.turn + 1 :
                                    current.turn + idle_snapshot_interval_turns;
    if( !state.initialized ) {
        state.initialized = true;
        state.last = current;
        if( !current.in_combat ) {
            return result;
        }
        state.last.in_combat = false;
    }

    const bool encounter_started = current.in_combat && !state.last.in_combat;
    std::vector<combat_social_event> events = detect_events( state.last, current );
    for( auto iter = state.last_event_turn.begin(); iter != state.last_event_turn.end(); ) {
        if( now - iter->second >= duplicate_event_gap_turns ) {
            iter = state.last_event_turn.erase( iter );
        } else {
            ++iter;
        }
    }
    if( encounter_started ) {
        ++state.encounter_generation;
    }

    // Consume physical facts captured by hooks since this observer's previous
    // turn.  They become candidates in the existing Combat Social scheduler;
    // the event stream never schedules speech on its own.
    const std::vector<world_event> physical_events = recent_world_events_for(
                who, state.last_seen_world_sequence, max_pending_candidates, candidate_max_age_turns );
    const bool has_first_sight_transition = std::any_of( events.begin(), events.end(),
    []( const combat_social_event &event ) {
        return event.observer_first_sight;
    } );
    const physical_batch_result physical_batch = has_first_sight_transition ?
            physical_batch_result{} : try_enqueue_physical_batch(
                who, physical_events, state.encounter_generation );
    if( physical_batch.queued ) {
        state.last_seen_world_sequence = latest_world_event_sequence();
        state.last = current;
        result.event_detected = true;
        result.request_queued = true;
        result.request_id = physical_batch.request_id;
        result.event = physical_batch.primary_event;
        return result;
    }
    if( physical_batch.deferred ) {
        state.last = current;
        result.event_detected = true;
        result.event = physical_batch.primary_event;
        return result;
    }
    for( const world_event &physical : physical_events ) {
        if( batched_event_ids.count( physical.sequence_id ) != 0 ) {
            continue;
        }
        combat_social_event converted;
        if( combat_event_from_world_event( who, physical, converted ) ) {
            narrable_event_ids.insert( converted.sequence_id );
            if( converted.encounter_generation == 0 ) {
                converted.encounter_generation = state.encounter_generation;
            }
            annotate_world_event( converted.sequence_id, "candidate", false, 0, {}, {},
                                  converted.encounter_generation );
            add_candidate( state, std::move( converted ), now );
        }
    }
    state.last_seen_world_sequence = latest_world_event_sequence();

    // Snapshot transitions are also recorded structurally for recent-history
    // prompts and diagnostics.  They are inserted directly as candidates to
    // avoid waiting for the observer's next movement turn.
    std::unordered_set<std::string> record_count_companion_for;
    for( combat_social_event &event : events ) {
        event.encounter_generation = state.encounter_generation;
        event.actor_id = who.getID().get_value();
        event.actor_name = who.get_name();
        event.actor_identity = "npc:" + std::to_string( event.actor_id );

        if( event.observer_first_sight ) {
            auto active = active_first_sight_facts.find( event.target_identity );
            if( active != active_first_sight_facts.end() &&
                !allied_group_still_sees_hostile( event.target_identity ) ) {
                active_first_sight_facts.erase( active );
                active = active_first_sight_facts.end();
            }
            if( active != active_first_sight_facts.end() ) {
                add_world_event_known_observer( active->second.sequence_id,
                                                who.getID().get_value() );
                event.sequence_id = active->second.sequence_id;
                event.group_already_verbalized = active->second.request_reserved ||
                        verbalized_event_ids.count( active->second.sequence_id ) != 0;
                if( event.group_already_verbalized ) {
                    debug_first_sight_decision( who, event, "known_by_only" );
                    continue;
                }
                add_candidate( state, event, now );
                continue;
            }
            record_count_companion_for.insert( event.target_identity );
        } else if( event.first_sight_count_companion &&
                   record_count_companion_for.count( event.target_identity ) == 0 ) {
            // A late observer has already joined the original typed fact.  Its
            // old count transition must not survive as a second candidate.
            continue;
        }

        world_event captured;
        captured.type = world_type_from_combat_event( event.type );
        captured.actor = snapshot_entity( &who );
        captured.actor.character_id = who.getID().get_value();
        captured.target.character_id = event.target_id;
        captured.target.name = event.target_name;
        captured.target.kind = event.target_id >= 0 ? "character" :
                               event.target_name.empty() ? "" : "monster";
        const combat_visible_creature *visible = !event.target_identity.empty() ?
                find_hostile_identity( current, event.target_identity ) :
                event.target_id >= 0 ? find_character( current, event.target_id ) :
                find_named_target( current, event.target_name );
        if( visible != nullptr ) {
            captured.target.x = current.x + visible->dx;
            captured.target.y = current.y + visible->dy;
            captured.target.z = current.z + visible->dz;
        }
        captured.importance = event.importance;
        captured.confirmed_outcome = true;
        captured.claim_level = event.claim_level;
        captured.body_part = event.body_part;
        captured.damage = event.damage;
        captured.attack_mode = event.attack_mode;
        captured.source = "combat_social_snapshot";
        captured.detail = event.detail;
        captured.encounter_generation = state.encounter_generation;
        captured.known_by_npc_ids.push_back( who.getID().get_value() );
        // Preserve the exact Combat Social enum name in the detail/source;
        // world_event_type is reserved for physical shared hooks.
        event.sequence_id = record_world_event( std::move( captured ) );
        narrable_event_ids.insert( event.sequence_id );
        if( event.target_identity.empty() ) {
            if( event.target_id >= 0 ) {
                event.target_identity = "character:" + std::to_string( event.target_id );
            } else if( const combat_visible_creature *visible = find_named_target( current,
                       event.target_name ) ) {
                event.target_identity = "visible:" + event.target_name + ":" +
                                        std::to_string( visible->dx ) + "," +
                                        std::to_string( visible->dy ) + "," +
                                        std::to_string( visible->dz );
            } else {
                event.target_identity = "snapshot:" + event.target_name;
            }
        }
        add_candidate( state, event, now );
        if( event.observer_first_sight ) {
            active_first_sight_facts[event.target_identity] = { event.sequence_id, false };
        }
    }
    state.last_seen_world_sequence = latest_world_event_sequence();
    state.last = current;
    expire_inactive_first_sight_facts();

    coalesce_grab_sequence( state, who.getID().get_value() );

    state.candidates.erase( std::remove_if( state.candidates.begin(), state.candidates.end(),
    [&]( const combat_social_state::candidate &candidate ) {
        const bool expired = now - candidate.last_turn > candidate_max_age_turns;
        if( expired ) {
            ++social_metrics.discarded_expiration;
            annotate_world_event( candidate.event.sequence_id, "discarded", false, 0,
                                  "candidate_expired" );
        }
        return expired;
    } ), state.candidates.end() );
    if( state.candidates.empty() ) {
        return result;
    }

    const auto selected = std::max_element( state.candidates.begin(), state.candidates.end(),
    []( const combat_social_state::candidate &lhs, const combat_social_state::candidate &rhs ) {
        return std::make_tuple( npc_ai::combat_social_speak_priority( lhs.event ), lhs.last_turn ) <
               std::make_tuple( npc_ai::combat_social_speak_priority( rhs.event ), rhs.last_turn );
    } );
    result.event_detected = true;
    result.event = selected->event;
    const int selected_speak_priority = combat_social_speak_priority( result.event );
    add_msg_debug( debugmode::DF_NPC_COMBATAI,
                   "COMBAT_SOCIAL_EVENT NPC=%s EVENT=%s TARGET=%s TURN=%d "
                   "VISIBLE_ENEMIES=%d VISIBLE_ALLIES=%d AUDIBLE_EVENTS=%d EVENT_IMPORTANCE=%d",
                   who.get_name(), combat_social_event_name( result.event.type ),
                   result.event.target_name, now, current.visible_enemy_count,
                   current.visible_ally_count, static_cast<int>( current.audible_events.size() ),
                   result.event.importance );
    const std::string key = event_key( result.event );
    const auto duplicate = state.last_event_turn.find( key );
    if( duplicate != state.last_event_turn.end() &&
        now - duplicate->second < duplicate_event_gap_turns ) {
        ++social_metrics.discarded_deduplication;
        add_msg_debug( debugmode::DF_NPC_COMBATAI,
                       "COMBAT_SOCIAL_EVENT NPC=%s EVENT=%s SOCIAL_DECISION=duplicate",
                       who.get_name(), combat_social_event_name( result.event.type ) );
        annotate_world_event( result.event.sequence_id, "discarded", false, 0,
                              "duplicate_event_identity" );
        debug_first_sight_decision( who, result.event, "duplicate" );
        state.candidates.erase( selected );
        return result;
    }

    const int required_gap = result.event.may_bypass_cooldown ?
                             ( batching_enabled ? urgent_request_gap_turns :
                               raw_urgent_request_gap_turns ) :
                             ( batching_enabled ? ordinary_request_gap_turns :
                               raw_ordinary_request_gap_turns );
    // 30/5 turns are secondary pacing for an equivalent event type.  A real
    // state transition (grab -> failed escape -> drag, for example) is allowed
    // to react immediately and exact-event dedup remains the stronger guard.
    if( ( result.event.type == state.last_requested_type || result.event.importance < 80 ) &&
        now - state.last_request_turn < required_gap ) {
        add_msg_debug( debugmode::DF_NPC_COMBATAI,
                       "COMBAT_SOCIAL_EVENT NPC=%s EVENT=%s SOCIAL_COOLDOWN=%d "
                       "SOCIAL_DECISION=kept_pending",
                       who.get_name(), combat_social_event_name( result.event.type ),
                       required_gap );
        debug_first_sight_decision( who, result.event, "kept_pending" );
        ++social_metrics.discarded_cooldown;
        return result;
    }
    const bool supersede = state.pending_request_id != 0 &&
                           ( result.event.type == combat_social_event_type::combat_end ||
                             ( result.event.may_bypass_cooldown &&
                               result.event.type != state.pending_type &&
                               selected_speak_priority >= state.pending_importance ) );
    if( state.pending_request_id != 0 && !supersede ) {
        debug_first_sight_decision( who, result.event, "kept_pending" );
        return result;
    }

    const std::string prompt = build_combat_social_prompt( who, current, result.event );
    std::vector<ai_target_snapshot> visible_targets = capture_visible_targets( current );
    const ai_enqueue_result queued = enqueue_combat_social_dialogue(
        who, prompt, static_cast<int>( result.event.type ),
        combat_social_event_name( result.event.type ), result.event.detail, selected_speak_priority,
        current.in_combat, state.encounter_generation, result.event.sequence_id,
        result.event.target_id, result.event.target_name, std::move( visible_targets ),
        shared_social_event_key( result.event ), current.visible_ally_count + 1, supersede );
    if( !queued.accepted ) {
        if( queued.error.rfind( "Social ", 0 ) == 0 ) {
            annotate_world_event( result.event.sequence_id, "discarded", false, 0,
                                  "social_director_budget" );
            state.candidates.erase( selected );
        }
        debug_first_sight_decision( who, result.event, "social_director" );
        return result;
    }

    state.pending_request_id = queued.request_id;
    state.pending_importance = selected_speak_priority;
    state.pending_type = result.event.type;
    state.last_request_turn = now;
    state.last_requested_type = result.event.type;
    state.last_event_turn[key] = now;
    ++social_metrics.inferences_queued;
    const std::size_t queue_depth = get_ai_request_queue().pending_count();
    observed_queue_depths.push_back( queue_depth );
    social_metrics.queue_depth_max = std::max( social_metrics.queue_depth_max, queue_depth );
    annotate_world_event( result.event.sequence_id, "selected", true, queued.request_id, {},
                          result.event.coalesced_sequences );
    state.candidates.erase( selected );
    reserve_group_first_sight_request( result.event );
    result.request_queued = true;
    result.request_id = queued.request_id;
    add_msg_debug( debugmode::DF_NPC_COMBATAI,
                   "AI_COMBAT_REQUEST_QUEUED NPC=%s EVENT=%s TARGET=%s TURN=%d "
                   "VISIBLE_ENEMIES=%d VISIBLE_ALLIES=%d AUDIBLE_EVENTS=%d "
                   "EVENT_IMPORTANCE=%d",
                   who.get_name(), combat_social_event_name( result.event.type ),
                   result.event.target_name, now, current.visible_enemy_count,
                   current.visible_ally_count, static_cast<int>( current.audible_events.size() ),
                   result.event.importance );
    debug_first_sight_decision( who, result.event, "queued" );
    if( is_important_memory( result.event ) ) {
        remember_combat_event( who, combat_social_event_name( result.event.type ),
                               result.event.detail, result.event.importance );
    }
    return result;
}

void apply_combat_social_ai_completion( npc &who, const ai_request_completion &completion,
                                        ai_completion_apply_timings *timings )
{
    const ai_request_snapshot &request = completion.request;
    if( !request.combat_slots.empty() ) {
        apply_combat_batch_completion( completion, timings );
        return;
    }
    combat_social_state &state = combat_states[npc_key( who )];
    if( state.pending_request_id != request.id ) {
        add_msg_debug( debugmode::DF_NPC_COMBATAI,
                       "AI_COMBAT_RESULT_STALE NPC=%s EVENT=%s reason=superseded", who.get_name(),
                       request.event_kind );
        return;
    }
    state.pending_request_id = 0;
    state.pending_importance = 0;

    const combat_perception_snapshot current = build_combat_perception_snapshot( who );
    const combat_social_event_type type =
        static_cast<combat_social_event_type>( request.combat_event_type );
    const int max_age = type == combat_social_event_type::combat_end
                            ? post_combat_result_max_age_turns
                            : combat_result_max_age_turns;
    const bool stale = request.encounter_generation != state.encounter_generation ||
                       current.turn - request.created_turn > max_age ||
                       !event_still_relevant( type, current, request.target_id );
    if( stale ) {
        add_msg_debug( debugmode::DF_NPC_COMBATAI, "AI_COMBAT_RESULT_STALE NPC=%s EVENT=%s TURN=%d",
                       who.get_name(), request.event_kind, current.turn );
        return;
    }
    if( completion.response.context_truncated || !completion.response.success ||
        completion.response.text.empty() ) {
        ++social_metrics.discarded_validation;
        add_msg_debug( debugmode::DF_NPC_COMBATAI,
                       "AI_COMBAT_RESULT NPC=%s EVENT=%s RESULT=error_or_empty", who.get_name(),
                       request.event_kind );
        return;
    }
    if( model_chose_silence( completion.response.text ) ) {
        add_msg_debug( debugmode::DF_NPC_COMBATAI,
                       "AI_COMBAT_RESULT_ACCEPTED NPC=%s EVENT=%s SOCIAL_DECISION=SILENT",
                       who.get_name(), request.event_kind );
        return;
    }

    std::string spoken = sanitize_speech( extract_spoken_text( completion.response.text ) );
    if( spoken.empty() ) {
        return;
    }
    const std::string language_code = request.dialogue_language_code.empty() ?
                                      current_dialogue_language_code() :
                                      request.dialogue_language_code;
    if( !generated_text_matches_dialogue_language( spoken, language_code ) ) {
        const ai_enqueue_result retry = enqueue_language_retry( request );
        if( retry.accepted ) {
            state.pending_request_id = retry.request_id;
            state.pending_importance = request.event_priority;
            ++social_metrics.inferences_queued;
            const std::size_t depth = get_ai_request_queue().pending_count();
            observed_queue_depths.push_back( depth );
            social_metrics.queue_depth_max = std::max( social_metrics.queue_depth_max, depth );
            add_msg_debug( debugmode::DF_NPC_COMBATAI,
                           "AI_COMBAT_RESULT_RETRY NPC=%s EVENT=%s reason=mixed_language",
                           who.get_name(), request.event_kind );
            return;
        }
        spoken = dialogue_language_fallback( request.event_kind, current.in_combat );
    }
    if( combat_social_text_has_unconfirmed_tactical_promise( spoken ) ) {
        ++social_metrics.discarded_tactical_promise;
        add_msg_debug( debugmode::DF_NPC_COMBATAI,
                       "AI_COMBAT_RESULT_STALE NPC=%s EVENT=%s reason=false_tactical_promise",
                       who.get_name(), request.event_kind );
        return;
    }
    if( response_mentions_entity_no_longer_visible( spoken, request, current ) ) {
        ++social_metrics.discarded_validation;
        add_msg_debug( debugmode::DF_NPC_COMBATAI,
                       "AI_COMBAT_RESULT_STALE NPC=%s EVENT=%s reason=entity_no_longer_visible",
                       who.get_name(), request.event_kind );
        return;
    }
    if( combat_social_text_claims_no_threats( spoken ) &&
        ( current.in_combat || current.visible_enemy_count > 0 ||
          current.tactical_danger > NPC_DANGER_VERY_LOW ) ) {
        ++social_metrics.discarded_validation;
        add_msg_debug( debugmode::DF_NPC_COMBATAI,
                       "AI_COMBAT_RESULT_STALE NPC=%s EVENT=%s reason=false_threat_cleared_claim",
                       who.get_name(), request.event_kind );
        return;
    }
    if( recent_speech_is_duplicate( who, spoken ) ) {
        ++social_metrics.discarded_deduplication;
        return;
    }
    measure_completion_phase( timings == nullptr ? nullptr : &timings->say_us, [&]() {
        say_ai_line( who, spoken, sounds::sound_t::alert );
    } );
    state.last_spoken_turn = current.turn;
    ++social_metrics.candidates_validated;
    ++social_metrics.lines_emitted;
    ++emitted_by_speaker[who.getID().get_value()];
    if( request.event_generation != 0 ) {
        verbalized_event_ids.insert( request.event_generation );
    }
    measure_completion_phase( timings == nullptr ? nullptr : &timings->memory_us, [&]() {
        remember_recent_speech( who, spoken, combat_social_intent_name( type ) );
        remember_exchange(
            who, "[EVENTO SOCIAL DE COMBATE " + request.event_kind + "] " + request.event_detail,
            spoken );
    } );
    measure_completion_phase( timings == nullptr ? nullptr :
                              &timings->npc_to_npc_schedule_us, [&]() {
        maybe_enqueue_npc_reply( who, spoken, request.origin, request.id, 0 );
    } );
    add_msg_debug( debugmode::DF_NPC_COMBATAI,
                   "AI_COMBAT_RESULT_ACCEPTED NPC=%s EVENT=%s NPC_SPEECH=%s", who.get_name(),
                   request.event_kind, one_line( spoken ) );
}

void reset_combat_social_state_for_test( const npc &who )
{
    combat_states.erase( npc_key( who ) );
}

void reset_all_combat_social_states()
{
    combat_states.clear();
    active_first_sight_facts.clear();
    cached_group_state = group_state_cache{};
    pending_combat_lines.clear();
    batched_event_ids.clear();
    last_group_line_turn = -1000000000;
    batching_enabled = true;
    reset_combat_social_metrics();
}

void notify_visible_enemy_killed( const monster &victim, const Creature *killer )
{
    if( g == nullptr || victim.is_hallucination() ) {
        return;
    }
    map &here = get_map();
    world_event captured;
    captured.type = world_event_type::enemy_killed;
    captured.actor = snapshot_entity( killer );
    captured.target = snapshot_entity( &victim );
    captured.importance = 84;
    captured.confirmed_outcome = true;
    captured.claim_level = world_event_claim_level::death_confirmed;
    captured.source = "monster::die";
    captured.detail = "El enemigo visible " + victim.disp_name() + " acaba de morir.";
    if( killer != nullptr ) {
        captured.detail += " El responsable fue " + killer->disp_name() + ".";
    }
    for( npc &observer : g->all_npcs() ) {
        if( !observer.is_player_ally() || observer.is_dead_state() ||
            !observer.sees( here, victim ) ||
            observer.attitude_to( victim ) != Creature::Attitude::HOSTILE ) {
            continue;
        }
        captured.known_by_npc_ids.push_back( observer.getID().get_value() );
    }
    if( !captured.known_by_npc_ids.empty() ) {
        record_world_event( std::move( captured ) );
    }
}

} // namespace npc_ai
