#include "npc_ai_spontaneous.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "calendar.h"
#include "game.h"
#include "item.h"
#include "item_location.h"
#include "map.h"
#include "npc.h"
#include "npc_ai_async.h"
#include "npc_ai_combat_social.h"
#include "npc_ai_context.h"
#include "npc_ai_debug.h"
#include "npc_ai_memory.h"
#include "npc_ai_profiler.h"
#include "omdata.h"
#include "overmapbuffer.h"
#include "output.h"
#include "point.h"
#include "rng.h"
#include "type_id.h"
#include "weather.h"


namespace
{


// ============================================================
// CONFIG
// ============================================================

// How often the engine may inspect for new speech-worthy events.
constexpr int evaluation_interval_turns = 20;

// Global NPC chatter protection.
constexpr int global_speech_gap_turns = 60;

// Darkness heuristic.
// The exact value is deliberately conservative;
// the actual light number is also sent to Qwen.
constexpr float dark_light_threshold = 3.5f;

// NPC must be reasonably near the player to chat with them.
constexpr int max_talk_distance = 16;


static const efftype_id effect_cold(
    "cold"
);

static const efftype_id effect_hot(
    "hot"
);


// ============================================================
// STATE
// ============================================================

struct npc_snapshot {
    std::string location_key;
    std::string location_name;

    bool outside = false;

    float ambient_light = 0.0f;
    bool dark = false;

    std::string weather_id;
    std::string temperature;

    int windspeed = 0;

    bool can_see_sky = false;
    bool night = false;

    moon_phase moon = MOON_PHASE_MAX;

    int cold = 0;
    int hot = 0;

    int hunger = 0;
    int thirst = 0;
    int sleepiness = 0;

    int pain = 0;
    int morale = 0;

    int hp_percent = 100;
    int stamina_percent = 100;

    bool combat = false;

    std::string activity;
    std::string weapon;
};


struct spontaneous_state {
    bool initialized = false;

    npc_snapshot last;

    int last_evaluation_turn = -1000000000;
    int last_model_call_turn = -1000000000;
    int last_spoken_turn = -1000000000;

