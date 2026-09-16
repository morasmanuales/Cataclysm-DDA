#pragma once
#ifndef CATA_SRC_NPC_AI_DEBUG_H
#define CATA_SRC_NPC_AI_DEBUG_H

#include <fstream>
#include <mutex>
#include <ostream>
#include <string>

namespace npc_ai
{

// Returns a portable path below CDDA's user directory.  Runtime diagnostics
// must never depend on a developer-specific absolute path.
std::string debug_file_path( const std::string &filename );

// Runtime diagnostic logging is off unless CDDA_NPC_AI_DEBUG=1.  Ordinary play
// must never pay for a file open on the NPC movement path.
bool runtime_debug_enabled();
void set_runtime_debug_enabled_for_test( bool enabled );

// Diagnostic sink that only opens a file when runtime debugging is enabled.
// While disabled it stays closed, converts to false and discards every
// insertion, so existing `if( debug )` call sites keep working and cost
// nothing beyond one predictable branch.
class debug_stream
{
    public:
        explicit debug_stream( const std::string &filename, bool truncate = false );

        explicit operator bool() const {
            return active_;
        }

        template<typename T>
        debug_stream &operator<<( const T &value ) {
            if( active_ ) {
                stream_ << value;
            }
            return *this;
        }

        debug_stream &operator<<( std::ostream & ( *manipulator )( std::ostream & ) ) {
            if( active_ ) {
                stream_ << manipulator;
            }
            return *this;
        }

    private:
        bool active_ = false;
        // Recursive: a handler that holds its own stream may call into another
        // routine (npc::ai_request_pickup, the wield router) that opens a second
        // stream on the same thread.  A plain mutex would self-deadlock there.
        std::unique_lock<std::recursive_mutex> lock_;
        std::ofstream stream_;
};

void append_debug_line( const std::string &filename, const std::string &line );

} // namespace npc_ai

#endif // CATA_SRC_NPC_AI_DEBUG_H
