#pragma once

#include <memory>
#include <vector>

namespace pscript {

struct TaskData;

struct TaskGroupData {
    std::vector<std::weak_ptr<TaskData>> children;
    std::weak_ptr<TaskData> owner_task;

    void track(const std::shared_ptr<TaskData>& child);
    void prune_dead_children();
    std::vector<std::shared_ptr<TaskData>> live_children() const;
};

}  // namespace pscript
