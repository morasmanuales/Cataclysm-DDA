#include "npc_ai_async.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <mutex>
#include <utility>

#include "avatar.h"
#include "calendar.h"
#include "color.h"
#include "debug.h"
#include "game.h"
#include "messages.h"
#include "npc.h"
#include "npc_ai_debug.h"
#include "npc_ai_memory.h"
#include "npc_ai_profiler.h"
#include "npc_ai_rescue.h"
#include "npc_ai_batch_pickup.h"
#include "npc_ai_combat_social.h"
#include "npc_ai_context.h"
#include "npc_ai_coordination.h"
#include "npc_ai_equipment_memory.h"
#include "npc_ai_event_stream.h"
#include "npc_ai_fire.h"
#include "npc_ai_goal.h"
#include "npc_ai_order_intent.h"
#include "npc_ai_pickup.h"
#include "npc_ai_spontaneous.h"
#include "npc_ai_survival.h"
#include "npc_ai_action_parser.h"
#include "npc_ai_vehicle_unload.h"
#include "npc_ai_watchlist.h"
#include "npc_ai_wield.h"
#include "sounds.h"
#include "string_formatter.h"
#include "translations.h"

namespace npc_ai
{

namespace
{

constexpr std::size_t max_direct_turns_per_npc = 4;
constexpr int fairness_burst_limit = 8;
constexpr int social_budget_retention_turns = 30;
constexpr std::size_t max_completions_per_pump = 2;
constexpr std::chrono::milliseconds completion_pump_budget{ 5 };
constexpr std::chrono::milliseconds slow_completion_threshold{ 5 };

std::int64_t monotonic_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch() ).count();
}

std::atomic<std::uint64_t> next_conversation_turn{ 1 };
std::atomic<bool> completion_clock_override_enabled{ false };
std::mutex completion_clock_mutex;
ai_completion_clock completion_clock_override;

std::chrono::steady_clock::time_point completion_pump_now()
{
    if( !completion_clock_override_enabled.load( std::memory_order_relaxed ) ) {
        return std::chrono::steady_clock::now();
    }
    std::lock_guard<std::mutex> lock( completion_clock_mutex );
    return completion_clock_override();
}

bool is_player_dialogue_request( const ai_request_type type )
{
    return type == ai_request_type::direct_dialogue ||
           type == ai_request_type::group_dialogue;
}

} // namespace

ai_request_queue::ai_request_queue( ai_request_executor executor,
                                    const bool start_automatically )
    : executor_( std::move( executor ) ),
      start_automatically_( start_automatically ) {}

ai_request_queue::ai_request_queue( ai_prompt_only_request_executor executor,
                                    const bool start_automatically )
    : ai_request_queue( [executor = std::move( executor )](
                            const std::string & prompt,
                            const std::string & )
{
    return executor( prompt );
},
start_automatically ) {}

ai_request_queue::~ai_request_queue()
{
    shutdown();
}

ai_enqueue_result ai_request_queue::enqueue( ai_request_snapshot request, const bool supersede )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( stopping_ ) {
        return { false, 0, "AI request worker is shutting down." };
    }

    if( !ollama_prompt_fits_context( request.prompt, request.system_prompt ) ) {
        return { false, 0, "AI prompt exceeds the configured Ollama context budget." };
    }

    if( is_player_dialogue_request( request.type ) &&
        pending_direct_by_npc_[request.npc_id] >= max_direct_turns_per_npc ) {
        return { false, 0, "The NPC dialogue queue is full." };
    }

    if( !request.deduplication_key.empty() ) {
        const auto existing = pending_keys_.find( request.deduplication_key );
        if( existing != pending_keys_.end() ) {
            if( !supersede ) {
                return { false, existing->second, "An equivalent AI request is already pending." };
            }
            cancelled_ids_.insert( existing->second );
            pending_keys_.erase( existing );
        }
    }

    if( !request.social_event_key.empty() && request.social_reaction_budget > 0 ) {
        for( auto iter = social_budgets_.begin(); iter != social_budgets_.end(); ) {
            if( request.created_turn - iter->second.created_turn > social_budget_retention_turns ) {
                iter = social_budgets_.erase( iter );
            } else {
                ++iter;
            }
        }
        social_budget_state &social = social_budgets_[request.social_event_key];
        if( social.speakers.empty() ) {
            social.created_turn = request.created_turn;
        }
        if( social.speakers.count( request.npc_id ) != 0 ||
            social.speakers.size() >= static_cast<std::size_t>( request.social_reaction_budget ) ) {
            return { false, 0, "Social reaction budget exhausted." };
        }
        social.speakers.insert( request.npc_id );
    }

    request.id = next_request_id_++;
    request.session_generation = generation_;
    const std::int64_t now = monotonic_ms();
    if( request.created_ms == 0 ) {
        request.created_ms = now;
    }
    if( request.prompt_ready_ms == 0 ) {
        request.prompt_ready_ms = now;
    }
    request.enqueued_ms = now;
    request.prompt_bytes = request.prompt.size();
    request.system_bytes = request.system_prompt.size();
    request.queue_depth_at_enqueue = active_requests_ + completions_.size();
    for( const std::deque<ai_request_snapshot> &queue : requests_ ) {
        request.queue_depth_at_enqueue += queue.size();
    }
    const std::uint64_t request_id = request.id;
    if( request.origin == conversation_origin::direct_player_dialogue ) {
        latest_direct_dialogue_request_id_ = request_id;
    }
    if( !request.deduplication_key.empty() ) {
        pending_keys_[request.deduplication_key] = request_id;
    }

    const int priority = static_cast<int>( request.priority );
    requests_[priority].push_back( std::move( request ) );
    if( is_player_dialogue_request( requests_[priority].back().type ) ) {
        ++pending_direct_by_npc_[requests_[priority].back().npc_id];
    }

    if( start_automatically_ && !started_ ) {
        stopping_ = false;
        started_ = true;
        worker_ = std::thread( &ai_request_queue::worker_loop, this );
    }
    work_available_.notify_one();
    return { true, request_id, "" };
}