    int next_ambient_turn = 0;
    std::uint64_t pending_request_id = 0;
};


struct speech_event {
    std::string kind;
    std::string detail;
    int priority = 0;
};


std::unordered_map<int, spontaneous_state>
spontaneous_states;


int global_last_spoken_turn =
    -1000000000;


// ============================================================
// HELPERS
// ============================================================

int npc_key(
    const npc &who
)
{
    return
        who.getID().get_value();
}


int current_turn_number()
{
    return
        to_turn<int>(
            calendar::turn
        );
}


int minutes_to_turns(
    const int minutes
)
{
    return
        to_turns<int>(
            time_duration::from_minutes(
                minutes
            )
        );
}


std::string trim_copy(
    std::string value
)
{
    const auto not_space =
        []( const unsigned char c ) {
            return
                !std::isspace(
                    c
                );
        };


    value.erase(
        value.begin(),
        std::find_if(
            value.begin(),
            value.end(),
            not_space
        )
    );


    value.erase(
        std::find_if(
            value.rbegin(),
            value.rend(),
            not_space
        ).base(),
        value.end()
    );


    return value;
}


std::string lower_ascii(
    std::string text
)
{
    for(
        char &c :
        text
    ) {

        if(
            c >= 'A' &&
            c <= 'Z'
        ) {

            c =
                static_cast<char>(
                    c - 'A' + 'a'
                );
        }
    }


    return text;
}


std::string one_line(
    std::string text
)
{
    for(
        char &c :
        text
    ) {

        if(
            c == '\r' ||
            c == '\n' ||
            c == '\t'
        ) {

            c =
                ' ';
        }
    }


    while(
        text.find(
            "  "
        ) !=
        std::string::npos
    ) {

        text.replace(
            text.find(
                "  "
            ),
            2,
            " "
        );
    }


    return
        trim_copy(
            text
        );
}


void debug_line(
    const std::string &line
)
{
    npc_ai::append_debug_line( "npc_ai_spontaneous_runtime.txt", line );
}


std::string moon_name(
    const moon_phase phase
)
{
    switch(
        phase
    ) {

        case MOON_NEW:
            return "luna nueva";

        case MOON_WAXING_CRESCENT:
            return "creciente";

        case MOON_HALF_MOON_WAXING:
            return "media luna creciente";

        case MOON_WAXING_GIBBOUS:
            return "gibosa creciente";

        case MOON_FULL:
            return "luna llena";

        case MOON_WANING_GIBBOUS:
            return "gibosa menguante";

        case MOON_HALF_MOON_WANING:
            return "media luna menguante";

        case MOON_WANING_CRESCENT:
            return "menguante";

        case MOON_PHASE_MAX:
            break;
    }


    return "desconocida";
}


std::string bool_word(
    const bool value
)
{
    return
        value ?
        "si" :
        "no";
}


// ============================================================
// REAL GAME SNAPSHOT
// ============================================================

npc_snapshot build_snapshot(
    npc &who
)
{
    npc_snapshot result;


    map &here =
        get_map();


    const tripoint_bub_ms pos =
        who.pos_bub(
            here
        );


    const tripoint_abs_omt omt =
        who.pos_abs_omt();


    const oter_id &oter =
        overmap_buffer.ter(
            omt
        );


    result.location_name =
        oter->get_name(
            om_vision_level::full
        );


    {
        std::ostringstream key;

        key
            << omt.x()
            << ","
            << omt.y()
            << ","
            << omt.z()
            << ":"
            << result.location_name;

        result.location_key =
            key.str();
    }


    result.outside =
        here.is_outside(
            pos
        );


    result.ambient_light =
        here.ambient_light_at(
            pos
        );


    result.dark =
        result.ambient_light <
        dark_light_threshold;


    weather_manager &weather =
        get_weather();


    result.weather_id =
        weather.weather_id.str();


    const units::temperature local_temperature =
        weather.get_temperature(
            pos
        );


    result.temperature =
        print_temperature(
            local_temperature,
            1
        );


    result.windspeed =
        weather.windspeed;


    result.can_see_sky =
        can_creature_see_sky(
            who
        );


    result.night =
        is_night(
            calendar::turn
        );


    result.moon =
        get_moon_phase(
            calendar::turn
        );


    result.cold =
        who.get_effect_int(
            effect_cold
        );


    result.hot =
        who.get_effect_int(
            effect_hot
        );


    result.hunger =
        who.get_hunger();


    result.thirst =
        who.get_thirst();


    result.sleepiness =
        who.get_sleepiness();


    result.pain =
        who.get_perceived_pain();


    result.morale =
        who.get_morale_level();


    result.hp_percent =
        who.hp_percentage();


    const int stamina_max =
        who.get_stamina_max();


    if(
        stamina_max > 0
    ) {

        result.stamina_percent =
            who.get_stamina() *
            100 /
            stamina_max;
    }


    result.combat =
        static_cast<bool>(
            who.get_current_attack()
        );


    result.activity =
        who.current_activity_id.str();


    const item_location wielded =
        who.get_wielded_item();


    if(
        wielded
    ) {

        result.weapon =
            remove_color_tags( wielded->tname() );
    }
    else {

        result.weapon =
            "ninguna";
    }


    return result;
}


// ============================================================
// EVENTS
// ============================================================

void add_event(
    std::vector<speech_event> &events,
    const std::string &kind,
    const std::string &detail,
    const int priority
)
{
    speech_event event;

    event.kind =
        kind;

    event.detail =
        detail;

    event.priority =
        priority;


    events.push_back(
        event
    );
}


std::vector<speech_event> detect_events(
    const npc_snapshot &before,
    const npc_snapshot &now,
    const bool ambient_due
)
{
    std::vector<speech_event>
    events;


    if(
        now.location_key !=
        before.location_key
    ) {

        add_event(
            events,
            "LOCATION_CHANGED",
            std::string(
                "Entramos o nos desplazamos a: "
            ) +
            now.location_name,
            now.dark ?
            85 :
            65
        );
    }


    if(
        before.outside &&
        !now.outside
    ) {

        add_event(
            events,
            "ENTERED_INTERIOR",
            now.dark ?
            "Acabamos de entrar a un interior oscuro." :
            "Acabamos de entrar a un interior.",
            now.dark ?
            90 :
            70
        );
    }


    if(
        !before.dark &&
        now.dark
    ) {

        add_event(
            events,
            "DARKNESS",
            "La zona actual se ha vuelto oscura o entramos a una zona con poca luz.",
            80
        );
    }


    if(
        now.weather_id !=
        before.weather_id
    ) {

        add_event(
            events,
            "WEATHER_CHANGED",
            std::string(
                "Cambio el clima de "
            ) +
            before.weather_id +
            " a " +
            now.weather_id +
            ".",
            50
        );
    }


    if(
        now.windspeed >= 20 &&
        before.windspeed < 20
    ) {

        add_event(
            events,
            "STRONG_WIND",
            std::string(
                "El viento aumento a "
            ) +
            std::to_string(
                now.windspeed
            ) +
            " mph.",
            now.cold > 0 ?
            85 :
            55
        );
    }


    if(
        now.cold >
        before.cold
    ) {

        add_event(
            events,
            "GETTING_COLD",
            std::string(
                "El NPC esta sintiendo mas frio. Intensidad de frio="
            ) +
            std::to_string(
                now.cold
            ) +
            ".",
            now.cold >= 2 ?
            95 :
            85
        );
    }


    if(
        now.hot >
        before.hot
    ) {

        add_event(
            events,
            "GETTING_HOT",
            std::string(
                "El NPC esta sintiendo mas calor. Intensidad de calor="
            ) +
            std::to_string(
                now.hot
            ) +
            ".",
            now.hot >= 2 ?
            90 :
            80
        );
    }


    if(
        now.pain >=
        before.pain + 5
    ) {

        add_event(
            events,
            "PAIN_INCREASED",
            std::string(
                "El dolor aumento a "
            ) +
            std::to_string(
                now.pain
            ) +
            ".",
            90
        );
    }


    if(
        now.hp_percent <= 60 &&
        before.hp_percent > 60
    ) {

        add_event(
            events,
            "INJURED",
            std::string(
                "La salud bajo a "
            ) +
            std::to_string(
                now.hp_percent
            ) +
            " por ciento.",
            90
        );
    }


    if(
        now.thirst >= 100 &&
        before.thirst < 100
    ) {

        add_event(
            events,
            "THIRSTY",
            std::string(
                "La sed alcanzo "
            ) +
            std::to_string(
                now.thirst
            ) +
            ".",
            65
        );
    }


    if(
        now.hunger >= 100 &&
        before.hunger < 100
    ) {

        add_event(
            events,
            "HUNGRY",
            std::string(
                "El hambre alcanzo "
            ) +
            std::to_string(
                now.hunger
            ) +
            ".",
            55
        );
    }


    if(
        now.sleepiness >= 300 &&
        before.sleepiness < 300
    ) {

        add_event(
            events,
            "TIRED",
            std::string(
                "El cansancio/somnolencia alcanzo "
            ) +
            std::to_string(
                now.sleepiness
            ) +
            ".",
            55
        );
    }


    if(
        now.stamina_percent <= 30 &&
        before.stamina_percent > 30
    ) {

        add_event(
            events,
            "LOW_STAMINA",
            std::string(
                "La stamina bajo a "
            ) +
            std::to_string(
                now.stamina_percent
            ) +
            " por ciento.",
            70
        );
    }


    if(
        now.morale <= -20 &&
        before.morale > -20
    ) {

        add_event(
            events,
            "LOW_MORALE",
            std::string(
                "La moral bajo a "
            ) +
            std::to_string(
                now.morale
            ) +
            ".",
            55
        );
    }


    if(
        now.night &&
        now.can_see_sky &&
        now.moon ==
        MOON_FULL &&
        (
            before.moon !=
            MOON_FULL ||
            !before.can_see_sky ||
            !before.night
        )
    ) {

        add_event(
            events,
            "FULL_MOON_VISIBLE",
            "Es de noche, el NPC puede ver el cielo y la fase lunar real es luna llena.",
            35
        );
    }


    if(
        now.activity !=
        before.activity &&
        !now.activity.empty()
    ) {

        add_event(
            events,
            "ACTIVITY_CHANGED",
            std::string(
                "Cambio la actividad actual del NPC a "
            ) +
            now.activity +
            ".",
            40
        );
    }


    if(
        ambient_due
    ) {

        add_event(
            events,
            "QUIET_MOMENT",
            "Ha pasado bastante tiempo sin que el motor proponga una conversacion espontanea. Evalua si este momento merece una observacion, una broma, una pregunta o simplemente silencio.",
            15
        );
    }


    return events;
}


// ============================================================
// EVENT MERGE
// ============================================================

speech_event merge_events(
    const std::vector<speech_event> &events
)
{
    speech_event result;


    if(
        events.empty()
    ) {

        return result;
    }


    std::size_t best_index =
        0;


    for(
        std::size_t i = 1;
        i < events.size();
        ++i
    ) {

        if(
            events[i].priority >
            events[best_index].priority
        ) {

            best_index =
                i;
        }
    }


    result.kind =
        events[best_index].kind;

    result.priority =
        events[best_index].priority;


    std::ostringstream details;


    for(
        std::size_t i = 0;
        i < events.size();
        ++i
    ) {

        if(
            i > 0
        ) {

            details
                << " ";
        }


        details
            << "["
            << events[i].kind
            << "] "
            << events[i].detail;
    }


    result.detail =
        details.str();


    return result;
}


// ============================================================
// MODEL RESPONSE PARSING
// ============================================================

bool model_chose_silence(
    const std::string &raw
)
{
    const std::string lower =
        lower_ascii(
            raw
        );


    if(
        lower.find(
            "decision=silent"
        ) !=
        std::string::npos
    ) {

        return true;
    }


    const std::string trimmed =
        trim_copy(
            lower
        );


    return
        trimmed ==
        "silent" ||
        trimmed ==
        "silencio";
}


struct text_field_extraction {
    bool found = false;
    std::string value;
};

text_field_extraction extract_text_field(
    const std::string &raw
)
{
    std::istringstream input(
        raw
    );


    std::string line;


    while(
        std::getline(
            input,
            line
        )
    ) {

        const std::string trimmed =
            trim_copy(
                line
            );


        const std::string lower =
            lower_ascii(
                trimmed
            );


        std::size_t search_from = 0;
        while( true ) {
            const std::size_t field_pos = lower.find( "text", search_from );
            if( field_pos == std::string::npos ) {
                break;
            }

            const bool starts_field = field_pos == 0 ||
                                      trimmed[field_pos - 1] == ',' ||
                                      std::isspace( static_cast<unsigned char>(
                                              trimmed[field_pos - 1] ) );
            std::size_t marker_pos = field_pos + 4;
            while( marker_pos < trimmed.size() &&
                   std::isspace( static_cast<unsigned char>( trimmed[marker_pos] ) ) ) {
                ++marker_pos;
            }
            if( starts_field && marker_pos < trimmed.size() &&
                ( trimmed[marker_pos] == '=' || trimmed[marker_pos] == ':' ) ) {
                return { true, trim_copy( trimmed.substr( marker_pos + 1 ) ) };
            }
            search_from = field_pos + 4;
        }
    }

    return {};
}


std::string clean_fallback_response(
    const std::string &raw
)
{
    std::istringstream input(
        raw
    );


    std::ostringstream output;

    std::string line;

    bool first =
        true;


    while(
        std::getline(
            input,
            line
        )
    ) {

        const std::string trimmed =
            trim_copy(
                line
            );


        if(
            trimmed.empty()
        ) {

            continue;
        }


        const std::string lower =
            lower_ascii(
                trimmed
            );


        if(
            lower.find(
                "decision="
            ) == 0 ||
            lower.find(
                "type="
            ) == 0 ||
            lower.find(
                "question="
            ) == 0
        ) {

            continue;
        }


        if(
            !first
        ) {

            output
                << " ";
        }


        output
            << trimmed;


        first =
            false;
    }


    return
        trim_copy(
            output.str()
        );
}


std::string sanitize_spoken_text(
    std::string text
)
{
    text =
        one_line(
            text
        );


    if(
        text.size() >= 2 &&
        (
            (
                text.front() == '"' &&
                text.back() == '"'
            ) ||
            (
                text.front() == '\'' &&
                text.back() == '\''
            )
        )
    ) {

        text =
            text.substr(
                1,
                text.size() - 2
            );
    }


    // Safety against an unexpectedly huge monologue.
    if(
        text.size() > 600
    ) {

        text.resize(
            600
        );
    }


    return
        trim_copy(
            text
        );
}


// ============================================================
// PROMPT
// ============================================================

std::string build_spontaneous_prompt(
    npc &who,
    const npc_snapshot &snapshot,
    const speech_event &event
)
{
    npc_ai::scoped_profile profile( npc_ai::profile_subsystem::async_preparation );
    const std::string synthetic_line =
        "[EVALUACION INTERNA DE HABLA ESPONTANEA] "
        "No es una frase pronunciada por el jugador. "
        "Evalua el momento actual, la conversacion reciente, "
        "tu memoria y lo que realmente percibes.";


    // The trailing block below is built first so its exact size can be
    // reserved inside the routed budget.  Otherwise a scene-heavy sensory
    // context fills the budget and this suffix pushes the prompt past the hard
    // transport limit, which the queue rejects at enqueue time.
    std::ostringstream extra;


    extra
        << "\n\n"
        << "ATRIBUTOS ACTUALES:\n"
        << "fuerza="
        << who.get_str()
        << "\n"
        << "destreza="
        << who.get_dex()
        << "\n"
        << "inteligencia="
        << who.get_int()
        << "\n"
        << "percepcion="
        << who.get_per()
        << "\n"
        << "\n"
        << "ESTADO FISICO Y EMOCIONAL REAL:\n"
        << "hambre="
        << snapshot.hunger
        << "\n"
        << "sed="
        << snapshot.thirst
        << "\n"
        << "somnolencia="
        << snapshot.sleepiness
        << "\n"
        << "dolor="
        << snapshot.pain
        << "\n"
        << "moral="
        << snapshot.morale
        << "\n"
        << "salud_porcentaje="
        << snapshot.hp_percent
        << "\n"
        << "stamina_porcentaje="
        << snapshot.stamina_percent
        << "\n"
        << "frio_intensidad="
        << snapshot.cold
        << "\n"
        << "calor_intensidad="
        << snapshot.hot
        << "\n"
        << "\n"
        << "ENTORNO REAL:\n"
        << "ubicacion="
        << snapshot.location_name
        << "\n"
        << "exterior="
        << bool_word(
               snapshot.outside
           )
        << "\n"
        << "luz_ambiente="
        << snapshot.ambient_light
        << "\n"
        << "oscuro="
        << bool_word(
               snapshot.dark
           )
        << "\n"
        << "clima_id="
        << snapshot.weather_id
        << "\n"
        << "temperatura="
        << snapshot.temperature
        << "\n"
        << "viento_mph="
        << snapshot.windspeed
        << "\n"
        << "puede_ver_cielo="
        << bool_word(
               snapshot.can_see_sky
           )
        << "\n"
        << "es_de_noche="
        << bool_word(
               snapshot.night
           )
        << "\n"
        << "fase_lunar="
        << moon_name(
               snapshot.moon
           )
        << "\n"
        << "\n"
        << "SITUACION:\n"
        << "combate_o_accion_de_combate="
        << bool_word(
               snapshot.combat
           )
        << "\n"
        << "actividad_id="
        << snapshot.activity
        << "\n"
        << "arma_en_manos="
        << snapshot.weapon
        << "\n"
        << "\n"
        << "EVENTO QUE ACTIVO ESTA EVALUACION:\n"
        << "tipo="
        << event.kind
        << "\n"
        << "prioridad="
        << event.priority
        << "\n"
        << "detalle="
        << event.detail
        << "\n\n"
        // Live scenario run 2026-09-03: "dolor=10" became "me duele mucho la
        // cabeza".  Pain here is a whole-body scalar; the model must not
        // place it or scale it beyond the number.
        << "REGLAS DEL EVENTO:\n"
        << "- dolor es un valor global sin parte del cuerpo: no digas donde te duele.\n"
        << "- La intensidad debe ser proporcional al valor: dolor<15 es leve, 15-40 "
        "moderado, >40 fuerte.\n"
        << "- No inventes la causa del evento ni detalles que no esten en los datos.\n"
        << "\nOUTPUT_LANGUAGE=" << npc_ai::current_dialogue_language_name() << "\n";


    const std::string extra_text = extra.str();
    std::string prompt =
        npc_ai::build_npc_prompt(
            who,
            synthetic_line,
            npc_ai::npc_prompt_purpose::spontaneous_dialogue,
            extra_text.size()
        );
    prompt += extra_text;


    return prompt;
}


// ============================================================
// SPEECH COOLDOWN
// ============================================================

int speech_gap_for_priority(
    const int priority
)
{
    if(
        priority >= 90
    ) {

        return
            minutes_to_turns(
                2
            );
    }


    if(
        priority >= 70
    ) {

        return
            minutes_to_turns(
                4
            );
    }


    if(
        priority >= 40
    ) {

        return
            minutes_to_turns(
                8
            );
    }


    return
        minutes_to_turns(
            15
        );
}


void schedule_next_ambient(
    spontaneous_state &state,
    const int now
)
{
    state.next_ambient_turn =
        now +
        minutes_to_turns(
            rng(
                15,
                35
            )
        );
}


} // namespace


