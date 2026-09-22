#include "devbox/state_store.hpp"
#include "devbox/resource_budget.hpp"
#include <algorithm>
#include <array>
#include <sqlite3.h>
#ifdef _WIN32
#include <aclapi.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#endif
#endif
namespace devbox {
namespace {
constexpr int application_id = 0x44425631, schema_version = 2;
constexpr std::size_t maximum_record = 1024 * 1024;
void key(std::string_view value, bool empty = false) {
    if ((!empty && value.empty()) || value.size() > 256 || value == "*" || value.find('\0') != value.npos)
        throw Error("STATE_INVALID_KEY");
}
void private_local_directory(const fs::path& directory, bool create) {
    if (!directory.is_absolute())
        throw Error("STATE_LOCAL_PATH_REQUIRED");
#ifdef _WIN32
    auto path = directory.lexically_normal();
    const auto text = path.native();
    if (text.starts_with(L"\\\\") && !(text.starts_with(L"\\\\?\\") && text.size() > 6 && text[5] == L':'))
        throw Error("STATE_NETWORK_OR_DEVICE_PATH_DENIED");
    for (auto parent = path.parent_path(); !parent.empty();) {
        const auto attributes = GetFileAttributesW(parent.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw Error("STATE_REPARSE_PARENT_DENIED");
        const auto next = parent.parent_path();
        if (next == parent)
            break;
        parent = next;
    }
    std::array<wchar_t, MAX_PATH> volume{};
    if (!GetVolumePathNameW(path.parent_path().c_str(), volume.data(), static_cast<DWORD>(volume.size())) ||
        GetDriveTypeW(volume.data()) != DRIVE_FIXED)
        throw Error("STATE_LOCAL_FIXED_DISK_REQUIRED");
    if (create && !fs::exists(path)) {
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
            throw Error("STATE_TOKEN_QUERY_FAILED");
        NativeHandle token(raw);
        DWORD length = 0;
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &length);
        std::vector<unsigned char> user(length);
        if (!GetTokenInformation(token.get(), TokenUser, user.data(), length, &length))
            throw Error("STATE_TOKEN_QUERY_FAILED");
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid))
            throw Error("STATE_TOKEN_QUERY_FAILED");
        ScopeExit free_sid([&] { LocalFree(sid); });
        const auto sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + std::wstring(sid) + L")";
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor,
                                                                  nullptr))
            throw Error("STATE_PRIVATE_ACL_FAILED");
        ScopeExit free_descriptor([&] { LocalFree(descriptor); });
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
        if (!CreateDirectoryW(path.c_str(), &security) && GetLastError() != ERROR_ALREADY_EXISTS)
            throw Error("STATE_DIRECTORY_CREATE_FAILED");
    }
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw Error("STATE_PRIVATE_DIRECTORY_REQUIRED");
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
        throw Error("STATE_TOKEN_QUERY_FAILED");
    NativeHandle token(raw);
    DWORD length = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &length);
    std::vector<unsigned char> user(length);
    if (!GetTokenInformation(token.get(), TokenUser, user.data(), length, &length))
        throw Error("STATE_TOKEN_QUERY_FAILED");
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl,
                              nullptr, &descriptor) != ERROR_SUCCESS)
        throw Error("STATE_PRIVATE_ACL_QUERY_FAILED");
    ScopeExit free_descriptor([&] { LocalFree(descriptor); });
    if (!acl)
        throw Error("STATE_PRIVATE_ACL_REQUIRED");
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void* raw_ace = nullptr;
        if (!GetAce(acl, index, &raw_ace))
            throw Error("STATE_PRIVATE_ACL_QUERY_FAILED");
        const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
        if (header->AceType == ACCESS_DENIED_ACE_TYPE)
            continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            throw Error("STATE_PRIVATE_ACL_REQUIRED");
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
        auto* sid = const_cast<DWORD*>(&ace->SidStart);
        if (!EqualSid(sid, reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid) &&
            !IsWellKnownSid(sid, WinLocalSystemSid))
            throw Error("STATE_PRIVATE_ACL_REQUIRED");
    }
