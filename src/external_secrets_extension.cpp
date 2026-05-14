#define DUCKDB_EXTENSION_MAIN

#include "external_secrets_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "yyjson.hpp"
#include <cctype>
#include <cstring>

namespace duckdb {
using namespace duckdb_yyjson; // NOLINT

static constexpr const char *EXTERNAL_SECRET_PROVIDER_ENV = "env";
static constexpr const char *EXTERNAL_SECRET_PROVIDER_FILE = "file";
static constexpr const char *EXTERNAL_SECRET_ENV_PREFIX = "DUCKDB_SECRET_";
static constexpr const char *EXTERNAL_SECRET_ENV_OPTION = "env_var";
static constexpr const char *EXTERNAL_SECRET_FILE_PATH_OPTION = "path";
static constexpr const char *EXTERNAL_SECRET_FILE_FORMAT_OPTION = "format";
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

struct ParsedSecretPayload {
	vector<string> scope;
	case_insensitive_map_t<Value> values;
};

struct YAMLValue {
	enum class Type : uint8_t { NULL_VALUE, SCALAR, MAP, LIST };

	Type type = Type::NULL_VALUE;
	string scalar_value;
	vector<pair<string, YAMLValue>> map_entries;
	vector<YAMLValue> list_entries;
};

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

static string SerializeJSONValue(yyjson_mut_val *value, yyjson_mut_doc *doc) {
	yyjson_mut_doc_set_root(doc, value);
	size_t len = 0;
	auto json = yyjson_mut_val_write(value, YYJSON_WRITE_NOFLAG, &len);
	if (!json) {
		throw SerializationException("Failed to serialize normalized secret payload");
	}
	string result(json, len);
	free(json);
	return result;
}

static string TrimCopy(const string &value) {
	idx_t start = 0;
	while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
		start++;
	}
	idx_t end = value.size();
	while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
		end--;
	}
	return value.substr(start, end - start);
}

static string StripInlineComment(const string &value) {
	bool in_single_quotes = false;
	bool in_double_quotes = false;
	for (idx_t i = 0; i < value.size(); i++) {
		auto ch = value[i];
		if (ch == '\'' && !in_double_quotes) {
			in_single_quotes = !in_single_quotes;
			continue;
		}
		if (ch == '"' && !in_single_quotes) {
			in_double_quotes = !in_double_quotes;
			continue;
		}
		if (ch == '#' && !in_single_quotes && !in_double_quotes) {
			if (i == 0 || std::isspace(static_cast<unsigned char>(value[i - 1]))) {
				return TrimCopy(value.substr(0, i));
			}
		}
	}
	return TrimCopy(value);
}

static string UnquoteYAMLScalar(const string &value) {
	if (value.size() >= 2 &&
	    ((value.front() == '\'' && value.back() == '\'') || (value.front() == '"' && value.back() == '"'))) {
		return value.substr(1, value.size() - 2);
	}
	return value;
}

struct YAMLLine {
	idx_t indent;
	string content;
};

static vector<YAMLLine> TokenizeYAML(const string &payload_text) {
	vector<YAMLLine> lines;
	for (const auto &raw_line : StringUtil::Split(payload_text, '\n')) {
		idx_t indent = 0;
		while (indent < raw_line.size() && raw_line[indent] == ' ') {
			indent++;
		}
		auto stripped = StripInlineComment(raw_line.substr(indent));
		if (stripped.empty()) {
			continue;
		}
		lines.push_back({indent, stripped});
	}
	return lines;
}

static optional_idx FindYAMLKeySeparator(const string &content) {
	bool in_single_quotes = false;
	bool in_double_quotes = false;
	for (idx_t i = 0; i < content.size(); i++) {
		auto ch = content[i];
		if (ch == '\'' && !in_double_quotes) {
			in_single_quotes = !in_single_quotes;
			continue;
		}
		if (ch == '"' && !in_single_quotes) {
			in_double_quotes = !in_double_quotes;
			continue;
		}
		if (ch == ':' && !in_single_quotes && !in_double_quotes) {
			if (i + 1 == content.size() || std::isspace(static_cast<unsigned char>(content[i + 1]))) {
				return i;
			}
		}
	}
	return optional_idx();
}

