#pragma once
#ifndef CATA_SRC_NPC_AI_ASYNC_H
#define CATA_SRC_NPC_AI_ASYNC_H

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "npc_ai_client.h"
#include "npc_ai_pickup.h"
#include "sounds.h"

class npc;

namespace npc_ai
{

enum class ai_request_priority : int {
    ambient = 0,
    npc_to_npc = 1,
    normal_combat = 2,
    urgent_combat = 3,
    immediate_danger = 4,
    player_dialogue = 5,

    // Compatibility names for existing callers/tests.  New code should use the
    // lane names above so scheduling intent is explicit.
    low = ambient,
    medium = normal_combat,
    high = player_dialogue
};

constexpr int ai_request_priority_count = 6;

enum class ai_request_type : int {
    spontaneous,
    combat_social,
    direct_dialogue,
    group_dialogue,
    legacy_prompt,
    watch_resolution,
    pickup_resolution,
    wield_resolution,
    order_resolution
};

enum class conversation_origin : int {
    direct_player_dialogue,
    group_player_dialogue,
    spontaneous_world_event,
    npc_initiated_social
};

struct ai_target_snapshot {
    std::string uid;
    std::string item_id;
    std::string name;
    int x = 0;
    int y = 0;
    int z = 0;
};

// C++-authoritative slot for one possible line in a Combat Social batch.  The
// model may provide text and a subset of allowed event ids, but cannot choose
// the speaker, priority, expiry, or audience.
struct ai_combat_utterance_slot {
    int slot_id = -1;
    int speaker_id = -1;
    std::string speaker_name;
    std::vector<std::uint64_t> allowed_event_ids;
    int combat_event_type = -1;
    int target_id = -1;
    int speak_priority = 0;
    int expiry_turn = 0;
};

struct ai_request_snapshot {
    std::uint64_t id = 0;
    std::uint64_t session_generation = 0;
    ai_request_priority priority = ai_request_priority::low;
    ai_request_type type = ai_request_type::spontaneous;
    conversation_origin origin = conversation_origin::spontaneous_world_event;

    int npc_id = -1;
    int player_id = -1;
    int created_turn = 0;
    int event_priority = 0;
    int combat_event_type = -1;
    int target_id = -1;
    int language_retry_count = 0;
    int reply_depth = 0;
    int source_npc_id = -1;
    bool danger_at_creation = false;

    std::uint64_t encounter_generation = 0;
    std::uint64_t event_generation = 0;
    std::uint64_t conversation_id = 0;

    // Monotonic request lifecycle timestamps.  They are process-local and are
    // used only for latency diagnostics; savegame/world time never depends on
    // them.
    std::int64_t created_ms = 0;
    std::int64_t prompt_ready_ms = 0;
    std::int64_t enqueued_ms = 0;
    std::int64_t dequeued_ms = 0;
    std::int64_t worker_started_ms = 0;
    std::int64_t http_started_ms = 0;
    std::int64_t http_completed_ms = 0;
    std::int64_t parse_completed_ms = 0;
    std::int64_t completion_queued_ms = 0;
    std::int64_t main_thread_received_ms = 0;
    std::int64_t validation_completed_ms = 0;
    std::int64_t displayed_or_discarded_ms = 0;
    std::size_t prompt_bytes = 0;
    std::size_t system_bytes = 0;
    std::size_t queue_depth_at_enqueue = 0;

    int npc_x = 0;
    int npc_y = 0;
    int npc_z = 0;
    int player_x = 0;
    int player_y = 0;
    int player_z = 0;

    std::string prompt;
    std::string system_prompt;
    std::string player_line;
    std::string dialogue_language_code;
    std::string dialogue_language_name;
    std::string event_kind;
    std::string event_detail;
    std::string source_npc_name;
    std::string context_categories;
    std::string deduplication_key;
    std::string social_event_key;
    acquisition_intent acquisition = acquisition_intent::automatic;
    std::string acquisition_intent_source;
    int social_reaction_budget = 0;
    int social_group_size = 1;
    int combat_batch_retry_count = 0;
    std::vector<ai_combat_utterance_slot> combat_slots;
    std::vector<ai_target_snapshot> targets;
};

struct ai_request_completion {
    ai_request_snapshot request;
    ai_response response;
};

// Optional main-thread phase timings collected only while async diagnostics
// are enabled.  Completion handlers update the phases they can isolate
// cleanly; zero means that phase did not run in that handler.
struct ai_completion_apply_timings {
    std::uint64_t say_us = 0;
    std::uint64_t memory_us = 0;
    std::uint64_t npc_to_npc_schedule_us = 0;
};

struct ai_enqueue_result {
    bool accepted = false;
    std::uint64_t request_id = 0;
    std::string error;
};

using ai_request_executor =
    std::function<ai_response( const std::string &, const std::string & )>;
// Keeps focused tests concise while production always supplies both fields.
using ai_prompt_only_request_executor =
    std::function<ai_response( const std::string & )>;

// Thread-safe transport only.  It never reads or writes CDDA world state.
class ai_request_queue
{
    public:
        explicit ai_request_queue( ai_request_executor executor = ask_llm,
                                   bool start_automatically = true );
        explicit ai_request_queue( ai_prompt_only_request_executor executor,
                                   bool start_automatically = true );
        ~ai_request_queue();

        ai_request_queue( const ai_request_queue & ) = delete;
        ai_request_queue &operator=( const ai_request_queue & ) = delete;

        ai_enqueue_result enqueue( ai_request_snapshot request,
                                   bool supersede = false );
        std::vector<ai_request_completion> take_completions( std::size_t max_count );

        void start();
        void invalidate_session();
        void shutdown();