std::vector<ai_request_completion> ai_request_queue::take_completions(
    const std::size_t max_count )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    std::vector<ai_request_completion> result;
    result.reserve( std::min( max_count, completions_.size() ) );
    while( result.size() < max_count && !completions_.empty() ) {
        int highest_priority = -1;
        int lowest_priority = ai_request_priority_count;
        for( const ai_request_completion &completion : completions_ ) {
            const int priority = static_cast<int>( completion.request.priority );
            highest_priority = std::max( highest_priority, priority );
            lowest_priority = std::min( lowest_priority, priority );
        }

        int selected_priority = highest_priority;
        if( highest_priority != static_cast<int>( ai_request_priority::player_dialogue ) &&
            completion_high_priority_burst_ >= fairness_burst_limit &&
            lowest_priority < highest_priority ) {
            selected_priority = lowest_priority;
        }
        const auto selected = std::find_if( completions_.begin(), completions_.end(),
        [selected_priority]( const ai_request_completion & completion ) {
            return static_cast<int>( completion.request.priority ) == selected_priority;
        } );
        ai_request_completion completion = std::move( *selected );
        completions_.erase( selected );
        if( selected_priority < highest_priority || selected_priority == 0 ) {
            completion_high_priority_burst_ = 0;
        } else {
            completion_high_priority_burst_ = std::min( fairness_burst_limit,
                                              completion_high_priority_burst_ + 1 );
        }
        if( !completion.request.deduplication_key.empty() ) {
            const auto pending = pending_keys_.find( completion.request.deduplication_key );
            if( pending != pending_keys_.end() && pending->second == completion.request.id ) {
                pending_keys_.erase( pending );
            }
        }
        if( is_player_dialogue_request( completion.request.type ) ) {
            auto direct = pending_direct_by_npc_.find( completion.request.npc_id );
            if( direct != pending_direct_by_npc_.end() && direct->second > 0 && --direct->second == 0 ) {
                pending_direct_by_npc_.erase( direct );
            }
        }
        completion.request.main_thread_received_ms = monotonic_ms();
        result.push_back( std::move( completion ) );
    }
    return result;
}

void ai_request_queue::start()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( started_ ) {
        return;
    }
    stopping_ = false;
    started_ = true;
    worker_ = std::thread( &ai_request_queue::worker_loop, this );
    work_available_.notify_one();
}

void ai_request_queue::clear_locked()
{
    for( std::deque<ai_request_snapshot> &queue : requests_ ) {
        queue.clear();
    }
    completions_.clear();
    pending_keys_.clear();
    cancelled_ids_.clear();
    pending_direct_by_npc_.clear();
    social_budgets_.clear();
    latest_direct_dialogue_request_id_ = 0;
    high_priority_burst_ = 0;
    completion_high_priority_burst_ = 0;
}

void ai_request_queue::invalidate_session()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    ++generation_;
    clear_locked();
    idle_.notify_all();
}

void ai_request_queue::shutdown()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        if( !started_ ) {
            clear_locked();
            return;
        }
        stopping_ = true;
        clear_locked();
    }
    work_available_.notify_all();
    if( worker_.joinable() ) {
        worker_.join();
    }
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        started_ = false;
        stopping_ = false;
        active_requests_ = 0;
        idle_.notify_all();
    }
}

std::uint64_t ai_request_queue::session_generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

std::size_t ai_request_queue::pending_count() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    std::size_t count = active_requests_ + completions_.size();
    for( const std::deque<ai_request_snapshot> &queue : requests_ ) {
        count += queue.size();
    }
    return count;
}

std::size_t ai_request_queue::ready_completion_count() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return completions_.size();
}

std::size_t ai_request_queue::pending_direct_count( const int npc_id ) const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    const auto found = pending_direct_by_npc_.find( npc_id );
    return found == pending_direct_by_npc_.end() ? 0 : found->second;
}

bool ai_request_queue::has_pending_key( const std::string &key ) const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return pending_keys_.find( key ) != pending_keys_.end();
}

bool ai_request_queue::npc_social_request_precedes_latest_direct_dialogue(
    const ai_request_snapshot &request ) const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return request.origin == conversation_origin::npc_initiated_social &&
           latest_direct_dialogue_request_id_ != 0 &&
           request.id < latest_direct_dialogue_request_id_;
}

bool ai_request_queue::wait_until_idle_for_test( const std::chrono::milliseconds timeout )
{
    std::unique_lock<std::mutex> lock( mutex_ );
    return idle_.wait_for( lock, timeout, [this]() {
        if( active_requests_ != 0 ) {
            return false;
        }
        for( const std::deque<ai_request_snapshot> &queue : requests_ ) {
            if( !queue.empty() ) {
                return false;
            }
        }
        return true;
    } );
}

void ai_request_queue::set_executor_for_test( ai_request_executor executor,
        const bool start_automatically )
{
    shutdown();
    std::lock_guard<std::mutex> lock( mutex_ );
    executor_ = std::move( executor );
    start_automatically_ = start_automatically;
}

void ai_request_queue::set_executor_for_test(
    ai_prompt_only_request_executor executor, const bool start_automatically )
{
    set_executor_for_test( [executor = std::move( executor )](
                               const std::string & prompt,
    const std::string & ) {
        return executor( prompt );
    },
    start_automatically );
}

bool ai_request_queue::pop_next_request( ai_request_snapshot &request )
{
    int highest = -1;
    for( int priority = ai_request_priority_count - 1; priority >= 0;
         --priority ) {
        if( !requests_[priority].empty() ) {
            highest = priority;
            break;
        }
    }
    if( highest < 0 ) {
        return false;
    }

    int selected = highest;
    if( high_priority_burst_ >= fairness_burst_limit ) {
        for( int priority = 0; priority < highest; ++priority ) {
            if( !requests_[priority].empty() ) {
                selected = priority;
                break;
            }
        }
    }
    request = std::move( requests_[selected].front() );
    requests_[selected].pop_front();
    request.dequeued_ms = monotonic_ms();
    if( selected < highest || selected == 0 ) {
        high_priority_burst_ = 0;
    } else {
        ++high_priority_burst_;
    }
    return true;
}