// ============================================================
// PUBLIC
// ============================================================

namespace npc_ai
{


spontaneous_response_parse_result parse_spontaneous_response( const std::string &raw )
{
    spontaneous_response_parse_result result;
    if( model_chose_silence( raw ) ) {
        result.decision = spontaneous_response_decision::silent;
        return result;
    }

    const text_field_extraction field = extract_text_field( raw );
    std::string candidate;
    if( field.found ) {
        candidate = field.value;
        if( candidate.empty() ) {
            result.empty_reason = "MODEL_EMPTY";
            return result;
        }
    } else {
        candidate = clean_fallback_response( raw );
        if( candidate.empty() ) {
            result.empty_reason = trim_copy( raw ).empty() ? "MODEL_EMPTY" : "NO_TEXT_FIELD";
            return result;
        }
    }

    result.text = sanitize_spoken_text( candidate );
    if( result.text.empty() ) {
        result.empty_reason = "SANITIZED_TO_EMPTY";
        return result;
    }
    result.decision = spontaneous_response_decision::talk;
    return result;
}


void process_spontaneous_speech(
    npc &who
)
{
    scoped_profile profile( profile_subsystem::spontaneous );
    // Only real allied companions.
    if(
        !who.is_player_ally() ||
        who.is_dead_state() ||
        who.in_sleep_state()
    ) {

        return;
    }


    map &here =
        get_map();


    Character &player =
        get_player_character();


    const tripoint_bub_ms npc_pos =
        who.pos_bub(
            here
        );


    const tripoint_bub_ms player_pos =
        player.pos_bub(
            here
        );


    // Don't have conversations with the player from another floor
    // or from very far away.
    if(
        npc_pos.z() !=
        player_pos.z() ||
        rl_dist(
            npc_pos,
            player_pos
        ) >
        max_talk_distance
    ) {

        return;
    }


    const int now =
        current_turn_number();


    spontaneous_state &state =
        spontaneous_states[
            npc_key(
                who
            )
        ];


    // --------------------------------------------------------
    // INITIAL SNAPSHOT
    // --------------------------------------------------------

    if(
        !state.initialized
    ) {

        state.initialized =
            true;

        state.last =
            build_snapshot(
                who
            );

        state.last_evaluation_turn =
            now;

        state.last_model_call_turn =
            now -
            minutes_to_turns(
                60
            );

        state.last_spoken_turn =
            now -
            minutes_to_turns(
                60
            );


        schedule_next_ambient(
            state,
            now
        );


        if( npc_ai::runtime_debug_enabled() ) {

            std::ostringstream debug;

            debug
                << "INIT NPC="
                << who.get_name()
                << " | location="
                << state.last.location_name
                << " | next_ambient_turn="
                << state.next_ambient_turn;


            debug_line(
                debug.str()
            );
        }


        return;
    }


    // --------------------------------------------------------
    // VERY CHEAP RATE LIMIT
    // --------------------------------------------------------

    if(
        now -
        state.last_evaluation_turn <
        evaluation_interval_turns
    ) {

        return;
    }


    state.last_evaluation_turn =
        now;


    const npc_snapshot current =
        build_snapshot(
            who
        );


    const bool ambient_due =
        now >=
        state.next_ambient_turn;


    const std::vector<speech_event>
    events =
        detect_events(
            state.last,
            current,
            ambient_due
        );


    // Update reality immediately.
    // This prevents the same event from triggering every tick.
    state.last =
        current;

    // Combat Life owns combat speech and has event/encounter-aware cooldowns.
    // Keep this broader ambient system from submitting a competing request.
    if( current.combat ) {
        return;
    }


    if(
        ambient_due
    ) {

        schedule_next_ambient(
            state,
            now
        );
    }


    if(
        events.empty()
    ) {

        return;
    }


    const speech_event event =
        merge_events(
            events
        );


    // --------------------------------------------------------
    // SPEECH SPAM PROTECTION
    // --------------------------------------------------------

    const int required_gap =
        speech_gap_for_priority(
            event.priority
        );


    if(
        now -
        state.last_spoken_turn <
        required_gap
    ) {

        if( npc_ai::runtime_debug_enabled() ) {

            std::ostringstream debug;

            debug
                << "SUPPRESSED_COOLDOWN NPC="
                << who.get_name()
                << " | event="
                << event.kind
                << " | priority="
                << event.priority;


            debug_line(
                debug.str()
            );
        }


        return;
    }


    if(
        now -
        global_last_spoken_turn <
        global_speech_gap_turns
    ) {

        debug_line(
            std::string(
                "SUPPRESSED_GLOBAL_CHATTER NPC="
            ) +
            who.get_name()
        );


        return;
    }


    // Don't block the game repeatedly if multiple events happen
    // immediately after one model request.
    if(
        now -
        state.last_model_call_turn <
        45
    ) {

        return;
    }


    // --------------------------------------------------------
    // BUILD REAL GROUNDED PROMPT
    // --------------------------------------------------------

    const std::string prompt =
        build_spontaneous_prompt(
            who,
            current,
            event
        );


    if( npc_ai::runtime_debug_enabled() ) {
        std::ostringstream debug;

        debug
            << "EVALUATE NPC="
            << who.get_name()
            << " | event="
            << event.kind
            << " | priority="
            << event.priority
            << " | location="
            << current.location_name
            << " | weather="
            << current.weather_id
            << " | temp="
            << current.temperature
            << " | wind="
            << current.windspeed
            << " | dark="
            << bool_word(
                   current.dark
               )
            << " | cold="
            << current.cold
            << " | combat="
            << bool_word(
                   current.combat
               );


        debug_line(
            debug.str()
        );
    }


    const ai_enqueue_result queued = enqueue_spontaneous_dialogue(
            who, prompt, event.kind, event.detail, event.priority, current.combat );
    if( queued.accepted ) {
        state.last_model_call_turn = now;
        state.pending_request_id = queued.request_id;
        debug_line( std::string( "ENQUEUED NPC=" ) + who.get_name() + " | event=" + event.kind );
    } else {
        // The queue rejects for several reasons (pending duplicate, context
        // budget, shutting down); surface the exact one instead of guessing.
        debug_line( std::string( "SUPPRESSED_PENDING NPC=" ) + who.get_name() +
                    " | event=" + event.kind + " | reason=" + queued.error +
                    " | prompt_bytes=" + std::to_string( prompt.size() ) );
        if( runtime_debug_enabled() ) {
            debug_stream rejected( "npc_ai_spontaneous_rejected_prompt.txt", true );
            if( rejected ) {
                rejected << prompt;
            }
        }
    }
}

namespace
{

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

} // namespace

void apply_spontaneous_ai_completion( npc &who, const ai_request_completion &completion,
                                      ai_completion_apply_timings *timings )
{
    const ai_request_snapshot &request = completion.request;
    spontaneous_state &state = spontaneous_states[npc_key( who )];
    if( state.pending_request_id != request.id ) {
        return;
    }
    state.pending_request_id = 0;

    if( get_ai_request_queue().npc_social_request_precedes_latest_direct_dialogue( request ) ) {
        debug_line( std::string( "SUPPRESSED_NPC_REPLY NPC=" ) + who.get_name() +
                    " origin=" + conversation_origin_name( request.origin ) +
                    " reason=direct_player_dialogue_started" );
        return;
    }

    const int now = current_turn_number();
    map &here = get_map();
    const Character &player = get_player_character();
    const npc_snapshot current = build_snapshot( who );
    const bool stale = !who.is_player_ally() || who.in_sleep_state() ||
                       now - request.created_turn > minutes_to_turns( 5 ) ||
                       who.posz() != player.posz() ||
                       rl_dist( who.pos_bub( here ), player.pos_bub( here ) ) > max_talk_distance ||
                       current.combat != request.danger_at_creation ||
                       ( request.reply_depth == 0 &&
                         now - global_last_spoken_turn < global_speech_gap_turns );
    if( stale ) {
        debug_line( std::string( "DISCARDED_STALE NPC=" ) + who.get_name() +
                    " | event=" + request.event_kind );
        return;
    }

    const ai_response &ai = completion.response;
    if( !ai.success ) {
        debug_line( std::string( "OLLAMA_ERROR NPC=" ) + who.get_name() );
        return;
    }
    debug_line( std::string( "OLLAMA_RAW=" ) + one_line( ai.text ) );
    const spontaneous_response_parse_result parsed = parse_spontaneous_response( ai.text );
    if( parsed.decision == spontaneous_response_decision::silent ) {
        debug_line( std::string( "DECISION=SILENT NPC=" ) + who.get_name() +
                    " | event=" + request.event_kind );
        return;
    }
    if( parsed.decision == spontaneous_response_decision::empty_text ) {
        debug_line( "DECISION=EMPTY_TEXT reason=" + parsed.empty_reason );
        return;
    }
    std::string spoken = parsed.text;
    const std::string language_code = request.dialogue_language_code.empty() ?
                                      current_dialogue_language_code() :
                                      request.dialogue_language_code;
    if( !generated_text_matches_dialogue_language( spoken, language_code ) ) {
        const ai_enqueue_result retry = enqueue_language_retry( request );
        if( retry.accepted ) {
            state.pending_request_id = retry.request_id;
            debug_line( std::string( "RETRY_LANGUAGE NPC=" ) + who.get_name() +
                        " | event=" + request.event_kind );
            return;
        }
        spoken = dialogue_language_fallback( request.event_kind, current.combat );
    }
    if( combat_social_text_has_unconfirmed_tactical_promise( spoken ) ||
        recent_speech_is_duplicate( who, spoken ) ) {
        debug_line( std::string( "DISCARDED_UNGROUNDED_OR_REPEATED NPC=" ) + who.get_name() );
        return;
    }

    measure_completion_phase( timings == nullptr ? nullptr : &timings->say_us, [&]() {
        say_ai_line( who, spoken );
    } );
    state.last_spoken_turn = now;
    global_last_spoken_turn = now;
    measure_completion_phase( timings == nullptr ? nullptr : &timings->memory_us, [&]() {
        remember_recent_speech( who, spoken, request.event_kind );
        remember_exchange( who, "[EVENTO ESPONTANEO " + request.event_kind + "] " +
                           request.event_detail, spoken );
    } );
    measure_completion_phase( timings == nullptr ? nullptr :
                              &timings->npc_to_npc_schedule_us, [&]() {
        maybe_enqueue_npc_reply( who, spoken, request.origin,
                                 request.conversation_id != 0 ? request.conversation_id : request.id,
                                 request.reply_depth );
    } );
    debug_line( std::string( "DECISION=TALK NPC=" ) + who.get_name() +
                " | event=" + request.event_kind + " | text=" + one_line( spoken ) );
    schedule_next_ambient( state, now );
}

bool maybe_enqueue_npc_reply( const npc &speaker, const std::string &spoken,
                              const conversation_origin origin,
                              const std::uint64_t conversation_id,
                              const int current_reply_depth )
{
    scoped_profile profile( profile_subsystem::npc_to_npc );
    constexpr int max_reply_depth = 1;
    const auto log_evaluation = [&]( const std::size_t candidates, const int selected,
                                     const char *suppressed_reason ) {
        if( !runtime_debug_enabled() ) {
            return;
        }
        std::ostringstream line;
        line << "NPC_TO_NPC_EVAL source_npc=" << speaker.getID().get_value()
             << " candidates=" << candidates << " selected=" << selected
             << " reply_depth=" << current_reply_depth << " suppressed_reason="
             << suppressed_reason;
        debug_line( line.str() );
    };
    if( !conversation_origin_allows_npc_reply( origin ) ) {
        log_evaluation( 0, -1, "origin_disallows_reply" );
        debug_line( std::string( "SUPPRESSED_NPC_REPLY origin=" ) +
                    conversation_origin_name( origin ) + " reason=player_dialogue" );
        return false;
    }
    if( current_reply_depth >= max_reply_depth ) {
        log_evaluation( 0, -1, "max_reply_depth" );
        return false;
    }
    if( g == nullptr ) {
        log_evaluation( 0, -1, "game_unavailable" );
        return false;
    }
    if( spoken.empty() ) {
        log_evaluation( 0, -1, "empty_speech" );
        return false;
    }

    map &here = get_map();
    std::vector<npc *> listeners;
    for( npc &candidate : g->all_npcs() ) {
        spontaneous_state &candidate_state = spontaneous_states[npc_key( candidate )];
        if( &candidate == &speaker || !candidate.is_active() || !candidate.is_player_ally() ||
            candidate.is_dead_state() || candidate.is_hallucination() || candidate.in_sleep_state() ||
            candidate.posz() != speaker.posz() || candidate_state.pending_request_id != 0 ||
            rl_dist( candidate.pos_bub( here ), speaker.pos_bub( here ) ) > max_talk_distance ||
            !candidate.sees( here, speaker ) ) {
            continue;
        }
        listeners.push_back( &candidate );
    }
    std::stable_sort( listeners.begin(), listeners.end(), [&]( const npc *lhs, const npc *rhs ) {
        const int lhs_distance = rl_dist( lhs->pos_bub( here ), speaker.pos_bub( here ) );
        const int rhs_distance = rl_dist( rhs->pos_bub( here ), speaker.pos_bub( here ) );
        return lhs_distance != rhs_distance ? lhs_distance < rhs_distance :
               lhs->get_name() < rhs->get_name();
    } );
    if( listeners.empty() ) {
        log_evaluation( 0, -1, "no_candidate" );
        return false;
    }

    npc &listener = *listeners.front();
    const std::string synthetic_line = "[CONVERSACION NPC A NPC] " + speaker.get_name() +
                                       " acaba de decir en voz alta: \"" + spoken + "\"";
    const std::string prompt = build_npc_prompt( listener, synthetic_line,
                               npc_prompt_purpose::npc_to_npc_reply );
    const ai_enqueue_result queued = enqueue_npc_reply_dialogue(
                                         listener, speaker, spoken, prompt,
                                         build_snapshot( listener ).combat, conversation_id,
                                         current_reply_depth + 1 );
    if( queued.accepted ) {
        spontaneous_states[npc_key( listener )].pending_request_id = queued.request_id;
    }
    log_evaluation( listeners.size(), listener.getID().get_value(),
                    queued.accepted ? "none" : "enqueue_rejected" );
    return queued.accepted;
}

void reset_spontaneous_state_for_test( const npc &who )
{
    spontaneous_states.erase( npc_key( who ) );
    if( spontaneous_states.empty() ) {
        global_last_spoken_turn = -1000000000;
    }
}

void reset_all_spontaneous_states()
{
    spontaneous_states.clear();
    global_last_spoken_turn = -1000000000;
}


} // namespace npc_ai
