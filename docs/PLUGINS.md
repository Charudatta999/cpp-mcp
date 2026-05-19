# Plugin System Roadmap

`cpp-mcp` does not load dynamic plugins yet. The intended design is:

- A `Plugin` interface that exposes metadata, tools, resources, prompts, and lifecycle hooks.
- A `PluginRegistry` that validates metadata and registers plugin-provided capabilities with the runtime.
- A platform-specific loader behind a small interface:
  - Linux: `dlopen`, `dlsym`, `dlclose`
  - Windows: `LoadLibraryW`, `GetProcAddress`, `FreeLibrary`
- A manifest file with name, version, ABI version, entry point, capabilities, and security policy.

## Required Safety Rules

- Plugins must declare an ABI version and fail closed on mismatch.
- Plugin load paths must be explicit and canonicalized.
- Production deployments should use allowlists and signatures before dynamic loading.
- Plugin shutdown must be deterministic and must not unload code while callbacks are active.

## Minimal C ABI Shape

Future plugins should expose a small C ABI and keep C++ types behind runtime-owned interfaces:

```cpp
extern "C" MCP_PLUGIN_EXPORT int mcp_plugin_abi_version();
extern "C" MCP_PLUGIN_EXPORT bool mcp_register_plugin(mcp::PluginRegistrar* registrar);
```

The C ABI keeps compiler and standard-library differences contained, which matters for Linux and Windows binary distribution.