void ai_request_queue::worker_loop()
{
    while( true ) {
        ai_request_snapshot request;
        {
            std::unique_lock<std::mutex> lock( mutex_ );
            work_available_.wait( lock, [this]() {
                if( stopping_ ) {
                    return true;
                }
                return std::any_of( std::begin( requests_ ), std::end( requests_ ),
                []( const std::deque<ai_request_snapshot> &queue ) {
                    return !queue.empty();
                } );
            } );
            if( stopping_ ) {
                return;
            }
            if( !pop_next_request( request ) ) {
                continue;
            }
            if( request.session_generation != generation_ ||
                cancelled_ids_.erase( request.id ) != 0 ) {
                idle_.notify_all();
                continue;
            }
            ++active_requests_;
        }

        ai_response response;
        request.worker_started_ms = monotonic_ms();
        request.http_started_ms = request.worker_started_ms;
        const auto execute = [&]( const std::string & prompt,
        const std::string & system_prompt ) {
            try {
                return executor_( prompt, system_prompt );
            } catch( const std::exception &error ) {
                return ai_response{false, "",
                                   std::string( "AI request executor failed: " ) +
                                   error.what()};
            } catch( ... ) {
                return ai_response{false, "", "AI request executor failed."};
            }
        };
        response = execute( request.prompt, request.system_prompt );
        // Explicit player turns are FIFO per NPC.  Keep their single language
        // retry inside the same asynchronous work item so a later turn cannot
        // overtake the corrected answer to an earlier one.
        if( is_player_dialogue_request( request.type ) &&
            response.success && request.language_retry_count == 0 &&
            !request.dialogue_language_code.empty() &&
            !generated_text_matches_dialogue_language(
                response.text, request.dialogue_language_code ) ) {
            ++request.language_retry_count;
            const std::string correction = dialogue_language_retry_instruction(
                                               request.dialogue_language_code, request.dialogue_language_name );
            request.system_prompt += "\n\n" + correction;
            request.prompt +=
                "\n\n=== CORRECCION DE SALIDA PARA ESTE REINTENTO ===\n" +
                correction;
            request.prompt_bytes = request.prompt.size();
            request.system_bytes = request.system_prompt.size();
            response = execute( request.prompt, request.system_prompt );
        }
        const std::int64_t fallback_completed_ms = monotonic_ms();
        request.http_completed_ms = response.http_completed_ms > 0
                                    ? response.http_completed_ms
                                    : fallback_completed_ms;
        request.parse_completed_ms = response.parse_completed_ms > 0 ?
                                     response.parse_completed_ms : request.http_completed_ms;

        {
            std::lock_guard<std::mutex> lock( mutex_ );
            --active_requests_;
            if( !stopping_ && request.session_generation == generation_ &&
                cancelled_ids_.erase( request.id ) == 0 ) {
                request.completion_queued_ms = monotonic_ms();
                completions_.push_back( { std::move( request ), std::move( response ) } );
            }
            idle_.notify_all();
        }
    }
}

ai_request_queue &get_ai_request_queue()
{
    // game::~game() owns shutdown; keeping the storage alive avoids static
    // destruction ordering hazards during process exit.
    static ai_request_queue *queue = new ai_request_queue();
    return *queue;
}

std::uint64_t next_conversation_turn_id()
{
    return next_conversation_turn.fetch_add( 1 );
}

int social_reaction_budget_for_priority( const int event_priority )
{
    return event_priority >= 97 ? 2 : 1;
}

const char *conversation_origin_name( const conversation_origin origin )
{
    switch( origin ) {
        case conversation_origin::direct_player_dialogue:
            return "DIRECT_PLAYER_DIALOGUE";
        case conversation_origin::group_player_dialogue:
            return "GROUP_PLAYER_DIALOGUE";
        case conversation_origin::spontaneous_world_event:
            return "SPONTANEOUS_WORLD_EVENT";
        case conversation_origin::npc_initiated_social:
            return "NPC_INITIATED_SOCIAL";
    }
    return "UNKNOWN";
}

bool conversation_origin_allows_npc_reply( const conversation_origin origin )
{
    return origin == conversation_origin::spontaneous_world_event ||
           origin == conversation_origin::npc_initiated_social;
}

namespace
{

int current_turn_number()
{
    return to_turn<int>( calendar::turn );
}

void capture_participants( ai_request_snapshot &request, const npc &who )
{
    const Character &player = get_player_character();
    request.npc_id = who.getID().get_value();
    request.player_id = player.getID().get_value();
    request.created_turn = current_turn_number();
    request.npc_x = who.pos_abs().x();
    request.npc_y = who.pos_abs().y();
    request.npc_z = who.pos_abs().z();
    request.player_x = player.pos_abs().x();
    request.player_y = player.pos_abs().y();
    request.player_z = player.pos_abs().z();
}

bool validate_participants( const ai_request_snapshot &request, npc *&who )
{
    if( request.session_generation != get_ai_request_queue().session_generation() || g == nullptr ||
        get_player_character().getID().get_value() != request.player_id ) {
        return false;
    }
    who = g->find_npc( character_id( request.npc_id ) );
    return who != nullptr && who->getID().get_value() == request.npc_id &&
           !who->is_dead_state() && !who->is_hallucination();
}

const char *request_type_name( const ai_request_type type )
{
    switch( type ) {
        case ai_request_type::spontaneous:
            return "SPONTANEOUS";
        case ai_request_type::combat_social:
            return "COMBAT_SOCIAL";
        case ai_request_type::direct_dialogue:
            return "DIRECT_PLAYER_DIALOGUE";
        case ai_request_type::group_dialogue:
            return "GROUP_PLAYER_DIALOGUE";
        case ai_request_type::legacy_prompt:
            return "LEGACY";
        case ai_request_type::watch_resolution:
            return "WATCH";
        case ai_request_type::pickup_resolution:
            return "PICKUP";
        case ai_request_type::wield_resolution:
            return "WIELD";
        case ai_request_type::order_resolution:
            return "ORDER";
    }
    return "UNKNOWN";
}

constexpr const char *ollama_diagnostics_file = "npc_ai_ollama_diagnostics.txt";

void log_dialogue_routing( const ai_request_snapshot &request,
                           const std::string &suppression_reason )
{
    if( !runtime_debug_enabled() ) {
        return;
    }
    debug_stream output( ollama_diagnostics_file );
    if( output ) {
        output << "NPC_AI_ROUTING request_id=" << request.id
               << " request_type=" << request_type_name( request.type )
               << " target_npc=" << request.npc_id
               << " origin=" << conversation_origin_name( request.origin )
               << " context_categories=" << request.context_categories
               << " npc_to_npc_reply_allowed="
               << ( conversation_origin_allows_npc_reply( request.origin ) ? "yes" : "no" )
               << " suppression_reason="
               << ( suppression_reason.empty() ? "none" : suppression_reason ) << '\n';
    }
}

std::int64_t elapsed_ms( const std::int64_t started, const std::int64_t completed )
{
    return started > 0 && completed >= started ? completed - started : 0;
}

struct latency_metric {
    std::uint64_t sum_ms = 0;
    std::int64_t max_ms = 0;

    void add( const std::int64_t value ) {
        const std::int64_t safe_value = std::max<std::int64_t>( 0, value );
        sum_ms += static_cast<std::uint64_t>( safe_value );
        max_ms = std::max( max_ms, safe_value );
    }

