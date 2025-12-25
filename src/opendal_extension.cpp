#define DUCKDB_EXTENSION_MAIN

#include "opendal_extension.hpp"

#include "opendal_file_system.hpp"
#include "opendal_secret.hpp"

#include "duckdb/main/config.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	auto &db_instance = loader.GetDatabaseInstance();
	auto &fs = db_instance.GetFileSystem();
	fs.RegisterSubSystem(make_uniq<OpenDALFileSystem>());

	RegisterOpenDALFileSystemOptions(db_instance);
	RegisterOpenDALSecrets(loader);
}

void OpendalExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string OpendalExtension::Name() {
	return "opendal";
}

std::string OpendalExtension::Version() const {
#ifdef EXT_VERSION_OPENDAL
	return EXT_VERSION_OPENDAL;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(opendal, loader) {
	duckdb::LoadInternal(loader);
}
}
