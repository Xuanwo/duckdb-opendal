#include "opendal_file_system.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#include "duckdb_opendal_ffi.h"

#include <algorithm>

namespace duckdb {

static atomic<bool> g_enable_s3_alias {false};
static atomic<bool> g_prefer_alias {true};
static atomic<idx_t> g_readahead_bytes {1024 * 1024};

static void SetEnableS3Alias(ClientContext &, SetScope, Value &parameter) {
	g_enable_s3_alias.store(BooleanValue::Get(parameter));
}

static void SetPreferAlias(ClientContext &, SetScope, Value &parameter) {
	g_prefer_alias.store(BooleanValue::Get(parameter));
}

static void SetReadaheadBytes(ClientContext &, SetScope, Value &parameter) {
	auto v = parameter.GetValue<int64_t>();
	if (v < 0) {
		throw InvalidInputException("opendal_readahead_bytes must be >= 0");
	}
	g_readahead_bytes.store(UnsafeNumericCast<idx_t>(v));
}

void RegisterOpenDALFileSystemOptions(DatabaseInstance &db) {
	auto &config = DBConfig::GetConfig(db);
	config.AddExtensionOption("opendal_enable_s3_alias", "Enable s3:// alias handled by OpenDAL", LogicalType::BOOLEAN,
	                          Value(false), SetEnableS3Alias);
	config.AddExtensionOption("opendal_prefer_alias", "Prefer OpenDAL over other file systems for enabled aliases",
	                          LogicalType::BOOLEAN, Value(true), SetPreferAlias);
	config.AddExtensionOption("opendal_readahead_bytes", "Readahead size in bytes for OpenDAL reads",
	                          LogicalType::BIGINT, Value::BIGINT(1024 * 1024), SetReadaheadBytes);
}

static void ThrowFromFFIError(const string &operation, const string &path, char *error) {
	string message = error ? string(error) : string("unknown error");
	if (error) {
		duckdb_opendal_string_free(error);
	}
	throw IOException("OpenDAL %s failed for \"%s\": %s", operation, path, message);
}

struct OpenDALOperator {
	explicit OpenDALOperator(duckdb_opendal_operator *op_p) : op(op_p) {
	}
	~OpenDALOperator() {
		if (op) {
			duckdb_opendal_operator_free(op);
		}
	}
	duckdb_opendal_operator *op;
};

struct OpenDALFileSystem::ResolvedPath {
	string original;
	string uri_prefix;
	string service;
	string object_path;
	unordered_map<string, string> options;
};

static bool IsAliasEnabledForPath(const string &path) {
	if (!g_enable_s3_alias.load()) {
		return false;
	}
	return StringUtil::StartsWith(path, "s3://") || StringUtil::StartsWith(path, "s3a://") ||
	       StringUtil::StartsWith(path, "s3n://");
}

OpenDALFileSystem::ResolvedPath OpenDALFileSystem::ResolvePath(const string &path) {
	ResolvedPath resolved;
	resolved.original = path;

	auto parse_fs_path = [&](const string &exposed_scheme, const string &fs_path) {
		resolved.service = "fs";
		auto p = fs_path;
		// Accept both `opendal+fs://path` and `opendal+fs:///abs/path`.
		// The URI part after `opendal+fs://` is treated as a filesystem path, not an authority.
		if (StringUtil::StartsWith(p, "/")) {
			resolved.uri_prefix = exposed_scheme + ":///";
			resolved.options["root"] = "/";
		} else {
			resolved.uri_prefix = exposed_scheme + "://";
			resolved.options["root"] = FileSystem::GetWorkingDirectory();
		}
		resolved.object_path = NormalizeObjectPath(p);
	};

	auto parse_scheme_uri = [&](const string &service, const string &exposed_scheme, const string &uri_rest) {
		// uri_rest is in form: //authority/path...
		auto rest = uri_rest;
		if (!StringUtil::StartsWith(rest, "//")) {
			throw InvalidInputException("Invalid OpenDAL URI: %s", path);
		}
		rest = rest.substr(2);
		auto slash_pos = rest.find('/');
		string authority;
		string raw_path;
		if (slash_pos == string::npos) {
			authority = rest;
			raw_path = "";
		} else {
			authority = rest.substr(0, slash_pos);
			raw_path = rest.substr(slash_pos); // includes leading '/'
		}
		resolved.service = service;

		if (service == "s3") {
			if (authority.empty()) {
				throw InvalidInputException("Invalid s3 URI, missing bucket: %s", path);
			}
			resolved.uri_prefix = exposed_scheme + "://" + authority + "/";
			resolved.options["bucket"] = authority;
			resolved.object_path = NormalizeObjectPath(raw_path);
			return;
		}

		resolved.uri_prefix = exposed_scheme + "://" + authority + "/";
		resolved.object_path = NormalizeObjectPath(authority + raw_path);
	};

	if (StringUtil::StartsWith(path, "opendal+")) {
		auto inner = path.substr(strlen("opendal+"));
		auto sep = inner.find("://");
		if (sep == string::npos) {
			throw InvalidInputException("Invalid OpenDAL URI: %s", path);
		}
		auto service = inner.substr(0, sep);
		auto rest = inner.substr(sep + 3); // after ://
		auto exposed_scheme = "opendal+" + service;
		if (service == "fs") {
			parse_fs_path(exposed_scheme, rest);
			return resolved;
		}
		if (service == "s3a" || service == "s3n") {
			service = "s3";
		}
		parse_scheme_uri(service, exposed_scheme, "//" + rest);
		return resolved;
	}

	if (IsAliasEnabledForPath(path)) {
		auto sep = path.find("://");
		auto scheme = path.substr(0, sep);
		auto service = scheme;
		auto rest = path.substr(sep + 1); // keep leading '/'
		if (service == "s3a" || service == "s3n") {
			service = "s3";
		}
		parse_scheme_uri(service, scheme, rest);
		return resolved;
	}

	throw InvalidInputException("Path is not handled by OpenDAL: %s", path);
}

string OpenDALFileSystem::NormalizeObjectPath(string object_path) {
	while (!object_path.empty() && object_path[0] == '/') {
		object_path.erase(0, 1);
	}
	return object_path;
}

OpenDALFileSystem::OpenDALFileSystem() {
}

OpenDALFileSystem::~OpenDALFileSystem() {
}

std::string OpenDALFileSystem::GetName() const {
	return "OpenDALFileSystem";
}

bool OpenDALFileSystem::IsManuallySet() {
	return g_prefer_alias.load();
}

bool OpenDALFileSystem::CanSeek() {
	return true;
}

string OpenDALFileSystem::PathSeparator(const string &) {
	return "/";
}

bool OpenDALFileSystem::CanHandleFile(const string &path) {
	if (StringUtil::StartsWith(path, "opendal+")) {
		return true;
	}
	return IsAliasEnabledForPath(path);
}

shared_ptr<OpenDALOperator> OpenDALFileSystem::GetOrCreateOperator(const ResolvedPath &resolved,
                                                                   optional_ptr<FileOpener> opener) {
	unordered_map<string, string> options = resolved.options;

	optional_ptr<SecretManager> secret_manager;
	unique_ptr<CatalogTransaction> transaction;
	if (opener) {
		secret_manager = FileOpener::TryGetSecretManager(opener);
		transaction = FileOpener::TryGetCatalogTransaction(opener);
	}
	if (secret_manager && transaction) {
		auto match = secret_manager->LookupSecret(*transaction, resolved.original, "opendal");
		if (match.HasMatch()) {
			auto &base = match.GetSecret();
			auto kv = dynamic_cast<const KeyValueSecret *>(&base);
			if (kv) {
				for (const auto &entry : kv->secret_map) {
					const auto &key = entry.first;
					const auto &val = entry.second;
					if (val.IsNull()) {
						continue;
					}
					if (val.type().id() == LogicalTypeId::VARCHAR) {
						options[key] = StringValue::Get(val);
					} else {
						options[key] = val.ToString();
					}
				}
			}
		}
	}

	if (resolved.service.empty()) {
		auto it = options.find("service");
		if (it == options.end() || it->second.empty()) {
			throw InvalidInputException("OpenDAL operator service is not specified for %s", resolved.original);
		}
	}

	string service = resolved.service.empty() ? options["service"] : resolved.service;
	options.erase("service");

	std::vector<std::pair<string, string>> kv_pairs;
	kv_pairs.reserve(options.size());
	for (auto &it : options) {
		kv_pairs.emplace_back(it.first, it.second);
	}
	std::sort(kv_pairs.begin(), kv_pairs.end(),
	          [](const std::pair<string, string> &a, const std::pair<string, string> &b) { return a.first < b.first; });
	string cache_key = service;
	for (auto &kvp : kv_pairs) {
		cache_key += '\n';
		cache_key += kvp.first;
		cache_key += '=';
		cache_key += kvp.second;
	}

	lock_guard<mutex> guard(operator_cache_lock);
	auto cached = operator_cache.find(cache_key);
	if (cached != operator_cache.end()) {
		return cached->second;
	}

	vector<const char *> keys;
	vector<const char *> values;
	keys.reserve(kv_pairs.size());
	values.reserve(kv_pairs.size());
	for (auto &kvp : kv_pairs) {
		keys.push_back(kvp.first.c_str());
		values.push_back(kvp.second.c_str());
	}

	char *err = nullptr;
	auto op = duckdb_opendal_operator_new(service.c_str(), keys.data(), values.data(), kv_pairs.size(), &err);
	if (!op) {
		ThrowFromFFIError("operator_new", resolved.original, err);
	}

	auto wrapped = make_shared_ptr<OpenDALOperator>(op);
	operator_cache.emplace(std::move(cache_key), wrapped);
	return wrapped;
}

struct OpenDALFileHandle final : public FileHandle {
	OpenDALFileHandle(FileSystem &fs, string path_p, FileOpenFlags flags_p, shared_ptr<OpenDALOperator> op_p,
	                  string object_path_p, idx_t file_size_p)
	    : FileHandle(fs, std::move(path_p), flags_p), op(std::move(op_p)), object_path(std::move(object_path_p)),
	      file_size(file_size_p), position(0), readahead_start(0), readahead_end(0) {
	}