    std::uint64_t average( const std::uint64_t samples ) const {
        return samples == 0 ? 0 : sum_ms / samples;
    }
};

struct latency_aggregate {
    std::uint64_t samples = 0;
    std::uint64_t successes = 0;
    std::uint64_t errors = 0;
    std::uint64_t retries = 0;
    std::uint64_t language_requests = 0;
    std::uint64_t prompt_bytes = 0;
    std::uint64_t system_bytes = 0;
    std::uint64_t response_bytes = 0;
    std::uint64_t prompt_eval_tokens = 0;
    std::uint64_t generated_tokens = 0;
    std::uint64_t truncated_contexts = 0;
    std::uint64_t queue_depth = 0;
    std::size_t max_prompt_bytes = 0;
    std::size_t max_system_bytes = 0;
    std::size_t max_response_bytes = 0;
    std::size_t max_queue_depth = 0;
    latency_metric prompt_build;
    latency_metric enqueue_delay;
    latency_metric queue_wait;
    latency_metric worker_start;
    latency_metric http;
    latency_metric parse;
    latency_metric completion_queue;
    latency_metric main_thread_pickup;
    latency_metric validation;
    latency_metric total;
};

std::mutex latency_diagnostics_mutex;
latency_aggregate latency_diagnostics;

void reset_latency_diagnostics()
{
    std::lock_guard<std::mutex> lock( latency_diagnostics_mutex );
    latency_diagnostics = latency_aggregate();
}

void log_request_latency( ai_request_completion &completion,
                          const std::int64_t validation_started_ms )
{
    if( !runtime_debug_enabled() ) {
        return;
    }

    ai_request_snapshot &request = completion.request;
    request.validation_completed_ms = monotonic_ms();
    request.displayed_or_discarded_ms = request.validation_completed_ms;

    const std::int64_t prompt_build =
        elapsed_ms( request.created_ms, request.prompt_ready_ms );
    const std::int64_t enqueue_delay =
        elapsed_ms( request.prompt_ready_ms, request.enqueued_ms );
    const std::int64_t queue_wait =
        elapsed_ms( request.enqueued_ms, request.dequeued_ms );
    const std::int64_t worker_start =
        elapsed_ms( request.dequeued_ms, request.worker_started_ms );
    const std::int64_t http =
        elapsed_ms( request.http_started_ms, request.http_completed_ms );
    const std::int64_t parse =
        elapsed_ms( request.http_completed_ms, request.parse_completed_ms );
    const std::int64_t completion_queue =
        elapsed_ms( request.parse_completed_ms, request.completion_queued_ms );
    const std::int64_t main_thread_pickup =
        elapsed_ms( request.completion_queued_ms, request.main_thread_received_ms );
    const std::int64_t validation =
        elapsed_ms( validation_started_ms, request.validation_completed_ms );
    const std::int64_t total =
        elapsed_ms( request.created_ms, request.displayed_or_discarded_ms );

    std::lock_guard<std::mutex> lock( latency_diagnostics_mutex );
    latency_aggregate &aggregate = latency_diagnostics;
    ++aggregate.samples;
    if( completion.response.success ) {
        ++aggregate.successes;
    } else {
        ++aggregate.errors;
    }
    aggregate.retries += static_cast<std::uint64_t>( request.language_retry_count );
    if( !request.dialogue_language_code.empty() &&
        ( is_player_dialogue_request( request.type ) ||
          request.language_retry_count == 0 ) ) {
        ++aggregate.language_requests;
    }
    aggregate.prompt_bytes += request.prompt_bytes;
    aggregate.system_bytes += request.system_bytes;
    aggregate.response_bytes += completion.response.text.size();
    aggregate.prompt_eval_tokens += completion.response.prompt_eval_count;
    aggregate.generated_tokens += completion.response.eval_count;
    aggregate.truncated_contexts += completion.response.context_truncated ? 1 : 0;
    aggregate.queue_depth += request.queue_depth_at_enqueue;
    aggregate.max_prompt_bytes =
        std::max( aggregate.max_prompt_bytes, request.prompt_bytes );
    aggregate.max_system_bytes =
        std::max( aggregate.max_system_bytes, request.system_bytes );
    aggregate.max_response_bytes =
        std::max( aggregate.max_response_bytes, completion.response.text.size() );
    aggregate.max_queue_depth =
        std::max( aggregate.max_queue_depth, request.queue_depth_at_enqueue );
    aggregate.prompt_build.add( prompt_build );
    aggregate.enqueue_delay.add( enqueue_delay );
    aggregate.queue_wait.add( queue_wait );
    aggregate.worker_start.add( worker_start );
    aggregate.http.add( http );
    aggregate.parse.add( parse );
    aggregate.completion_queue.add( completion_queue );
    aggregate.main_thread_pickup.add( main_thread_pickup );
    aggregate.validation.add( validation );
    aggregate.total.add( total );

    debug_stream output( ollama_diagnostics_file );
    if( !output ) {
        return;
    }
    output << "NPC_AI_LATENCY_REQUEST request_id=" << request.id
           << " conversation_turn_id=" << request.conversation_id
           << " type=" << request_type_name( request.type )
           << " origin=" << conversation_origin_name( request.origin )
           << " context_categories=" << request.context_categories
           << " npc_to_npc_reply_allowed="
           << ( conversation_origin_allows_npc_reply( request.origin ) ? "yes" : "no" )
           << " lane=" << static_cast<int>( request.priority )
           << " npc=" << request.npc_id
           << " prompt_bytes=" << request.prompt_bytes
           << " system_bytes=" << request.system_bytes
           << " response_bytes=" << completion.response.text.size()
           << " prompt_eval_count=" << completion.response.prompt_eval_count
           << " eval_count=" << completion.response.eval_count
           << " context_truncated="
           << ( completion.response.context_truncated ? "yes" : "no" )
           << " queue_depth=" << request.queue_depth_at_enqueue
           << " prompt_build_ms=" << prompt_build
           << " enqueue_delay_ms=" << enqueue_delay
           << " queue_wait_ms=" << queue_wait
           << " worker_start_ms=" << worker_start << " http_ms=" << http
           << " parse_ms=" << parse << " completion_queue_ms=" << completion_queue
           << " main_thread_pickup_ms=" << main_thread_pickup
           << " validation_ms=" << validation << " total_ms=" << total
           << " retry_count=" << request.language_retry_count << " result="
           << ( completion.response.success ? "success" : "transport_error" )
           << '\n';

    const std::uint64_t samples = aggregate.samples;
    output << "NPC_AI_LATENCY_SUMMARY samples=" << samples
           << " successes=" << aggregate.successes
           << " errors=" << aggregate.errors << " retries=" << aggregate.retries
           << " language_requests=" << aggregate.language_requests
           << " language_retry_rate_percent_x100="
           << ( aggregate.language_requests == 0
                ? 0
                : aggregate.retries * 10000 / aggregate.language_requests )
           << " prompt_bytes_avg=" << aggregate.prompt_bytes / samples
           << " prompt_bytes_max=" << aggregate.max_prompt_bytes
           << " system_bytes_avg=" << aggregate.system_bytes / samples
           << " system_bytes_max=" << aggregate.max_system_bytes
           << " response_bytes_avg=" << aggregate.response_bytes / samples
           << " response_bytes_max=" << aggregate.max_response_bytes
           << " prompt_eval_tokens_avg=" << aggregate.prompt_eval_tokens / samples
           << " generated_tokens_avg=" << aggregate.generated_tokens / samples
           << " truncated_contexts=" << aggregate.truncated_contexts
           << " queue_depth_avg=" << aggregate.queue_depth / samples
           << " queue_depth_max=" << aggregate.max_queue_depth
           << " prompt_build_ms_avg=" << aggregate.prompt_build.average( samples )
           << " prompt_build_ms_max=" << aggregate.prompt_build.max_ms
           << " enqueue_delay_ms_avg=" << aggregate.enqueue_delay.average( samples )
           << " enqueue_delay_ms_max=" << aggregate.enqueue_delay.max_ms
           << " queue_wait_ms_avg=" << aggregate.queue_wait.average( samples )
           << " queue_wait_ms_max=" << aggregate.queue_wait.max_ms
           << " worker_start_ms_avg=" << aggregate.worker_start.average( samples )
           << " worker_start_ms_max=" << aggregate.worker_start.max_ms
           << " http_ms_avg=" << aggregate.http.average( samples )
           << " http_ms_max=" << aggregate.http.max_ms
           << " parse_ms_avg=" << aggregate.parse.average( samples )
           << " parse_ms_max=" << aggregate.parse.max_ms
           << " completion_queue_ms_avg="
           << aggregate.completion_queue.average( samples )
           << " completion_queue_ms_max=" << aggregate.completion_queue.max_ms
           << " main_thread_pickup_ms_avg="
           << aggregate.main_thread_pickup.average( samples )
           << " main_thread_pickup_ms_max=" << aggregate.main_thread_pickup.max_ms
           << " validation_ms_avg=" << aggregate.validation.average( samples )
           << " validation_ms_max=" << aggregate.validation.max_ms
           << " total_ms_avg=" << aggregate.total.average( samples )
           << " total_ms_max=" << aggregate.total.max_ms << '\n';
}

class latency_log_scope
{
    public:
        explicit latency_log_scope( ai_request_completion &completion ) :
            completion_( completion ), started_ms_( monotonic_ms() ) {}
        ~latency_log_scope() {
            log_request_latency( completion_, started_ms_ );
        }
    private:
        ai_request_completion &completion_;
        std::int64_t started_ms_;
};

} // namespace

