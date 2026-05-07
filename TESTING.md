# Testes no Glide

O framework de testes do Glide tem 3 camadas:

- **L1 — Unit**: lógica isolada de uma função ou struct.
- **L2 — Integration**: múltiplos módulos juntos, por exemplo `fs` + `os`.
- **L3 — Golden**: programa completo cuja `stdout` é comparada com um arquivo `.expected`.

## Layout

- Arquivos de teste ficam em `tests/` com sufixo `_test.glide`.
- Funções de teste começam com `test_` e retornam `int`.
- Retorno `0` significa pass.
- Goldens ficam em `tests/golden/` ou ao lado dos `.glide` correspondentes.
- Arquivos golden usam sufixo `.expected`.

## Rodando

```sh
./glide test
```

Roda todos os testes no diretório atual.

```sh
./glide test tests/
```

Roda os arquivos `*_test.glide` do diretório.

```sh
./glide test tests/foo_test.glide
```

Roda um arquivo de teste específico.

```sh
./glide test --golden tests/golden
```

Roda golden tests: build, run e diff da `stdout` contra o `.expected`.

## API de assertions

As assertions ficam em `stdlib::testing`.

- `assert!(cond)` — checagem booleana.
- `assert_msg!(cond, msg)` — checagem booleana com mensagem customizada. Suporta `${interp}`.
- `assert_eq!(a, b)` — igualdade para `int`, `bool` e `char`.
- `assert_str_eq!(a, b)` — igualdade para string. Use isto para strings, nunca `assert_eq!` com string.
- `assert_not!(cond)` — negação booleana.

## Camadas

### L1 — Unit

Use para testar lógica isolada de uma função ou struct.

Exemplos:

- cálculo puro;
- validação de entrada;
- método de uma struct sem dependências externas.

### L2 — Integration

Use para testar múltiplos módulos trabalhando juntos.

Exemplos:

- `fs` + `os`;
- parser + typechecker;
- biblioteca usando outra biblioteca.

### L3 — Golden

Use para testar um programa completo.

O runner compila o programa, executa e compara a `stdout` com o arquivo `.expected`.

## Escrevendo um teste

Exemplo curto:

```glide
import stdlib::testing::*;
import stdlib::math::*;

fn test_ipow_basic() -> int {
    assert_eq!(ipow(2, 10), 1024);
    return 0;
}
```

## Caveats

- O golden runner normaliza CRLF para LF, então `.expected` pode usar LF mesmo no Windows.
- Tests rodam um arquivo por vez. Cada arquivo vira um binário, então state global não cruza arquivos.
