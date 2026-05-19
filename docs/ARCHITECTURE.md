# cpp-mcp Architecture

`cpp-mcp` is moving toward a layered native MCP runtime:

```text
Applications
Gateway Layer
MCP Runtime
Tool Dispatcher
Protocol Layer
Transport Layer
OS / Network / IPC
```

## Current Production Baseline

- `include/mcp/core`: protocol types, JSON-RPC builders/parsers, and structured error types.
- `include/mcp/transport`: transport interface plus stdio and HTTP client implementations.
- `include/mcp/server`: MCP server facade that registers tools, resources, and prompts.
- `include/mcp/client`: HTTP client facade for MCP discovery and invocation.
- `include/mcp/gateway`: config-driven REST-to-MCP gateway schema and safety helpers.

## Boundary Rules

- Transport implementations only move complete JSON-RPC messages.
- Protocol helpers parse, validate, and serialize protocol data without performing I/O.
- Server and client own orchestration and should depend on transport interfaces, not concrete transport details.
- Gateway code is application-layer code and must remain optional at build time.

## Refactor Roadmap

1. Extract server dispatch into an independent protocol engine with no transport ownership.
2. Introduce request context with trace id, deadline, cancellation state, and peer metadata.
3. Add a runtime executor abstraction for synchronous, asynchronous, and streaming handlers.
4. Move gateway HTTP execution behind a reusable outbound HTTP interface.
5. Add plugin discovery and loading behind a registry interface.

This baseline keeps those seams explicit without forcing premature abstractions into the current small codebase.
