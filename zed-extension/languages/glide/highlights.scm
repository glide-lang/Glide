; Keywords
[
  "fn"
  "let"
  "mut"
  "const"
  "struct"
  "trait"
  "interface"
  "impl"
  "import"
  "in"
  "pub"
  "enum"
  "match"
  "defer"
  "if"
  "else"
  "while"
  "for"
  "break"
  "continue"
  "return"
  "spawn"
  "chan"
  "as"
  "sizeof"
  "new"
  "move"
  "type"
  "extern"
  "c_include"
  "c_link"
  "c_raw"
  "macro"
  "naked"
  "asm"
  "volatile"
  "dyn"
  "defer_err"
] @keyword

; Self type
(self_type) @type.builtin

; @cfg(...) attribute
(cfg_attr "@" @attribute "cfg" @attribute)

; Generic proc-macro attributes: @derive(...), @handler, @proc_macro,
; @proc_derive(Name), etc. The `@` and the attribute name share the
; @attribute color so the whole tag reads as one token.
(proc_attr "@" @attribute name: (identifier) @attribute)

; trait + supertraits
(trait_decl name: (identifier) @type)
(trait_method_sig name: (identifier) @function)
(type_bound (identifier) @type)
(type_param name: (identifier) @type.parameter)

; dyn Trait
(dyn_type "dyn" @keyword
          trait: (identifier) @type)

; Inline asm strings get treated as embedded asm
(asm_block (asm_line (string_literal) @string.special))
(asm_operand (string_literal) @string.special)

; c_raw block contents — neutral so the rest of the editor doesn't
; try to color C as Glide.
(raw_brace_block) @comment.block

; Namespaced method call: `obj.NS::method()` — NS is left without a
; capture so the editor renders it in the default identifier color
; (typically white). Most themes paint @namespace the same purple as
; @type, which makes modules and types indistinguishable.
(member_expr field: (identifier) @function.method)

; Built-in / known primitive types
((identifier) @type.builtin
 (#match? @type.builtin "^(int|uint|long|ulong|i8|i16|i32|i64|u8|u16|u32|u64|usize|isize|f32|f64|float|bool|char|string|void)$"))

; Constants
(bool_literal) @boolean
(null_literal) @constant.builtin

; Literals
(number_literal) @number
(float_literal)  @number.float
(string_literal) @string
(char_literal)   @string

; Comments
(line_comment)  @comment
(block_comment) @comment

; Types
(named_type (identifier) @type)
(chan_type)
"chan" @type.builtin

; Operators / punctuation (generic — overridden by more specific captures below)
[
  "+" "-" "/" "%" "*"
  "==" "!=" "<" "<=" ">" ">="
  "&&" "||" "??"
  "&" "|" "^" "~" "<<" ">>"
  "=" "+=" "-=" "*=" "/=" "%=" "&=" "|=" "^=" "<<=" ">>="
  "++" "--"
  "->"
] @operator

[ "{" "}" "(" ")" "[" "]" ] @punctuation.bracket
[ ";" "," ":" "." "::" ] @punctuation.delimiter

; `!` as unary not (when it appears as a unary op)
(unary_expr "!" @operator)

; Function declaration & calls
(fn_decl   name: (identifier) @function)
(extern_fn name: (identifier) @function)
(extern_type name: (identifier) @type)
(call_expr callee: (identifier_expr (identifier) @function.call))

; Struct / interface / impl
(struct_decl name: (identifier) @type)
(interface_decl name: (identifier) @type)
(interface_method_sig name: (identifier) @function)
(impl_decl interface: (identifier) @type)
(struct_field name: (identifier) @property)
(struct_lit_field name: (identifier) @property)
(struct_literal type: (identifier) @type)
(new_expr type: (identifier) @type)

; Members
(member_expr field: (identifier) @property)

; Method calls (override the @property above for call_expr targets)
(call_expr
  callee: (member_expr field: (identifier) @function.method))

; Params and locals
(param name: (identifier) @variable.parameter)
(let_stmt name: (identifier) @variable)
(const_stmt name: (identifier) @constant)

; `import stdlib::hashmap::{X, Y};` — selective imports tag the leaves
; as types since they refer to imported items.
(import_brace_list (identifier) @type)
(import_items (identifier) @type)

; `hashmap::HashMap::new()` — color each segment by role:
;   * lower-case (modules) are LEFT UNCAPTURED so they fall back to the
;     default identifier color (white in most themes); using @namespace
;     here ends up purple in themes that share a color between
;     @namespace and @type, making modules indistinguishable from types.
;   * upper-case → @type (struct / trait / enum)
;   * trailing member → @function (call target)
((path_expr type: (identifier) @type)
 (#match? @type "^[A-Z]"))

((path_expr part: (identifier) @type)
 (#match? @type "^[A-Z]"))

(path_expr member: (identifier) @function)

; Macro calls (placed last so `!` and macro name override the generic
; operator/identifier captures above)
(macro_call name: (identifier) @function.macro
            "!" @function.macro)

(method_macro_call name: (identifier) @function.macro
                   "!" @function.macro)

; Path macro call: `Type::name!(args)` or `module::name!(args)`.
; Lower-case (module) is left uncaptured (default color); upper-case
; (type) gets @type. Macro name itself stays @function.macro.
((path_macro_call type: (identifier) @type
                  name: (identifier) @function.macro
                  "!" @function.macro)
 (#match? @type "^[A-Z]"))
(path_macro_call name: (identifier) @function.macro
                 "!" @function.macro)

; Macro definition: `macro name!(matchers) { body }`
(macro_def name: (identifier) @function.macro
           "!" @function.macro)

; Matcher binders: `$x:expr` and the `$($x:expr),*` form
(macro_matcher_var
  "$" @punctuation.special
  name: (identifier) @variable.parameter
  kind: (identifier) @type.builtin)

(macro_matcher_rep
  "$" @punctuation.special
  name: (identifier) @variable.parameter
  kind: (identifier) @type.builtin
  "*" @punctuation.special)

; Body placeholder `$x` and repetition `$( ... );*`
(macro_var_expr
  "$" @punctuation.special
  name: (identifier) @variable.parameter)

(macro_rep_stmt
  "$" @punctuation.special
  "*" @punctuation.special)