ai_enqueue_result
enqueue_direct_dialogue( const npc &who, const std::string &player_line,
                         const std::string &prompt,
                         const std::uint64_t conversation_turn_id )
{
    scoped_profile profile( profile_subsystem::async_preparation );
    ai_request_snapshot request;
    request.priority = ai_request_priority::player_dialogue;
    request.type = ai_request_type::direct_dialogue;
    request.origin = conversation_origin::direct_player_dialogue;
    request.prompt = prompt;
    request.system_prompt =
        build_npc_system_prompt( who, npc_prompt_purpose::direct_dialogue );
    request.player_line = player_line;
    request.context_categories = context_intent_name( classify_context_intent( player_line ) );
    request.dialogue_language_code = current_dialogue_language_code();
    request.dialogue_language_name = current_dialogue_language_name();
    request.conversation_id = conversation_turn_id != 0
                              ? conversation_turn_id
                              : next_conversation_turn_id();
    capture_participants( request, who );
    request.deduplication_key = "direct:" + std::to_string( request.player_id ) +
                                ":" + std::to_string( request.npc_id ) + ":" +
                                std::to_string( request.conversation_id );
    return get_ai_request_queue().enqueue( std::move( request ) );
}

ai_enqueue_result enqueue_group_dialogue(
    const npc &who, const std::string &player_line, const std::string &prompt,
    const std::uint64_t conversation_turn_id )
{
    scoped_profile profile( profile_subsystem::async_preparation );
    ai_request_snapshot request;
    request.priority = ai_request_priority::player_dialogue;
    request.type = ai_request_type::group_dialogue;
    request.origin = conversation_origin::group_player_dialogue;
    request.prompt = prompt;
    request.system_prompt =
        build_npc_system_prompt( who, npc_prompt_purpose::direct_dialogue );
    request.player_line = player_line;
    request.context_categories = context_intent_name( classify_context_intent( player_line ) );
    request.dialogue_language_code = current_dialogue_language_code();
    request.dialogue_language_name = current_dialogue_language_name();
    request.conversation_id = conversation_turn_id;
    capture_participants( request, who );
    request.deduplication_key = "group:" + std::to_string( request.player_id ) +
                                ":" + std::to_string( request.npc_id ) + ":" +
                                std::to_string( request.conversation_id );
    return get_ai_request_queue().enqueue( std::move( request ) );
}

ai_enqueue_result enqueue_spontaneous_dialogue( const npc &who,
        const std::string &prompt,
        const std::string &event_kind,
        const std::string &event_detail,
        const int event_priority,
        const bool danger_at_creation )
{
    scoped_profile profile( profile_subsystem::async_preparation );
    ai_request_snapshot request;
    request.priority = ai_request_priority::ambient;
    request.type = ai_request_type::spontaneous;
    request.origin = conversation_origin::spontaneous_world_event;
    request.prompt = prompt;
    request.system_prompt =
        build_npc_system_prompt( who, npc_prompt_purpose::spontaneous_dialogue );
    request.dialogue_language_code = current_dialogue_language_code();
    request.dialogue_language_name = current_dialogue_language_name();
    request.event_kind = event_kind;
    request.context_categories = "SPONTANEOUS";
    request.event_detail = event_detail;
    request.event_priority = event_priority;
    request.danger_at_creation = danger_at_creation;
    capture_participants( request, who );
    request.deduplication_key =
        "spontaneous:" + std::to_string( request.npc_id ) + ":" + event_kind;
    request.social_event_key =
        "spontaneous:" + std::to_string( request.created_turn ) + ":" + event_kind +
        ":" + event_detail;
    request.social_reaction_budget =
        social_reaction_budget_for_priority( event_priority );
    return get_ai_request_queue().enqueue( std::move( request ) );
}

