<p align="center">
  <h1 align="center">cpp-mcp</h1>
  <p align="center">
    A lightweight, dependency-conscious C++20 framework for the
    <a href="https://modelcontextprotocol.io/">Model Context Protocol</a>.
    <br/>
    <b>One binary. Any REST API. Just JSON.</b>
  </p>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg" alt="C++20"/>
  <img src="https://img.shields.io/badge/Dependencies-RapidJSON%20%2B%20optional%20curl-green.svg" alt="Dependencies"/>
  <img src="https://img.shields.io/badge/Linking-Fully%20Static-purple.svg" alt="Static"/>
  <img src="https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg" alt="Platform"/>
  <img src="https://img.shields.io/badge/License-GPLv3-yellow.svg" alt="License"/>
</p>

---

## Why cpp-mcp?

MCP servers are typically written in Python or JavaScript. **cpp-mcp** gives you the same capability with:

- **Small native binary** — no Python or Node.js runtime
- **Config-driven API gateway** — add GitHub, Jira, GitLab, Slack, or _any_ REST API by writing a JSON file. **Zero recompilation.**
- **Fully static linking** — only depends on `KERNEL32.dll` on Windows
- **Native HTTP** — WinHTTP on Windows, opt-in libcurl on Linux/macOS

## Quick Start

### Build (Windows + Clang)

```bash
git clone https://github.com/your-org/cpp-mcp.git
cd cpp-mcp
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Connect any REST API — no code needed

```bash
# GitHub — 11 tools, zero code
api_gateway.exe configs/github.json

# Jira — 30 tools, zero code
api_gateway.exe configs/jira.json

# GitLab — 5 tools, zero code
api_gateway.exe configs/gitlab.json

# Your own API — write a JSON config, zero code
api_gateway.exe configs/your_api.json
```

### Use with Claude Desktop

```json
{
  "mcpServers": {
    "github": {
      "command": "D:/path/to/api_gateway.exe",
      "args": ["D:/path/to/configs/github.json"],
      "env": { "GITHUB_TOKEN": "ghp_xxxxx" }
    }
  }
}
```

That's it. Claude can now list repos, search code, create issues, and more.

---

## Features

| Feature | Details |
|---------|---------|
| **MCP Primitives** | Tools, Resources, Prompts — full spec |
| **JSON-RPC 2.0** | Request, response, notification, error lifecycle |
| **Stdio Transport** | For Claude Desktop, Cursor, VS Code Copilot, Windsurf |
| **HTTP Transport** | WinHTTP (Windows), libcurl (Linux/macOS) |
| **API Gateway** | Config-driven, supports any REST API |
| **Auth** | Bearer token, Basic auth, API key header |
| **Path Templates** | `{{variable}}` substitution in URL paths |
| **Param Routing** | Parameters auto-routed to path / query / body / header |
| **Static Linking** | Single binary, zero runtime dependencies |
| **C++20** | No Boost, no heavy frameworks |

---

## The API Gateway

The killer feature. One compiled binary (`api_gateway.exe`) turns any REST API into an MCP server at runtime via a JSON config:

### Config Schema

```json
{
  "server": {
    "name": "my-api",
    "version": "1.0.0"
  },
  "defaults": {
    "base_url": "https://api.example.com",
    "headers": {
      "Accept": "application/json"
    },
    "auth": {
      "type": "bearer",
      "token_env": "MY_API_TOKEN"
    }
  },
  "tools": [
    {
      "name": "list_items",
      "description": "List all items",
      "method": "GET",
      "path": "/v1/items",
      "parameters": [
        {
          "name": "limit",
          "type": "string",
          "description": "Max results",
          "required": false,
          "location": "query",
          "default": "10"
        }
      ]
    },
    {
      "name": "get_item",
      "description": "Get an item by ID",
      "method": "GET",
      "path": "/v1/items/{{item_id}}",
      "parameters": [
        {
          "name": "item_id",
          "type": "string",
          "description": "Item ID",
          "required": true,
          "location": "path"
        }
      ]
    },
    {
      "name": "create_item",
      "description": "Create a new item",
      "method": "POST",
      "path": "/v1/items",
      "parameters": [
        {
          "name": "title",
          "type": "string",
          "description": "Item title",
          "required": true,
          "location": "body"
        }
      ]
    }
  ]
}
```

### Parameter Locations

| Location | Behavior |
|----------|----------|
| `path` | Substituted into URL via `{{name}}` template |
| `query` | Appended as `?key=value` |
| `body` | Assembled into JSON request body |
| `header` | Sent as HTTP header |

### Auth Types

| Type | Config | Env Vars |
|------|--------|----------|
| `bearer` | `"token_env": "VAR_NAME"` | `VAR_NAME=ghp_xxx` |
| `basic` | `"username_env"` + `"password_env"` | `USER=x`, `PASS=y` |
| `api_key` | `"header_name"` + `"key_env"` | `KEY=xxx` |

---

## Pre-built API Configs

Ready-to-use configs in `configs/`:

### GitHub — 11 tools

Search repos, list issues/PRs/commits, get file contents, create issues, and more.

```bash
set GITHUB_TOKEN=ghp_xxxxx
api_gateway.exe configs/github.json
```

| Tool | Description |
|------|-------------|
| `get_user` | Get user profile |
| `get_authenticated_user` | Get current user |
| `list_repos` | List repos for user/org |
| `get_repo` | Get repo details |
| `list_issues` | List issues (state filter) |
| `create_issue` | Create a new issue |
| `list_pull_requests` | List PRs |
| `get_file_contents` | Read file from repo |
| `search_repos` | Search repositories |
| `search_code` | Search code |
| `list_commits` | List commits |

### Jira — 30 tools

Full Jira coverage aggregated from [sooperset/mcp-atlassian](https://github.com/sooperset/mcp-atlassian) (5.2k★) and [nguyenvanduocit/jira-mcp](https://github.com/nguyenvanduocit/jira-mcp).

```bash
set JIRA_EMAIL=you@company.com
set JIRA_API_TOKEN=your_token
api_gateway.exe configs/jira.json
```

| Category | Tools |
|----------|-------|
| **Search** | `jira_search` |
| **Issues** | `jira_get_issue`, `jira_create_issue`, `jira_update_issue`, `jira_delete_issue`, `jira_assign_issue` |
| **Workflow** | `jira_get_transitions`, `jira_transition_issue` |
| **Comments** | `jira_add_comment`, `jira_get_comments` |
| **Worklogs** | `jira_add_worklog`, `jira_get_worklogs` |
| **History** | `jira_get_issue_history` |
| **Links** | `jira_link_issues`, `jira_get_issue_watchers`, `jira_add_watcher` |
| **Projects** | `jira_list_projects`, `jira_get_project`, `jira_list_issue_types`, `jira_list_statuses` |
| **Boards** | `jira_list_boards`, `jira_get_board_backlog` |
| **Sprints** | `jira_list_sprints`, `jira_get_sprint`, `jira_get_sprint_issues` |
| **Versions** | `jira_list_versions`, `jira_get_version` |
| **Users** | `jira_user_search`, `jira_get_myself` |
| **Server** | `jira_get_server_info` |

### GitLab — 5 tools

```bash
set GITLAB_TOKEN=glpat-xxxxx
api_gateway.exe configs/gitlab.json
```

| Tool | Description |
|------|-------------|
| `list_projects` | List your projects |
| `get_project` | Get project details |
| `list_merge_requests` | List MRs |
| `list_issues` | List issues |
| `create_issue` | Create new issue |

---

## Writing Custom MCP Servers in C++

For cases where you need custom logic beyond REST API mapping:

### Stdio Server

```cpp
#include "mcp/mcp.hpp"

