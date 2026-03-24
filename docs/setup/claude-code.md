# TokToken Setup: Claude Code

## MCP Server Configuration

Claude Code registers MCP servers via the `claude mcp add` CLI command. **This command must be run by the user in their terminal, not by the agent.** The agent cannot execute `claude` commands from within a session.

Instruct the user to run one of the following commands in their terminal:

```bash
# User-scoped (available across all projects -- recommended)
claude mcp add-json --scope user toktoken '{"command":"toktoken","args":["serve"]}'

# Or project-scoped (creates .mcp.json in project root, shareable via git)
claude mcp add-json --scope project toktoken '{"command":"toktoken","args":["serve"]}'

# Verify
claude mcp list
```

### Alternative: write .mcp.json directly (project-scoped)

If the user prefers not to use the `claude` CLI, or if you (the agent) need to set up project-scoped MCP autonomously, create or update `.mcp.json` in the project root:

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

**If `.mcp.json` already exists, read it first, merge the `toktoken` entry into the existing `mcpServers` object, and write it back. Do not overwrite existing servers.**

### Scope options

| Scope | How to set | Storage | Visibility |
| ----- | ---------- | ------- | ---------- |
| `--scope user` | `claude mcp add-json` (user runs) | `~/.claude/settings.json` | All projects, current user |
| `--scope project` | `claude mcp add-json` or write `.mcp.json` | `.mcp.json` in project root | Shared via version control |
| `--scope local` (default) | `claude mcp add-json` (user runs) | `~/.claude/settings.local.json` (per-project path) | Current project, current user |

After adding, the user must restart the Claude Code session (close and reopen the conversation) to load the new MCP tools.

## Auto-Approve TokToken Tools (Optional)

By default, Claude Code prompts for permission each time an MCP tool is called. Ask the user if they want to auto-approve all TokToken tools. If yes, add the following entries to the appropriate settings file based on the chosen scope:

| Scope | Settings file |
| ----- | ------------- |
| **Project** | `.claude/settings.local.json` in project root |
| **User** | `~/.claude/settings.json` |

Add these entries to the `permissions.allow` array:

```json
{
    "permissions": {
        "allow": [
            "mcp__toktoken__help",
            "mcp__toktoken__stats",
            "mcp__toktoken__codebase_detect",
            "mcp__toktoken__projects_list",
            "mcp__toktoken__cache_clear",
            "mcp__toktoken__index_create",
            "mcp__toktoken__index_update",
            "mcp__toktoken__index_github",
            "mcp__toktoken__index_file",
            "mcp__toktoken__search_symbols",
            "mcp__toktoken__search_text",
            "mcp__toktoken__search_cooccurrence",
            "mcp__toktoken__search_similar",
            "mcp__toktoken__find_importers",
            "mcp__toktoken__find_references",
            "mcp__toktoken__find_callers",
            "mcp__toktoken__find_dead",
            "mcp__toktoken__suggest",
            "mcp__toktoken__inspect_outline",
            "mcp__toktoken__inspect_symbol",
            "mcp__toktoken__inspect_file",
            "mcp__toktoken__inspect_tree",
            "mcp__toktoken__inspect_bundle",
            "mcp__toktoken__inspect_dependencies",
            "mcp__toktoken__inspect_hierarchy",
            "mcp__toktoken__inspect_blast_radius",
            "mcp__toktoken__inspect_cycles"
        ]
    }
}
```

If the settings file already exists, **read it first**, merge the new entries into the existing `permissions.allow` array, and write it back. Do not overwrite existing permissions.

## Rules Setup

Create the file `~/.claude/rules/toktoken.md` (global) or `.claude/rules/toktoken.md` (project-scoped) with the rules template from [rules-template.md](../rules-template.md).

## Official Documentation

- [Claude Code MCP](https://docs.anthropic.com/en/docs/claude-code/mcp)