ai_enqueue_result enqueue_npc_reply_dialogue(
    const npc &listener, const npc &speaker, const std::string &spoken,
    const std::string &prompt, const bool danger_at_creation,
    const std::uint64_t conversation_id, const int reply_depth )
{
    scoped_profile profile( profile_subsystem::async_preparation );
    ai_request_snapshot request;
    request.priority = ai_request_priority::npc_to_npc;
    request.type = ai_request_type::spontaneous;
    request.origin = conversation_origin::npc_initiated_social;
    request.prompt = prompt;
    request.system_prompt =
        build_npc_system_prompt( listener, npc_prompt_purpose::npc_to_npc_reply );
    request.dialogue_language_code = current_dialogue_language_code();
    request.dialogue_language_name = current_dialogue_language_name();
    request.event_kind = "NPC_TO_NPC_REPLY";
    request.context_categories = "NPC_SOCIAL";
    request.event_detail = speaker.get_name() + " dijo: " + spoken;
    request.event_priority = 86;
    request.danger_at_creation = danger_at_creation;
    request.source_npc_id = speaker.getID().get_value();
    request.source_npc_name = speaker.get_name();
    request.conversation_id = conversation_id;
    request.reply_depth = reply_depth;
    capture_participants( request, listener );
    request.deduplication_key = "npc_reply:" + std::to_string( request.npc_id ) +
                                ":" + std::to_string( conversation_id );
    request.social_event_key = "npc_reply:" + std::to_string( conversation_id );
    request.social_reaction_budget = 1;
    return get_ai_request_queue().enqueue( std::move( request ) );
}

ai_enqueue_result enqueue_combat_social_dialogue(
    const npc &who, const std::string &prompt, const int combat_event_type,
    const std::string &event_kind, const std::string &event_detail,
    const int event_priority, const bool danger_at_creation,
    const std::uint64_t encounter_generation,
    const std::uint64_t event_generation, const int target_id,
    const std::string &target_name,
    std::vector<ai_target_snapshot> visible_targets,
    const std::string &social_event_key, const int social_group_size,
    const bool supersede )
{
    scoped_profile profile( profile_subsystem::async_preparation );
    ai_request_snapshot request;
    request.priority =
        event_priority >= 97   ? ai_request_priority::immediate_danger
        : event_priority >= 94 ? ai_request_priority::urgent_combat
        : ai_request_priority::normal_combat;
    request.type = ai_request_type::combat_social;
    request.origin = conversation_origin::spontaneous_world_event;
    request.prompt = prompt;
    request.system_prompt =
        build_npc_system_prompt( who, npc_prompt_purpose::combat_social );
    request.dialogue_language_code = current_dialogue_language_code();
    request.dialogue_language_name = current_dialogue_language_name();
    request.event_kind = event_kind;
    request.context_categories = "COMBAT_PERCEPTION";
    request.event_detail = event_detail;
    request.event_priority = event_priority;
    request.combat_event_type = combat_event_type;
    request.target_id = target_id;
    request.danger_at_creation = danger_at_creation;
    request.encounter_generation = encounter_generation;
    request.event_generation = event_generation;
    request.targets = std::move( visible_targets );
    capture_participants( request, who );
    request.deduplication_key = "combat_social:" + std::to_string( request.npc_id );
    request.social_event_key = social_event_key;
    request.social_reaction_budget =
        social_reaction_budget_for_priority( event_priority );
    request.social_group_size = social_group_size;
    if( !target_name.empty() &&
        std::none_of( request.targets.begin(), request.targets.end(),
    [&]( const ai_target_snapshot & target ) {
    return target.name == target_name;
} ) ) {
        ai_target_snapshot target;
        target.uid = "event_target";
        target.name = target_name;
        request.targets.push_back( std::move( target ) );
    }
    return get_ai_request_queue().enqueue( std::move( request ), supersede );
}

ai_enqueue_result enqueue_combat_social_batch_dialogue(
    const npc &coordinator, const std::string &prompt,
    const std::string &system_prompt, const int event_priority,
    const bool danger_at_creation, const std::uint64_t encounter_generation,
    std::vector<ai_combat_utterance_slot> slots,
    const std::string &batch_key, const int retry_count )
{
    scoped_profile profile( profile_subsystem::async_preparation );
    if( slots.empty() ) {
        return {false, 0, "Combat Social batch has no C++ speaker slots."};
    }
    ai_request_snapshot request;
    request.priority =
        event_priority >= 97   ? ai_request_priority::immediate_danger
        : event_priority >= 94 ? ai_request_priority::urgent_combat
        : ai_request_priority::normal_combat;
    request.type = ai_request_type::combat_social;
    request.origin = conversation_origin::spontaneous_world_event;
    request.prompt = prompt;
    request.system_prompt = system_prompt;
    request.dialogue_language_code = current_dialogue_language_code();
    request.dialogue_language_name = current_dialogue_language_name();
    request.event_kind = "COMBAT_BATCH";
    request.context_categories = "COMBAT_PERCEPTION";
    request.event_priority = event_priority;
    request.combat_event_type = slots.front().combat_event_type;
    request.target_id = slots.front().target_id;
    request.danger_at_creation = danger_at_creation;
    request.encounter_generation = encounter_generation;
    request.combat_slots = std::move( slots );
    request.combat_batch_retry_count = retry_count;
    capture_participants( request, coordinator );
    request.deduplication_key = "combat_batch:" + batch_key;
    request.social_event_key = "combat_batch:" + batch_key;
    // Cohort construction already enforces the Social Director's inference
    // budget.  Per-speaker reaction accounting would incorrectly turn a
    // multi-line inference back into a one-request-per-speaker path.
    request.social_reaction_budget = 0;
    request.social_group_size = static_cast<int>( request.combat_slots.size() );
    return get_ai_request_queue().enqueue( std::move( request ) );
}

ai_enqueue_result enqueue_legacy_ai_prompt( const std::string &prompt )
{
    ai_request_snapshot request;
    request.priority = ai_request_priority::player_dialogue;
    request.type = ai_request_type::legacy_prompt;
    request.prompt = prompt;
    request.player_id = get_player_character().getID().get_value();
    request.created_turn = current_turn_number();
    request.deduplication_key = "legacy:" + std::to_string( request.player_id );
    return get_ai_request_queue().enqueue( std::move( request ) );
}

ai_enqueue_result enqueue_language_retry( const ai_request_snapshot &request )
{
    if( request.language_retry_count > 0 ) {
        return {false, 0, "Language correction already attempted."};
    }
    ai_request_snapshot retry = request;
    retry.id = 0;
    ++retry.language_retry_count;
    if( retry.dialogue_language_code.empty() ) {
        retry.dialogue_language_code = current_dialogue_language_code();
    }
    if( retry.dialogue_language_name.empty() ) {
        retry.dialogue_language_name = current_dialogue_language_name();
    }
    const std::string correction = dialogue_language_retry_instruction(
                                       retry.dialogue_language_code, retry.dialogue_language_name );
    retry.system_prompt += "\n\n" + correction;
    retry.prompt +=
        "\n\n=== CORRECCION DE SALIDA PARA ESTE REINTENTO ===\n" + correction;
    retry.deduplication_key += ":language_retry";
    return get_ai_request_queue().enqueue( std::move( retry ) );
}