#else
    if (create && ::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
        throw Error("STATE_DIRECTORY_CREATE_FAILED");
    struct stat info{};
    if (::lstat(directory.c_str(), &info) || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() ||
        (info.st_mode & 0077))
        throw Error("STATE_PRIVATE_DIRECTORY_REQUIRED");
    struct statfs volume{};
    if (::statfs(directory.c_str(), &volume))
        throw Error("STATE_FILESYSTEM_QUERY_FAILED");
#if defined(__linux__)
    const auto type = static_cast<std::uint32_t>(volume.f_type);
    // Qualified local filesystems: ext, XFS, Btrfs, tmpfs, overlayfs, F2FS, ZFS.
    constexpr std::array<std::uint32_t, 7> local{0xef53,     0x58465342, 0x9123683e, 0x01021994,
                                                 0x794c7630, 0xf2f52010, 0x2fc12fc1};
    if (std::find(local.begin(), local.end(), type) == local.end())
        throw Error("STATE_FILESYSTEM_NOT_QUALIFIED_FOR_LOCAL_WAL");
#else
    if (!(volume.f_flags & MNT_LOCAL))
        throw Error("STATE_LOCAL_FILESYSTEM_REQUIRED");
#endif
#endif
}
void check(sqlite3* db, int code) {
    if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW)
        throw Error("STATE_SQLITE_ERROR_" + std::to_string(sqlite3_extended_errcode(db)));
}
class Statement {
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;

  public:
    Statement(sqlite3* db, const std::string& sql) : db_(db) {
        check(db, sqlite3_prepare_v3(db, sql.c_str(), static_cast<int>(sql.size()), SQLITE_PREPARE_PERSISTENT,
                                     &statement_, nullptr));
    }
    ~Statement() {
        sqlite3_finalize(statement_);
    }
    Statement(const Statement&) = delete;
    void bind(int at, std::string_view value) {
        check(db_, sqlite3_bind_text(statement_, at, value.data(), static_cast<int>(value.size()),
                                     SQLITE_TRANSIENT));
    }
    void bind(int at, std::uint64_t value) {
        if (value > static_cast<std::uint64_t>(INT64_MAX))
            throw Error("STATE_INTEGER_RANGE");
        check(db_, sqlite3_bind_int64(statement_, at, static_cast<sqlite3_int64>(value)));
    }
    bool step() {
        const auto result = sqlite3_step(statement_);
        check(db_, result);
        return result == SQLITE_ROW;
    }
    std::string text(int at) const {
        const auto size = sqlite3_column_bytes(statement_, at);
        if (size < 0 || size > static_cast<int>(maximum_record))
            throw Error("STATE_RECORD_SIZE");
        const auto* data = sqlite3_column_text(statement_, at);
        return data ? std::string(reinterpret_cast<const char*>(data), static_cast<std::size_t>(size)) : "";
    }
    std::uint64_t integer(int at) const {
        const auto value = sqlite3_column_int64(statement_, at);
        if (value < 0)
            throw Error("STATE_CORRUPT_COUNTER");
        return static_cast<std::uint64_t>(value);
    }
    int steps() const {
        return sqlite3_stmt_status(statement_, SQLITE_STMTSTATUS_VM_STEP, 0);
    }
    std::size_t bytes(int at) const {
        return static_cast<std::size_t>(sqlite3_column_bytes(statement_, at));
    }
};
class SqliteStore final : public StateStore {
    fs::path directory_;
    sqlite3* db_ = nullptr;
    std::unique_ptr<FileLock> writer_;
    NativeHandle database_guard_;
    std::function<void(std::string_view)> transition_hook_;
    mutable std::mutex mutex_;
    std::uint64_t generation_ = 0;
    mutable int last_query_steps_ = 0;
    void sql(const char* statement) const {
        check(db_, sqlite3_exec(db_, statement, nullptr, nullptr, nullptr));
    }
    std::uint64_t scalar(const char* query) const {
        Statement statement(db_, query);
        if (!statement.step())
            throw Error("STATE_MISSING_METADATA");
        return statement.integer(0);
    }
    void writable() const {
        if (!writer_ || !generation_ ||
            scalar("SELECT generation FROM control WHERE singleton=1") != generation_)
            throw Error("STATE_WRITER_FENCED");
    }
    static StateRecord record(Statement& row) {
        return StateRecord{row.text(0),
                           row.text(1),
                           row.text(2),
                           row.text(3),
                           row.text(4),
                           row.integer(5),
                           Json::parse(row.text(6))};
    }

