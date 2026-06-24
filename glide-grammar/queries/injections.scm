; The body of `c_raw! { ... }` is verbatim C, captured by the lexer and passed
; straight to the C backend. Inject the C grammar so the editor highlights it
; as real C instead of one neutral block color.
(c_raw_block
  (raw_brace_block) @injection.content
  (#set! injection.language "c"))
