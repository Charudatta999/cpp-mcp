<p align="center">
  <h1 align="center">cpp-mcp</h1>
  <p align="center">
    A lightweight, dependency-conscious C++20 framework for the
    <a href="https://modelcontextprotocol.io/">Model Context Protocol (MCP)</a>.
    <br/>
    <b>One binary. Any REST API. Just JSON.</b>
  </p>
</p>

<p align="center">
  <a href="https://github.com/Charudatta999/cpp-mcp/actions"><img src="https://github.com/Charudatta999/cpp-mcp/actions/workflows/ci.yml/badge.svg" alt="CI"/></a>
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg" alt="C++20"/>
  <img src="https://img.shields.io/badge/Dependencies-RapidJSON%20only-green.svg" alt="Dependencies"/>
  <img src="https://img.shields.io/badge/Linking-Fully%20Static-purple.svg" alt="Static"/>
  <img src="https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg" alt="Platform"/>
  <img src="https://img.shields.io/badge/License-LGPLv3-blue.svg" alt="License"/>
</p>

> **C++ MCP Server** | **MCP C++ SDK** | **Config-driven MCP API Gateway** | **Static MCP binary for AI agents**
>
> Build MCP servers in C++ with zero runtime dependencies. A secure, supply-chain-safe alternative to Python and TypeScript MCP implementations. Fully static binary with AES-256-GCM transport authentication for enterprise deployment. Works with Claude Desktop, VS Code Copilot, Cursor, and any MCP-compatible AI client.

---

## Why cpp-mcp?

MCP servers are typically written in Python or JavaScript. **cpp-mcp** gives you the same capability with:

- **Small native binary** — no Python or Node.js runtime
- **Config-driven API gateway** — add GitHub, Jira, GitLab, Slack, or _any_ REST API by writing a JSON file. **Zero recompilation.**
- **Fully static linking** — only depends on `KERNEL32.dll` on Windows; zero shared deps for vendored Linux builds
- **Native HTTP** — WinHTTP on Windows, vendored static libcurl on Linux/macOS
- **AES-256-GCM transport auth** — platform-native crypto (BCrypt/CNG on Windows, OpenSSL on Unix)

## Quick Start

### Build (Windows)

```bash
git clone https://github.com/Charudatta999/cpp-mcp.git
cd cpp-mcp
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Build (Linux — vendored static)

```bash
# Build vendored OpenSSL 3.3 + curl 8.11 from source (one-time)
chmod +x third_party/build_deps.sh
third_party/build_deps.sh all

# Build cpp-mcp against vendored static libs
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Clean source artifacts (keeps compiled static libs)
third_party/build_deps.sh clean
```

### Build (Linux — Container, e.g. Oracle Linux 8)

```bash
podman build -t cpp-mcp-oel8 -f Containerfile.oel8 .
# Builds vendored deps, compiles, runs tests, verifies static linkage
```

### Connect any REST API — no code needed

```bash
# GitHub — 11 tools, zero code
api_gateway configs/github.json

# Jira — 30 tools, zero code
api_gateway configs/jira.json

# GitLab — 5 tools, zero code
api_gateway configs/gitlab.json

# Your own API — write a JSON config, zero code
api_gateway configs/your_api.json
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
| **HTTP Client Transport** | WinHTTP (Windows), libcurl (Linux/macOS) |
| **HTTP Server Transport** | Built-in TCP server for hosting MCP over HTTP POST |
| **API Gateway** | Config-driven, supports any REST API |
| **REST Auth** | Bearer token, Basic auth, API key header |
| **Transport Auth** | AES-256-GCM encrypted token header (BCrypt on Windows, OpenSSL on Unix) |
| **Path Templates** | `{{variable}}` substitution in URL paths |
| **Param Routing** | Parameters auto-routed to path / query / body / header |
| **Signal Handling** | Graceful SIGINT/SIGTERM shutdown with cleanup callbacks |
| **Structured Logging** | Leveled logging (Trace→Fatal) with pluggable sinks |
| **Static Linking** | Single binary, zero runtime dependencies |
| **CI/CD** | GitHub Actions: Windows MSVC, Linux OEL8 glibc 2.28 (GCC 13), ASan/UBSan |
| **Container Build** | Containerfile for Oracle Linux 8 (enterprise Linux) |
| **C++20** | No Boost, no heavy frameworks |

---

## The API Gateway

The killer feature. One compiled binary (`api_gateway`) turns any REST API into an MCP server at runtime via a JSON config:

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

### REST Auth Types

