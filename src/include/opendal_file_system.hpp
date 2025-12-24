#pragma once

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {

class DatabaseInstance;
struct OpenDALOperator;

class OpenDALFileSystem final : public FileSystem {
public:
	explicit OpenDALFileSystem();
	~OpenDALFileSystem() override;

	std::string GetName() const override;
	bool CanHandleFile(const string &path) override;
	bool IsManuallySet() override;
	bool CanSeek() override;
	string PathSeparator(const string &path) override;

	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags, optional_ptr<FileOpener> opener) override;

	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;

	int64_t GetFileSize(FileHandle &handle) override;
	FileType GetFileType(FileHandle &handle) override;

	bool DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) override;
	bool FileExists(const string &filename, optional_ptr<FileOpener> opener) override;
	bool ListFilesExtended(const string &directory, const std::function<void(OpenFileInfo &info)> &callback,
	                       optional_ptr<FileOpener> opener) override;
	bool SupportsListFilesExtended() const override;
	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener) override;

	void Seek(FileHandle &handle, idx_t location) override;
	idx_t SeekPosition(FileHandle &handle) override;

	bool OnDiskFile(FileHandle &handle) override;

private:
	struct ResolvedPath;

	shared_ptr<OpenDALOperator> GetOrCreateOperator(const ResolvedPath &resolved, optional_ptr<FileOpener> opener);
	static ResolvedPath ResolvePath(const string &path);
	static string NormalizeObjectPath(string object_path);

	static bool HasAnyGlob(const string &path);
	vector<OpenFileInfo> GlobInternal(const string &pattern, FileOpener *opener);

private:
	mutex operator_cache_lock;
	unordered_map<string, shared_ptr<OpenDALOperator>> operator_cache;
};

void RegisterOpenDALFileSystemOptions(DatabaseInstance &db);

} // namespace duckdb