	void Close() override {
	}

	shared_ptr<OpenDALOperator> op;
	string object_path;
	idx_t file_size;

	idx_t position;
	vector<uint8_t> readahead;
	idx_t readahead_start;
	idx_t readahead_end;
};

unique_ptr<FileHandle> OpenDALFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                   optional_ptr<FileOpener> opener) {
	if (flags.OpenForWriting()) {
		throw NotImplementedException("OpenDALFileSystem does not support write");
	}

	auto resolved = ResolvePath(path);
	auto op = GetOrCreateOperator(resolved, opener);

	bool exists = false;
	bool is_dir = false;
	uint64_t size = 0;
	char *err = nullptr;
	if (!duckdb_opendal_stat(op->op, resolved.object_path.c_str(), &exists, &is_dir, &size, &err)) {
		ThrowFromFFIError("stat", path, err);
	}
	if (!exists) {
		throw IOException("OpenDAL file does not exist: %s", path);
	}
	if (is_dir) {
		throw IOException("OpenDAL path is a directory: %s", path);
	}

	return make_uniq<OpenDALFileHandle>(*this, path, flags, std::move(op), resolved.object_path,
	                                    UnsafeNumericCast<idx_t>(size));
}

static idx_t ReadExact(OpenDALOperator &op, const string &object_path, idx_t location, uint8_t *buffer,
                       idx_t buffer_len, const string &original_path) {
	idx_t total = 0;
	while (total < buffer_len) {
		size_t bytes_read = 0;
		char *err = nullptr;
		bool ok = duckdb_opendal_read(op.op, object_path.c_str(), UnsafeNumericCast<uint64_t>(location + total),
		                              buffer + total, UnsafeNumericCast<size_t>(buffer_len - total), &bytes_read, &err);
		if (!ok) {
			ThrowFromFFIError("read", original_path, err);
		}
		if (bytes_read == 0) {
			throw IOException(
			    "Could not read enough bytes from file \"%s\": attempted to read %llu bytes from location %llu",
			    original_path, buffer_len - total, location + total);
		}
		total += UnsafeNumericCast<idx_t>(bytes_read);
	}
	return total;
}

void OpenDALFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &h = handle.Cast<OpenDALFileHandle>();
	if (nr_bytes < 0) {
		throw IOException("Invalid read size");
	}
	auto n = UnsafeNumericCast<idx_t>(nr_bytes);
	if (n == 0) {
		return;
	}
	if (location + n > h.file_size) {
		throw IOException(
		    "Could not read enough bytes from file \"%s\": attempted to read %llu bytes from location %llu",
		    handle.path, n, location);
	}

	auto readahead_bytes = g_readahead_bytes.load();
	if (readahead_bytes > 0 && location >= h.readahead_start && (location + n) <= h.readahead_end) {
		auto offset = location - h.readahead_start;
		memcpy(buffer, h.readahead.data() + offset, n);
		return;
	}

	if (readahead_bytes > 0 && n < readahead_bytes) {
		auto to_read = MinValue(readahead_bytes, h.file_size - location);
		h.readahead.resize(to_read);
		ReadExact(*h.op, h.object_path, location, h.readahead.data(), to_read, handle.path);
		h.readahead_start = location;
		h.readahead_end = location + to_read;

		memcpy(buffer, h.readahead.data(), n);
		return;
	}

	ReadExact(*h.op, h.object_path, location, reinterpret_cast<uint8_t *>(buffer), n, handle.path);
}

