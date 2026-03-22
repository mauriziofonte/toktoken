# TokToken Language Support

TokToken supports 47 programming languages. Symbol extraction uses [universal-ctags](https://ctags.io/) for most languages and built-in custom parsers for 14 additional ones.

---

## Indexed Languages

These languages are indexed by default. File discovery is based on the extension; symbol extraction is handled by ctags or a custom parser.

### Ctags-based languages

These languages are parsed by universal-ctags. Symbol extraction quality depends on the ctags version installed.

| Language | Extensions |
| -------- | ---------- |
| Bash/Shell | `.sh`, `.bash`, `.zsh` |
| C | `.c` |
| C++ | `.cpp`, `.cxx`, `.cc`, `.h`, `.hpp`, `.hxx`, `.hh` |
| C# | `.cs` |
| Clojure | `.clj`, `.cljs`, `.cljc` |
| Dart | `.dart` |
| Elixir | `.ex`, `.exs` |
| Emacs Lisp | `.el` |
| Erlang | `.erl`, `.hrl` |
| Fortran | `.f90`, `.f95`, `.f03`, `.f08`, `.f`, `.for`, `.fpp` |
| Go | `.go` |
| Groovy | `.groovy`, `.gradle` |
| Haskell | `.hs`, `.lhs` |
| Java | `.java` |
| JavaScript | `.js`, `.jsx`, `.mjs`, `.cjs` |
| Kotlin | `.kt`, `.kts` |
| Lua | `.lua` |
| Markdown | `.md`, `.markdown`, `.mdx` |
| Objective-C | `.m` |
| Objective-C++ | `.mm` |
| OCaml | `.ml`, `.mli` |
| Perl | `.pl`, `.pm` |
| PHP | `.php`, `.phtml`, `.inc` |
| Protocol Buffers | `.proto` |
| Python | `.py`, `.pyw` |
| R | `.r`, `.R` |
| Ruby | `.rb` |
| Rust | `.rs` |
| Scala | `.scala`, `.sc` |
| SQL | `.sql` |
| Swift | `.swift` |
| TypeScript | `.ts`, `.tsx`, `.mts` |
| Verilog/SystemVerilog | `.v`, `.sv` |
| Vim | `.vim` |

> **Note**: `.h` files are detected as C++ because ctags produces better results when parsing headers as C++. C-only headers are still extracted correctly — ctags handles C constructs within C++ mode.

### Custom parsers

These languages use TokToken's built-in parsers (no ctags dependency). Each parser targets the specific constructs that matter for code navigation in that language.

| Language | Extensions | What is extracted |
| -------- | ---------- | ----------------- |
| Assembly | `.asm`, `.s` | Labels, functions, macros, sections, data definitions |
| AutoHotkey | `.ahk` | Classes, functions, methods (with parent scoping), hotkeys, `#HotIf` directives. Supports both v1 and v2 syntax |
| Blade | `.blade.php` | `@section`, `@component`, `@slot` directives |
| EJS | `.ejs` | Embedded JavaScript code blocks |
| GDScript | `.gd` | Functions, classes, signals, enums, constants, exported vars |
| Gleam | `.gleam` | Functions, types, constants, imports |
| GraphQL | `.graphql`, `.gql` | Types, interfaces, unions, enums, inputs, scalars, queries, mutations, subscriptions, fragments |
| HCL/Terraform | `.tf`, `.hcl`, `.tfvars` | Resources, variables, modules, outputs, providers, data sources, locals |
| Julia | `.jl` | Functions, macros, structs, abstract types, modules, constants |
| Nix | `.nix` | Top-level attributes and functions |
| OpenAPI/Swagger | `.yaml`, `.yml`, `.json` (detected by basename/extension) | API info, path operations with operationId, schema definitions. Supports JSON (cJSON) and YAML (state machine), both Swagger v2 and OpenAPI v3 |
| Verse | `.verse` | Classes, functions, top-level variables |
| Vue SFC | `.vue` | Script/template/style sections, component exports, Composition API (ref, computed, watch, lifecycle hooks, provide/inject) |
| XML/XUL | `.xml`, `.xul` | Document root element, elements with `id` attributes, `<script src>` and `<link href>` references, preceding comment docstrings |

---

## Smart Filter Exclusions (default: on)

The following extensions are in TokToken's extension table but are **excluded by default** when the smart filter is active. They are only indexed when the smart filter is disabled.

| Extension | Language | Reason for exclusion |
| --------- | -------- | -------------------- |
| `.css` | CSS | Selectors extracted as "class" symbols — pollutes search results |
| `.scss` | SCSS | Same as CSS — preprocessor selectors are not useful code symbols |
| `.less` | LESS | Same as CSS |
| `.sass` | SASS | Same as CSS |
| `.html`, `.htm` | HTML | Tags extracted as symbols — not useful for code navigation |
| `.svg` | SVG | Vector graphics (XML-based) — no meaningful code symbols |
| `.toml` | TOML | Configuration data, no meaningful code symbols |
| `.graphql`, `.gql` | GraphQL | Excluded from ctags (custom parser handles these directly) |
| `.xml`, `.xul` | XML/XUL | Excluded from ctags (custom parser handles these directly) |
| `.yaml`, `.yml` | YAML | Only OpenAPI/Swagger files are parsed (custom parser); generic YAML has no meaningful code symbols |

To include the ctags-based ones:

- CLI: `toktoken index:create --full`
- Config: `"smart_filter": false` in `.toktoken.json`
- MCP: pass `"full": true` to `index_create` or `index_update`

> **Note**: GraphQL and XML/XUL files are always processed by the custom parser regardless of the smart filter setting. The smart filter exclusion only prevents ctags from also processing them (which would produce duplicate/inferior results).
>
> **Note**: Markdown files (`.md`, `.markdown`, `.mdx`) are **always indexed** regardless of the smart filter setting. Headings are extracted as documentation-specific symbol kinds: `chapter` (H1), `section` (H2), `subsection` (H3-H6). Unlike CSS/HTML, Markdown headings produce high-quality navigable symbols that do not pollute code search results.

---

## Adding Languages via Configuration

Use `extra_extensions` in `.toktoken.json` to map additional file extensions to existing language parsers:

```json
{
  "index": {
    "extra_extensions": {
      "svx": "svelte",
      "tsx": "typescript"
    }
  }
}
```

Or via environment variable:

```bash
export TOKTOKEN_EXTRA_EXTENSIONS="svx:svelte"
```

> **Note**: `extra_extensions` affects language detection only. The mapped extension must already be in TokToken's source extension list for the file to be discovered during indexing.

---

## Language Filtering

Index only specific languages:

```json
{
  "index": {
    "languages": ["python", "javascript", "typescript"]
  }
}
```

When `languages` is empty (default), all supported languages are indexed.

---

## Limitations

- **Nested functions**: some languages (e.g., Python nested `def`, JavaScript closures) may not have full nesting support depending on the ctags version.
- **Macro-generated symbols**: C/C++ symbols generated by macros are not extracted unless ctags recognizes the pattern.
- **Dynamic languages**: duck-typed languages (Python, Ruby, JS) have limited type information in symbol signatures.
- **Template languages**: only the 14 custom parsers above are supported. Other template engines (Jinja2, Handlebars, etc.) are stripped to their embedded language before ctags processing.
