#include "opendal_secret.hpp"

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"

namespace duckdb {

static unique_ptr<BaseSecret> CreateOpenDALSecretFromConfig(ClientContext &context, CreateSecretInput &input) {
	auto secret = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);

	secret->TrySetValue("service", input);
	secret->TrySetValue("root", input);
	secret->TrySetValue("endpoint", input);
	secret->TrySetValue("region", input);

	secret->TrySetValue("access_key_id", input);
	secret->TrySetValue("secret_access_key", input);
	secret->TrySetValue("session_token", input);

	Value config;
	if (secret->TryGetValue("config", config)) {
		// Keep it as-is if a secret has already set it.
	} else {
		auto it = input.options.find("config");
		if (it != input.options.end()) {
			config = it->second;
		}
	}

	if (!config.IsNull()) {
		if (config.type().id() != LogicalTypeId::MAP) {
			throw InvalidInputException("OpenDAL secret option \"config\" must be a MAP(VARCHAR, VARCHAR)");
		}
		for (const auto &entry : ListValue::GetChildren(config)) {
			const auto &kv = StructValue::GetChildren(entry);
			if (kv.size() != 2) {
				continue;
			}
			auto key = kv[0].ToString();
			if (kv[1].type().id() == LogicalTypeId::VARCHAR) {
				secret->secret_map[key] = Value(StringValue::Get(kv[1]));
			} else {
				secret->secret_map[key] = Value(kv[1].ToString());
			}
		}
	}

	secret->redact_keys = {"secret_access_key", "session_token"};
	return std::move(secret);
}

void RegisterOpenDALSecrets(ExtensionLoader &loader) {
	SecretType secret_type;
	secret_type.name = "opendal";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	secret_type.extension = "opendal";
	loader.RegisterSecretType(std::move(secret_type));

	CreateSecretFunction config_fun;
	config_fun.secret_type = "opendal";
	config_fun.provider = "config";
	config_fun.function = CreateOpenDALSecretFromConfig;

	config_fun.named_parameters["service"] = LogicalType::VARCHAR;
	config_fun.named_parameters["root"] = LogicalType::VARCHAR;
	config_fun.named_parameters["endpoint"] = LogicalType::VARCHAR;
	config_fun.named_parameters["region"] = LogicalType::VARCHAR;

	config_fun.named_parameters["access_key_id"] = LogicalType::VARCHAR;
	config_fun.named_parameters["secret_access_key"] = LogicalType::VARCHAR;
	config_fun.named_parameters["session_token"] = LogicalType::VARCHAR;

	config_fun.named_parameters["config"] = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);

	loader.RegisterFunction(std::move(config_fun));
}

} // namespace duckdb
