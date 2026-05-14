# External Secrets for 🦆 DuckDB

`external_secrets` extension adds two generic secret providers to DuckDB:

- `env` supports loading secret payloads from environment variables.
- `file` supports loading secret payloads from JSON or YAML files.

The goal: let the secret values come from places that work well in Kubernetes, Docker, CI, and other container-based 
environments. These platforms commonly attach secrets as environment variables or mounted text files; this extension 
makes those forms usable as DuckDB secrets.

## Motivation

In practice, there are two common ways to create secrets today:

- `CREATE SECRET` SQL statement, where the credentials are written directly in SQL.
- `CREATE PERSISTENT SECRET`, where DuckDB stores the secret under `~/.duckdb/stored_secrets`.

Both are problematic for containerized deployments (and
with [Quack](https://duckdb.org/library/super-secret-next-big-thing-for-duckdb/) this is where everything seem to be
going). Pasting secrets in plain SQL can be logged, copied into notebooks, or checked into scripts by accident. 
Persistent secrets avoid this, but DuckDB stores them as binary files, which makes them hard to manage with tools that
expect plain structured secret values (Kubernetes `Secret` definitions, Docker-Compose files, env-files etc.).

Most deployment systems already know how to pass a secret like this:

```yaml
type: s3
name: warehouse_s3
scope: s3://warehouse/
key_id: AKIA...
secret: ...
region: eu-west-1
```

`external_secrets` bridges that gap. DuckDB still sees a normal typed secret, while the runtime provides the payload as
JSON or YAML.

See also DuckDB's Secrets Manager documentation:
https://duckdb.org/docs/current/configuration/secrets_manager.html

## Quick start: environment variable

The `env` provider reads a JSON object from an environment variable and maps the top-level keys into the DuckDB secret.

```sh
export DUCKDB_SECRET_warehouse_s3='{"key_id":"AKIA...","secret":"...","region":"eu-west-1"}'
```

In DuckDB:
```sql
CREATE SECRET warehouse_s3 (
    TYPE s3,
    PROVIDER env,
    SCOPE 's3://warehouse/'
);
```

By default, the provider looks for `DUCKDB_SECRET_<secret_name_uppercase>`, in this case `DUCKDB_SECRET_WAREHOUSE_S3`.

There is `ENV_VAR` option when the environment variable name should not depend on the DuckDB secret name:

```sql
CREATE SECRET warehouse_s3 (
    TYPE s3,
    PROVIDER env,
    ENV_VAR 'AWS_WAREHOUSE_SECRET',
    SCOPE 's3://warehouse/'
);
```

Environment variable payloads must be JSON objects.

## Quick start: JSON/YAML file

The `file` provider reads a secret payload from disk. 

`/secrets/s3.yaml`:
```yaml
type: s3
name: warehouse_s3
scope: s3://warehouse/
key_id: AKIA...
secret: ...
region: eu-west-1
```

Then in DuckDB:
```sql
CREATE SECRET warehouse_s3 (
    TYPE s3,
    PROVIDER file,
    PATH '/secrets/s3.yaml'
);
```

`PATH` is required. `FORMAT` is optional:

```sql
CREATE SECRET warehouse_s3 (
    TYPE s3,
    PROVIDER file,
    PATH '/secrets/s3-secret',
    FORMAT 'yaml'
);
```

If `FORMAT` is omitted or set to `auto`, the extension infers the format from the file extension.

## Kubernetes examples

Environment variable injection:

```yaml
apiVersion: v1
kind: Secret
metadata:
  name: duckdb-s3
type: Opaque
stringData:
  DUCKDB_SECRET_WAREHOUSE_S3: |
    {"key_id":"AKIA...","secret":"...","region":"eu-west-1"}
```

Then in Deployment:
```yaml
envFrom:
  - secretRef:
      name: duckdb-s3
```

DuckDB SQL to attach secret:

```sql
CREATE SECRET warehouse_s3 (
    TYPE s3,
    PROVIDER env,
    SCOPE 's3://warehouse/'
);
```

Mounted file injection writes the `s3.yaml` key to `/secrets/s3.yaml` inside the container:

```yaml
apiVersion: v1
kind: Secret
metadata:
  name: duckdb-s3-file
type: Opaque
stringData:
  s3.yaml: |
    type: s3
    name: warehouse_s3
    scope: s3://warehouse/
    key_id: AKIA...
    secret: ...
    region: eu-west-1

---

apiVersion: v1
kind: Pod
metadata:
  name: duckdb
spec:
  containers:
    - name: duckdb
      image: duckdb/duckdb:latest
      volumeMounts:
        - name: duckdb-s3-secret
          mountPath: /secrets
          readOnly: true
  volumes:
    - name: duckdb-s3-secret
      secret:
        secretName: duckdb-s3-file
```

Then attach DuckDB secret from the mounted file:

```sql
CREATE SECRET warehouse_s3 (
    TYPE s3,
    PROVIDER file,
    PATH '/secrets/s3.yaml'
);
```

This keeps the SQL stable while the deployment platform owns the secret value.

## Payload format

Payloads are mappings. Top-level keys become DuckDB secret key/value pairs, with a few metadata keys handled specially.

- `type` is optional metadata and is validated against the requested DuckDB secret type when present.
- `name` is optional metadata and is ignored by the provider.
- `provider` is optional metadata and is ignored by the provider.
- `scope` is optional metadata and is used when the SQL statement does not specify `SCOPE`.

JSON example:

```json
{
  "type": "s3",
  "name": "warehouse_s3",
  "scope": "s3://warehouse/",
  "key_id": "AKIA...",
  "secret": "...",
  "region": "eu-west-1"
}
```

YAML example:

```yaml
type: postgres
name: reporting_db
scope:
  - postgres://primary/
  - postgres://replica/
host: postgres.example.com
port: 5432
database: analytics
user: duckdb
password: ...
```

If both SQL and the payload specify `scope`, the SQL `SCOPE` wins.

## Supported secret types

The extension registers `PROVIDER env` and `PROVIDER file` for:

- `s3`
- `azure`
- `gcs`
- `r2`
- `huggingface`
- `postgres`

The underlying DuckDB extension for the secret type still needs to be available when DuckDB requires one. For example:

- `s3`, `gcs`, `r2`, and `huggingface` are provided by DuckDB's `httpfs` extension.
- `postgres` is provided by DuckDB's `postgres` extension.

The `http` secret type is intentionally not registered here because DuckDB already has a built-in `http/env` provider.

## Build and test

Build the extension:

```sh
make
```

Run SQL tests:

```sh
make test
```

Useful targets:

- `make`
- `make test`
- `make test_debug`

Current SQL tests live in:

- `test/sql/external_env_s3.test`
- `test/sql/external_file_s3.test`
- `test/sql/external_env_postgres.test`
- `test/sql/external_file_postgres.test`
## Notes

- Environment variable payloads are JSON only.
- File payloads can be JSON or YAML.
- The file provider requires `PATH`.
- `FORMAT` accepts `auto`, `json`, `yaml`, or `yml`.
- These providers create normal DuckDB secrets; they only change where the payload comes from.

🦆
