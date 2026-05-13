#define DUCKDB_EXTENSION_MAIN

#include "external_secrets_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

namespace duckdb {

inline void ExternalSecretsScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "...........🦆 " + name.GetString());
	});
}

inline void ExternalSecretsOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "ExternalSecrets " + name.GetString() + ", my linked OpenSSL version is " +
		                                           OPENSSL_VERSION_TEXT);
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	// Register a scalar function
	auto external_secrets_scalar_function =
	    ScalarFunction("external_secrets", {LogicalType::VARCHAR}, LogicalType::VARCHAR, ExternalSecretsScalarFun);

	loader.RegisterFunction(external_secrets_scalar_function);

	// Register another scalar function
	auto external_secrets_openssl_version_scalar_function = ScalarFunction("external_secrets_openssl_version", {LogicalType::VARCHAR},
	                                                             LogicalType::VARCHAR, ExternalSecretsOpenSSLVersionScalarFun);
	loader.RegisterFunction(external_secrets_openssl_version_scalar_function);
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