| Type | Config | Env Vars |
|------|--------|----------|
| `bearer` | `"token_env": "VAR_NAME"` | `VAR_NAME=ghp_xxx` |
| `basic` | `"username_env"` + `"password_env"` | `USER=x`, `PASS=y` |
| `api_key` | `"header_name"` + `"key_env"` | `KEY=xxx` |

---

## Transport Authentication (AES-256-GCM)

For securing MCP transport channels (e.g. HTTP server mode), cpp-mcp provides symmetric-key authentication using AES-256-GCM:

- **256-bit key**, **96-bit random IV** per encryption, **128-bit GCM auth tag**
- Encrypted token sent as `Mcp-Auth-Token` HTTP header
- Server decrypts and validates at the transport layer, before any JSON-RPC dispatch

**Key provisioning** — pick whichever suits your deployment:

| Source | How |
|--------|-----|
| Environment variable | `MCP_AUTH_KEY=<64-hex-chars>` |
| File | Raw 32 bytes or 64 hex chars on disk |
| Programmatic | Pass raw bytes via `KeyConfig` |

**Crypto backends** — zero external deps on Windows:

| Platform | Backend | Notes |
|----------|---------|-------|
| Windows | BCrypt (CNG) | FIPS 140-2 certified, ships with OS |
| Linux/macOS | OpenSSL libcrypto | Hardware-accelerated, vendored or system |

---

## Pre-built API Configs

Ready-to-use configs in `configs/`:

### GitHub — 11 tools

Search repos, list issues/PRs/commits, get file contents, create issues, and more.

```bash
export GITHUB_TOKEN=ghp_xxxxx
./api_gateway configs/github.json
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

Full Jira coverage including issues, sprints, boards, worklogs, and more.

```bash
export JIRA_EMAIL=you@company.com
export JIRA_API_TOKEN=your_token
./api_gateway configs/jira.json
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
export GITLAB_TOKEN=glpat-xxxxx
./api_gateway configs/gitlab.json
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

### HTTP Server

```cpp
#include "mcp/mcp.hpp"

int main() {
    mcp::HttpServerConfig config;
    config.bind_address = "127.0.0.1";
    config.port = 8080;

    auto transport = std::make_unique<mcp::HttpServerTransport>(config);
    mcp::McpServer server({"my-server", "1.0.0"}, std::move(transport));

    // Register tools, resources, prompts...
    mcp::install_signal_handlers([&]() { server.stop(); });
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
    // result.content[0] -> TextContent{"Hello, World!"}
}
```

---

## Project Structure

```
cpp-mcp/
├── CMakeLists.txt
├── Containerfile.oel8              # OEL8 container build (vendored static)
├── .github/workflows/ci.yml       # CI: Windows MSVC, Linux GCC/Clang, ASan
├── include/mcp/
│   ├── mcp.hpp                     # Umbrella include
│   ├── core/
│   │   ├── types.hpp               # MCP type definitions
│   │   ├── errors.hpp              # Error codes & exceptions
│   │   ├── json_utils.hpp          # JSON-RPC builders & parsers
│   │   └── log.hpp                 # Structured leveled logging
│   ├── transport/
│   │   ├── transport.hpp           # Abstract transport interface
│   │   ├── stdio_transport.hpp     # Stdin/stdout transport
│   │   ├── http_transport.hpp      # HTTP client transport (WinHTTP/curl)
│   │   └── http_server_transport.hpp # HTTP server transport (TCP)
│   ├── server/
│   │   ├── server.hpp              # MCP server
│   │   └── signal_handler.hpp      # Graceful shutdown signal handling
│   ├── client/
│   │   └── client.hpp              # MCP client
│   ├── auth/
│   │   └── auth.hpp                # AES-256-GCM transport auth
│   └── gateway/
│       └── api_gateway.hpp         # Config-driven API gateway
├── src/                            # Implementation files
├── examples/
│   ├── example_server.cpp          # Tool + Resource + Prompt demo
│   ├── example_calculator.cpp      # Calculator tool server
│   └── github_server.cpp           # Hardcoded GitHub server (legacy)
├── tests/
│   ├── cpp_mcp_tests.cpp           # Core protocol tests
│   └── auth_tests.cpp              # Auth module tests
├── benchmarks/
│   └── benchmark_smoke.cpp         # Benchmark smoke tests
├── configs/                        # Ready-to-use API configs
│   ├── github.json                 # GitHub API — 11 tools
│   ├── jira.json                   # Jira API — 30 tools
│   └── gitlab.json                 # GitLab API — 5 tools
└── third_party/
    ├── rapidjson/                  # Header-only JSON (bundled)
    └── build_deps.sh              # Vendored OpenSSL + curl builder
```

---

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

### Linux (vendored static — recommended for deployment)