static YAMLValue ParseYAMLBlock(const vector<YAMLLine> &lines, idx_t &index, idx_t indent);

static YAMLValue ParseYAMLScalar(const string &value_text) {
	YAMLValue result;
	if (value_text == "null" || value_text == "Null" || value_text == "NULL" || value_text == "~") {
		result.type = YAMLValue::Type::NULL_VALUE;
		return result;
	}
	result.type = YAMLValue::Type::SCALAR;
	result.scalar_value = UnquoteYAMLScalar(value_text);
	return result;
}

static YAMLValue ParseYAMLSequence(const vector<YAMLLine> &lines, idx_t &index, idx_t indent) {
	YAMLValue result;
	result.type = YAMLValue::Type::LIST;
	while (index < lines.size()) {
		const auto &line = lines[index];
		if (line.indent < indent) {
			break;
		}
		if (line.indent != indent || !StringUtil::StartsWith(line.content, "- ")) {
			break;
		}
		auto item_text = TrimCopy(line.content.substr(2));
		index++;
		if (item_text.empty()) {
			if (index >= lines.size() || lines[index].indent <= indent) {
				throw SerializationException("Invalid YAML sequence indentation in secret file");
			}
			result.list_entries.push_back(ParseYAMLBlock(lines, index, lines[index].indent));
			continue;
		}
		result.list_entries.push_back(ParseYAMLScalar(item_text));
	}
	return result;
}

static YAMLValue ParseYAMLMap(const vector<YAMLLine> &lines, idx_t &index, idx_t indent) {
	YAMLValue result;
	result.type = YAMLValue::Type::MAP;
	while (index < lines.size()) {
		const auto &line = lines[index];
		if (line.indent < indent) {
			break;
		}
		if (line.indent != indent || StringUtil::StartsWith(line.content, "- ")) {
			break;
		}
		auto separator = FindYAMLKeySeparator(line.content);
		if (!separator.IsValid()) {
			throw SerializationException("Invalid YAML mapping entry '%s' in secret file", line.content);
		}
		auto key = TrimCopy(line.content.substr(0, separator.GetIndex()));
		auto value_text = TrimCopy(line.content.substr(separator.GetIndex() + 1));
		index++;
		if (value_text.empty()) {
			if (index >= lines.size() || lines[index].indent <= indent) {
				YAMLValue null_value;
				null_value.type = YAMLValue::Type::NULL_VALUE;
				result.map_entries.emplace_back(key, std::move(null_value));
				continue;
			}
			result.map_entries.emplace_back(key, ParseYAMLBlock(lines, index, lines[index].indent));
			continue;
		}
		result.map_entries.emplace_back(key, ParseYAMLScalar(value_text));
	}
	return result;
}

static YAMLValue ParseYAMLBlock(const vector<YAMLLine> &lines, idx_t &index, idx_t indent) {
	if (index >= lines.size()) {
		throw SerializationException("Unexpected end of YAML secret file");
	}
	if (StringUtil::StartsWith(lines[index].content, "- ")) {
		return ParseYAMLSequence(lines, index, indent);
	}
	return ParseYAMLMap(lines, index, indent);
}

static Value YAMLValueToSecretValue(const YAMLValue &node);

