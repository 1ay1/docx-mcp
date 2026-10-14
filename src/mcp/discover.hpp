// Discovery: a task-oriented map of every tool, so a model that sees only a
// few tools (clients that rank or cap them) can still find and run the rest.
#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace docx::server {

struct ToolDef;

struct TaskGroup {
    const char* area;                // "Text", "Tables", ...
    const char* says;                // what users ask for, in their words
    std::vector<const char*> tools;  // tool names, most used first
};

// Every tool appears in exactly one group (tested).
const std::vector<TaskGroup>& task_groups();

// help {topic?}: the catalog by task, or the tools matching a topic/word,
// with their argument lists. `all` is the full catalog (not the profile).
nlohmann::json help_result(const std::vector<ToolDef>& all, const std::string& topic);

// Ready-made workflows offered as MCP prompts.
struct Workflow {
    const char* name;
    const char* title;
    const char* description;
    struct Arg { const char* name; const char* description; bool required; };
    std::vector<Arg> args;
    // Builds the user message from the arguments.
    std::string (*text)(const std::vector<std::pair<std::string, std::string>>&);
};
const std::vector<Workflow>& workflows();

// The task catalog as Markdown, served as the docx://guide resource.
std::string guide_markdown(const std::vector<ToolDef>& all);

}  // namespace docx::server
