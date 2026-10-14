// docx-mcp: MCP server (stdio) that edits Word documents.
//
// The transport is newline-delimited JSON-RPC on stdin/stdout. stdout carries
// protocol frames only; anything human-readable goes to stderr.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <new>
#include <string>

// only the protocol + server headers: the capability layer needs std::stop_token,
// which Apple's libc++ hides behind -fexperimental-library
#include <mcp/server.hpp>

#include "mcp/session.hpp"
#include "mcp/discover.hpp"
#include "mcp/tools.hpp"
#include "util/crash.hpp"
#include "util/fsio.hpp"
#include "util/util.hpp"

namespace {

#ifndef DOCX_MCP_VERSION
#define DOCX_MCP_VERSION "dev"
#endif
constexpr const char* kVersion = DOCX_MCP_VERSION;

void usage() {
    std::fputs(
        "docx-mcp " DOCX_MCP_VERSION " - edit Word documents over the Model Context Protocol (stdio)\n"
        "\n"
        "usage: docx-mcp [--author NAME] [--tools PROFILE] [--root DIR] [--list-tools] [--version]\n"
        "\n"
        "  --author NAME   author for tracked changes and comments (or DOCX_MCP_AUTHOR)\n"
        "  --tools PROFILE full (85 tools, default), core (32), review (26), or a comma list\n"
        "                  of tool names (or DOCX_MCP_TOOLS). Use core for clients with tool limits.\n"
        "  --root DIR      resolve relative paths against DIR (or DOCX_MCP_ROOT)\n"
        "  --list-tools    print the tool catalog as JSON and exit\n"
        "  --version       print the version and exit\n"
        "\n"
        "Run it from an MCP client; it speaks JSON-RPC on stdin/stdout.\n",
        stderr);
}

// Lines bigger than this are refused instead of being buffered without end.
constexpr std::size_t kMaxLine = 256u << 20;  // 256 MB (base64 pictures can be big)

std::string error_frame(const nlohmann::json& id, int code, const std::string& msg) {
    nlohmann::json j = {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", msg}}}};
    return j.dump();
}

}  // namespace

int main(int argc, char** argv) {
    docx::util::install_crash_handler();
    docx::util::set_binary_stdio();
    std::ios::sync_with_stdio(false);

    docx::server::Sessions sessions;
    bool list = false;
    std::string root;
    std::string profile;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if ((a == "--author" || a == "-a") && i + 1 < argc) sessions.default_author = argv[++i];
        else if (a.rfind("--author=", 0) == 0) sessions.default_author = a.substr(9);
        else if (a == "--root" && i + 1 < argc) root = argv[++i];
        else if (a.rfind("--root=", 0) == 0) root = a.substr(7);
        else if (a == "--tools" && i + 1 < argc) profile = argv[++i];
        else if (a.rfind("--tools=", 0) == 0) profile = a.substr(8);
        else if (a == "--list-tools") list = true;
        else if (a == "--version" || a == "-V") { std::printf("%s\n", kVersion); return 0; }
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--stdio") {}  // some clients pass this; stdio is the only transport
        else { std::fprintf(stderr, "docx-mcp: unknown argument %s\n\n", argv[i]); usage(); return 2; }
    }
    if (sessions.default_author == "docx-mcp") {
        std::string env = docx::util::env("DOCX_MCP_AUTHOR");
        if (!env.empty()) sessions.default_author = env;
    }
    if (root.empty()) root = docx::util::env("DOCX_MCP_ROOT");
    if (!root.empty()) {
        std::error_code ec;
        std::filesystem::current_path(docx::util::u8path(docx::util::normalize_path(root)), ec);
        if (ec) { std::fprintf(stderr, "docx-mcp: cannot use --root %s: %s\n", root.c_str(), ec.message().c_str()); return 2; }
    }