ai_enqueue_result enqueue_command_resolution(
    const npc &who, const ai_request_type type, const std::string &player_line,
    const std::string &prompt, std::vector<ai_target_snapshot> targets,
    const acquisition_intent acquisition,
    const std::string &acquisition_intent_source,
    const std::string &event_detail )
{
    ai_request_snapshot request;
    request.priority = ai_request_priority::player_dialogue;
    request.type = type;
    request.origin = conversation_origin::direct_player_dialogue;
    request.prompt = prompt;
    request.event_detail = event_detail;
    const npc_prompt_purpose purpose =
        type == ai_request_type::watch_resolution
        ? npc_prompt_purpose::watch_resolution
        : type == ai_request_type::pickup_resolution
        ? npc_prompt_purpose::pickup_resolution
        : type == ai_request_type::order_resolution
        ? npc_prompt_purpose::order_resolution
        : npc_prompt_purpose::wield_resolution;
    request.system_prompt = build_npc_system_prompt( who, purpose );
    request.player_line = player_line;
    request.context_categories = "COMMAND_RESOLUTION";
    request.acquisition = acquisition;
    request.acquisition_intent_source = acquisition_intent_source;
    request.targets = std::move( targets );
    capture_participants( request, who );
    request.deduplication_key = "direct:" + std::to_string( request.player_id ) +
                                ":" + std::to_string( request.npc_id );
    return get_ai_request_queue().enqueue( std::move( request ) );
}

void say_ai_line( const npc &who, const std::string &text, const sounds::sound_t priority )
{
    who.say_colored( text, c_green, c_cyan, priority );
}

void say_command_reply( npc &who, const std::string &message )
{
    say_ai_line( who, message, sounds::sound_t::order );
    // process_ai_completions() and the talk command both run after the normal
    // sound-marker pass for this input cycle.  Flush just-created speech now
    // so the player sees the answer before issuing another command.
    sounds::process_sound_markers( &get_player_character() );
}

namespace
{

struct completion_apply_result {
    bool applied = false;
    bool dropped_stale = false;
    std::uint64_t validation_us = 0;
    ai_completion_apply_timings timings;
};

struct slow_completion_record {
    std::uint64_t request_id = 0;
    std::uint64_t event_id = 0;
    int npc_id = -1;
    int source_npc_id = -1;
    std::string origin;
    std::string type;
    std::uint64_t total_us = 0;
    std::uint64_t validation_us = 0;
    ai_completion_apply_timings timings;
};

template<typename Callable>
void measure_main_thread_phase( const bool enabled, std::uint64_t &elapsed_us,
                                Callable &&action )
{
    if( !enabled ) {
        action();
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    action();
    elapsed_us += static_cast<std::uint64_t>( std::max<std::int64_t>( 0,
                  std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - started ).count() ) );
}

completion_apply_result apply_ai_completion( ai_request_completion &completion,
        const bool collect_timings )
{
    completion_apply_result result;
    const ai_request_snapshot &request = completion.request;
    const auto validation_started = std::chrono::steady_clock::now();
    const auto finish_validation = [&]() {
        if( collect_timings ) {
            result.validation_us = static_cast<std::uint64_t>( std::max<std::int64_t>( 0,
                                   std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - validation_started ).count() ) );
        }
    };

    if( request.type == ai_request_type::legacy_prompt ) {
        if( request.session_generation != get_ai_request_queue().session_generation() ||
            get_player_character().getID().get_value() != request.player_id ) {
            finish_validation();
            result.dropped_stale = true;
            return result;
        }
        finish_validation();
        if( completion.response.success ) {
            add_msg( m_good, "Qwen3: %s", completion.response.text );
        } else {
            add_msg( m_bad, _( "Ollama error: %s" ), completion.response.error );
        }
        result.applied = true;
        return result;
    }

    npc *who = nullptr;
    if( !validate_participants( request, who ) ) {
        finish_validation();
        result.dropped_stale = true;
        return result;
    }
    finish_validation();
    ai_completion_apply_timings *timings = collect_timings ? &result.timings : nullptr;
    if( request.type == ai_request_type::spontaneous ) {
        apply_spontaneous_ai_completion( *who, completion, timings );
        result.applied = true;
        return result;
    }
    if( request.type == ai_request_type::combat_social ) {
        apply_combat_social_ai_completion( *who, completion, timings );
        result.applied = true;
        return result;
    }
    if( request.type == ai_request_type::watch_resolution ) {
        apply_watch_ai_completion( *who, completion );
        result.applied = true;
        return result;
    }
    if( request.type == ai_request_type::pickup_resolution ) {
        apply_pickup_ai_completion( *who, completion );
        result.applied = true;
        return result;
    }
    if( request.type == ai_request_type::wield_resolution ) {
        apply_wield_ai_completion( *who, completion );
        result.applied = true;
        return result;
    }
    if( request.type == ai_request_type::order_resolution ) {
        apply_order_intent_completion( *who, completion );
        result.applied = true;
        return result;
    }

    if( !completion.response.success ) {
        log_dialogue_routing( request, "transport_error" );
        add_msg( m_bad, _( "Ollama error: %s" ), completion.response.error );
        result.applied = true;
        return result;
    }

    std::string npc_line = completion.response.text;
    strip_watch_markers( npc_line );
    const std::string language_code = request.dialogue_language_code.empty() ?
                                      current_dialogue_language_code() :
                                      request.dialogue_language_code;
    if( !generated_text_matches_dialogue_language( npc_line, language_code ) ) {
        const ai_enqueue_result retry = enqueue_language_retry( request );
        if( retry.accepted ) {
            result.applied = true;
            return result;
        }
        npc_line = dialogue_language_fallback( request.event_kind, false );
    }
    if( combat_social_text_has_unconfirmed_tactical_promise( npc_line ) ) {
        npc_line = dialogue_language_fallback( request.event_kind, false );
    }
    if( npc_line.empty() ) {
        npc_line = _( "Okay." );
    }
    if( request.type == ai_request_type::group_dialogue &&
        recent_speech_is_duplicate( *who, npc_line ) ) {
        log_dialogue_routing( request, "cross_speaker_duplicate" );
        result.applied = true;
        return result;
    }
    measure_main_thread_phase( collect_timings, result.timings.say_us, [&]() {
        say_ai_line( *who, npc_line );
    } );
    measure_main_thread_phase( collect_timings, result.timings.memory_us, [&]() {
        remember_exchange( *who, request.player_line, npc_line );
    } );
    log_dialogue_routing( request, "" );
    result.applied = true;
    return result;
}

