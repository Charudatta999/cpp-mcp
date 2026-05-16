# MCP Client Configuration Guide

Configs for connecting the cpp-mcp servers to various AI coding tools.

> **Note:** Replace the paths below with your actual build output paths.

---

## Claude Desktop

**File:** `%APPDATA%\Claude\claude_desktop_config.json`

Copy the contents of `claude_desktop_config.json` into your existing config
(merge the `mcpServers` object if you already have other servers).

```json
{
  "mcpServers": {
    "calculator": {
      "command": "D:/cjadhav/Work/apps/cpp-mcp/build/example_calculator.exe"
    }
  }
}
```

After editing, **restart Claude Desktop** for changes to take effect.

---

## Cursor

**Project-level:** `.cursor/mcp.json` in your project root  
**Global:** `~/.cursor/mcp.json`

```json
{
  "mcpServers": {
    "calculator": {
      "command": "D:/cjadhav/Work/apps/cpp-mcp/build/example_calculator.exe"
    }
  }
}
```

Then open **Cursor Settings → MCP** to verify the server shows up as connected.

---

## VS Code (GitHub Copilot)

**Workspace-level:** `.vscode/mcp.json` (already created in this project)  
**User-level:** Add to `settings.json` under `"mcp.servers"`

```json
{
  "servers": {
    "calculator": {
      "type": "stdio",
      "command": "D:/cjadhav/Work/apps/cpp-mcp/build/example_calculator.exe"
    }
  }
}
```

Alternatively, in your **`settings.json`**:

```json
{
  "mcp": {
    "servers": {
      "calculator": {
        "type": "stdio",
        "command": "D:/cjadhav/Work/apps/cpp-mcp/build/example_calculator.exe"
      }
    }
  }
}
```

GitHub Copilot agent mode will auto-discover tools from the MCP server.

---

## Windsurf (Codeium)

**File:** `~/.codeium/windsurf/mcp_config.json`

```json
{
  "mcpServers": {
    "calculator": {
      "command": "D:/cjadhav/Work/apps/cpp-mcp/build/example_calculator.exe"
    }
  }
}
```

---

## Continue (VS Code / JetBrains)

**File:** `~/.continue/config.yaml`

```yaml
mcpServers:
  - name: calculator
    command: D:/cjadhav/Work/apps/cpp-mcp/build/example_calculator.exe
  - name: example-server
    command: D:/cjadhav/Work/apps/cpp-mcp/build/example_server.exe
```

---

## Zed

**File:** `~/.config/zed/settings.json`

```json
{
  "context_servers": {
    "calculator": {
      "command": {
        "path": "D:/cjadhav/Work/apps/cpp-mcp/build/example_calculator.exe",
        "args": []
      }
    }
  }
}
```

---

## Generic (any MCP client using stdio)

All cpp-mcp servers use the **stdio transport** — they read JSON-RPC from stdin
and write responses to stdout. Any MCP-compliant client that supports stdio can
use them by simply specifying the executable path as the command.

### Testing manually

```powershell
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}' | .\build\example_calculator.exe
```
