# External Secrets DuckDB extension

`external_secrets` adds generic secret providers to DuckDB so secrets can be sourced outside SQL text and reused across
built-in DuckDB secret types.

Today the extension provides:

- `PROVIDER env` for `s3`, `azure`, `gcs`, `r2`, `huggingface`, and `postgres`
- JSON-decoded secret payloads from environment variables
- a default env var naming convention of `DUCKDB_SECRET_<secret_name>`
- an `ENV_VAR '...'` override when the secret name should not determine the env var name

## Motivation

DuckDB already supports `CREATE SECRET`, but in many setups the secret values themselves should come from an external
source instead of being embedded directly in SQL or stored in DuckDB-managed binary secret files.

This extension keeps the DuckDB secret model, but lets the actual payload come from another provider.

## Env provider

The `env` provider reads a JSON object from an environment variable and maps the top-level keys into the created DuckDB
secret.

Default lookup:

```sql
CREATE
SECRET s3_env_secret (
    TYPE s3,
    PROVIDER env,
    SCOPE 's3://my-bucket/'
);
```

This looks for:

```text
DUCKDB_SECRET_s3_env_secret
```

An uppercased fallback is also accepted, so in practice `DUCKDB_SECRET_S3_ENV_SECRET` works too.

Explicit env var override:

```sql
CREATE
SECRET bucket_access_secret (
    TYPE s3,
    PROVIDER env,
    ENV_VAR 'DUCKDB_SECRET_S3_ENV_SECRET',
    SCOPE 's3://my-bucket/'
);
```

Example payload:

```json
{
  "key_id": "abc123",
  "secret": "shhh"
}
```

The provider currently expects the environment variable payload to be a JSON object.

## Supported secret types

The extension currently registers `PROVIDER env` for:

- `s3`
- `azure`
- `gcs`
- `r2`
- `huggingface`
- `postgres`

The underlying DuckDB extension for the secret type still needs to exist when applicable. For example:

- `s3` support comes from `httpfs`
- `postgres` support comes from `postgres`

## Tests

Current SQL tests live in:

- `test/sql/external_secrets_s3.test`
- `test/sql/external_secrets_postgres.test`

They cover both behaviors for each type:

- explicit `ENV_VAR '...'`
- implicit env var name resolution from the secret name

Test fixtures are provided through:

- `test/local_test_env.mk`

The `Makefile` includes that file only for test-related targets.

Run tests with:

```sh
make test
```

## Build

Build the extension with:

```sh
make
```

Useful targets:

- `make`
- `make test`
- `make test_debug`

## Roadmap

The next provider planned for this extension is file-based lookup.

Planned direction:

- read secret payloads from files instead of env vars
- support both JSON and YAML file formats
- keep the same generic mapping into DuckDB secret key/value pairs

That means the extension is moving toward a small family of external secret providers rather than env-only secret
injection.