static yyjson_mut_val *YAMLValueToJSONValue(const YAMLValue &node, yyjson_mut_doc *doc) {
	switch (node.type) {
	case YAMLValue::Type::NULL_VALUE:
		return yyjson_mut_null(doc);
	case YAMLValue::Type::SCALAR: {
		auto scalar = node.scalar_value;
		if (scalar == "true" || scalar == "True" || scalar == "TRUE") {
			return yyjson_mut_bool(doc, true);
		}
		if (scalar == "false" || scalar == "False" || scalar == "FALSE") {
			return yyjson_mut_bool(doc, false);
		}
		if (scalar == "null" || scalar == "Null" || scalar == "NULL" || scalar == "~") {
			return yyjson_mut_null(doc);
		}
		try {
			size_t idx = 0;
			auto number = std::stoll(scalar, &idx);
			if (idx == scalar.size()) {
				return yyjson_mut_sint(doc, number);
			}
		} catch (...) {
		}
		try {
			size_t idx = 0;
			auto number = std::stoull(scalar, &idx);
			if (idx == scalar.size()) {
				return yyjson_mut_uint(doc, number);
			}
		} catch (...) {
		}
		try {
			size_t idx = 0;
			auto number = std::stod(scalar, &idx);
			if (idx == scalar.size()) {
				return yyjson_mut_real(doc, number);
			}
		} catch (...) {
		}
		return yyjson_mut_strcpy(doc, scalar.c_str());
	}
	case YAMLValue::Type::LIST: {
		auto *arr = yyjson_mut_arr(doc);
		for (const auto &child : node.list_entries) {
			yyjson_mut_arr_add_val(arr, YAMLValueToJSONValue(child, doc));
		}
		return arr;
	}
	case YAMLValue::Type::MAP: {
		auto *obj = yyjson_mut_obj(doc);
		for (const auto &entry : node.map_entries) {
			yyjson_mut_obj_add_val(doc, obj, entry.first.c_str(), YAMLValueToJSONValue(entry.second, doc));
		}
		return obj;
	}
	}
	throw InternalException("Unsupported YAML node type");
}

static Value YAMLValueToSecretValue(const YAMLValue &node) {
	if (node.type == YAMLValue::Type::NULL_VALUE) {
		return Value();
	}
	if (node.type == YAMLValue::Type::SCALAR) {
		auto scalar = node.scalar_value;
		if (scalar == "true" || scalar == "True" || scalar == "TRUE") {
			return Value::BOOLEAN(true);
		}
		if (scalar == "false" || scalar == "False" || scalar == "FALSE") {
			return Value::BOOLEAN(false);
		}
		if (scalar == "null" || scalar == "Null" || scalar == "NULL" || scalar == "~") {
			return Value();
		}
		try {
			size_t idx = 0;
			auto number = std::stoll(scalar, &idx);
			if (idx == scalar.size()) {
				return Value::BIGINT(number);
			}
		} catch (...) {
		}
		try {
			size_t idx = 0;
			auto number = std::stoull(scalar, &idx);
			if (idx == scalar.size()) {
				return Value::UBIGINT(number);
			}
		} catch (...) {
		}
		try {
			size_t idx = 0;
			auto number = std::stod(scalar, &idx);
			if (idx == scalar.size()) {
				return Value::DOUBLE(number);
			}
		} catch (...) {
		}
		return Value(scalar);
	}

	yyjson_mut_doc *doc = yyjson_mut_doc_new(nullptr);
	if (!doc) {
		throw SerializationException("Failed to allocate normalized YAML document");
	}
	auto *json_value = YAMLValueToJSONValue(node, doc);
	auto result = Value(SerializeJSONValue(json_value, doc));
	yyjson_mut_doc_free(doc);
	return result;
}

static vector<string> ParseScopeOption(const Value &value) {
	vector<string> result;
	if (value.IsNull()) {
		return result;
	}
	if (value.type().id() == LogicalTypeId::VARCHAR) {
		result.push_back(StringValue::Get(value));
		return result;
	}
	if (value.type().id() == LogicalTypeId::LIST) {
		for (const auto &child : ListValue::GetChildren(value)) {
			if (child.type().id() != LogicalTypeId::VARCHAR) {
				throw SerializationException("Secret scope entries must be strings");
			}
			result.push_back(StringValue::Get(child));
		}
		return result;
	}
	throw SerializationException("Secret scope must be a string or list of strings");
}

static vector<string> ParseScopeValue(const YAMLValue &node) {
	vector<string> result;
	if (node.type == YAMLValue::Type::NULL_VALUE) {
		return result;
	}
	if (node.type == YAMLValue::Type::SCALAR) {
		result.push_back(node.scalar_value);
		return result;
	}
	if (node.type == YAMLValue::Type::LIST) {
		for (const auto &child : node.list_entries) {
			if (child.type != YAMLValue::Type::SCALAR) {
				throw SerializationException("Secret scope entries must be strings");
			}
			result.push_back(child.scalar_value);
		}
		return result;
	}
	throw SerializationException("Secret scope must be a string or list of strings");
}

