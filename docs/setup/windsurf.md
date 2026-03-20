# TokToken Setup: Windsurf

## MCP Server Configuration

Add the following entry to Windsurf's MCP configuration file. **If the file already exists, read it first, merge the `toktoken` entry into the existing `mcpServers` object, and write it back. Do not overwrite the file.**

```json
{
    "mcpServers": {
        "toktoken": {
            "command": "toktoken",
            "args": ["serve"]
        }
    }
}
```

### Config file location

| Path |
| ---- |
| `~/.codeium/windsurf/mcp_config.json` |

After editing the config file, restart Windsurf to load the new MCP server.

## Auto-Approve TokToken Tools (Optional)

By default, Windsurf prompts for approval each time an MCP tool is called. Ask the user if they want to auto-approve all TokToken tools. If yes, add an `alwaysAllow` array to the `toktoken` server entry:

```json
{
    "mcpServers": {
        "toktoken": {
            "command": "toktoken",
            "args": ["serve"],
            "alwaysAllow": [
                "codebase_detect",
                "index_create",
                "index_update",
                "index_github",
                "search_symbols",
                "search_text",
                "inspect_outline",
                "inspect_symbol",
                "inspect_file",
                "inspect_tree",
                "inspect_bundle",
                "stats",
                "projects_list",
                "cache_clear",
                "find_importers",
                "find_references",
                "inspect_blast_radius",
                "inspect_cycles",
                "find_dead",
                "index_file"
            ]
        }
    }
}
```

## Rules Setup

Create `.windsurf/rules/toktoken.md` in your project root with the rules template from [rules-template.md](../rules-template.md).

## Official Documentation

- [Windsurf MCP](https://docs.windsurf.com/windsurf/cascade/mcp)
