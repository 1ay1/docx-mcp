// docx-mcp: MCP server (stdio) that edits Word documents.
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include <mcp/mcp.hpp>

#include "mcp/session.hpp"
#include "mcp/tools.hpp"

namespace {

constexpr const char* kVersion = "0.1.0";

void usage() {
    std::puts(
        "docx-mcp - edit Word documents over the Model Context Protocol (stdio)\n"
        "\n"
        "usage: docx-mcp [--author NAME] [--list-tools] [--version]\n"
        "\n"
        "  --author NAME   default author for tracked changes and comments\n"
        "  --list-tools    print the tool catalog as JSON and exit\n");
}

}  // namespace

int main(int argc, char** argv) {
    docx::server::Sessions sessions;
    bool list = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--author" && i + 1 < argc) sessions.default_author = argv[++i];
        else if (a == "--list-tools") list = true;
        else if (a == "--version") { std::puts(kVersion); return 0; }
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown argument %s\n", argv[i]); usage(); return 2; }
    }
    if (const char* env = std::getenv("DOCX_MCP_AUTHOR"); env && *env && sessions.default_author == "docx-mcp")
        sessions.default_author = env;

    auto tools = docx::server::make_tools();
    if (list) {
        nlohmann::json j = nlohmann::json::array();
        for (auto& t : tools) j.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.schema}});
        std::cout << j.dump(2) << "\n";
        return 0;
    }

    mcp::Server server{mcp::Implementation{"docx-mcp", kVersion}};
    server.set_instructions(docx::server::server_instructions());
    for (auto& t : tools) {
        mcp::Tool spec;
        spec.name = t.name;
        spec.title = t.title;
        spec.description = t.description.empty() ? t.title : t.description;
        if (t.schema.contains("properties")) spec.inputSchema.properties = t.schema["properties"];
        if (t.schema.contains("required"))
            spec.inputSchema.required = t.schema["required"].get<std::vector<std::string>>();
        spec.annotations = mcp::ToolAnnotations{};
        spec.annotations->title = t.title;
        spec.annotations->readOnlyHint = t.read_only;
        spec.annotations->destructiveHint = t.destructive;
        spec.annotations->openWorldHint = false;
        auto run = t.run;
        server.register_tool(std::move(spec), [&sessions, run](const nlohmann::json& args) -> mcp::CallToolResult {
            mcp::CallToolResult r;
            try {
                std::lock_guard lk(sessions.mu);
                nlohmann::json out = run(sessions, args.is_object() ? args : nlohmann::json::object());
                r.content = {mcp::text(out.dump(2))};
                if (out.is_object()) r.structuredContent = out;
                else r.structuredContent = nlohmann::json{{"result", out}};
            } catch (const std::exception& e) {
                r.content = {mcp::text(std::string("error: ") + e.what())};
                r.isError = true;
            }
            return r;
        });
    }

    // One line in, the engine stepped, the replies out.
    mcp::Engine engine;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        mcp::Effects fx = mcp::step(engine, mcp::Received{line});
        for (auto& f : fx.send) std::cout << f << '\n';
        for (auto& c : fx.calls) std::cout << server.handle(c) << '\n';
        for (auto& n : fx.notifications) server.handle(n);
        std::cout.flush();
    }
    return 0;
}