static void ApplyMetadataOption(const CreateSecretInput &input, ParsedSecretPayload &payload) {
	auto type_option = payload.values.find("type");
	if (type_option != payload.values.end() &&
	    !StringUtil::CIEquals(StringValue::Get(type_option->second), input.type)) {
		throw InvalidInputException("Secret file type '%s' does not match requested type '%s'",
		                            StringValue::Get(type_option->second), input.type);
	}
	payload.values.erase("type");

	payload.values.erase("name");

	payload.values.erase("provider");

	auto scope_option = payload.values.find("scope");
	if (scope_option != payload.values.end()) {
		if (input.scope.empty()) {
			payload.scope = ParseScopeOption(scope_option->second);
		}
		payload.values.erase("scope");
	}
}

static ParsedSecretPayload ParseJSONSecretPayload(const CreateSecretInput &input, const string &payload_text,
                                                  const string &source_name) {
	auto doc = yyjson_read(payload_text.c_str(), payload_text.size(), YYJSON_READ_ALLOW_INVALID_UNICODE);
	if (!doc) {
		throw SerializationException("Failed to parse JSON from '%s'", source_name);
	}
	auto *root = yyjson_doc_get_root(doc);
	if (!root || yyjson_get_tag(root) != (YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE)) {
		yyjson_doc_free(doc);
		throw SerializationException("Secret payload '%s' must contain a JSON object", source_name);
	}

	ParsedSecretPayload payload;
	size_t idx, max;
	yyjson_val *key, *value;
	yyjson_obj_foreach(root, idx, max, key, value) {
		auto key_name = string(yyjson_get_str(key), yyjson_get_len(key));
		payload.values[key_name] = JSONValueToSecretValue(value);
	}
	yyjson_doc_free(doc);
	ApplyMetadataOption(input, payload);
	return payload;
}

static ParsedSecretPayload ParseYAMLSecretPayload(const CreateSecretInput &input, const string &payload_text,
                                                  const string &source_name) {
	auto lines = TokenizeYAML(payload_text);
	if (lines.empty()) {
		throw SerializationException("Secret payload '%s' must contain a YAML mapping", source_name);
	}
	idx_t index = 0;
	auto root = ParseYAMLBlock(lines, index, lines[0].indent);
	if (root.type != YAMLValue::Type::MAP) {
		throw SerializationException("Secret payload '%s' must contain a YAML mapping", source_name);
	}

	ParsedSecretPayload payload;
	for (const auto &entry : root.map_entries) {
		auto key = entry.first;
		if (key == "scope") {
			if (input.scope.empty()) {
				payload.scope = ParseScopeValue(entry.second);
			}
			continue;
		}
		if (key == "type") {
			if (entry.second.type != YAMLValue::Type::SCALAR ||
			    !StringUtil::CIEquals(entry.second.scalar_value, input.type)) {
				throw InvalidInputException("Secret file type '%s' does not match requested type '%s'",
				                            entry.second.type == YAMLValue::Type::SCALAR ? entry.second.scalar_value
				                                                                         : "<non-scalar>",
				                            input.type);
			}
			continue;
		}
		if (key == "name") {
			continue;
		}
		if (key == "provider") {
			continue;
		}
		payload.values[key] = YAMLValueToSecretValue(entry.second);
	}
	return payload;
}

static string ResolveSecretFilePath(const CreateSecretInput &input) {
	auto option = input.options.find(EXTERNAL_SECRET_FILE_PATH_OPTION);
	if (option == input.options.end()) {
		throw InvalidInputException("Secret option '%s' is required for provider '%s'",
		                            EXTERNAL_SECRET_FILE_PATH_OPTION, EXTERNAL_SECRET_PROVIDER_FILE);
	}
	auto path = StringValue::Get(option->second);
	if (path.empty()) {
		throw InvalidInputException("Secret option '%s' must not be empty", EXTERNAL_SECRET_FILE_PATH_OPTION);
	}
	return path;
}

