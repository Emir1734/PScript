#include "stdlib/stdlib.h"
#include "stdlib/native_impl.h"

#include "runtime/runtime.h"
#include "runtime/task_group.h"

namespace pscript {

bool is_native_task_group(const Value& value) {
    return value.kind == ValueKind::Class && value.class_value && value.class_value->native_task_group != nullptr;
}

Value make_native_task_group(std::uint32_t class_index) {
    Value object = Runtime::make_class(class_index, "TaskGroup", {});
    object.class_value->native_task_group = std::make_shared<TaskGroupData>();
    return object;
}

Value call_native_task_group_method(const Value& object, std::string_view name, const std::vector<Value>& args,
                                    Runtime& runtime) {
    if (!is_native_task_group(object)) {
        throw RuntimeError("invalid TaskGroup", RuntimeErrorCategory::Type);
    }
    const std::shared_ptr<TaskGroupData>& group = object.class_value->native_task_group;
    if (name == "spawn") {
        if (args.size() != 1) {
            throw RuntimeError("'spawn' expects 1 argument", RuntimeErrorCategory::Type);
        }
        if (args[0].kind != ValueKind::Task || !args[0].task) {
            throw RuntimeError("'spawn' expects a Task", RuntimeErrorCategory::Type);
        }
        group->track(args[0].task);
        args[0].task->task_group_member = group;
        if (runtime.async_hooks().track_task_group_child) {
            runtime.async_hooks().track_task_group_child(args[0].task);
        }
        return args[0];
    }
    if (name == "wait") {
        if (!args.empty()) {
            throw RuntimeError("'wait' expects 0 arguments", RuntimeErrorCategory::Type);
        }
        std::vector<Value> tasks;
        for (const std::shared_ptr<TaskData>& child : group->live_children()) {
            tasks.push_back(Runtime::make_task(child));
        }
        if (!runtime.async_hooks().create_wait_all_task) {
            throw RuntimeError("wait requires async scheduler", RuntimeErrorCategory::Runtime);
        }
        return Runtime::make_task(runtime.async_hooks().create_wait_all_task(tasks));
    }
    if (name == "enter") {
        if (!args.empty()) {
            throw RuntimeError("'enter' expects 0 arguments", RuntimeErrorCategory::Type);
        }
        if (runtime.async_hooks().push_task_group) {
            runtime.async_hooks().push_task_group(group);
        }
        if (runtime.async_hooks().track_task) {
            auto task = std::make_shared<TaskData>();
            runtime.async_hooks().track_task(task);
            task->state = TaskState::Completed;
            task->result = object;
            return Runtime::make_task(task);
        }
        return object;
    }
    if (name == "exit") {
        if (args.size() > 1) {
            throw RuntimeError("'exit' expects 0 or 1 argument", RuntimeErrorCategory::Type);
        }
        std::vector<Value> tasks;
        for (const std::shared_ptr<TaskData>& child : group->live_children()) {
            tasks.push_back(Runtime::make_task(child));
        }
        if (runtime.async_hooks().pop_task_group) {
            runtime.async_hooks().pop_task_group(group);
        }
        if (tasks.empty()) {
            if (runtime.async_hooks().track_task) {
                auto task = std::make_shared<TaskData>();
                runtime.async_hooks().track_task(task);
                task->state = TaskState::Completed;
                task->result = Runtime::make_null();
                return Runtime::make_task(task);
            }
            return Runtime::make_null();
        }
        if (!runtime.async_hooks().create_wait_all_task) {
            throw RuntimeError("exit requires async scheduler", RuntimeErrorCategory::Runtime);
        }
        return Runtime::make_task(runtime.async_hooks().create_wait_all_task(tasks));
    }
    throw RuntimeError("TaskGroup has no method '" + std::string(name) + "'", RuntimeErrorCategory::Type);
}

}  // namespace pscript