```bash
# One-time: build vendored OpenSSL 3.3 + curl 8.11 from source
third_party/build_deps.sh all

# Build (auto-detects vendored libs, enables curl)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Verify: no shared openssl/curl/crypto dependencies
ldd build/api_gateway | grep -E 'libssl|libcurl|libcrypto'  # should be empty
```

### Linux (system libraries)

```bash
# Install deps (Ubuntu/Debian)
sudo apt install libcurl4-openssl-dev libssl-dev

cmake -B build -DMCP_USE_CURL=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Container (Oracle Linux 8)

```bash
podman build -t cpp-mcp-oel8 -f Containerfile.oel8 .
```

Builds vendored OpenSSL + curl from source, compiles with GCC 13 + `-Werror`, runs tests, and verifies fully static linkage.

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `MCP_BUILD_EXAMPLES` | `ON` | Build example programs |
| `MCP_BUILD_GATEWAY` | `ON` | Build the API gateway executable |
| `MCP_BUILD_TESTS` | `ON` | Build the test suite |
| `MCP_BUILD_BENCHMARKS` | `ON` | Build benchmark executables |
| `MCP_USE_CURL` | `OFF` | Enable libcurl HTTP backend (Linux/macOS) |
| `MCP_WARNINGS_AS_ERRORS` | `OFF` | Treat compiler warnings as errors |
| `MCP_ENABLE_SANITIZERS` | `OFF` | Enable ASan + UBSan (Clang/GCC) |

### Tests

```bash
ctest --test-dir build --output-on-failure
```

---

## HTTP Backend

| Platform | Backend | Dependencies |
|----------|---------|-------------|
| Windows  | WinHTTP | None (system DLL) |
| Linux    | libcurl | Vendored static or system (`-DMCP_USE_CURL=ON`) |
| macOS    | libcurl | Vendored static or system (`-DMCP_USE_CURL=ON`) |

---

## Client Configuration

### Claude Desktop

Add to `%APPDATA%\Claude\claude_desktop_config.json` (Windows) or `~/Library/Application Support/Claude/claude_desktop_config.json` (macOS):

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

## Adding a New API

1. Create a JSON file (e.g. `configs/slack.json`)
2. Define `server`, `defaults` (base URL, auth), and `tools` (endpoints + params)
3. Run: `./api_gateway configs/slack.json`

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

## CI

GitHub Actions runs on every push and PR to `main`:

| Job | Platform | Compiler | glibc | What |
|-----|----------|----------|-------|------|
| **Windows** | windows-latest | MSVC | N/A | Debug + Release, tests |
| **Linux (OEL8)** | Oracle Linux 8 container | GCC 13 | **2.28** | Release, vendored static, `-Werror`, tests, linkage verification, binary artifacts |
| **Sanitizers** | ubuntu-latest | Clang 17 | ~2.39 | Debug, ASan + UBSan |

Linux binaries are built against **glibc 2.28** (the lowest supported enterprise glibc) and uploaded as CI artifacts — portable to RHEL 8+, OEL 8+, Amazon Linux 2, and any distro with glibc >= 2.28.

---

## License

**LGPL-3.0** — you can link cpp-mcp into commercial/proprietary products without open-sourcing your application. If you modify cpp-mcp itself, those modifications must be released under LGPL-3.0.

---

## Comparison with Other MCP Implementations

| | **cpp-mcp** | Python/TS MCP servers | Other C++ MCP libs |
|---|---|---|---|
| **Runtime deps** | None (static binary) | Python/Node.js + 200+ packages | Dynamic libs (httplib, nlohmann-json) |
| **Supply chain surface** | RapidJSON only (header-only, bundled) | PyPI/npm ecosystem | Git submodules |
| **API gateway** | Config-driven, zero code | Requires code per integration | Not available |
| **Transport auth** | AES-256-GCM (platform-native) | Token headers | Token headers |
| **Enterprise Linux** | OEL8/glibc 2.28, CI-verified static | Depends on system Python | Not addressed |
| **Binary size** | Single static executable | Entire runtime + venv | Dynamic, multiple shared libs |

---

<sub>
<b>Keywords:</b> C++ MCP server, Model Context Protocol C++, MCP SDK C++, MCP API gateway,
cpp mcp framework, C++ AI agent, MCP static binary, secure MCP server, MCP Claude Desktop,
MCP VS Code, MCP Cursor, JSON-RPC C++, REST to MCP, supply chain secure MCP,
enterprise MCP server, C++20 MCP, static linked MCP, AES-256-GCM MCP auth,
MCP server no dependencies, MCP alternative to Python, mcp-server-cpp
</sub>