        std::uint64_t session_generation() const;
        std::size_t pending_count() const;
        std::size_t ready_completion_count() const;
        std::size_t pending_direct_count( int npc_id ) const;
        bool has_pending_key( const std::string &key ) const;
        bool npc_social_request_precedes_latest_direct_dialogue(
            const ai_request_snapshot &request ) const;

        bool wait_until_idle_for_test( std::chrono::milliseconds timeout );
        void set_executor_for_test( ai_request_executor executor,
                                    bool start_automatically = true );
        void set_executor_for_test( ai_prompt_only_request_executor executor,
                                    bool start_automatically = true );

    private:
        void worker_loop();
        bool pop_next_request( ai_request_snapshot &request );
        void clear_locked();

        mutable std::mutex mutex_;
        std::condition_variable work_available_;
        std::condition_variable idle_;
        std::deque<ai_request_snapshot> requests_[ai_request_priority_count];
        std::deque<ai_request_completion> completions_;
        std::unordered_map<std::string, std::uint64_t> pending_keys_;
        std::unordered_set<std::uint64_t> cancelled_ids_;
        std::unordered_map<int, std::size_t> pending_direct_by_npc_;
        struct social_budget_state {
            int created_turn = 0;
            std::unordered_set<int> speakers;
        };
        std::unordered_map<std::string, social_budget_state> social_budgets_;
        ai_request_executor executor_;
        std::thread worker_;
        std::uint64_t next_request_id_ = 1;
        std::uint64_t generation_ = 1;
        std::uint64_t latest_direct_dialogue_request_id_ = 0;
        std::size_t active_requests_ = 0;
        int high_priority_burst_ = 0;
        int completion_high_priority_burst_ = 0;
        bool start_automatically_ = true;
        bool started_ = false;
        bool stopping_ = false;
};

ai_request_queue &get_ai_request_queue();

std::uint64_t next_conversation_turn_id();
int social_reaction_budget_for_priority( int event_priority );
const char *conversation_origin_name( conversation_origin origin );
bool conversation_origin_allows_npc_reply( conversation_origin origin );

ai_enqueue_result
enqueue_direct_dialogue( const npc &who, const std::string &player_line,
                         const std::string &prompt,
                         std::uint64_t conversation_turn_id = 0 );
ai_enqueue_result
enqueue_group_dialogue( const npc &who, const std::string &player_line,
                        const std::string &prompt,
                        std::uint64_t conversation_turn_id );
ai_enqueue_result enqueue_spontaneous_dialogue( const npc &who,
        const std::string &prompt,
        const std::string &event_kind,
        const std::string &event_detail,
        int event_priority,
        bool danger_at_creation );
ai_enqueue_result
enqueue_npc_reply_dialogue( const npc &listener, const npc &speaker,
                            const std::string &spoken, const std::string &prompt,
                            bool danger_at_creation,
                            std::uint64_t conversation_id, int reply_depth );
ai_enqueue_result enqueue_combat_social_dialogue(
    const npc &who, const std::string &prompt, int combat_event_type,
    const std::string &event_kind, const std::string &event_detail,
    int event_priority, bool danger_at_creation,
    std::uint64_t encounter_generation, std::uint64_t event_generation,
    int target_id, const std::string &target_name,
    std::vector<ai_target_snapshot> visible_targets,
    const std::string &social_event_key, int social_group_size, bool supersede );
ai_enqueue_result enqueue_combat_social_batch_dialogue(
    const npc &coordinator, const std::string &prompt,
    const std::string &system_prompt, int event_priority,
    bool danger_at_creation, std::uint64_t encounter_generation,
    std::vector<ai_combat_utterance_slot> slots,
    const std::string &batch_key, int retry_count = 0 );
ai_enqueue_result enqueue_legacy_ai_prompt( const std::string &prompt );
ai_enqueue_result enqueue_language_retry( const ai_request_snapshot &request );
ai_enqueue_result enqueue_command_resolution(
    const npc &who, ai_request_type type, const std::string &player_line,
    const std::string &prompt, std::vector<ai_target_snapshot> targets = {},
    acquisition_intent acquisition = acquisition_intent::automatic,
    const std::string &acquisition_intent_source = "",
    const std::string &event_detail = "" );

// Delivers a reply to an explicit player order immediately through CDDA's
// normal speech/hearing channel.  Async completions arrive after the regular
// sound-marker pass for the current input cycle, so leaving them queued until
// the next cycle can make a failed order appear to receive no answer.
void say_command_reply( npc &who, const std::string &message );

// Every line produced by the NPC AI goes through here so it is recognisable
// in the message log: speaker name in green, words in cyan.  Vanilla chatter
// keeps the default colour.
void say_ai_line( const npc &who, const std::string &text,
                  sounds::sound_t priority = sounds::sound_t::speech );

// Must only be called by the main game thread.
void process_ai_completions();
void begin_ai_session();
void end_ai_session();

// Clears every process-wide per-NPC AI map.  Exposed so tests can assert that
// no module keeps state across a world change.
void reset_all_ai_session_state();
void shutdown_ai_requests();

void set_ai_request_executor_for_test( ai_request_executor executor,
                                       bool start_automatically = true );
void set_ai_request_executor_for_test( ai_prompt_only_request_executor executor,
                                       bool start_automatically = true );
using ai_completion_clock =
    std::function<std::chrono::steady_clock::time_point()>;
void set_ai_completion_clock_for_test( ai_completion_clock clock );
void reset_ai_completion_clock_for_test();
void reset_ai_request_system_for_test();

} // namespace npc_ai

#endif // CATA_SRC_NPC_AI_ASYNC_H