int64_t OpenDALFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &h = handle.Cast<OpenDALFileHandle>();
	if (nr_bytes < 0) {
		throw IOException("Invalid read size");
	}
	auto n = UnsafeNumericCast<idx_t>(nr_bytes);
	if (h.position >= h.file_size) {
		return 0;
	}
	auto to_read = MinValue(n, h.file_size - h.position);
	if (to_read == 0) {
		return 0;
	}
	ReadExact(*h.op, h.object_path, h.position, reinterpret_cast<uint8_t *>(buffer), to_read, handle.path);
	h.position += to_read;
	return UnsafeNumericCast<int64_t>(to_read);
}

void OpenDALFileSystem::Seek(FileHandle &handle, idx_t location) {
	auto &h = handle.Cast<OpenDALFileHandle>();
	h.position = location;
}

idx_t OpenDALFileSystem::SeekPosition(FileHandle &handle) {
	auto &h = handle.Cast<OpenDALFileHandle>();
	return h.position;
}

int64_t OpenDALFileSystem::GetFileSize(FileHandle &handle) {
	auto &h = handle.Cast<OpenDALFileHandle>();
	return UnsafeNumericCast<int64_t>(h.file_size);
}

FileType OpenDALFileSystem::GetFileType(FileHandle &) {
	return FileType::FILE_TYPE_REGULAR;
}

bool OpenDALFileSystem::DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) {
	auto resolved = ResolvePath(directory);
	auto op = GetOrCreateOperator(resolved, opener);

	bool exists = false;
	bool is_dir = false;
	uint64_t size = 0;
	char *err = nullptr;
	if (!duckdb_opendal_stat(op->op, resolved.object_path.c_str(), &exists, &is_dir, &size, &err)) {
		ThrowFromFFIError("stat", directory, err);
	}
	return exists && is_dir;
}

bool OpenDALFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	auto resolved = ResolvePath(filename);
	auto op = GetOrCreateOperator(resolved, opener);

	bool exists = false;
	bool is_dir = false;
	uint64_t size = 0;
	char *err = nullptr;
	if (!duckdb_opendal_stat(op->op, resolved.object_path.c_str(), &exists, &is_dir, &size, &err)) {
		ThrowFromFFIError("stat", filename, err);
	}
	return exists && !is_dir;
}

bool OpenDALFileSystem::SupportsListFilesExtended() const {
	return true;
}

