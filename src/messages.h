// duel-engine: ygopro-core messages <-> JSON. AGPL-3.0-or-later.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include "json.hpp"

namespace msg {
using json = nlohmann::json;

// Short name of a message type ("move", "select_card"...), or "" when unknown.
const char* name(int type);
// True when the message asks a player to decide something (the duel waits for a response).
bool is_prompt(int type);
// Decodes one message body (the bytes after the type byte). Fields follow the core's own layout; a message
// without a known layout, or whose layout didn't consume the body exactly, also carries "raw" (hex) and,
// for the latter, "bad": true (a layout to fix).
json decode(int type, const uint8_t* body, size_t len);
// Builds the core's response bytes for a prompt from the client's JSON answer. Returns false (with err) when
// the answer doesn't fit the prompt; the core also checks and asks again (MSG_RETRY) when it's illegal.
bool encode_response(int type, const json& prompt, const json& answer, std::string& out, std::string& err);
}