    if (profile.empty()) profile = docx::util::env("DOCX_MCP_TOOLS");
    std::vector<docx::server::ToolDef> tools;
    try {
        tools = docx::server::select_tools(docx::server::make_tools(), profile);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "docx-mcp: %s\n", e.what());
        return 2;
    }
    if (list) {
        nlohmann::json j = nlohmann::json::array();
        for (auto& t : tools) j.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.schema}});
        std::cout << j.dump(2) << "\n";
        return 0;
    }

    mcp::Server server{mcp::Implementation{"docx-mcp", kVersion}};
    server.set_instructions(docx::server::server_instructions());
    // Workflows as prompts, and the task catalog as a resource, for clients
    // that surface them (slash commands, attachable context).
    static const auto full_catalog = docx::server::make_tools();
    for (auto& w : docx::server::workflows()) {
        mcp::Prompt p;
        p.name = w.name;
        p.title = w.title;
        p.description = w.description;
        std::vector<mcp::PromptArgument> args;
        for (auto& a : w.args) {
            mcp::PromptArgument pa;
            pa.name = a.name;
            pa.description = a.description;
            pa.required = a.required;
            args.push_back(pa);
        }
        p.arguments = args;
        auto build = w.text;
        auto need = w.args;
        server.register_prompt(p, [build, need](const std::vector<std::pair<std::string, std::string>>& in) {
            for (auto& a : need) {
                if (!a.required) continue;
                bool have = false;
                for (auto& [k, v] : in) have |= k == a.name && !v.empty();
                if (!have) throw jsonrpc::RpcError(jsonrpc::errc::InvalidParams, std::string("missing argument '") + a.name + "'");
            }
            mcp::GetPromptResult r;
            r.messages = {mcp::PromptMessage{mcp::Role::User, mcp::text(build(in))}};
            return r;
        });
    }
    {
        mcp::Resource res;
        res.uri = "docx://guide";
        res.name = "guide";
        res.title = "docx-mcp tools by task";
        res.description = "Every tool grouped by task, with arguments. Useful when a client shows only some tools.";
        res.mimeType = "text/markdown";
        server.register_resource(res, [](const std::string& uri) {
            mcp::ReadResourceResult r;
            mcp::TextResourceContents c;
            c.uri = uri;
            c.text = docx::server::guide_markdown(full_catalog);
            c.mimeType = "text/markdown";
            r.contents = {c};
            return r;
        });
    }
    // Version negotiation (MCP lifecycle spec): answer with the client's
    // version when we speak it, else with our newest. Everything this server
    // uses (tools, text + structured content) exists in all of these.
    server.on_initialize([&server](const mcp::InitializeParams& p) {
        static const char* known[] = {"2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
        mcp::InitializeResult out;
        out.protocolVersion = known[0];
        for (auto v : known)
            if (p.protocolVersion == v) out.protocolVersion = v;
        out.capabilities = server.capabilities();
        out.serverInfo = mcp::Implementation{"docx-mcp", kVersion};
        out.instructions = docx::server::server_instructions();
        return out;
    });
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
        spec.annotations->idempotentHint = t.read_only;
        spec.annotations->openWorldHint = false;
        const docx::server::ToolDef* def = &t;
        server.register_tool(std::move(spec), [&sessions, def](const nlohmann::json& in) -> mcp::CallToolResult {
            mcp::CallToolResult r;
            try {
                std::lock_guard lk(sessions.mu);
                nlohmann::json out = docx::server::call_tool(*def, sessions, in);
                // pictures (rendered pages) go out as image blocks, not inside the JSON text
                std::vector<mcp::ContentBlock> images;
                if (out.contains("_images")) {
                    for (auto& im : out["_images"])
                        images.push_back(mcp::image(im.value("data", ""), im.value("mime", "image/png")));
                    out.erase("_images");
                }
                r.content = {mcp::text(out.contains("_text") && out.size() == 1 ? out["_text"].get<std::string>()
                                                                                 : out.dump(2))};
                for (auto& b : images) r.content.push_back(std::move(b));
                r.structuredContent = out;
            } catch (const std::bad_alloc&) {
                r.content = {mcp::text("error: out of memory while running " + def->name)};
                r.isError = true;
            } catch (const nlohmann::json::exception& e) {
                // a value of the wrong type inside an object argument ("zoom": "120%")
                std::string m = e.what();
                if (auto p = m.find("] "); p != std::string::npos) m = m.substr(p + 2);
                r.content = {mcp::text("error: a value has the wrong type (" + m +
                                       "); check the property types in the tool description")};
                r.isError = true;
            } catch (const std::exception& e) {
                r.content = {mcp::text(std::string("error: ") + e.what())};
                r.isError = true;
            } catch (...) {
                r.content = {mcp::text("error: unexpected failure in " + def->name)};
                r.isError = true;
            }
            return r;
        });
    }

    mcp::Engine engine;
    auto write = [](const std::string& frame) {
        std::cout << frame << '\n';
    };
    // One frame in -> zero or more frames out. Never lets an exception escape.
    auto handle = [&](const std::string& line) -> std::vector<std::string> {
        std::vector<std::string> out;
        try {
            mcp::Effects fx = mcp::step(engine, mcp::Received{line});
            for (auto& f : fx.send) out.push_back(f);
            for (auto& c : fx.calls) out.push_back(server.handle(c));
            for (auto& n : fx.notifications) server.handle(n);
        } catch (const std::exception& e) {
            out.push_back(error_frame(nullptr, -32603, std::string("internal error: ") + e.what()));
        }
        return out;
    };

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // tolerate a UTF-8 BOM at the start of the stream
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
            line.erase(0, 3);
        if (docx::util::trim(line).empty()) continue;
        if (line.size() > kMaxLine) {
            write(error_frame(nullptr, -32600, "message too large"));
            std::cout.flush();
            continue;
        }
        // JSON-RPC batch (MCP 2025-03-26 clients): answer each, reply with an array
        std::size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line[first] == '[') {
            nlohmann::json batch;
            try { batch = nlohmann::json::parse(line); } catch (const std::exception& e) {
                write(error_frame(nullptr, -32700, std::string("parse error: ") + e.what()));
                std::cout.flush();
                continue;
            }
            if (!batch.is_array() || batch.empty()) {
                write(error_frame(nullptr, -32600, "invalid request: empty batch"));
                std::cout.flush();
                continue;
            }
            nlohmann::json replies = nlohmann::json::array();
            for (auto& item : batch)
                for (auto& f : handle(item.dump())) {
                    try { replies.push_back(nlohmann::json::parse(f)); } catch (...) {}
                }
            if (!replies.empty()) write(replies.dump());
            std::cout.flush();
            continue;
        }
        for (auto& f : handle(line)) write(f);
        std::cout.flush();
    }
    return 0;
}
