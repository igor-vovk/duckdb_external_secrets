#define DUCKDB_EXTENSION_MAIN

#include "external_secrets_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "yyjson.hpp"

namespace duckdb {
using namespace duckdb_yyjson; // NOLINT

static constexpr const char *EXTERNAL_SECRET_PROVIDER_ENV = "env";
static constexpr const char *EXTERNAL_SECRET_ENV_PREFIX = "DUCKDB_SECRET_";
static constexpr const char *EXTERNAL_SECRET_ENV_OPTION = "env_var";
static constexpr const char *GENERIC_ENV_SECRET_TYPES[] = {"s3", "azure", "gcs", "r2", "huggingface", "postgres"};

static string GetDefaultSecretEnvVarName(const string &secret_name) {
	return string(EXTERNAL_SECRET_ENV_PREFIX) + secret_name;
}

static string ResolveSecretEnvVarName(const CreateSecretInput &input) {
	auto option = input.options.find(EXTERNAL_SECRET_ENV_OPTION);
	if (option == input.options.end()) {
		return GetDefaultSecretEnvVarName(input.name);
	}
	auto env_var_name = StringValue::Get(option->second);
	if (env_var_name.empty()) {
		throw InvalidInputException("Secret option '%s' must not be empty", EXTERNAL_SECRET_ENV_OPTION);
	}
	return env_var_name;
}

static const char *TryGetSecretPayload(const string &env_var_name) {
	if (auto value = std::getenv(env_var_name.c_str())) {
		return value;
	}
	auto upper_env_var_name = StringUtil::Upper(env_var_name);
	if (upper_env_var_name == env_var_name) {
		return nullptr;
	}
	return std::getenv(upper_env_var_name.c_str());
}

static Value JSONValueToSecretValue(yyjson_val *value) {
	auto json_tag = unsafe_yyjson_get_tag(value);
	switch (json_tag) {
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC:
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE:
		return Value(string(yyjson_get_str(value), yyjson_get_len(value)));
	case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE:
	case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE:
		return Value::BOOLEAN(yyjson_get_bool(value));
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT:
		return Value::UBIGINT(unsafe_yyjson_get_uint(value));
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT:
		return Value::BIGINT(unsafe_yyjson_get_sint(value));
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL:
		return Value::DOUBLE(unsafe_yyjson_get_real(value));
	case YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE:
		return Value();
	case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
	case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE: {
		size_t len = 0;
		auto json = yyjson_val_write(value, YYJSON_WRITE_NOFLAG, &len);
		if (!json) {
			throw SerializationException("Failed to serialize nested JSON value from environment payload");
		}
		string result(json, len);
		free(json);
		return Value(result);
	}
	default:
		throw SerializationException("Unsupported JSON value in environment payload");
	}
}

static unique_ptr<BaseSecret> CreateExternalSecretFromEnv(ClientContext &context, CreateSecretInput &input) {
	(void)context;

	auto scope = input.scope;
	if (scope.empty()) {
		scope = {""};
	}

	auto env_var_name = ResolveSecretEnvVarName(input);
	auto payload = TryGetSecretPayload(env_var_name);
	if (!payload) {
		throw InvalidInputException("Environment variable '%s' was not found for secret '%s'", env_var_name,
		                            input.name);
	}

	auto doc = yyjson_read(payload, std::strlen(payload), YYJSON_READ_ALLOW_INVALID_UNICODE);
	if (!doc) {
		throw SerializationException("Failed to parse JSON from environment variable '%s'", env_var_name);
	}

	auto *root = yyjson_doc_get_root(doc);
	if (!root || yyjson_get_tag(root) != (YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE)) {
		yyjson_doc_free(doc);
		throw SerializationException("Environment variable '%s' must contain a JSON object", env_var_name);
	}

	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	size_t idx, max;
	yyjson_val *key, *value;
	yyjson_obj_foreach(root, idx, max, key, value) {
		auto key_name = string(yyjson_get_str(key), yyjson_get_len(key));
		secret->secret_map[key_name] = JSONValueToSecretValue(value);
	}
	yyjson_doc_free(doc);

	return std::move(secret);
}

static void LoadInternal(ExtensionLoader &loader) {
	for (auto *secret_type_name : GENERIC_ENV_SECRET_TYPES) {
		CreateSecretFunction env_secret_function;
		env_secret_function.secret_type = secret_type_name;
		env_secret_function.provider = EXTERNAL_SECRET_PROVIDER_ENV;
		env_secret_function.function = CreateExternalSecretFromEnv;
		env_secret_function.named_parameters[EXTERNAL_SECRET_ENV_OPTION] = LogicalType::VARCHAR;
		loader.RegisterFunction(env_secret_function);
	}
}

void ExternalSecretsExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string ExternalSecretsExtension::Name() {
	return "external_secrets";
}

std::string ExternalSecretsExtension::Version() const {
#ifdef EXT_VERSION_EXTERNAL_SECRETS
	return EXT_VERSION_EXTERNAL_SECRETS;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(external_secrets, loader) {
	duckdb::LoadInternal(loader);
}
}
