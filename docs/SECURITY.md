# Security Notes

## Current Controls

- Stdio transport rejects messages larger than 10 MiB.
- API gateway path parameters are sanitized before template substitution.
- API gateway header values strip CR, LF, and NUL characters.
- API gateway URL encoding uses RFC 3986 percent encoding.
- Windows gateway requests enforce TLS 1.2 or newer for HTTPS.
- Gateway tool errors avoid returning internal transport details to MCP clients.

## Build-Time Hardening

Use sanitizer smoke runs during development:

```bash
cmake -S . -B build-sanitize -DMCP_ENABLE_SANITIZERS=ON -DMCP_BUILD_EXAMPLES=OFF
cmake --build build-sanitize
ctest --test-dir build-sanitize --output-on-failure
```

Use warning gates in CI and release branches:

```bash
cmake -S . -B build-werror -DMCP_WARNINGS_AS_ERRORS=ON
cmake --build build-werror
```

## Known Gaps

- No process sandbox is implemented yet for tool handlers.
- No resource quota manager exists yet for per-request CPU, memory, or time limits.
- Plugin loading is not implemented yet, so signature verification and allowlists are future work.
- Request tracing exists only as a roadmap item.

Do not expose the API gateway to untrusted networks as a general HTTP proxy. It should be launched by an MCP host with explicit JSON configuration and narrowly scoped credentials.
