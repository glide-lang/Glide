const PREC = {
  assign: 1,
  or: 2,
  and: 3,
  bit_or: 4,
  bit_xor: 5,
  bit_and: 6,
  equal: 7,
  compare: 8,
  shift: 9,
  add: 10,
  mul: 11,
  cast: 12,
  unary: 13,
  postfix: 14,
};

module.exports = grammar({
  name: 'glide',

  word: $ => $.identifier,

  extras: $ => [
    /\s+/,
    $.line_comment,
    $.block_comment,
  ],

  conflicts: $ => [
    [$.struct_literal, $.identifier_expr],
  ],

  rules: {
    source_file: $ => repeat($._top_item),

    _top_item: $ => choice(
      $.import_stmt,
      $.fn_decl,
      $.extern_fn,
      $.extern_type,
      $.c_include,
      $.c_link,
      $.c_raw_block,
      $.struct_decl,
      $.enum_decl,
      $.trait_decl,
      $.interface_decl,
      $.impl_decl,
      $.let_stmt,
      $.const_stmt,
      $.type_alias,
      $.macro_def,
      // Statement-level forms also accepted at the root so doc-comment code
      // snippets (which are typically fn-body fragments) parse and highlight
      // cleanly. Glide proper rejects most of these outside a fn — that's the
      // typer's job, not the grammar's.
      $.if_stmt,
      $.while_stmt,
      $.for_stmt,
      $.match_stmt,
      $.return_stmt,
      $.spawn_stmt,
      $.defer_stmt,
      $.break_stmt,
      $.continue_stmt,
      $.block,
      $.expression_statement,
    ),

    c_include: $ => seq('c_include', $.string_literal, ';'),
    c_link: $ => seq('c_link', $.string_literal, ';'),

    type_alias: $ => seq(
      optional('pub'),
      'type',
      field('name', $.identifier),
      field('target', $._type),
      ';',
    ),

    enum_decl: $ => seq(
      optional('pub'),
      'enum',
      field('name', $.identifier),
      '{',
      optional(seq(
        $.enum_variant,
        repeat(seq(',', $.enum_variant)),
        optional(','),
      )),
      '}',
    ),

    enum_variant: $ => seq(
      field('name', $.identifier),
      optional(seq(
        '(',
        $._type,
        repeat(seq(',', $._type)),
        ')',
      )),
    ),

    import_stmt: $ => seq(
      'import',
      field('path', choice($.string_literal, $.import_path)),
      optional(field('items', $.import_items)),
      ';',
    ),

    import_path: $ => seq(
      $.identifier,
      repeat(seq('::', $.identifier)),
      // `import a::b::*;` and `import a::b::{X, Y};` tail forms.
      optional(seq('::', choice('*', $.import_brace_list))),
    ),

    import_brace_list: $ => seq(
      '{',
      $.identifier,
      repeat(seq(',', $.identifier)),
      optional(','),
      '}',
    ),

    import_items: $ => seq(
      '{',
      $.identifier,
      repeat(seq(',', $.identifier)),
      optional(','),
      '}',
    ),

    // ---- declarations ----

    fn_decl: $ => seq(
      optional($.cfg_attr),
      optional('pub'),
      optional('naked'),
      'fn',
      field('name', $.identifier),
      optional($.type_params),
      field('params', $.param_list),
      optional(seq('->', field('return_type', $._type))),
      field('body', $.block),
    ),

    cfg_attr: $ => seq('@', 'cfg', '(', $.string_literal, ')'),

    type_params: $ => seq(
      '<',
      $.type_param,
      repeat(seq(',', $.type_param)),
      '>',
    ),

    type_param: $ => seq(
      field('name', $.identifier),
      optional(seq(':', $.type_bound, repeat(seq('+', $.type_bound)))),
    ),

    type_bound: $ => $.identifier,

    extern_fn: $ => seq(
      optional('pub'),
      'extern',
      optional($.string_literal),
      'fn',
      field('name', $.identifier),
      field('params', $.extern_param_list),
      optional(seq('->', field('return_type', $._type))),
      ';',
    ),

    extern_type: $ => seq(
      'extern',
      'type',
      field('name', $.identifier),
      optional(seq('=', field('c_repr', $.string_literal))),
      ';',
    ),

    extern_param_list: $ => seq(
      '(',
      optional(seq(
        $.param,
        repeat(seq(',', $.param)),
        optional(seq(',', '...')),
      )),
      ')',
    ),

    param_list: $ => seq(
      '(',
      optional(seq(
        $.param,
        repeat(seq(',', $.param)),
        optional(','),
      )),
      ')',
    ),

    param: $ => seq(
      field('name', $.identifier),
      ':',
      field('type', $._type),
    ),

    struct_decl: $ => seq(
      optional('pub'),
      'struct',
      field('name', $.identifier),
      optional($.type_params),
      '{',
      optional(seq(
        $.struct_field,
        repeat(seq(',', $.struct_field)),
        optional(','),
      )),
      '}',
    ),

    struct_field: $ => seq(
      optional('pub'),
      optional('!'),
      field('name', $.identifier),
      ':',
      field('type', $._type),
    ),

    trait_decl: $ => seq(
      optional('pub'),
      'trait',
      field('name', $.identifier),
      optional($.type_params),
      optional(seq(':', $.type_bound, repeat(seq('+', $.type_bound)))),
      '{',
      repeat(choice($.trait_method_sig, $.fn_decl)),
      '}',
    ),

    trait_method_sig: $ => seq(
      'fn',
      field('name', $.identifier),
      field('params', $.param_list),
      optional(seq('->', field('return_type', $._type))),
      ';',
    ),

    interface_decl: $ => seq(
      'interface',
      field('name', $.identifier),
      '{',
      repeat($.interface_method_sig),
      '}',
    ),

    interface_method_sig: $ => seq(
      'fn',
      field('name', $.identifier),
      field('params', $.param_list),
      optional(seq('->', field('return_type', $._type))),
      ';',
    ),

    impl_decl: $ => seq(
      'impl',
      optional($.type_params),
      field('interface', $.identifier),
      optional($.type_args),
      optional(seq('for', field('target', $._type))),
      optional(seq('in', field('namespace', $.identifier))),
      '{',
      repeat(choice($.fn_decl, $.macro_def)),
      '}',
    ),

    // `macro name!($x:expr, $($y:expr),*) { body }`
    // Or stub form `macro name!($x:expr, ...);` used by builtins to give
    // the LSP a hover target without a real expansion (codegen
    // intercepts the call directly).
    macro_def: $ => seq(
      optional('pub'),
      'macro',
      field('name', $.identifier),
      '!',
      '(',
      optional(seq(
        $._macro_matcher,
        repeat(seq(',', $._macro_matcher)),
        optional(','),
      )),
      ')',
      choice(field('body', $.block), ';'),
    ),

    _macro_matcher: $ => choice(
      $.macro_matcher_var,
      $.macro_matcher_rep,
    ),

    // `$x:expr`
    macro_matcher_var: $ => seq(
      '$',
      field('name', $.identifier),
      ':',
      field('kind', $.identifier),
    ),

    // `$($x:expr),*` — variadic with optional separator (`,` or `;`).
    macro_matcher_rep: $ => seq(
      '$',
      '(',
      '$',
      field('name', $.identifier),
      ':',
      field('kind', $.identifier),
      ')',
      optional(field('separator', choice(',', ';'))),
      '*',
    ),

    // ---- statements ----

    _statement: $ => choice(
      $.let_stmt,
      $.const_stmt,
      $.fn_decl,
      $.struct_decl,
      $.if_stmt,
      $.while_stmt,
      $.while_let_stmt,
      $.for_stmt,
      $.for_in_stmt,
      $.match_stmt,
      $.return_stmt,
      $.spawn_stmt,
      $.defer_stmt,
      $.break_stmt,
      $.continue_stmt,
      $.asm_block,
      $.c_raw_block,
      $.block,
      $.macro_rep_stmt,
      $.expression_statement,
    ),

    while_let_stmt: $ => seq(
      'while', 'let',
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
      '=',
      field('value', $._expression),
      field('body', $.block),
    ),

    for_in_stmt: $ => seq(
      'for',
      field('name', $.identifier),
      'in',
      field('start', $._expression),
      optional(seq(choice('..', '..='), field('end', $._expression))),
      field('body', $.block),
    ),

    asm_block: $ => seq(
      'asm',
      optional('volatile'),
      '{',
      repeat($.asm_line),
      optional($.asm_operands),
      '}',
    ),

    asm_line: $ => $.string_literal,

    asm_operands: $ => seq(
      ':',
      optional($.asm_operand_list),
      optional(seq(':', optional($.asm_operand_list))),
      optional(seq(':', optional($.asm_clobber_list))),
    ),

    asm_operand_list: $ => seq(
      $.asm_operand,
      repeat(seq(',', $.asm_operand)),
    ),

    asm_operand: $ => seq(
      $.string_literal,
      '(',
      $._expression,
      ')',
    ),

    asm_clobber_list: $ => seq(
      $.string_literal,
      repeat(seq(',', $.string_literal)),
    ),

    // `c_raw! { ... }` — body is captured verbatim by the Glide lexer
    // (no token interpretation inside). The tree-sitter grammar matches
    // the opener + a brace-balanced body via the recursive
    // `raw_brace_block` rule, so editor highlight stays sane.
    c_raw_block: $ => seq(
      'c_raw',
      '!',
      $.raw_brace_block,
    ),

    raw_brace_block: $ => seq(
      '{',
      repeat(choice(
        $.raw_brace_block,
        $._raw_atom,
      )),
      '}',
    ),

    _raw_atom: _ => token(prec(-1, /[^{}]+/)),

    // `$( body );*` — body repetition inside a macro def.
    macro_rep_stmt: $ => seq(
      '$',
      '(',
      repeat($._statement),
      ')',
      optional(choice(',', ';')),
      '*',
    ),

    match_stmt: $ => seq(
      'match',
      field('scrutinee', $._expression),
      '{',
      repeat($.match_arm),
      '}',
    ),

    match_arm: $ => seq(
      field('pattern', $._pattern),
      '=>',
      field('body', choice($._expression, $.block)),
      optional(','),
    ),

    _pattern: $ => choice(
      $.wildcard_pattern,
      $.variant_pattern,
      $.literal_pattern,
      $.bind_pattern,
    ),

    wildcard_pattern: _ => '_',
    bind_pattern: $ => prec(1, $.identifier),
    literal_pattern: $ => choice(
      $.number_literal,
      $.float_literal,
      $.string_literal,
      $.char_literal,
      $.bool_literal,
      $.null_literal,
    ),
    variant_pattern: $ => prec(2, seq(
      optional(seq($.identifier, '::')),
      $.identifier,
      optional(seq(
        '(',
        optional(seq(
          $._pattern,
          repeat(seq(',', $._pattern)),
          optional(','),
        )),
        ')',
      )),
    )),

    let_stmt: $ => seq(
      'let',
      optional('mut'),
      field('name', $.identifier),
      optional(field('auto_drop', '*')),
      optional(seq(':', field('type', $._type))),
      optional(seq('=', field('value', $._expression))),
      ';',
    ),

    const_stmt: $ => seq(
      optional('pub'),
      'const',
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
      '=',
      field('value', $._expression),
      ';',
    ),

    if_stmt: $ => seq(
      'if',
      field('condition', $._expression),
      field('consequence', $.block),
      optional(field('alternative', seq('else', choice($.if_stmt, $.block)))),
    ),

    while_stmt: $ => seq(
      'while',
      field('condition', $._expression),
      field('body', $.block),
    ),

    for_stmt: $ => seq(
      'for',
      optional($._for_init),
      ';',
      optional(field('condition', $._expression)),
      ';',
      optional(field('step', $._expression)),
      field('body', $.block),
    ),

    _for_init: $ => choice(
      seq(
        'let',
        optional('mut'),
        field('name', $.identifier),
        optional(seq(':', field('type', $._type))),
        optional(seq('=', field('value', $._expression))),
      ),
      $._expression,
    ),

    return_stmt: $ => seq('return', optional($._expression), ';'),
    spawn_stmt:  $ => seq('spawn', $._expression, ';'),
    defer_stmt:  $ => seq('defer', $._expression, ';'),
    break_stmt:  $ => seq('break', ';'),
    continue_stmt: $ => seq('continue', ';'),

    block: $ => seq('{', repeat($._statement), '}'),

    expression_statement: $ => seq($._expression, ';'),

    // ---- types ----

    _type: $ => choice(
      $.named_type,
      $.self_type,
      $.dyn_type,
      $.pointer_type,
      $.borrow_type,
      $.borrow_mut_type,
      $.chan_type,
      $.slice_type,
      $.fn_ptr_type,
      $.result_type,
    ),

    self_type: _ => 'Self',

    dyn_type: $ => seq('dyn', field('trait', $.identifier)),

    result_type: $ => seq('!', $._type),

    named_type:      $ => prec.right(seq(
      $.identifier,
      optional($.type_args),
    )),

    type_args: $ => prec.right(seq('<', $._type, repeat(seq(',', $._type)), '>')),
    pointer_type:    $ => seq('*', $._type),
    borrow_type:     $ => prec.right(seq('&', $._type)),
    borrow_mut_type: $ => prec.right(seq('&', 'mut', $._type)),
    chan_type:       $ => seq('chan', '<', $._type, '>'),
    slice_type:      $ => seq('[', ']', $._type),
    fn_ptr_type:     $ => seq(
      'fn',
      '(',
      optional(seq(
        $._fn_ptr_param,
        repeat(seq(',', $._fn_ptr_param)),
        optional(','),
      )),
      ')',
      optional(seq('->', field('return_type', $._type))),
    ),
    _fn_ptr_param: $ => choice(
      seq($.identifier, ':', $._type),
      $._type,
    ),

    // ---- expressions ----

    _expression: $ => choice(
      $.assignment,
      $._binary_or_unary,
    ),

    assignment: $ => prec.right(PREC.assign, seq(
      field('lhs', $._binary_or_unary),
      field('op', choice('=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>=')),
      field('rhs', $._expression),
    )),

    _binary_or_unary: $ => choice(
      $.binary_expr,
      $.unary_expr,
      $.cast_expr,
      $._postfix_chain,
    ),

    binary_expr: $ => choice(
      ...[
        ['||', PREC.or],
        ['&&', PREC.and],
        ['|',  PREC.bit_or],
        ['^',  PREC.bit_xor],
        ['&',  PREC.bit_and],
        ['==', PREC.equal],
        ['!=', PREC.equal],
        ['<',  PREC.compare],
        ['<=', PREC.compare],
        ['>',  PREC.compare],
        ['>=', PREC.compare],
        ['<<', PREC.shift],
        ['>>', PREC.shift],
        ['+',  PREC.add],
        ['-',  PREC.add],
        ['*',  PREC.mul],
        ['/',  PREC.mul],
        ['%',  PREC.mul],
      ].map(([op, p]) => prec.left(p, seq(
        field('left', $._binary_or_unary),
        field('op', op),
        field('right', $._binary_or_unary),
      ))),
    ),

    unary_expr: $ => prec(PREC.unary, seq(
      field('op', choice('-', '!', '~', '*', '&', seq('&', 'mut'))),
      field('operand', $._binary_or_unary),
    )),

    cast_expr: $ => prec.left(PREC.cast, seq(
      $._binary_or_unary,
      'as',
      $._type,
    )),

    _postfix_chain: $ => choice(
      $._primary,
      $.call_expr,
      $.index_expr,
      $.member_expr,
      $.postfix_expr,
      $.method_macro_call,
    ),

    call_expr: $ => prec(PREC.postfix, seq(
      field('callee', $._postfix_chain),
      field('args', $.argument_list),
    )),

    argument_list: $ => seq(
      '(',
      optional(seq(
        $._expression,
        repeat(seq(',', $._expression)),
        optional(','),
      )),
      ')',
    ),

    index_expr: $ => prec(PREC.postfix, seq(
      field('object', $._postfix_chain),
      '[',
      field('index', $._expression),
      ']',
    )),

    member_expr: $ => prec(PREC.postfix, seq(
      field('object', $._postfix_chain),
      '.',
      // Plain field, or qualified `NS::method` for impl-on-primitive
      // namespaced calls (e.g. `seven.tour::squared()`).
      choice(
        field('field', $.identifier),
        seq(
          field('namespace', $.identifier),
          '::',
          field('field', $.identifier),
        ),
      ),
    )),

    postfix_expr: $ => prec(PREC.postfix, seq(
      $._postfix_chain,
      field('op', choice('++', '--', '?')),
    )),

    _primary: $ => choice(
      $.parenthesized,
      $.array_literal,
      $.macro_call,
      $.path_macro_call,
      $.path_expr,
      $.struct_literal,
      $.new_expr,
      $.sizeof_expr,
      $.fn_expr,
      $.macro_var_expr,
      $.identifier_expr,
      $.number_literal,
      $.float_literal,
      $.string_literal,
      $.char_literal,
      $.bool_literal,
      $.null_literal,
    ),

    // `$ident` placeholder inside a macro body. Outside a macro body this
    // would be a parse error in Glide proper; we accept it here so highlighting
    // stays sane while authoring.
    macro_var_expr: $ => seq('$', field('name', $.identifier)),

    fn_expr: $ => seq(
      optional('move'),
      'fn',
      field('params', $.param_list),
      optional(seq('->', field('return_type', $._type))),
      field('body', $.block),
    ),

    // `Type::member` and `a::b::c::member` qualified path expressions.
    // Multi-segment form is used by qualified imports
    // (`stdlib::math::min_int(…)`).
    path_expr: $ => prec(20, seq(
      field('type', $.identifier),
      '::',
      repeat(seq($.identifier, '::')),
      field('member', $.identifier),
    )),

    macro_call: $ => prec(20, seq(
      field('name', $.identifier),
      '!',
      '(',
      optional(seq(
        $._expression,
        repeat(seq(',', $._expression)),
        optional(','),
      )),
      ')',
    )),

    // `recv.name!(args)` — type-attached macro called instance-style.
    method_macro_call: $ => prec(PREC.postfix, seq(
      field('object', $._postfix_chain),
      '.',
      field('name', $.identifier),
      '!',
      '(',
      optional(seq(
        $._expression,
        repeat(seq(',', $._expression)),
        optional(','),
      )),
      ')',
    )),

    // `Type::name!(args)` — type-attached macro called via path qualifier.
    path_macro_call: $ => prec(20, seq(
      field('type', $.identifier),
      '::',
      field('name', $.identifier),
      '!',
      '(',
      optional(seq(
        $._expression,
        repeat(seq(',', $._expression)),
        optional(','),
      )),
      ')',
    )),

    array_literal: $ => seq(
      '[',
      optional(seq(
        $._expression,
        repeat(seq(',', $._expression)),
        optional(','),
      )),
      ']',
    ),

    parenthesized: $ => seq('(', $._expression, ')'),

    // No `prec` here so identifier_expr + block (e.g. `if abc { ... }`)
    // wins via conflict resolution when struct_literal can't parse the body.
    struct_literal: $ => seq(
      field('type', $.identifier),
      '{',
      $.struct_lit_field,
      repeat(seq(',', $.struct_lit_field)),
      optional(','),
      '}',
    ),

    struct_lit_field: $ => seq(
      field('name', $.identifier),
      ':',
      field('value', $._expression),
    ),

    new_expr: $ => seq(
      'new',
      field('type', $.identifier),
      '{',
      optional(seq(
        $.struct_lit_field,
        repeat(seq(',', $.struct_lit_field)),
        optional(','),
      )),
      '}',
    ),

    sizeof_expr: $ => seq('sizeof', '(', $._type, ')'),

    identifier_expr: $ => $.identifier,

    // ---- tokens ----

    identifier: _ => /[A-Za-z_][A-Za-z0-9_]*/,

    number_literal: _ => token(choice(
      /0[xX][0-9A-Fa-f_]+/,
      /0[bB][01_]+/,
      /0[oO][0-7_]+/,
      /[0-9][0-9_]*/,
    )),

    float_literal: _ => token(choice(
      /[0-9][0-9_]*\.[0-9][0-9_]*([eE][+-]?[0-9_]+)?/,
      /[0-9][0-9_]*[eE][+-]?[0-9_]+/,
    )),

    // String literal — supports `${expr}` interpolation segments. The
    // grammar treats the placeholder as an opaque inner span; the
    // compiler lowers each `${...}` to a `format!` arg.
    string_literal: _ => token(seq(
      '"',
      repeat(choice(/[^"\\\n]/, /\\./)),
      '"',
    )),

    char_literal: _ => token(seq(
      "'",
      choice(/[^'\\]/, /\\./),
      "'",
    )),

    bool_literal: _ => choice('true', 'false'),
    null_literal: _ => 'null',

    line_comment:  _ => token(seq('//', /[^\n]*/)),
    block_comment: _ => token(seq('/*', /[^*]*\*+([^/*][^*]*\*+)*/, '/')),
  },
});
