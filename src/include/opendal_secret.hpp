#pragma once

namespace duckdb {

class ExtensionLoader;

void RegisterOpenDALSecrets(ExtensionLoader &loader);

} // namespace duckdb
