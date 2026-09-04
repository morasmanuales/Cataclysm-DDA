#pragma once
#ifndef CATA_SRC_NPC_AI_CONTEXT_H
#define CATA_SRC_NPC_AI_CONTEXT_H

#include <cstddef>
#include <string>

class npc;

namespace npc_ai
{

enum class context_intent : int {
    greeting,
    self_state,
    self_inventory,
    perception_brief,
    perception_detailed,
    current_situation,
    memory,
    watch,
    spontaneous,
    npc_social,
    general
};

// Selects the stable system contract while keeping world state and the current
// interaction in the user prompt.
enum class npc_prompt_purpose : int {
    direct_dialogue,
    spontaneous_dialogue,
    npc_to_npc_reply,
    combat_social,
    watch_resolution,
    pickup_resolution,
    wield_resolution,
    order_resolution
};

context_intent classify_context_intent( const std::string &player_line );
const char *context_intent_name( context_intent intent );
std::size_t context_prompt_budget_bytes( context_intent intent );

bool is_current_sensory_query( const std::string &player_line );
bool is_current_self_query( const std::string &player_line );
bool is_scene_inspection_query( const std::string &player_line );

// Language used by the running game for visible AI-generated dialogue.
std::string current_dialogue_language_code();
std::string current_dialogue_language_name();
bool current_dialogue_language_is_spanish();
std::string localized_ai_message( const std::string &translated_message,
                                  const std::string &spanish_fallback );
bool generated_text_matches_dialogue_language( const std::string &text,
        const std::string &language_code );
std::string dialogue_language_fallback( const std::string &event_kind,
                                        bool combat_active );
std::string
dialogue_language_retry_instruction( const std::string &language_code,
                                     const std::string &language_name );

std::string build_npc_system_prompt(
    const npc &who,
    npc_prompt_purpose purpose = npc_prompt_purpose::direct_dialogue );

// reserved_suffix_bytes: room the caller will append after this prompt (for
// example the spontaneous weather/event block).  It is subtracted from the
// routed budget so the final prompt plus system prompt still fits the hard
// transport context, instead of being rejected at enqueue time.
std::string build_npc_prompt(
    const npc &who, const std::string &player_line,
    npc_prompt_purpose purpose = npc_prompt_purpose::direct_dialogue,
    std::size_t reserved_suffix_bytes = 0 );

} // namespace npc_ai

#endif // CATA_SRC_NPC_AI_CONTEXT_H
