#pragma once

#include "compiler/bytecode.h"
#include "runtime/runtime.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pscript {

enum class StdlibModuleId {
    Math,
    String,
    Collections,
    Path,
    Fs,
    Io,
    Os,
    Process,
    Sys,
    Time,
    DateTime,
    Random,
    Json,
    Re,
    Encoding,
    Net,
    Http,
    Log,
    Crypto,
    Sql,
    Testing,
    Cli,
    Url,
    Uuid,
    Csv,
    Xml,
    Archive,
};

enum class NativeFunctionId : std::uint32_t {
    MathSqrt = 1,
    MathPow,
    MathFloor,
    MathCeil,
    MathSin,
    MathCos,
    MathTan,
    MathAsin,
    MathAcos,
    MathAtan,
    MathAtan2,
    MathLog,
    MathLog10,
    MathExp,
    StringJoin,
    PathJoin,
    PathBasename,
    PathDirname,
    PathExtension,
    PathStem,
    PathNormalize,
    PathAbsolute,
    PathResolve,
    PathIsAbsolute,
    PathIsRelative,
    PathRelative,
    FsExists,
    FsIsFile,
    FsIsDir,
    FsRead,
    FsWrite,
    FsAppend,
    FsCopy,
    FsMove,
    FsRemove,
    FsCreateDir,
    FsRemoveDir,
    FsListDir,
    FsAsyncRead,
    FsAsyncWrite,
    FsAsyncAppend,
    IoOpen,
    OsName,
    OsPlatform,
    OsArch,
    OsCwd,
    OsChdir,
    OsEnv,
    OsSetEnv,
    OsRemoveEnv,
    OsHasEnv,
    OsHomeDir,
    OsTempDir,
    ProcessId,
    ProcessExit,
    ProcessRun,
    TimeNow,
    TimeUnix,
    TimeMonotonic,
    TimeSleep,
    DateTimeNow,
    DateTimeToday,
    DateTimeUtcNow,
    DateTimeFromUnix,
    DateTimeCreate,
    DateTimeParse,
    RandomInt,
    RandomFloat,
    RandomChoice,
    RandomShuffle,
    RandomSeed,
    JsonEncode,
    JsonDecode,
    JsonRead,
    JsonWrite,
    JsonValid,
    ReIsMatch,
    ReSearch,
    ReFindAll,
    ReReplace,
    ReSplit,
    EncodingEncode,
    EncodingDecode,
    NetConnect,
    NetListen,
    NetUdp,
    NetResolve,
    HttpGet,
    HttpPost,
    HttpRequest,
    HttpServer,
    LogDebug,
    LogInfo,
    LogWarning,
    LogError,
    LogSetLevel,
    CryptoSha256,
    CryptoSha512,
    CryptoSha3_256,
    CryptoSha3_512,
    CryptoBlake2b,
    CryptoBlake2s,
    CryptoHmacSha256,
    CryptoHmacSha512,
    CryptoRandomBytes,
    CryptoCompare,
    CryptoHasher,
    CryptoHashFile,
    CryptoHashIo,
    CryptoPbkdf2Sha256,
    CryptoScrypt,
    CryptoArgon2id,
    CryptoEncryptAesGcm,
    CryptoDecryptAesGcm,
    CryptoEncryptChacha20Poly1305,
    CryptoDecryptChacha20Poly1305,
    SqlOpen,
    TestingEqual,
    TestingNotEqual,
    TestingTrue,
    TestingFalse,
    TestingNull,
    TestingNotNull,
    TestingThrows,
    TestingFail,
    TestingRun,
    CliArgs,
    CliArg,
    CliOption,
    CliFlag,
    CliHas,
    CliCommand,
    CliHelp,
    CliVersion,
    UrlParse,
    UrlBuild,
    UrlJoin,
    UrlEncode,
    UrlDecode,
    UrlParseQuery,
    UrlBuildQuery,
    UuidV4,
    UuidV7,
    UuidParse,
    CsvParse,
    CsvStringify,
    CsvReader,
    CsvWriter,
    XmlParse,
    XmlStringify,
    ArchiveOpen,
    ArchiveCreate,
};