bool OpenDALFileSystem::ListFilesExtended(const string &directory,
                                          const std::function<void(OpenFileInfo &info)> &callback,
                                          optional_ptr<FileOpener> opener) {
	auto resolved = ResolvePath(directory);
	auto op = GetOrCreateOperator(resolved, opener);

	auto prefix = resolved.object_path;
	if (!prefix.empty() && prefix.back() != '/') {
		prefix.push_back('/');
	}

	char *err = nullptr;
	auto lister = duckdb_opendal_list(op->op, prefix.c_str(), false, &err);
	if (!lister) {
		if (err) {
			ThrowFromFFIError("list", directory, err);
		}
		return false;
	}

	// Ensure lister freed on all exits.
	struct ListerGuard {
		duckdb_opendal_lister *lister;
		~ListerGuard() {
			if (lister) {
				duckdb_opendal_lister_free(lister);
			}
		}
	} guard {lister};

	char *entry_path = nullptr;
	bool is_dir = false;
	uint64_t size = 0;
	while (duckdb_opendal_lister_next(lister, &entry_path, &is_dir, &size)) {
		if (!entry_path) {
			continue;
		}
		string entry_full(entry_path);
		duckdb_opendal_string_free(entry_path);
		entry_path = nullptr;

		if (!prefix.empty()) {
			if (!StringUtil::StartsWith(entry_full, prefix)) {
				continue;
			}
			entry_full = entry_full.substr(prefix.size());
		}

		if (entry_full.empty()) {
			continue;
		}
		auto slash = entry_full.find('/');
		if (slash != string::npos) {
			// Only expose direct children. Allow a single trailing slash for directories.
			if (!is_dir || entry_full.back() != '/' || slash != entry_full.size() - 1) {
				continue;
			}
			entry_full.pop_back();
		}

		OpenFileInfo info(entry_full);
		info.extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
		auto &options = info.extended_info->options;
		options.emplace("type", Value(is_dir ? "directory" : "file"));
		if (!is_dir) {
			options.emplace("file_size", Value::BIGINT(UnsafeNumericCast<int64_t>(size)));
		}
		callback(info);
	}

	return true;
}

bool OpenDALFileSystem::HasAnyGlob(const string &path) {
	return FileSystem::HasGlob(path);
}

vector<OpenFileInfo> OpenDALFileSystem::GlobInternal(const string &pattern, FileOpener *opener) {
	vector<OpenFileInfo> result;

	if (!HasAnyGlob(pattern)) {
		if (FileExists(pattern, opener)) {
			result.emplace_back(pattern);
		}
		return result;
	}

	auto resolved = ResolvePath(pattern);
	auto op = GetOrCreateOperator(resolved, opener);

	auto object_pattern = resolved.object_path;
	auto last_sep = object_pattern.rfind('/');
	string dir_prefix;
	string name_glob;
	if (last_sep == string::npos) {
		dir_prefix = "";
		name_glob = object_pattern;
	} else {
		dir_prefix = object_pattern.substr(0, last_sep + 1);
		name_glob = object_pattern.substr(last_sep + 1);
	}
	if (dir_prefix.empty() && name_glob.empty()) {
		return result;
	}
	if (HasAnyGlob(dir_prefix)) {
		throw IOException("OpenDAL glob currently only supports wildcards in the last path segment: \"%s\"", pattern);
	}

	char *err = nullptr;
	auto lister = duckdb_opendal_list(op->op, dir_prefix.c_str(), false, &err);
	if (!lister) {
		if (err) {
			ThrowFromFFIError("list", pattern, err);
		}
		return result;
	}

	struct ListerGuard {
		duckdb_opendal_lister *lister;
		~ListerGuard() {
			if (lister) {
				duckdb_opendal_lister_free(lister);
			}
		}
	} guard {lister};

	char *entry_path = nullptr;
	bool is_dir = false;
	uint64_t size = 0;
	while (duckdb_opendal_lister_next(lister, &entry_path, &is_dir, &size)) {
		if (!entry_path) {
			continue;
		}
		string entry_object(entry_path);
		duckdb_opendal_string_free(entry_path);
		entry_path = nullptr;

		if (is_dir) {
			continue;
		}
		if (!dir_prefix.empty()) {
			if (!StringUtil::StartsWith(entry_object, dir_prefix)) {
				continue;
			}
			auto name = entry_object.substr(dir_prefix.size());
			if (name.empty() || StringUtil::Contains(name, "/")) {
				continue;
			}
			if (!duckdb::Glob(name.c_str(), name.size(), name_glob.c_str(), name_glob.size())) {
				continue;
			}
		} else {
			if (StringUtil::Contains(entry_object, "/")) {
				continue;
			}
			if (!duckdb::Glob(entry_object.c_str(), entry_object.size(), name_glob.c_str(), name_glob.size())) {
				continue;
			}
		}

		result.emplace_back(resolved.uri_prefix + entry_object);
	}

	return result;
}

vector<OpenFileInfo> OpenDALFileSystem::Glob(const string &path, FileOpener *opener) {
	return GlobInternal(path, opener);
}

bool OpenDALFileSystem::OnDiskFile(FileHandle &handle) {
	auto resolved = ResolvePath(handle.path);
	return resolved.service == "fs";
}

} // namespace duckdb
