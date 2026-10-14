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

// Instructions sent to the client at initialize.
std::string server_instructions();

}  // namespace docx::server