  public:
    SqliteStore(fs::path directory, StateStoreOptions options)
        : directory_(std::move(directory)), transition_hook_(std::move(options.transition_hook)) {
        private_local_directory(directory_, options.writable);
        if (options.writable)
            writer_ =
                std::make_unique<FileLock>(directory_ / ".writer.lock", options.writer_wait, Cancel{}, true);
        const auto database = directory_ / "metadata.sqlite3";
        if (fs::exists(database) && fs::symlink_status(database).type() != fs::file_type::regular)
            throw Error("STATE_DATABASE_ALIAS_DENIED");
#ifdef _WIN32
        database_guard_.reset(CreateFileW(
            database.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            options.writable ? OPEN_ALWAYS : OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!database_guard_ || !GetFileInformationByHandle(database_guard_.get(), &info) ||
            info.nNumberOfLinks != 1 ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            throw Error("STATE_DATABASE_ALIAS_DENIED");
#else
        database_guard_.reset(
            ::open(database.c_str(),
                   (options.writable ? O_RDWR | O_CREAT : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW, 0600));
        struct stat info{};
        if (!database_guard_ || ::fstat(database_guard_.get(), &info) || !S_ISREG(info.st_mode) ||
            info.st_nlink != 1 || info.st_uid != ::geteuid() || (info.st_mode & 0077))
            throw Error("STATE_DATABASE_ALIAS_DENIED");
#endif
        const auto flags =
            (options.writable ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY) |
            SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW;
        const auto opened = sqlite3_open_v2(path_text(database).c_str(), &db_, flags, nullptr);
        ScopeExit cleanup([&] {
            if (db_)
                sqlite3_close_v2(db_);
            db_ = nullptr;
        });
        if (!db_)
            throw Error("STATE_DATABASE_OPEN_FAILED");
        check(db_, opened);
        sqlite3_extended_result_codes(db_, 1);
        check(db_, sqlite3_busy_timeout(db_, 1000));
        check(db_, sqlite3_db_config(db_, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr));
        check(db_, sqlite3_db_config(db_, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr));
        check(db_, sqlite3_db_config(db_, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, nullptr));
        sqlite3_limit(db_, SQLITE_LIMIT_LENGTH, static_cast<int>(maximum_record + 8192));
        sqlite3_limit(db_, SQLITE_LIMIT_SQL_LENGTH, 65536);
        sqlite3_limit(db_, SQLITE_LIMIT_COLUMN, 64);
        sql("PRAGMA foreign_keys=ON; PRAGMA mmap_size=0; PRAGMA cache_size=-8192; PRAGMA temp_store=MEMORY;");
        const auto version = scalar("PRAGMA user_version"), identity = scalar("PRAGMA application_id");
        if ((version == 0 && identity != 0) ||
            (version != 0 && ((version != 1 && version != schema_version) || identity != application_id)))
            throw Error("STATE_SCHEMA_INCOMPATIBLE: downgrade or foreign database refused");
        if (!version) {
            if (!options.writable ||
                scalar("SELECT count(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'") != 0)
                throw Error("STATE_UNINITIALIZED_OR_FOREIGN_DATABASE");
            sql("BEGIN IMMEDIATE;");
            ScopeExit rollback([&] { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); });
            sql(R"SQL(
CREATE TABLE control(singleton INTEGER PRIMARY KEY CHECK(singleton=1),generation INTEGER NOT NULL,batch_count INTEGER NOT NULL DEFAULT 0);
INSERT INTO control VALUES(1,0,0);
CREATE TABLE batches(id TEXT PRIMARY KEY,fingerprint TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE records(kind TEXT NOT NULL,id TEXT NOT NULL,principal TEXT NOT NULL,group_id TEXT NOT NULL,status TEXT NOT NULL,revision INTEGER NOT NULL,data TEXT NOT NULL,PRIMARY KEY(kind,id)) WITHOUT ROWID;
CREATE INDEX records_principal ON records(kind,principal,id);
CREATE INDEX records_group ON records(kind,group_id,id);
CREATE INDEX records_status ON records(kind,status,id);
CREATE INDEX records_principal_status ON records(kind,principal,status,id);
CREATE INDEX records_group_status ON records(kind,group_id,status,id);
CREATE INDEX records_principal_group ON records(kind,principal,group_id,id);
CREATE INDEX records_principal_group_status ON records(kind,principal,group_id,status,id);
CREATE TABLE counts(kind TEXT NOT NULL,principal TEXT NOT NULL,status TEXT NOT NULL,n INTEGER NOT NULL CHECK(n>=0),PRIMARY KEY(kind,principal,status)) WITHOUT ROWID;
CREATE TRIGGER records_insert AFTER INSERT ON records BEGIN
INSERT INTO counts VALUES(new.kind,'*','*',1) ON CONFLICT DO UPDATE SET n=n+1;
INSERT INTO counts VALUES(new.kind,new.principal,'*',1) ON CONFLICT DO UPDATE SET n=n+1;
INSERT INTO counts VALUES(new.kind,'*',new.status,1) ON CONFLICT DO UPDATE SET n=n+1;
INSERT INTO counts VALUES(new.kind,new.principal,new.status,1) ON CONFLICT DO UPDATE SET n=n+1;
END;
CREATE TRIGGER records_update AFTER UPDATE ON records BEGIN
UPDATE counts SET n=n-1 WHERE kind=old.kind AND principal IN('*',old.principal) AND status IN('*',old.status);
INSERT INTO counts VALUES(new.kind,'*','*',1) ON CONFLICT DO UPDATE SET n=n+1;
INSERT INTO counts VALUES(new.kind,new.principal,'*',1) ON CONFLICT DO UPDATE SET n=n+1;
INSERT INTO counts VALUES(new.kind,'*',new.status,1) ON CONFLICT DO UPDATE SET n=n+1;
INSERT INTO counts VALUES(new.kind,new.principal,new.status,1) ON CONFLICT DO UPDATE SET n=n+1;
END;
CREATE TABLE events(run_id TEXT NOT NULL,sequence INTEGER NOT NULL,type TEXT NOT NULL,data TEXT NOT NULL,PRIMARY KEY(run_id,sequence)) WITHOUT ROWID;
PRAGMA application_id=1145198129;
PRAGMA user_version=2;
)SQL");
            sql("COMMIT;");
            rollback.disarm();
        }
        if (version == 1) {
            if (!options.writable)
                throw Error("STATE_SCHEMA_REQUIRES_MIGRATION");
            sql("BEGIN IMMEDIATE;");
            ScopeExit rollback([&] { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); });
            sql("CREATE TABLE batches(id TEXT PRIMARY KEY,fingerprint TEXT NOT NULL) WITHOUT ROWID; "
                "ALTER TABLE control ADD COLUMN batch_count INTEGER NOT NULL DEFAULT 0; "
                "UPDATE control SET generation=generation+1 WHERE singleton=1; PRAGMA user_version=2;");
            sql("COMMIT;");
            rollback.disarm();
        }
        if (options.writable) {
            {
                Statement mode(db_, "PRAGMA journal_mode=WAL");
                if (!mode.step() || mode.text(0) != "wal")
                    throw Error("STATE_WAL_UNAVAILABLE");
            }
            sql("PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=1000;");
            if (options.maximum_bytes < 1024 * 1024 || options.maximum_bytes > 64ULL * 1024 * 1024 * 1024)
                throw Error("STATE_DISK_BUDGET_RANGE");
            const auto pages = options.maximum_bytes / scalar("PRAGMA page_size");
            {
                Statement bound(db_, "PRAGMA max_page_count=" + std::to_string(pages));
                if (!bound.step() || bound.integer(0) > pages)
                    throw Error("STATE_DISK_BUDGET_EXCEEDED");
            }
            sql("BEGIN IMMEDIATE;");
            ScopeExit rollback([&] { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); });
            generation_ = scalar("SELECT generation FROM control WHERE singleton=1") + 1;
            if (generation_ > max_safe_integer)
                throw Error("STATE_GENERATION_EXHAUSTED");
            Statement fence(db_, "UPDATE control SET generation=? WHERE singleton=1");
            fence.bind(1, generation_);
            fence.step();
            sql("COMMIT;");
            rollback.disarm();
        } else {
            Statement mode(db_, "PRAGMA journal_mode");
            if (!mode.step() || mode.text(0) != "wal")
                throw Error("STATE_WAL_UNAVAILABLE");
            generation_ = scalar("SELECT generation FROM control WHERE singleton=1");
        }
        cleanup.disarm();
    }
    ~SqliteStore() override {
        if (db_)
            sqlite3_close_v2(db_);
    }
    std::optional<StateRecord> get(std::string_view kind, std::string_view id) const override {
        key(kind);
        key(id);
        std::lock_guard lock(mutex_);
        Statement row(
            db_, "SELECT kind,id,principal,group_id,status,revision,data FROM records WHERE kind=? AND id=?");
        row.bind(1, kind);
        row.bind(2, id);
        auto result = row.step() ? std::optional(record(row)) : std::nullopt;
        last_query_steps_ = row.steps();
        return result;
    }
    StatePage list(const StateQuery& query) const override {
        key(query.kind);
        if (query.after)
            key(*query.after, true);
        if (!query.limit || query.limit > 100)
            throw Error("STATE_PAGE_LIMIT");
        std::lock_guard lock(mutex_);
        // Choose the covering filter prefix explicitly. Statistics can lag behind rapid admission;
        // planner guesses must not turn a small page into a scan of unrelated owners/groups.
        std::string index;
        if (query.principal)
            index = "records_principal";
        if (query.group)
            index += index.empty() ? "records_group" : "_group";
        if (query.status)
            index += index.empty() ? "records_status" : "_status";
        std::string sql = "SELECT kind,id,principal,group_id,status,revision,data FROM records";
        if (!index.empty())
            sql += " INDEXED BY " + index;
        sql += " WHERE kind=? AND id>?";
        for (const auto& [value, column] :
             {std::pair{query.principal, "principal"}, {query.group, "group_id"}, {query.status, "status"}})
            if (value) {
                key(*value, true);
                sql += std::string(" AND ") + column + "=?";
            }
        sql += " ORDER BY id LIMIT ?";
        Statement row(db_, sql);
        row.bind(1, query.kind);
        row.bind(2, query.after.value_or(""));
        int at = 3;
        for (const auto& value : {query.principal, query.group, query.status})
            if (value)
                row.bind(at++, *value);
        row.bind(at, query.limit + 1);
        StatePage page;
        std::size_t page_bytes = 0;
        while (row.step()) {
            if (page.records.size() == query.limit ||
                (!page.records.empty() && row.bytes(6) > 4 * 1024 * 1024 - page_bytes)) {
                page.next = page.records.back().id;
                break;
            }
            page_bytes += row.bytes(6);
            page.records.push_back(record(row));
        }
        last_query_steps_ = row.steps();
        return page;
    }
    std::uint64_t count(std::string_view kind, const std::optional<std::string>& principal,
                        const std::optional<std::string>& status) const override {
        key(kind);
        if (principal)
            key(*principal);
        if (status)
            key(*status);
        std::lock_guard lock(mutex_);
        Statement row(db_, "SELECT n FROM counts WHERE kind=? AND principal=? AND status=?");
        row.bind(1, kind);
        row.bind(2, principal.value_or("*"));
        row.bind(3, status.value_or("*"));
        const auto result = row.step() ? row.integer(0) : 0;
        last_query_steps_ = row.steps();
        return result;
    }
    void apply(std::span<const StateMutation> mutations, std::span<const StateEvent> events) override {
        (void)apply_impl({}, mutations, events);
    }
    bool apply_once(std::string_view id, std::span<const StateMutation> mutations,
                    std::span<const StateEvent> events) override {
        key(id);
        return apply_impl(std::string(id), mutations, events);
    }
    bool apply_impl(const std::optional<std::string>& batch, std::span<const StateMutation> mutations,
                    std::span<const StateEvent> events) {
        if (mutations.size() > 256 || events.size() > 256 || (mutations.empty() && events.empty()))
            throw Error("STATE_BATCH_LIMIT");
        std::string fingerprint;
        if (batch) {
            Json intent{{"mutations", Json::array()}, {"events", Json::array()}};
            std::size_t bytes = 0;
            for (const auto& change : mutations) {
                const auto& value = change.record;
                bytes += value.data.dump().size() + value.kind.size() + value.id.size() +
                         value.principal.size() + value.group.size() + value.status.size() + 256;
                if (bytes > 8 * 1024 * 1024)
                    throw Error("STATE_BATCH_BYTES");
                intent["mutations"].push_back(Json{{"kind", value.kind},
                                                   {"id", value.id},
                                                   {"principal", value.principal},
                                                   {"group", value.group},
                                                   {"status", value.status},
                                                   {"expected_revision", change.expected_revision},
                                                   {"data", value.data}});
            }
            for (const auto& event : events) {
                bytes += event.data.dump().size() + event.run.size() + event.type.size() + 128;
                if (bytes > 8 * 1024 * 1024)
                    throw Error("STATE_BATCH_BYTES");
                intent["events"].push_back(Json{{"run", event.run},
                                                {"type", event.type},
                                                {"sequence", event.sequence},
                                                {"data", event.data}});
            }
            fingerprint = sha256(bounded_json_dump(canonical_json(intent), 8 * 1024 * 1024));
        }
        std::lock_guard lock(mutex_);
        writable();
        sql("BEGIN IMMEDIATE;");
        ScopeExit rollback([&] { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); });
        writable();
        if (batch) {
            Statement receipt(db_, "SELECT fingerprint FROM batches WHERE id=?");
            receipt.bind(1, *batch);
            if (receipt.step()) {
                if (receipt.text(0) != fingerprint)
                    throw Error("STATE_BATCH_ID_CONFLICT");
                return true;
            }
            if (scalar("SELECT batch_count FROM control WHERE singleton=1") >= 1000000)
                throw Error("STATE_BATCH_RECEIPT_CAPACITY");
        }
        for (const auto& change : mutations) {
            const auto& value = change.record;
            for (const auto* field : {&value.kind, &value.id, &value.principal, &value.status})
                key(*field);
            key(value.group, true);
            if (change.expected_revision >= max_safe_integer)
                throw Error("STATE_REVISION_EXHAUSTED");
            const auto data = value.data.dump();
            if (data.size() > maximum_record)
                throw Error("STATE_RECORD_SIZE: store artifacts separately");
            Statement previous(db_, "SELECT revision,principal,group_id FROM records WHERE kind=? AND id=?");
            previous.bind(1, value.kind);
            previous.bind(2, value.id);
            const bool exists = previous.step();
            const auto revision = exists ? previous.integer(0) : 0;
            if (revision != change.expected_revision)
                throw Error("STATE_REVISION_CONFLICT");
            if (exists && (previous.text(1) != value.principal || previous.text(2) != value.group))
                throw Error("STATE_OWNERSHIP_CHANGE_DENIED");
            Statement write(db_,
                            "INSERT INTO records VALUES(?,?,?,?,?,?,?) ON CONFLICT(kind,id) DO UPDATE SET "
                            "principal=excluded.principal,group_id=excluded.group_id,status=excluded.status,"
                            "revision=excluded.revision,data=excluded.data");
            int at = 1;
            for (const auto* field : {&value.kind, &value.id, &value.principal, &value.group, &value.status})
                write.bind(at++, *field);
            write.bind(6, revision + 1);
            write.bind(7, data);
            write.step();
            if (transition_hook_)
                transition_hook_("mutation_written");
        }
        for (const auto& event : events) {
            key(event.run);
            key(event.type);
            const auto data = event.data.dump();
            if (data.size() > maximum_record)
                throw Error("STATE_EVENT_SIZE");
            Statement last(db_, "SELECT coalesce(max(sequence),0) FROM events WHERE run_id=?");
            last.bind(1, event.run);
            last.step();
            const auto sequence = last.integer(0) + 1;
            if (sequence > max_safe_integer || (event.sequence && event.sequence != sequence))
                throw Error("STATE_EVENT_SEQUENCE_CONFLICT");
            Statement write(db_, "INSERT INTO events VALUES(?,?,?,?)");
            write.bind(1, event.run);
            write.bind(2, sequence);
            write.bind(3, event.type);
            write.bind(4, data);
            write.step();
            if (transition_hook_)
                transition_hook_("event_written");
        }
        if (batch) {
            Statement receipt(db_, "INSERT INTO batches VALUES(?,?)");
            receipt.bind(1, *batch);
            receipt.bind(2, fingerprint);
            receipt.step();
            sql("UPDATE control SET batch_count=batch_count+1 WHERE singleton=1");
        }
        if (transition_hook_)
            transition_hook_("before_commit");
        sql("COMMIT;");
        rollback.disarm();
        if (transition_hook_)
            transition_hook_("after_commit");
        return false;
    }
    std::vector<StateEvent> events(std::string_view run, std::uint64_t after,
                                   std::size_t limit) const override {
        key(run);
        if (!limit || limit > 100)
            throw Error("STATE_PAGE_LIMIT");
        std::lock_guard lock(mutex_);
        Statement row(db_, "SELECT run_id,type,sequence,data FROM events WHERE run_id=? AND sequence>? ORDER "
                           "BY sequence LIMIT ?");
        row.bind(1, run);
        row.bind(2, after);
        row.bind(3, limit);
        std::vector<StateEvent> result;
        std::size_t page_bytes = 0;
        while (row.step()) {
            if (!result.empty() && row.bytes(3) > 4 * 1024 * 1024 - page_bytes)
                break;
            page_bytes += row.bytes(3);
            result.push_back(StateEvent{row.text(0), row.text(1), row.integer(2), Json::parse(row.text(3))});
        }
        last_query_steps_ = row.steps();
        return result;
    }
    std::uint64_t generation() const override {
        return generation_;
    }
    void release_writer() override {
        std::lock_guard lock(mutex_);
        writer_.reset();
    }
    Json diagnostics() const override {
        std::lock_guard lock(mutex_);
        return Json{{"schema_version", schema_version},
                    {"writer_generation", generation_},
                    {"writable", writer_ != nullptr},
                    {"sqlite_version", sqlite3_libversion()},
                    {"last_query_vm_steps", last_query_steps_},
                    {"journal_mode", "wal"},
                    {"durability", "full"},
                    {"artifact_storage", "external_files"}};
    }
};
} // namespace
std::shared_ptr<StateStore> open_state_store(const fs::path& directory, StateStoreOptions options) {
    return std::make_shared<SqliteStore>(directory, options);
}
void ensure_private_state_directory(const fs::path& directory) {
    private_local_directory(directory, true);
}
} // namespace devbox
