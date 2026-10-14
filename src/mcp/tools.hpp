// MCP tool catalog: every Word operation as a tool with a JSON schema.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mcp/session.hpp"

namespace docx::server {

using Json = nlohmann::json;

struct ToolDef {
    std::string name, title, description;
    Json schema;          // JSON Schema object for arguments ("properties" + "required")
    bool read_only = false;
    bool destructive = false;
    std::function<Json(Sessions&, const Json&)> run;  // returns a JSON result
};

std::vector<ToolDef> make_tools();

// Named subsets of the catalog. "full" = everything. "core" fits clients
// with tool limits (Cursor) and small context windows. "review" is for
// commenting and track-changes work. A comma list of tool names also works.
std::vector<ToolDef> select_tools(std::vector<ToolDef> all, const std::string& profile);
std::vector<std::string> profile_names();

// Instructions sent to the client at initialize.
std::string server_instructions();

// Coerce arguments to the tool's schema the way models tend to get them
// wrong: numbers/booleans/objects/arrays sent as strings, a single value
// where a list is expected, wrong-case enum values, unknown argument names
// that are close to a real one. Throws with a clear message when an argument
// can't be made to fit. Returns notes about corrections it made.
std::vector<std::string> normalize_args(const ToolDef& t, Json& args);

// Run one tool the way tools/call does: normalize arguments, run, wrap
// non-object results, add correction notes. Throws on failure.
Json call_tool(const ToolDef& t, Sessions& S, Json args);

}  // namespace docx::server
