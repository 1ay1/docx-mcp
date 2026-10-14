# Setting up clients

docx-mcp is one executable that speaks MCP over stdio. Every MCP client can
run it. Download the binary for your system from the
[latest release](https://github.com/1ay1/docx-mcp/releases/latest):

| System | File |
|---|---|
| Windows x64 | `docx-mcp-windows-x86_64.exe` |
| macOS Apple silicon | `docx-mcp-macos-arm64` |
| macOS Intel | `docx-mcp-macos-x86_64` |
| Linux x64 | `docx-mcp-linux-x86_64` |
| Linux arm64 | `docx-mcp-linux-arm64` |

The Linux builds are fully static and run on any distribution. The Windows
build needs nothing beyond Windows 10 or later.

macOS and Linux: make it executable (`chmod +x docx-mcp-*`). On macOS, if
Gatekeeper blocks the download, run `xattr -d com.apple.quarantine docx-mcp-macos-*`.

In the examples below, replace the path with where you put the file. On
Windows, JSON needs doubled backslashes (`C:\\Tools\\docx-mcp.exe`) or forward
slashes (`C:/Tools/docx-mcp.exe`).

## Options

| Flag | Env var | What it does |
|---|---|---|
| `--author NAME` | `DOCX_MCP_AUTHOR` | Name on tracked changes and comments |
| `--tools PROFILE` | `DOCX_MCP_TOOLS` | `full` (87 tools, default), `core` (34), `essential` (14), `review` (28), or a comma list of tool names |
| `--root DIR` | `DOCX_MCP_ROOT` | Folder that relative paths resolve against |

Every profile includes `help` and `run_tool`. `help` lists everything by task
("tables", "add a comment") and `run_tool` runs any tool by name, so a model
can still reach a tool its client didn't show it. The tools are also listed by
task in the `docx://guide` resource, and `review_document`, `draft_document`,
`redline` and `fill_template` are offered as prompts.

Use `--tools core` or `--tools essential` for clients that limit the number of
tools (Cursor allows about 40 across all servers), or for small local models.

## Claude Desktop

Edit `claude_desktop_config.json`:

- Windows: `%APPDATA%\Claude\claude_desktop_config.json`
- macOS: `~/Library/Application Support/Claude/claude_desktop_config.json`

```json
{
  "mcpServers": {
    "docx": {
      "command": "C:/Tools/docx-mcp.exe",
      "args": ["--author", "Your Name"]
    }
  }
}
```

Restart Claude Desktop.

## Claude Code

```sh
claude mcp add docx -- /path/to/docx-mcp --author "Your Name"
```

## Cursor

`~/.cursor/mcp.json` (global) or `.cursor/mcp.json` (project):

```json
{
  "mcpServers": {
    "docx": {
      "command": "/path/to/docx-mcp",
      "args": ["--author", "Your Name", "--tools", "core"]
    }
  }
}
```

## VS Code (GitHub Copilot)

`.vscode/mcp.json`:

```json
{
  "servers": {
    "docx": {
      "type": "stdio",
      "command": "/path/to/docx-mcp",
      "args": ["--author", "Your Name"]
    }
  }
}
```

## Windsurf

`~/.codeium/windsurf/mcp_config.json`:

```json
{
  "mcpServers": {
    "docx": { "command": "/path/to/docx-mcp", "args": ["--author", "Your Name"] }
  }
}
```

## agentty

`~/.agentty/mcp.json` (all projects) or `.agentty/mcp.json` (one project; agentty asks you to approve it first):

```json
{
  "mcpServers": {
    "docx": {
      "command": "/path/to/docx-mcp",
      "args": ["--author", "Your Name"],
      "timeoutMs": 120000,
      "trustAnnotations": true,
      "tools": { "pin": ["help", "run_tool", "open_document", "read_document", "save_document"] }
    }
  }
}
```

agentty sends the model only the MCP tools that best match each request (16
of them). Pinning `help` and `run_tool` keeps every other tool one call away:
without them a model asked "what can you do?" sees an arbitrary 16 and
concludes the rest don't exist. `trustAnnotations` lets agentty believe
the server's read-only marks, so tools like `read_document` and `list_*` are
treated as reads instead of as commands that can write and run anything.
`agentty mcp-status` shows whether it's loaded.

## Zed

In `settings.json`:

```json
{
  "context_servers": {
    "docx": {
      "command": { "path": "/path/to/docx-mcp", "args": ["--author", "Your Name"] }
    }
  }
}
```

## Continue, Cline, Goose, LM Studio, and others

Any client with stdio MCP support works. Set the command to the binary and
the arguments as above. With local models in LM Studio or Ollama-based
clients, `--tools core` keeps the tool list small enough for the model to
use well.

## Paths

Tools take normal paths for the system the server runs on. These all work:

- `C:\Users\me\Documents\Report.docx`, `C:/Users/me/Documents/Report.docx`
- `/Users/me/Documents/Report.docx`, `~/Documents/Report.docx`
- `file:///C:/Users/me/Report.docx`
- Names with spaces or non-ASCII characters (`Rapport final é.docx`)
- Relative paths, which resolve against `--root` or the client's working directory

If Word has the file open on Windows, saving over it fails with a message to
close it or save under another name. Word locks files it has open.

## Checking it works

```sh
docx-mcp --version
docx-mcp --tools core --list-tools
```

The server prints nothing on stdout until a client talks to it. Everything
human-readable goes to stderr.
