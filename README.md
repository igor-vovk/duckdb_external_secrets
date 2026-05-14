# External Secrets for DuckDB



`external_secrets` adds generic secret providers to DuckDB so secrets can be sourced outside SQL text and reused across
built-in DuckDB secret types.

The extension provides:

- Secret loading from environment variables
- Loading secrets from `yaml` or `json` files
- `PROVIDER file` for `s3`, `azure`, `gcs`, `r2`, `huggingface`, and `postgres`
- JSON-decoded secret payloads from environment variables
- JSON and YAML file payloads loaded from disk
- a default env var naming convention of `DUCKDB_SECRET_<secret_name>`
- an `ENV_VAR '...'` override when the secret name should not determine the env var name
- a required `PATH '...'` option for file-backed secrets
- optional file format detection from `.json`, `.yaml`, or `.yml`

## Motivation

DuckDB supports 2 main ways of creating secrets:
* via SQL-query `CREATE SECRET`, with specifying secret details such as sensitive tokens in the SQL query itself. 
  This is often suboptimal, because sql queries can be logged, or 
but in many setups the secret values themselves should come from an external
source instead of being embedded directly in SQL or stored in DuckDB-managed binary secret files.

This extension keeps the DuckDB secret model, but lets the actual payload come from another provider.

## Env provider

The `env` provider reads a JSON object from an environment variable and maps the top-level keys into the created DuckDB
secret.

Default lookup:

```sql
CREATE SECRET my_s3 (
    TYPE s3,
    PROVIDER env,
    SCOPE 's3://my-bucket/'
);
```

It looks for env variables with `DUCKDB_SECRET_` prefix ending with the name of the secret, e.g., `DUCKDB_SECRET_MY_S3`

Env variables can be passed explicitly:

```sql
CREATE SECRET bucket_access_secret (
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

## File provider

The `file` provider reads a secret payload from a file on disk.

Supported formats:

- JSON object payloads
- YAML mappings

The file provider requires `PATH '...'` and optionally accepts `FORMAT 'json'` or `FORMAT 'yaml'`. If `FORMAT` is
omitted, the extension infers the format from the file extension.

JSON example:

```sql
CREATE SECRET bucket_file_secret (
    TYPE s3,
    PROVIDER file,
    PATH 'test/fixtures/s3_secret.json'
);
```

YAML example:

```sql
CREATE SECRET postgres_file_secret (
    TYPE postgres,
    PROVIDER file,
    PATH 'test/fixtures/postgres_secret.yaml'
);
```

The file payload can include dbt-style metadata keys such as:

- `type`
- `name`
- `scope`

Current normalization rules:

- `type` is validated against the requested DuckDB secret type
- `name` is treated as metadata and ignored
- `scope` is used when SQL scope is omitted
- the remaining top-level keys become secret key/value pairs

Example YAML payload:

```yaml
type: postgres
name: reporting_db_secret
scope:
  - postgres://yaml-host/
host: yaml-host
port: 5432
database: yaml_db
user: yaml_user
password: yaml_password
```

## Supported secret types

The extension currently registers `PROVIDER env` and `PROVIDER file` for:

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

- `test/sql/external_env_s3.test`
- `test/sql/external_file_s3.test`
- `test/sql/external_env_postgres.test`
- `test/sql/external_file_postgres.test`

They cover:

- explicit `ENV_VAR '...'`
- implicit env var name resolution from the secret name
- missing env var handling
- JSON file lookup
- YAML file lookup
- missing file handling
- scope resolution through `which_secret(...)`

Test fixtures are provided through:

- `test/local_test_env.mk`
- `test/fixtures/s3_secret.json`
- `test/fixtures/postgres_secret.yaml`

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

The extension is now provider-oriented rather than env-only.

Near-term direction:

- refine the file-provider payload conventions as more dbt/Kubernetes use-cases come in
- keep provider parsing separate from secret-object construction
- expand coverage for additional supported DuckDB secret types if needed
