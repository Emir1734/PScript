#include "runtime/collection_ops.h"

#include "runtime/iterator.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace pscript {
namespace {

[[noreturn]] void collection_fail(const std::string& message) {
    throw RuntimeError(message, RuntimeErrorCategory::Type);
}

void expect_callable(const Value& value, std::string_view what) {
    if (value.kind != ValueKind::Function || !value.function) {
        collection_fail(std::string(what) + " expects a callable function");
    }
}

bool truthy_value(const Value& value) {
    return Runtime::is_truthy(value);
}

std::string key_to_string(const Value& value) {
    if (value.kind != ValueKind::String) {
        collection_fail("group_by key function must return a string");
    }
    return value.string;
}

double as_sort_number(const Value& value) {
    if (value.kind == ValueKind::Integer) {
        return static_cast<double>(value.integer);
    }
    if (value.kind == ValueKind::Float) {
        return value.floating;
    }
    collection_fail("sort_by key values must be comparable numbers or strings");
}

int compare_sort_keys(const Value& left, const Value& right) {
    if (left.kind == ValueKind::String && right.kind == ValueKind::String) {
        if (left.string < right.string) {
            return -1;
        }
        if (left.string > right.string) {
            return 1;
        }
        return 0;
    }
    if ((left.kind == ValueKind::Integer || left.kind == ValueKind::Float) &&
        (right.kind == ValueKind::Integer || right.kind == ValueKind::Float)) {
        const double a = as_sort_number(left);
        const double b = as_sort_number(right);
        if (a < b) {
            return -1;
        }
        if (a > b) {
            return 1;
        }
        return 0;
    }
    collection_fail("sort_by key values must be comparable numbers or strings");
}

bool parse_descending_flag(Runtime& runtime, const std::vector<Value>& args, std::size_t index) {
    if (args.size() <= index) {
        return false;
    }
    const Value& flag = args[index];
    if (flag.kind == ValueKind::Bool) {
        return flag.boolean;
    }
    if (flag.kind == ValueKind::Dict && flag.dict) {
        const Value* descending = Runtime::dict_lookup(*flag.dict, "descending");
        if (descending == nullptr) {
            collection_fail("sort_by options dict expects 'descending' key");
        }
        if (descending->kind != ValueKind::Bool) {
            collection_fail("sort_by descending option must be a bool");
        }
        return descending->boolean;
    }
    collection_fail("sort_by descending argument must be a bool or options dict");
}

}  // namespace

std::vector<Value> materialize_iterable(Runtime& runtime, const Value& iterable) {
    Value iterator = runtime_get_iter(iterable);
    std::vector<Value> items;
    Value element;
    while (native_iterator_try_next(iterator, element)) {
        items.push_back(std::move(element));
    }
    return items;
}

Value builtin_filter(Runtime& runtime, const std::vector<Value>& args) {
    if (args.size() != 2) {
        collection_fail("'filter' expects 2 arguments");
    }
    expect_callable(args[1], "filter");
    std::vector<Value> out;
    for (Value& item : materialize_iterable(runtime, args[0])) {
        if (truthy_value(runtime.invoke_callable(args[1], {item}))) {
            out.push_back(std::move(item));
        }
    }
    return Runtime::make_list(std::move(out));
}

Value builtin_map(Runtime& runtime, const std::vector<Value>& args) {
    if (args.size() != 2) {
        collection_fail("'map' expects 2 arguments");
    }
    expect_callable(args[1], "map");
    std::vector<Value> out;
    for (Value& item : materialize_iterable(runtime, args[0])) {
        out.push_back(runtime.invoke_callable(args[1], {item}));
    }
    return Runtime::make_list(std::move(out));
}

Value builtin_sort_by(Runtime& runtime, const std::vector<Value>& args) {
    if (args.size() < 2 || args.size() > 3) {
        collection_fail("'sort_by' expects 2 or 3 arguments");
    }
    expect_callable(args[1], "sort_by");
    const bool descending = parse_descending_flag(runtime, args, 2);
    std::vector<Value> items = materialize_iterable(runtime, args[0]);
    std::vector<std::pair<Value, Value>> keyed;
    keyed.reserve(items.size());
    for (Value& item : items) {
        keyed.emplace_back(runtime.invoke_callable(args[1], {item}), std::move(item));
    }
    const auto less = [&](const auto& a, const auto& b) { return compare_sort_keys(a.first, b.first) < 0; };
    std::stable_sort(keyed.begin(), keyed.end(), less);
    if (descending) {
        std::reverse(keyed.begin(), keyed.end());
    }
    std::vector<Value> sorted;
    sorted.reserve(keyed.size());
    for (auto& entry : keyed) {
        sorted.push_back(std::move(entry.second));
    }
    return Runtime::make_list(std::move(sorted));
}

Value builtin_group_by(Runtime& runtime, const std::vector<Value>& args) {
    if (args.size() != 2) {
        collection_fail("'group_by' expects 2 arguments");
    }
    expect_callable(args[1], "group_by");
    std::vector<std::pair<std::string, Value>> groups;
    for (Value& item : materialize_iterable(runtime, args[0])) {
        const std::string key = key_to_string(runtime.invoke_callable(args[1], {item}));
        auto found = std::find_if(groups.begin(), groups.end(),
                                  [&](const auto& entry) { return entry.first == key; });
        if (found == groups.end()) {
            groups.emplace_back(key, Runtime::make_list({item}));
        } else {
            if (!found->second.list) {
                collection_fail("internal group_by list error");
            }
            found->second.list->items.push_back(std::move(item));
        }
    }
    return Runtime::make_dict(std::move(groups));
}

