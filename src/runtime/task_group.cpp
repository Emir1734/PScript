#include "runtime/task_group.h"
#include "runtime/runtime.h"

namespace pscript {

void TaskGroupData::track(const std::shared_ptr<TaskData>& child) {
    if (!child) {
        return;
    }
    prune_dead_children();
    children.push_back(child);
}

void TaskGroupData::prune_dead_children() {
    children.erase(std::remove_if(children.begin(), children.end(),
                                  [](const std::weak_ptr<TaskData>& weak) { return weak.expired(); }),
                   children.end());
}

std::vector<std::shared_ptr<TaskData>> TaskGroupData::live_children() const {
    std::vector<std::shared_ptr<TaskData>> out;
    out.reserve(children.size());
    for (const std::weak_ptr<TaskData>& weak : children) {
        if (auto locked = weak.lock()) {
            out.push_back(std::move(locked));
        }
    }
    return out;
}

}  // namespace pscript