int main() {
    auto transport = std::make_unique<mcp::StdioTransport>();
    mcp::McpServer server({"my-server", "1.0.0"}, std::move(transport));

    mcp::ToolDefinition tool;
    tool.name        = "hello";
    tool.description = "Say hello";
    tool.input_schema.properties_json = R"({
        "name": {"type": "string", "description": "Who to greet"}
    })";
    tool.input_schema.required = {"name"};

    server.add_tool(tool, [](const mcp::ParamMap& args) {
        mcp::ToolResult r;
        r.content.push_back(mcp::TextContent{"Hello, " + args.at("name") + "!"});
        return r;
    });

    server.run();
}
```

### HTTP Client

```cpp
#include "mcp/mcp.hpp"

int main() {
    auto transport = std::make_unique<mcp::HttpClientTransport>(
        "http://localhost:8080");
    mcp::McpClient client(std::move(transport));

    client.initialize();

    auto tools = client.list_tools();
    for (const auto& t : tools)
        printf("Tool: %s\n", t.name.c_str());

    auto result = client.call_tool("hello", {{"name", "World"}});
    // result.content[0] → TextContent{"Hello, World!"}
}
```

---

## Project Structure

```

## Production Roadmap

The production hardening roadmap is tracked in:

- [Architecture](docs/ARCHITECTURE.md)
- [Security](docs/SECURITY.md)
- [Plugin system](docs/PLUGINS.md)
- [Benchmarking](docs/BENCHMARKS.md)
cpp-mcp/
├── CMakeLists.txt
├── include/mcp/
│   ├── mcp.hpp                    # Umbrella include
│   ├── core/
│   │   ├── types.hpp              # MCP type definitions
│   │   ├── errors.hpp             # Error codes & exceptions
│   │   └── json_utils.hpp         # JSON-RPC builders & parsers
│   ├── transport/
│   │   ├── transport.hpp          # Abstract transport interface
│   │   ├── stdio_transport.hpp    # Stdin/stdout transport
│   │   └── http_transport.hpp     # HTTP client transport
│   ├── server/
│   │   └── server.hpp             # MCP server
│   ├── client/
│   │   └── client.hpp             # MCP client
│   └── gateway/
│       └── api_gateway.hpp        # Config-driven API gateway
├── src/
│   ├── transport/
│   │   ├── stdio_transport.cpp
│   │   └── http_transport.cpp
│   ├── server/
│   │   └── server.cpp
│   ├── client/
│   │   └── client.cpp
│   └── gateway/
│       └── api_gateway.cpp        # Generic API gateway binary
├── examples/
│   ├── example_server.cpp         # Tool + Resource + Prompt demo
│   ├── example_calculator.cpp     # Calculator tool server
│   └── github_server.cpp          # Hardcoded GitHub server (legacy)
├── configs/                       # Ready-to-use API configs
│   ├── github.json                # GitHub API — 11 tools
│   ├── jira.json                  # Jira API — 30 tools
│   ├── gitlab.json                # GitLab API — 5 tools
│   └── README.md                  # Client config guide
└── third_party/
    └── rapidjson/                 # Header-only JSON (bundled)