static string ResolveSecretFileFormat(const CreateSecretInput &input, const string &path) {
	auto option = input.options.find(EXTERNAL_SECRET_FILE_FORMAT_OPTION);
	if (option != input.options.end()) {
		auto format = StringUtil::Lower(StringValue::Get(option->second));
		if (format == "json" || format == "yaml" || format == "yml") {
			return format == "yml" ? "yaml" : format;
		}
		if (format != "auto") {
			throw InvalidInputException("Secret option '%s' must be one of: auto, json, yaml",
			                            EXTERNAL_SECRET_FILE_FORMAT_OPTION);
		}
	}
	auto lower_path = StringUtil::Lower(path);
	if (StringUtil::EndsWith(lower_path, ".json")) {
		return "json";
	}
	if (StringUtil::EndsWith(lower_path, ".yaml") || StringUtil::EndsWith(lower_path, ".yml")) {
		return "yaml";
	}
	throw InvalidInputException("Could not infer secret file format from path '%s'; set FORMAT 'json' or FORMAT 'yaml'",
	                            path);
}

static ParsedSecretPayload ParseSecretPayloadFromFile(ClientContext &context, const CreateSecretInput &input,
                                                      const string &path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS);
	if (!handle) {
		throw InvalidInputException("Secret file '%s' was not found for secret '%s'", path, input.name);
	}
	auto file_size = handle->GetFileSize();
	string payload_text;
	payload_text.resize(file_size);
	if (file_size > 0) {
		fs.Read(*handle, &payload_text[0], file_size);
	}
	auto format = ResolveSecretFileFormat(input, path);
	if (format == "json") {
		return ParseJSONSecretPayload(input, payload_text, path);
	}
	return ParseYAMLSecretPayload(input, payload_text, path);
}

static unique_ptr<BaseSecret> CreateSecretFromPayload(const CreateSecretInput &input, ParsedSecretPayload payload) {
	auto scope = input.scope.empty() ? payload.scope : input.scope;
	if (scope.empty()) {
		scope = {""};
	}

	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	for (auto &entry : payload.values) {
		secret->secret_map[entry.first] = entry.second;
	}
	return std::move(secret);
}

static unique_ptr<BaseSecret> CreateExternalSecretFromEnv(ClientContext &context, CreateSecretInput &input) {
	(void)context;

	auto env_var_name = ResolveSecretEnvVarName(input);
	auto payload = TryGetSecretPayload(env_var_name);
	if (!payload) {
		throw InvalidInputException("Environment variable '%s' was not found for secret '%s'", env_var_name,
		                            input.name);
	}
	auto parsed = ParseJSONSecretPayload(input, payload, env_var_name);
	return CreateSecretFromPayload(input, std::move(parsed));
}

static unique_ptr<BaseSecret> CreateExternalSecretFromFile(ClientContext &context, CreateSecretInput &input) {
	auto path = ResolveSecretFilePath(input);
	auto parsed = ParseSecretPayloadFromFile(context, input, path);
	return CreateSecretFromPayload(input, std::move(parsed));
}

static void LoadInternal(ExtensionLoader &loader) {
	for (auto *secret_type_name : GENERIC_ENV_SECRET_TYPES) {
		CreateSecretFunction env_secret_function;
		env_secret_function.secret_type = secret_type_name;
		env_secret_function.provider = EXTERNAL_SECRET_PROVIDER_ENV;
		env_secret_function.function = CreateExternalSecretFromEnv;
		env_secret_function.named_parameters[EXTERNAL_SECRET_ENV_OPTION] = LogicalType::VARCHAR;
		loader.RegisterFunction(env_secret_function);

		CreateSecretFunction file_secret_function;
		file_secret_function.secret_type = secret_type_name;
		file_secret_function.provider = EXTERNAL_SECRET_PROVIDER_FILE;
		file_secret_function.function = CreateExternalSecretFromFile;
		file_secret_function.named_parameters[EXTERNAL_SECRET_FILE_PATH_OPTION] = LogicalType::VARCHAR;
		file_secret_function.named_parameters[EXTERNAL_SECRET_FILE_FORMAT_OPTION] = LogicalType::VARCHAR;
		loader.RegisterFunction(file_secret_function);
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