enum class NativeClassKind : std::uint32_t {
    None = 0,
    Stack = 1,
    Queue = 2,
    Deque = 3,
    FileHandle = 4,
    DateTime = 5,
    Duration = 6,
    TcpConnection = 7,
    TcpServer = 8,
    UdpSocket = 9,
    HttpResponse = 10,
    HttpRequest = 11,
    HttpServer = 12,
    CryptoHasher = 13,
    Database = 14,
    SqlRows = 15,
    SqlStatement = 16,
    SqlTransaction = 17,
    TaskGroup = 18,
    AsyncStream = 19,
    Archive = 20,
};

void append_root_async_classes(Bytecode& bc);

bool is_stdlib_module_specifier(const std::string& specifier);
StdlibModuleId stdlib_module_id(const std::string& specifier);
const std::vector<std::string_view>& stdlib_module_specifiers();
std::vector<std::string_view> stdlib_module_export_names(const std::string& specifier);

std::uint32_t register_stdlib_module(const std::string& specifier, Bytecode& root);
void initialize_native_module(std::uint32_t module_index, const Bytecode& root,
                              std::vector<std::optional<Value>>& globals, Runtime& runtime);

Value call_native_function(NativeFunctionId id, Runtime& runtime, const std::vector<Value>& args);
Value call_native_collection_method(const Value& object, std::string_view name, const std::vector<Value>& args);
Value call_native_file_handle_method(const Value& object, std::string_view name, const std::vector<Value>& args,
                                     Runtime& runtime);
Value call_native_network_method(const Value& object, std::string_view name, const std::vector<Value>& args,
                                 Runtime& runtime);
Value call_native_http_method(const Value& object, std::string_view name, const std::vector<Value>& args,
                              Runtime& runtime);
Value call_native_crypto_hasher_method(const Value& object, std::string_view name, const std::vector<Value>& args);
Value make_native_collection(NativeClassKind kind, std::uint32_t class_index);
Value make_native_file_handle(std::uint32_t class_index, std::string path, std::string mode);
Value call_native_class_method(const Value& object, std::string_view name, const std::vector<Value>& args,
                               Runtime& runtime);
Value make_native_task_group(std::uint32_t class_index);
bool is_native_task_group(const Value& value);
Value call_native_task_group_method(const Value& object, std::string_view name, const std::vector<Value>& args,
                                    Runtime& runtime);
bool is_native_class_object(const Value& object);
bool is_native_network(const Value& object);
bool is_native_http_server(const Value& object);
bool is_native_crypto_hasher(const Value& object);
bool is_native_database(const Value& object);
bool is_native_archive(const Value& object);
Value native_sql_open(Runtime& runtime, const std::vector<Value>& args);
Value native_archive_open(Runtime& runtime, const std::vector<Value>& args);
Value native_archive_create(Runtime& runtime, const std::vector<Value>& args);
Value call_native_database_method(const Value& object, std::string_view name, const std::vector<Value>& args,
                                  Runtime& runtime);
Value call_native_sql_rows_method(const Value& object, std::string_view name, const std::vector<Value>& args);
Value call_native_sql_statement_method(const Value& object, std::string_view name, const std::vector<Value>& args);
Value call_native_sql_transaction_method(const Value& object, std::string_view name, const std::vector<Value>& args,
                                         Runtime& runtime);
bool is_native_sql_statement(const Value& object);
bool is_native_sql_transaction(const Value& object);
bool is_native_collection(const Value& object);
bool is_native_datetime(const Value& object);
bool is_native_duration(const Value& object);
NativeClassKind native_class_kind(const Value& object);
NativeClassKind native_collection_kind(const Value& object);

Value datetime_add(const Value& datetime, const Value& duration);
Value datetime_subtract_duration(const Value& datetime, const Value& duration);
Value datetime_subtract_datetime(const Value& left, const Value& right);
Value duration_negate(const Value& duration);
int compare_datetime(const Value& left, const Value& right);
int compare_duration(const Value& left, const Value& right);
bool datetime_values_equal(const Value& left, const Value& right);
bool duration_values_equal(const Value& left, const Value& right);

}  // namespace pscript
