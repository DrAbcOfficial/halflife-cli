"""halflife-cli MCP tooling implementation package.

halflife_mcp.py is the entry point (MCP server + tool definitions); this
package holds the building blocks by responsibility:

- models:        pydantic result models returned by the MCP tools
- plugin_config: reads the plugin's halflifecli.toml and port file
- screenshot:    in-game screenshot capture and PNG conversion
- usermsg:       UserMsg monitor output parsers and schema loading
- manager:       the game session state machine behind the tools
"""
