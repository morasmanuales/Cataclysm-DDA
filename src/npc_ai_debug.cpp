#include "npc_ai_debug.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>

#include "path_info.h"

namespace npc_ai
{

namespace
{

// -1 keeps the environment decision, 0/1 is a deliberate test override.
std::atomic<int> runtime_debug_override{ -1 };
std::recursive_mutex debug_output_mutex;

} // namespace

std::string debug_file_path( const std::string &filename )
{
    return ( std::filesystem::u8path( PATH_INFO::user_dir() ) / filename ).u8string();
}

bool runtime_debug_enabled()
{
    const int override_value = runtime_debug_override.load( std::memory_order_relaxed );
    if( override_value >= 0 ) {
        return override_value != 0;
    }
    static const bool environment_enabled = []() {
        const char *value = std::getenv( "CDDA_NPC_AI_DEBUG" );
        return value != nullptr && std::string( value ) == "1";
    }();
    return environment_enabled;
}

void set_runtime_debug_enabled_for_test( const bool enabled )
{
    runtime_debug_override.store( enabled ? 1 : 0, std::memory_order_relaxed );
}

debug_stream::debug_stream( const std::string &filename, const bool truncate )
{
    if( !runtime_debug_enabled() ) {
        return;
    }
    lock_ = std::unique_lock<std::recursive_mutex>( debug_output_mutex );
    stream_.open( debug_file_path( filename ),
                  std::ios::binary | ( truncate ? std::ios::trunc : std::ios::app ) );
    active_ = stream_.is_open();
}

void append_debug_line( const std::string &filename, const std::string &line )
{
    debug_stream output( filename );
    if( !output ) {
        return;
    }
    output << line << '\n';
}

} // namespace npc_ai
