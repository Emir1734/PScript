#include "stdlib/stdlib.h"
#include "stdlib/native_boundary.h"
#include "stdlib/native_impl.h"

#include "runtime/runtime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace pscript {
namespace {

[[noreturn]] void math_fail(const std::string& message) {
    throw RuntimeError(message, RuntimeErrorCategory::Value);
}

double to_float(const Value& value) {
    if (value.kind == ValueKind::Integer) {
        return static_cast<double>(value.integer);
    }
    if (value.kind == ValueKind::Float) {
        return value.floating;
    }
    math_fail("math function expects a numeric argument");
}

Value from_double(double value, bool prefer_int) {
    if (prefer_int && std::floor(value) == value && value >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
        value <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return Runtime::make_integer(static_cast<std::int64_t>(value));
    }
    return Runtime::make_float(value);
}

Value unary_numeric(const Value& arg, double (*fn)(double), bool prefer_int_input) {
    const bool prefer_int = prefer_int_input && arg.kind == ValueKind::Integer;
    return from_double(fn(to_float(arg)), prefer_int);
}

}  // namespace

Value call_native_function(NativeFunctionId id, Runtime& runtime, const std::vector<Value>& args) try {
    if (native_boundary::test_fault_hook != nullptr) {
        native_boundary::test_fault_hook();
    }
    switch (id) {
        case NativeFunctionId::MathSqrt: {
            if (args.size() != 1) {
                math_fail("'sqrt' expects 1 argument");
            }
            const double value = to_float(args[0]);
            if (value < 0.0) {
                math_fail("math domain error");
            }
            return from_double(std::sqrt(value), args[0].kind == ValueKind::Integer);
        }
        case NativeFunctionId::MathPow: {
            if (args.size() != 2) {
                math_fail("'pow' expects 2 arguments");
            }
            const bool prefer_int = args[0].kind == ValueKind::Integer && args[1].kind == ValueKind::Integer;
            return from_double(std::pow(to_float(args[0]), to_float(args[1])), prefer_int);
        }
        case NativeFunctionId::MathFloor:
            if (args.size() != 1) {
                math_fail("'floor' expects 1 argument");
            }
            return from_double(std::floor(to_float(args[0])), false);
        case NativeFunctionId::MathCeil:
            if (args.size() != 1) {
                math_fail("'ceil' expects 1 argument");
            }
            return from_double(std::ceil(to_float(args[0])), false);
        case NativeFunctionId::MathSin:
            if (args.size() != 1) {
                math_fail("'sin' expects 1 argument");
            }
            return unary_numeric(args[0], std::sin, false);
        case NativeFunctionId::MathCos:
            if (args.size() != 1) {
                math_fail("'cos' expects 1 argument");
            }
            return unary_numeric(args[0], std::cos, false);
        case NativeFunctionId::MathTan:
            if (args.size() != 1) {
                math_fail("'tan' expects 1 argument");
            }
            return unary_numeric(args[0], std::tan, false);
        case NativeFunctionId::MathAsin: {
            if (args.size() != 1) {
                math_fail("'asin' expects 1 argument");
            }
            const double value = to_float(args[0]);
            if (value < -1.0 || value > 1.0) {
                math_fail("math domain error");
            }
            return from_double(std::asin(value), false);
        }
        case NativeFunctionId::MathAcos: {
            if (args.size() != 1) {
                math_fail("'acos' expects 1 argument");
            }
            const double value = to_float(args[0]);
            if (value < -1.0 || value > 1.0) {
                math_fail("math domain error");
            }
            return from_double(std::acos(value), false);
        }
        case NativeFunctionId::MathAtan:
            if (args.size() != 1) {
                math_fail("'atan' expects 1 argument");
            }
            return unary_numeric(args[0], std::atan, false);
        case NativeFunctionId::MathAtan2: {
            if (args.size() != 2) {
                math_fail("'atan2' expects 2 arguments");
            }
            return from_double(std::atan2(to_float(args[0]), to_float(args[1])), false);
        }
        case NativeFunctionId::MathLog: {
            if (args.size() != 1) {
                math_fail("'log' expects 1 argument");
            }
            const double value = to_float(args[0]);
            if (value <= 0.0) {
                math_fail("math domain error");
            }
            return from_double(std::log(value), false);
        }
        case NativeFunctionId::MathLog10: {
            if (args.size() != 1) {
                math_fail("'log10' expects 1 argument");
            }
            const double value = to_float(args[0]);
            if (value <= 0.0) {
                math_fail("math domain error");
            }
            return from_double(std::log10(value), false);
        }
        case NativeFunctionId::MathExp:
            if (args.size() != 1) {
                math_fail("'exp' expects 1 argument");
            }
            return unary_numeric(args[0], std::exp, false);
        case NativeFunctionId::StringJoin: {
            if (args.size() != 2) {
                throw RuntimeError("'join' expects 2 arguments");
            }
            if (args[0].kind != ValueKind::List || !args[0].list) {
                throw RuntimeError("'join' expects a list as first argument", RuntimeErrorCategory::Type);
            }
            if (args[1].kind != ValueKind::String) {
                throw RuntimeError("'join' expects a string separator", RuntimeErrorCategory::Type);
            }
            std::string out;
            const std::string& sep = args[1].string;
            for (std::size_t i = 0; i < args[0].list->items.size(); ++i) {
                if (i > 0) {
                    out += sep;
                }
                out += Runtime::format(args[0].list->items[i]);
            }
            return Runtime::make_string(std::move(out));
        }
        case NativeFunctionId::PathJoin:
            return native_path_join(args);
        case NativeFunctionId::PathBasename:
            return native_path_basename(args);
        case NativeFunctionId::PathDirname:
            return native_path_dirname(args);
        case NativeFunctionId::PathExtension:
            return native_path_extension(args);
        case NativeFunctionId::PathStem:
            return native_path_stem(args);
        case NativeFunctionId::PathNormalize:
            return native_path_normalize(args);
        case NativeFunctionId::PathAbsolute:
            return native_path_absolute(args);
        case NativeFunctionId::PathResolve:
            return native_path_resolve(args);
        case NativeFunctionId::PathIsAbsolute:
            return native_path_is_absolute(args);
        case NativeFunctionId::PathIsRelative:
            return native_path_is_relative(args);
        case NativeFunctionId::PathRelative:
            return native_path_relative(args);
        case NativeFunctionId::FsExists:
            return native_fs_exists(args);
        case NativeFunctionId::FsIsFile:
            return native_fs_is_file(args);
        case NativeFunctionId::FsIsDir:
            return native_fs_is_dir(args);
        case NativeFunctionId::FsRead:
            return native_fs_read(args);
        case NativeFunctionId::FsWrite:
            return native_fs_write(args);
        case NativeFunctionId::FsAppend:
            return native_fs_append(args);
        case NativeFunctionId::FsCopy:
            return native_fs_copy(args);
        case NativeFunctionId::FsMove:
            return native_fs_move(args);
        case NativeFunctionId::FsRemove:
            return native_fs_remove(args);
        case NativeFunctionId::FsCreateDir:
            return native_fs_create_dir(args);
        case NativeFunctionId::FsRemoveDir:
            return native_fs_remove_dir(args);
        case NativeFunctionId::FsListDir:
            return native_fs_list_dir(args);
        case NativeFunctionId::FsAsyncRead:
            return native_fs_async_read(runtime, args);
        case NativeFunctionId::FsAsyncWrite:
            return native_fs_async_write(runtime, args);
        case NativeFunctionId::FsAsyncAppend:
            return native_fs_async_append(runtime, args);
        case NativeFunctionId::IoOpen:
            return native_io_open(args);
        case NativeFunctionId::OsName:
            return native_os_name(args);
        case NativeFunctionId::OsPlatform:
            return native_os_platform(args);
        case NativeFunctionId::OsArch:
            return native_os_arch(args);
        case NativeFunctionId::OsCwd:
            return native_os_cwd(args);
        case NativeFunctionId::OsChdir:
            return native_os_chdir(args);
        case NativeFunctionId::OsEnv:
            return native_os_env(args);
        case NativeFunctionId::OsSetEnv:
            return native_os_set_env(args);
        case NativeFunctionId::OsRemoveEnv:
            return native_os_remove_env(args);
        case NativeFunctionId::OsHasEnv:
            return native_os_has_env(args);
        case NativeFunctionId::OsHomeDir:
            return native_os_home_dir(args);
        case NativeFunctionId::OsTempDir:
            return native_os_temp_dir(args);
        case NativeFunctionId::ProcessId:
            return native_process_id(args);
        case NativeFunctionId::ProcessExit:
            native_process_exit(runtime, args);
            return Runtime::make_null();
        case NativeFunctionId::ProcessRun:
            return native_process_run(args);
        case NativeFunctionId::TimeNow:
            return native_time_now(args);
        case NativeFunctionId::TimeUnix:
            return native_time_unix(args);
        case NativeFunctionId::TimeMonotonic:
            return native_time_monotonic(args);
        case NativeFunctionId::TimeSleep:
            return native_time_sleep(args);
        case NativeFunctionId::DateTimeNow:
            return native_datetime_now(args);
        case NativeFunctionId::DateTimeToday:
            return native_datetime_today(args);
        case NativeFunctionId::DateTimeUtcNow:
            return native_datetime_utc_now(args);
        case NativeFunctionId::DateTimeFromUnix:
            return native_datetime_from_unix(args);
        case NativeFunctionId::DateTimeCreate:
            return native_datetime_create(args);
        case NativeFunctionId::DateTimeParse:
            return native_datetime_parse(args);
        case NativeFunctionId::RandomInt:
            return native_random_int(args);
        case NativeFunctionId::RandomFloat:
            return native_random_float(args);
        case NativeFunctionId::RandomChoice:
            return native_random_choice(args);
        case NativeFunctionId::RandomShuffle:
            return native_random_shuffle(args);
        case NativeFunctionId::RandomSeed:
            return native_random_seed(args);
        case NativeFunctionId::JsonEncode:
            return native_json_encode(args);
        case NativeFunctionId::JsonDecode:
            return native_json_decode(args);
        case NativeFunctionId::JsonRead:
            return native_json_read(args);
        case NativeFunctionId::JsonWrite:
            return native_json_write(args);
        case NativeFunctionId::JsonValid:
            return native_json_valid(args);
        case NativeFunctionId::ReIsMatch:
            return native_re_is_match(args);
        case NativeFunctionId::ReSearch:
            return native_re_search(args);
        case NativeFunctionId::ReFindAll:
            return native_re_find_all(args);
        case NativeFunctionId::ReReplace:
            return native_re_replace(args);
        case NativeFunctionId::ReSplit:
            return native_re_split(args);
        case NativeFunctionId::EncodingEncode:
            return native_encoding_encode(args);
        case NativeFunctionId::EncodingDecode:
            return native_encoding_decode(args);
        case NativeFunctionId::NetConnect:
            return native_net_connect(runtime, args);
        case NativeFunctionId::NetListen:
            return native_net_listen(runtime, args);
        case NativeFunctionId::NetUdp:
            return native_net_udp(runtime, args);
        case NativeFunctionId::NetResolve:
            return native_net_resolve(runtime, args);
        case NativeFunctionId::HttpGet:
            return native_http_get(runtime, args);
        case NativeFunctionId::HttpPost:
            return native_http_post(runtime, args);
        case NativeFunctionId::HttpRequest:
            return native_http_request(runtime, args);
        case NativeFunctionId::HttpServer:
            return native_http_server(runtime, args);
        case NativeFunctionId::LogDebug:
            return native_log_debug(runtime, args);
        case NativeFunctionId::LogInfo:
            return native_log_info(runtime, args);
        case NativeFunctionId::LogWarning:
            return native_log_warning(runtime, args);
        case NativeFunctionId::LogError:
            return native_log_error(runtime, args);
        case NativeFunctionId::LogSetLevel:
            return native_log_set_level(runtime, args);
        case NativeFunctionId::CryptoSha256:
            return native_crypto_sha256(args);
        case NativeFunctionId::CryptoSha512:
            return native_crypto_sha512(args);
        case NativeFunctionId::CryptoSha3_256:
            return native_crypto_sha3_256(args);
        case NativeFunctionId::CryptoSha3_512:
            return native_crypto_sha3_512(args);
        case NativeFunctionId::CryptoBlake2b:
            return native_crypto_blake2b(args);
        case NativeFunctionId::CryptoBlake2s:
            return native_crypto_blake2s(args);
        case NativeFunctionId::CryptoHmacSha256:
            return native_crypto_hmac_sha256(args);
        case NativeFunctionId::CryptoHmacSha512:
            return native_crypto_hmac_sha512(args);
        case NativeFunctionId::CryptoRandomBytes:
            return native_crypto_random_bytes(args);
        case NativeFunctionId::CryptoCompare:
            return native_crypto_compare(args);
        case NativeFunctionId::CryptoHasher:
            return native_crypto_hasher(args);
        case NativeFunctionId::CryptoHashFile:
            return native_crypto_hash_file(args);
        case NativeFunctionId::CryptoHashIo:
            return native_crypto_hash_io(runtime, args);
        case NativeFunctionId::CryptoPbkdf2Sha256:
            return native_crypto_pbkdf2_sha256(args);
        case NativeFunctionId::CryptoScrypt:
            return native_crypto_scrypt(args);
        case NativeFunctionId::CryptoArgon2id:
            return native_crypto_argon2id(args);
        case NativeFunctionId::CryptoEncryptAesGcm:
            return native_crypto_encrypt_aes_gcm(args);
        case NativeFunctionId::CryptoDecryptAesGcm:
            return native_crypto_decrypt_aes_gcm(args);
        case NativeFunctionId::CryptoEncryptChacha20Poly1305:
            return native_crypto_encrypt_chacha20_poly1305(args);
        case NativeFunctionId::CryptoDecryptChacha20Poly1305:
            return native_crypto_decrypt_chacha20_poly1305(args);
        case NativeFunctionId::SqlOpen:
            return native_sql_open(runtime, args);
        case NativeFunctionId::TestingEqual:
            return native_testing_equal(runtime, args);
        case NativeFunctionId::TestingNotEqual:
            return native_testing_not_equal(runtime, args);
        case NativeFunctionId::TestingTrue:
            return native_testing_true(runtime, args);
        case NativeFunctionId::TestingFalse:
            return native_testing_false(runtime, args);
        case NativeFunctionId::TestingNull:
            return native_testing_null(runtime, args);
        case NativeFunctionId::TestingNotNull:
            return native_testing_not_null(runtime, args);
        case NativeFunctionId::TestingThrows:
            return native_testing_throws(runtime, args);
        case NativeFunctionId::TestingFail:
            return native_testing_fail(runtime, args);
        case NativeFunctionId::TestingRun:
            return native_testing_run(runtime, args);
        case NativeFunctionId::CliArgs:
            return native_cli_args(runtime, args);
        case NativeFunctionId::CliArg:
            return native_cli_arg(runtime, args);
        case NativeFunctionId::CliOption:
            return native_cli_option(runtime, args);
        case NativeFunctionId::CliFlag:
            return native_cli_flag(runtime, args);
        case NativeFunctionId::CliHas:
            return native_cli_has(runtime, args);
        case NativeFunctionId::CliCommand:
            return native_cli_command(runtime, args);
        case NativeFunctionId::CliHelp:
            return native_cli_help(runtime, args);
        case NativeFunctionId::CliVersion:
            return native_cli_version(runtime, args);
        case NativeFunctionId::UrlParse:
            return native_url_parse(args);
        case NativeFunctionId::UrlBuild:
            return native_url_build(args);
        case NativeFunctionId::UrlJoin:
            return native_url_join(args);
        case NativeFunctionId::UrlEncode:
            return native_url_encode(args);
        case NativeFunctionId::UrlDecode:
            return native_url_decode(args);
        case NativeFunctionId::UrlParseQuery:
            return native_url_parse_query(args);
        case NativeFunctionId::UrlBuildQuery:
            return native_url_build_query(args);
        case NativeFunctionId::UuidV4:
            return native_uuid_v4(args);
        case NativeFunctionId::UuidV7:
            return native_uuid_v7(args);
        case NativeFunctionId::UuidParse:
            return native_uuid_parse(args);
        case NativeFunctionId::CsvParse:
            return native_csv_parse(args);
        case NativeFunctionId::CsvStringify:
            return native_csv_stringify(args);
        case NativeFunctionId::CsvReader:
            return native_csv_reader(args);
        case NativeFunctionId::CsvWriter:
            return native_csv_writer(args);
        case NativeFunctionId::XmlParse:
            return native_xml_parse(args);
        case NativeFunctionId::XmlStringify:
            return native_xml_stringify(args);
        case NativeFunctionId::ArchiveOpen:
            return native_archive_open(runtime, args);
        case NativeFunctionId::ArchiveCreate:
            return native_archive_create(runtime, args);
    }
    throw RuntimeError("unknown native function");
} catch (const RuntimeError&) {
    throw;
} catch (const ProcessExit&) {
    throw;
} catch (const std::bad_alloc&) {
    throw RuntimeError("out of memory");
} catch (const std::exception& ex) {
    throw RuntimeError(std::string(ex.what()));
}

}  // namespace pscript
