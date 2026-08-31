#include "stackplz/platform.h"

#include "compat.h"

static void spz_hook_callbacks_for(const struct spz_hook_callbacks *callbacks,
                                   enum spz_hook_id id, void **before,
                                   void **after)
{
    *after = NULL;
    if (id == SPZ_HOOK_FINISH_TASK_SWITCH) {
        *before = callbacks->finish_before;
        *after = callbacks->finish_after;
    } else if (id == SPZ_HOOK_DO_EXIT) {
        *before = callbacks->exit_before;
    } else if (id == SPZ_HOOK_BREAKPOINT) {
        *before = callbacks->break_before;
    } else if (id == SPZ_HOOK_WATCHPOINT) {
        *before = callbacks->watch_before;
    } else {
        *before = callbacks->step_before;
    }
}

static uint32_t spz_hook_argument_count(const struct spz_device_profile *profile,
                                       enum spz_hook_id id)
{
    if (profile == NULL)
        return 0U;
    if (id == SPZ_HOOK_FINISH_TASK_SWITCH)
        return profile->hooks.finish_task_switch_args;
    if (id == SPZ_HOOK_DO_EXIT)
        return profile->hooks.do_exit_args;
    if (id == SPZ_HOOK_BREAKPOINT)
        return profile->hooks.breakpoint_handler_args;
    if (id == SPZ_HOOK_WATCHPOINT)
        return profile->hooks.watchpoint_handler_args;
    return profile->hooks.single_step_handler_args;
}

static enum spz_runtime_symbol spz_hook_symbol(enum spz_hook_id id)
{
    static const enum spz_runtime_symbol symbols[SPZ_HOOK_COUNT] = {
        SPZ_SYMBOL_FINISH_TASK_SWITCH,
        SPZ_SYMBOL_DO_EXIT,
        SPZ_SYMBOL_BREAKPOINT_HANDLER,
        SPZ_SYMBOL_WATCHPOINT_HANDLER,
        SPZ_SYMBOL_SINGLE_STEP_HANDLER,
    };

    return symbols[(uint32_t)id];
}

void spz_hooks_remove(struct spz_hook_set *hooks)
{
    uint32_t cursor;
    uint32_t removed = 0U;

    if (hooks == NULL || hooks->backend.unwrap == NULL ||
        hooks->backend.quiesce == NULL)
        return;
    cursor = SPZ_HOOK_COUNT;
    while (cursor != 0U) {
        enum spz_hook_id id = (enum spz_hook_id)(cursor - 1U);
        void *before;
        void *after;

        cursor--;
        if (hooks->installed[(uint32_t)id] == 0U)
            continue;
        spz_hook_callbacks_for(&hooks->callbacks, id, &before, &after);
        hooks->backend.unwrap(hooks->backend.context,
                              hooks->targets[(uint32_t)id], before, after);
        hooks->installed[(uint32_t)id] = 0U;
        removed = 1U;
    }
    if (removed != 0U)
        hooks->backend.quiesce(hooks->backend.context);
}

int spz_hooks_install(struct spz_hook_set *hooks,
                      const struct spz_profile_runtime *runtime,
                      const struct spz_hook_backend *backend,
                      const struct spz_hook_callbacks *callbacks, void *udata)
{
    uint32_t index;

    if (hooks == NULL || runtime == NULL || backend == NULL || callbacks == NULL ||
        backend->wrap == NULL || backend->unwrap == NULL ||
        backend->quiesce == NULL ||
        runtime->state != SPZ_PROFILE_READY || runtime->hooks_allowed == 0U)
        return -EINVAL;
    memset(hooks, 0, sizeof(*hooks));
    hooks->backend = *backend;
    hooks->callbacks = *callbacks;
    hooks->udata = udata;

    for (index = 0U; index < SPZ_HOOK_COUNT; index++) {
        enum spz_hook_id id = (enum spz_hook_id)index;
        void *before;
        void *after;
        uint32_t argument_count;
        int result;

        hooks->targets[index] = runtime->symbols[spz_hook_symbol(id)];
        if (hooks->targets[index] == 0U) {
            spz_hooks_remove(hooks);
            return -ENOENT;
        }
        spz_hook_callbacks_for(callbacks, id, &before, &after);
        argument_count = spz_hook_argument_count(runtime->profile, id);
        if (before == NULL || (id == SPZ_HOOK_FINISH_TASK_SWITCH && after == NULL) ||
            (argument_count != 1U && argument_count != 3U)) {
            spz_hooks_remove(hooks);
            return -EINVAL;
        }
        result = backend->wrap(backend->context, hooks->targets[index],
                               argument_count, before, after, udata);
        if (result != 0) {
            spz_hooks_remove(hooks);
            return result < 0 ? result : -EIO;
        }
        hooks->installed[index] = 1U;
    }
    return 0;
}