void log_async_drain( const std::size_t queue_depth_before, const std::size_t taken,
                      const std::size_t applied, const std::size_t deferred,
                      const std::size_t dropped_stale, const std::size_t queue_depth_after,
                      const std::uint64_t total_us, const bool budget_stop,
                      const bool count_stop,
                      const std::vector<slow_completion_record> &slow_completions )
{
    if( !runtime_debug_enabled() ) {
        return;
    }
    debug_stream output( ollama_diagnostics_file );
    if( !output ) {
        return;
    }
    output << "ASYNC_DRAIN turn=" << current_turn_number()
           << " queue_depth_before=" << queue_depth_before
           << " taken=" << taken << " applied=" << applied
           << " deferred=" << deferred << " dropped_stale=" << dropped_stale
           << " queue_depth_after=" << queue_depth_after << " total_us=" << total_us
           << " budget_stop=" << ( budget_stop ? "yes" : "no" )
           << " count_stop=" << ( count_stop ? "yes" : "no" ) << '\n';
    for( const slow_completion_record &slow : slow_completions ) {
        output << "ASYNC_COMPLETION_SLOW request_id=" << slow.request_id
               << " event_id=" << slow.event_id << " npc_id=" << slow.npc_id
               << " source_npc_id=" << slow.source_npc_id
               << " source=" << slow.origin << " type=" << slow.type
               << " total_us=" << slow.total_us
               << " validation_us=" << slow.validation_us
               << " say_us=" << slow.timings.say_us
               << " memory_us=" << slow.timings.memory_us
               << " annotation_us=0"
               << " npc_to_npc_schedule_us=" << slow.timings.npc_to_npc_schedule_us
               << '\n';
    }
}

} // namespace

void process_ai_completions()
{
    ai_request_queue &queue = get_ai_request_queue();
    const bool collect_timings = runtime_debug_enabled();
    std::size_t queue_depth_before = 0;
    std::size_t queue_depth_after = 0;
    std::size_t taken = 0;
    std::size_t applied = 0;
    std::size_t dropped_stale = 0;
    bool budget_stop = false;
    bool count_stop = false;
    std::uint64_t total_us = 0;
    std::vector<slow_completion_record> slow_completions;

    {
        scoped_profile profile( profile_subsystem::async_main_thread );
        queue_depth_before = queue.ready_completion_count();
        const auto wall_started = std::chrono::steady_clock::now();
        const auto budget_started = completion_pump_now();
        while( taken < max_completions_per_pump ) {
            std::vector<ai_request_completion> completions = queue.take_completions( 1 );
            if( completions.empty() ) {
                break;
            }
            ++taken;
            ai_request_completion &completion = completions.front();
            const auto completion_started = std::chrono::steady_clock::now();
            completion_apply_result result;
            std::uint64_t completion_us = 0;
            {
                const latency_log_scope latency( completion );
                result = apply_ai_completion( completion, collect_timings );
                completion_us = static_cast<std::uint64_t>( std::max<std::int64_t>( 0,
                                std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - completion_started ).count() ) );
            }
            applied += result.applied ? 1 : 0;
            dropped_stale += result.dropped_stale ? 1 : 0;
            if( collect_timings && completion_us >= static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        slow_completion_threshold ).count() ) ) {
                const ai_request_snapshot &request = completion.request;
                slow_completions.push_back( { request.id, request.event_generation,
                                              request.npc_id, request.source_npc_id,
                                              conversation_origin_name( request.origin ),
                                              request_type_name( request.type ), completion_us,
                                              result.validation_us, result.timings } );
            }
            if( completion_pump_now() - budget_started >= completion_pump_budget &&
                queue.ready_completion_count() > 0 ) {
                budget_stop = true;
                break;
            }
        }
        queue_depth_after = queue.ready_completion_count();
        count_stop = !budget_stop && taken >= max_completions_per_pump &&
                     queue_depth_after > 0;
        total_us = static_cast<std::uint64_t>( std::max<std::int64_t>( 0,
                   std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - wall_started ).count() ) );
    }
    log_async_drain( queue_depth_before, taken, applied, queue_depth_after,
                     dropped_stale, queue_depth_after, total_us, budget_stop,
                     count_stop, slow_completions );
}

// Every module that keeps per-NPC state in a process-wide map must be cleared
// here.  Character ids are reused across saves, so a surviving entry silently
// attaches one character's cooldowns, goals or tasks to a different character
// in the next world, and the maps would otherwise grow for the whole process.
void reset_all_ai_session_state()
{
    reset_latency_diagnostics();
    reset_world_event_stream();
    reset_all_combat_social_states();
    reset_all_recent_speech();
    reset_all_spontaneous_states();
    reset_all_goals();
    reset_all_rescues();
    reset_all_survival_state();
    reset_all_start_fire_tasks();
    reset_all_vehicle_unload_tasks();
    reset_all_food_batches();
    reset_all_assignments();
    reset_all_equipment_memory_cache();
    reset_watch_cache();
}

void begin_ai_session()
{
    get_ai_request_queue().invalidate_session();
    reset_all_ai_session_state();
}

void end_ai_session()
{
    get_ai_request_queue().invalidate_session();
    reset_all_ai_session_state();
}

void shutdown_ai_requests()
{
    get_ai_request_queue().shutdown();
}

void set_ai_request_executor_for_test( ai_request_executor executor,
                                       const bool start_automatically )
{
    get_ai_request_queue().set_executor_for_test( std::move( executor ),
            start_automatically );
}

void set_ai_request_executor_for_test( ai_prompt_only_request_executor executor,
                                       const bool start_automatically )
{
    get_ai_request_queue().set_executor_for_test( std::move( executor ),
            start_automatically );
}

void set_ai_completion_clock_for_test( ai_completion_clock clock )
{
    std::lock_guard<std::mutex> lock( completion_clock_mutex );
    completion_clock_override = std::move( clock );
    completion_clock_override_enabled.store( true, std::memory_order_relaxed );
}

void reset_ai_completion_clock_for_test()
{
    completion_clock_override_enabled.store( false, std::memory_order_relaxed );
    std::lock_guard<std::mutex> lock( completion_clock_mutex );
    completion_clock_override = ai_completion_clock();
}

void reset_ai_request_system_for_test()
{
    reset_ai_completion_clock_for_test();
    get_ai_request_queue().set_executor_for_test( ask_llm, true );
    get_ai_request_queue().invalidate_session();
}

} // namespace npc_ai