Value builtin_take(Runtime& runtime, const std::vector<Value>& args) {
    if (args.size() != 2) {
        collection_fail("'take' expects 2 arguments");
    }
    if (args[1].kind != ValueKind::Integer) {
        collection_fail("'take' count must be an integer");
    }
    if (args[1].integer < 0) {
        collection_fail("'take' count must be non-negative");
    }
    const std::int64_t limit = args[1].integer;
    std::vector<Value> out;
    Value iterator = runtime_get_iter(args[0]);
    Value element;
    while (out.size() < static_cast<std::size_t>(limit) && native_iterator_try_next(iterator, element)) {
        out.push_back(std::move(element));
    }
    return Runtime::make_list(std::move(out));
}

bool parallel_callable_is_worker_safe(const Value& callable) {
    if (callable.kind != ValueKind::Function || !callable.function) {
        return false;
    }
    return callable.function->captures.empty();
}

namespace {

std::atomic<std::size_t> g_parallel_active{0};
std::atomic<std::size_t> g_parallel_peak{0};

void parallel_track_enter() {
    const std::size_t active = g_parallel_active.fetch_add(1, std::memory_order_relaxed) + 1;
    std::size_t peak = g_parallel_peak.load(std::memory_order_relaxed);
    while (active > peak && !g_parallel_peak.compare_exchange_weak(peak, active, std::memory_order_relaxed)) {
    }
}

void parallel_track_leave() {
    g_parallel_active.fetch_sub(1, std::memory_order_relaxed);
}

struct ParallelTrackGuard {
    ParallelTrackGuard() { parallel_track_enter(); }
    ~ParallelTrackGuard() { parallel_track_leave(); }
};

}  // namespace

std::size_t parallel_map_last_peak_concurrency() {
    return g_parallel_peak.load(std::memory_order_relaxed);
}

Value builtin_parallel_map(Runtime& runtime, const std::vector<Value>& args) {
    if (args.size() != 2) {
        collection_fail("'parallel.map' expects 2 arguments");
    }
    expect_callable(args[1], "parallel.map");
    if (!parallel_callable_is_worker_safe(args[1])) {
        collection_fail(
            "parallel.map requires a module function without captures (closures and captured state are not supported)");
    }
    const std::vector<Value> items = materialize_iterable(runtime, args[0]);
    if (items.empty()) {
        return Runtime::make_list({});
    }
    if (std::getenv("PSCRIPT_DISABLE_PARALLEL") != nullptr) {
        std::vector<Value> sequential;
        sequential.reserve(items.size());
        for (const Value& item : items) {
            sequential.push_back(runtime.invoke_callable(args[1], {item}));
        }
        return Runtime::make_list(std::move(sequential));
    }
    if (!runtime.has_parallel_thread_invoker_factory()) {
        std::vector<Value> sequential;
        sequential.reserve(items.size());
        for (const Value& item : items) {
            sequential.push_back(runtime.invoke_callable(args[1], {item}));
        }
        return Runtime::make_list(std::move(sequential));
    }
    if (items.size() == 1) {
        return Runtime::make_list({runtime.invoke_callable(args[1], {items[0]})});
    }

    g_parallel_peak.store(0, std::memory_order_relaxed);
    std::vector<Value> results(items.size());
    std::atomic<bool> failed{false};
    std::mutex error_mutex;
    std::exception_ptr first_error;
    std::size_t worker_count = items.size();
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw > 0) {
        worker_count = std::min(worker_count, static_cast<std::size_t>(hw));
    }
    worker_count = std::min(worker_count, static_cast<std::size_t>(16));
    if (worker_count < 2) {
        worker_count = 2;
    }
    worker_count = std::min(worker_count, items.size());

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    const auto worker = [&](std::size_t begin, std::size_t end) {
        try {
            ParallelTrackGuard track;
            Runtime::CallableInvoker invoker = runtime.create_parallel_thread_invoker();
            for (std::size_t i = begin; i < end; ++i) {
                if (failed.load(std::memory_order_relaxed)) {
                    break;
                }
                results[i] = invoker(args[1], {items[i]});
            }
        } catch (...) {
            if (!failed.exchange(true)) {
                std::lock_guard lock(error_mutex);
                first_error = std::current_exception();
            }
        }
    };
    const std::size_t chunk = (items.size() + worker_count - 1) / worker_count;
    for (std::size_t w = 0; w < worker_count; ++w) {
        const std::size_t begin = w * chunk;
        if (begin >= items.size()) {
            break;
        }
        const std::size_t end = std::min(items.size(), begin + chunk);
        workers.emplace_back(worker, begin, end);
    }
    for (std::thread& thread : workers) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    if (first_error) {
        std::rethrow_exception(first_error);
    }
    return Runtime::make_list(std::move(results));
}

}  // namespace pscript