```

## Building

### Windows (Clang + Ninja — recommended)

```bash
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Windows (MSVC)

```bash
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

### Linux / macOS

```bash
cmake -B build -DMCP_USE_CURL=ON
cmake --build build
```

If libcurl isn't system-wide, point to it:

```bash
cmake -B build -DMCP_USE_CURL=ON \
      -DCURL_INCLUDE_DIR=/path/to/curl/include \
      -DCURL_LIBRARY=/path/to/libcurl.a
```

### Build Artifacts

| Binary | Size | DLL Dependencies |
|--------|------|-----------------|
| `api_gateway.exe` | platform/build dependent | `KERNEL32.dll` only on fully static Windows builds |
| `example_server.exe` | 420 KB | `KERNEL32.dll` only |
| `example_calculator.exe` | 462 KB | `KERNEL32.dll` only |

Fully static — runs on any x64 Windows machine, no runtime install needed.

---

## Client Configuration

### Claude Desktop

Add to `%APPDATA%\Claude\claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "github": {
      "command": "D:/path/to/api_gateway.exe",
      "args": ["D:/path/to/configs/github.json"],
      "env": { "GITHUB_TOKEN": "ghp_xxxxx" }
    },
    "jira": {
      "command": "D:/path/to/api_gateway.exe",
      "args": ["D:/path/to/configs/jira.json"],
      "env": {
        "JIRA_EMAIL": "you@company.com",
        "JIRA_API_TOKEN": "your_token"
      }
    }
  }
}
```

### VS Code (GitHub Copilot)

Add `.vscode/mcp.json` to your workspace:

```json
{
  "servers": {
    "github": {
      "type": "stdio",
      "command": "D:/path/to/api_gateway.exe",
      "args": ["D:/path/to/configs/github.json"],
      "env": { "GITHUB_TOKEN": "ghp_xxxxx" }
    }
  }
}
```

### Cursor

Add to `.cursor/mcp.json`:

```json
{
  "mcpServers": {
    "github": {
      "command": "D:/path/to/api_gateway.exe",
      "args": ["D:/path/to/configs/github.json"],
      "env": { "GITHUB_TOKEN": "ghp_xxxxx" }
    }
  }
}
```

### Windsurf

Add to `~/.codeium/windsurf/mcp_config.json` (same format as Cursor).

---

## HTTP Backend

| Platform | Backend | Dependencies |
|----------|---------|-------------|
| Windows  | WinHTTP | None (system lib) |
| Linux    | libcurl | User-provided (`-DMCP_USE_CURL=ON`) |
| macOS    | libcurl | User-provided (`-DMCP_USE_CURL=ON`) |

The curl extension is **opt-in** — set `MCP_USE_CURL=ON` at build time and link your own libcurl. The framework does not bundle or download curl.

---

## Adding a New API

1. Create a JSON file (e.g. `configs/slack.json`)
2. Define `server`, `defaults` (base URL, auth), and `tools` (endpoints + params)
3. Run: `api_gateway.exe configs/slack.json`

No C++ code. No recompilation. Same binary.

Example — Slack in 15 lines:

```json
{
  "server": { "name": "slack", "version": "1.0.0" },
  "defaults": {
    "base_url": "https://slack.com/api",
    "auth": { "type": "bearer", "token_env": "SLACK_TOKEN" }
  },
  "tools": [
    {
      "name": "post_message",
      "description": "Post a message to a Slack channel",
      "method": "POST",
      "path": "/chat.postMessage",
      "parameters": [
        { "name": "channel", "type": "string", "description": "Channel ID", "required": true, "location": "body" },
        { "name": "text", "type": "string", "description": "Message text", "required": true, "location": "body" }
      ]
    }
  ]
}
```

---

## License

GPLv3
