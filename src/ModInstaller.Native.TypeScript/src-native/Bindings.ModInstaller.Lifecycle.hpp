#ifndef MODINSTALLER_LIFECYCLE_HPP
#define MODINSTALLER_LIFECYCLE_HPP

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include <algorithm>
#include <utility>
#include <type_traits>
#include <uv.h>
#include "Utils.Return.hpp"
#include "Utils.Callbacks.hpp"

namespace Bindings::ModInstaller {
using namespace Napi;
using namespace Utils;
using namespace ::ModInstaller::Native;

template<class T> void FreeResult(T* value) {
    if (!value) return;
    common_dealloc(value->error);
    if constexpr (std::is_same_v<T, return_value_json> || std::is_same_v<T, return_value_string>)
        common_dealloc(value->value);
    common_dealloc(value);
}
template<class T> T* Failure(const char16_t* message) noexcept {
    try { return Create(T{Copy(message)}); } catch (...) { return nullptr; }
}
template<class T> T* CallbackFailure(const Error& error) noexcept {
    try { return Create(T{Copy(GetErrorMessage(error))}); }
    catch (...) { return Failure<T>(u"Installer callback failed or JavaScript is closing"); }
}
inline std::optional<std::u16string> Own(const char16_t* value) {
    return value ? std::optional<std::u16string>(value) : std::nullopt;
}
inline Value ToJS(Env env, const std::optional<std::u16string>& value) {
    return value ? Value(String::New(env, *value)) : Value(env.Null());
}
inline void CheckStatus(Env env, napi_status status) {
    if (status == napi_ok) return;
    bool pending = false;
    // Capture and clear an actual JS exception before the synchronous wrapper
    // rethrows it. Creating a second Error would leave the first one pending.
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) throw Error::New(env);
    throw Error::New(env, "Failed to initialize native installer lifecycle");
}

struct PendingBase { virtual void Cancel() = 0; virtual ~PendingBase() = default; };
template<class T> struct Pending final : PendingBase {
    std::mutex gate;
    std::condition_variable cv;
    bool done = false;
    T* result = nullptr;
    ~Pending() { FreeResult(result); }
    void Complete(T* value) {
        std::lock_guard<std::mutex> lock(gate);
        if (done) { FreeResult(value); return; }
        result = value; done = true; cv.notify_one();
    }
    bool IsDone() { std::lock_guard<std::mutex> lock(gate); return done; }
    void Cancel() override { Complete(nullptr); }
    T* Take() {
        std::unique_lock<std::mutex> lock(gate);
        cv.wait(lock, [&] { return done; });
        T* value = result; result = nullptr; return value;
    }
};
struct State;
struct Task {
    std::shared_ptr<State> state;
    std::function<void(Env)> run;
};
// napi_deferred has no public deletion API if shutdown prevents settlement.
// Retain JS resolve/reject functions instead: all three references can be
// deleted without entering JS, including after Worker termination.
struct Completion {
    napi_env env;
    napi_ref promise = nullptr, resolve = nullptr, reject = nullptr;
    bool accepting = true;
    unsigned calls = 0;
    napi_status initialization = napi_ok;
    explicit Completion(napi_env value) : env(value) {}
    ~Completion() { Reset(); }
    void Reset() noexcept {
        for (auto* ref : {&promise, &resolve, &reject})
            if (*ref) napi_delete_reference(env, std::exchange(*ref, nullptr));
    }
    static std::shared_ptr<Completion> New(Napi::Env env) {
        auto result = std::make_shared<Completion>(env);
        struct Rollback {
            std::shared_ptr<Completion> value;
            bool accepted = false;
            ~Rollback() { value->accepting = false; if (!accepted) value->Reset(); }
        } rollback{result};
        // The constructor call roots this context. A replaced constructor may
        // retain the executor, but cannot retain native refs or initialize late.
        const std::weak_ptr<Completion> weak = result;
        const auto executor = Function::New(env, [weak](const CallbackInfo& info) {
            auto result = weak.lock();
            if (!result || !result->accepting) return;
            if (++result->calls != 1 || !info[0].IsFunction() || !info[1].IsFunction()) {
                result->initialization = napi_invalid_arg; return;
            }
            result->initialization = napi_create_reference(info.Env(), info[0], 1, &result->resolve);
            if (result->initialization == napi_ok)
                result->initialization = napi_create_reference(info.Env(), info[1], 1, &result->reject);
        });
        napi_value global = nullptr, constructor = nullptr, promise = nullptr, callback = executor;
        napi_status status = napi_get_global(env, &global);
        if (status == napi_ok) status = napi_get_named_property(env, global, "Promise", &constructor);
        if (status == napi_ok) status = napi_new_instance(env, constructor, 1, &callback, &promise);
        result->accepting = false;
        CheckStatus(env, status);
        CheckStatus(env, result->initialization);
        if (result->calls != 1 || !result->resolve || !result->reject)
            throw Error::New(env, "Promise constructor did not initialize installer completion");
        CheckStatus(env, napi_create_reference(env, promise, 1, &result->promise));
        rollback.accepted = true;
        return result;
    }
    Napi::Value PromiseValue() {
        napi_value value = nullptr;
        CheckStatus(env, napi_get_reference_value(env, promise, &value));
        return Napi::Value(env, value);
    }
    napi_status Conclude(napi_value value, bool rejected) noexcept {
        napi_value function = nullptr, receiver = nullptr, ignored = nullptr;
        napi_status status = napi_get_reference_value(env, rejected ? reject : resolve, &function);
        if (status == napi_ok) status = napi_get_undefined(env, &receiver);
        if (status == napi_ok) status = napi_call_function(env, receiver, function, 1, &value, &ignored);
        // Never attempt the other resolver after failure. JS may already have
        // consumed this one-shot completion before returning an error status.
        Reset();
        return status;
    }
};
struct Operation {
    std::shared_ptr<Completion> completion;
    std::optional<std::u16string> error;
    std::optional<std::u16string> json;
    bool ready = false;
    bool settled = false;
    explicit Operation(Env env) : completion(Completion::New(env)) {}
};

struct State : std::enable_shared_from_this<State> {
    napi_env env;
    uv_loop_t* loop = nullptr;
    uv_timer_t timer{};
    napi_threadsafe_function tsfn = nullptr;
    napi_async_cleanup_hook_handle hook = nullptr;
    std::shared_ptr<State>* hookRoot = nullptr;
    std::array<napi_ref, 7> callbacks{}; // weak; inner JS object's Symbol array owns callbacks
    napi_ref self = nullptr; // weak except while an accepted install is live
    void* handler = nullptr;
    const std::thread::id jsThread = std::this_thread::get_id();
    std::mutex gate;
    std::atomic<bool> closing{false};
    bool envClosing = false;
    bool finished = false;
    bool creatingOperation = false, creatingDispose = false;
    std::atomic<bool> tsfnFinalized{false};
    std::atomic<unsigned> frames{0};
    std::vector<std::weak_ptr<PendingBase>> pending;
    std::vector<std::shared_ptr<Operation>> operations;
    std::shared_ptr<Completion> dispose;
    explicit State(napi_env value) : env(value) {}

    struct Frame {
        std::shared_ptr<State> state;
        explicit Frame(std::shared_ptr<State> value) : state(std::move(value)) { ++state->frames; }
        ~Frame() { --state->frames; }
    };
    Function Callback(unsigned index) {
        napi_value value = nullptr;
        if (napi_get_reference_value(env, callbacks[index], &value) != napi_ok || !value)
            throw Error::New(env, "Installer callback is no longer available");
        return Function(env, value);
    }
    // The one bridge reference is held until close. Admission and abort share
    // this gate, so no background caller can use a released TSFN handle.
    bool Queue(std::function<void(Env)> action) {
        std::lock_guard<std::mutex> lock(gate);
        if (closing) return false;
        auto task = std::make_unique<Task>(Task{shared_from_this(), std::move(action)});
        if (napi_call_threadsafe_function(tsfn, task.get(), napi_tsfn_nonblocking) != napi_ok) return false;
        task.release();
        return true;
    }
    static void CallJS(napi_env env, napi_value, void*, void* data) noexcept {
        std::unique_ptr<Task> task(static_cast<Task*>(data));
        // N-API also drains aborted tasks with no JS environment. Owned argument
        // copies and waiter state are released in both paths.
        if (!env || task->state->closing) return;
        try { task->run(Env(env)); }
        // No C++ exception may cross this C ABI. In particular, throwing a
        // Napi::Error back into JS can itself throw during Worker termination.
        catch (...) { task->state->Close(true); }
    }
    template<class T> T* Invoke(unsigned index, std::function<T*(Env, Function)> action) noexcept {
        Frame frame(shared_from_this());
        try {
            if (closing) return Failure<T>(u"Installer is closing");
            if (std::this_thread::get_id() == jsThread) return action(Env(env), Callback(index));
            auto waiter = std::make_shared<Pending<T>>();
            {
                std::lock_guard<std::mutex> lock(gate);
                if (closing) return Failure<T>(u"Installer is closing");
                pending.erase(std::remove_if(pending.begin(), pending.end(), [](auto& item) { return item.expired(); }), pending.end());
                pending.emplace_back(waiter);
            }
            auto state = shared_from_this();
            if (!Queue([state, waiter, index, action = std::move(action)](Env env) {
                if (waiter->IsDone()) return;
                try { waiter->Complete(action(env, state->Callback(index))); }
                catch (const Error& error) { waiter->Complete(CallbackFailure<T>(error)); }
                catch (const std::exception&) { waiter->Complete(Failure<T>(u"Installer callback failed")); }
            })) waiter->Cancel();
            return waiter->Take();
        } catch (const Error& error) { return CallbackFailure<T>(error); }
          catch (...) { return Failure<T>(u"Installer callback failed"); }
    }
    bool ManagedIdle() {
        if (!handler) return true;
        auto* value = handler_is_idle(handler);
        const bool idle = value && !value->error && value->value;
        FreeResult(value);
        return idle;
    }
    // This path runs from raw TSFN/libuv callbacks. Use status-returning N-API
    // throughout: no exception recovery here may try to enter JS again.
    bool Settle(const std::shared_ptr<Operation>& op) noexcept {
        if (op->settled) return true;
        op->settled = true;
        napi_handle_scope scope = nullptr;
        napi_status status = napi_open_handle_scope(env, &scope);
        napi_value value = nullptr, message = nullptr;
        bool rejected = op->error.has_value();
        if (status == napi_ok && rejected) {
            status = napi_create_string_utf16(env, op->error->data(), op->error->size(), &message);
            if (status == napi_ok) status = napi_create_error(env, nullptr, message, &value);
        } else if (status == napi_ok && op->json) {
            napi_value global = nullptr, json = nullptr, parse = nullptr;
            status = napi_create_string_utf16(env, op->json->data(), op->json->size(), &message);
            if (status == napi_ok) status = napi_get_global(env, &global);
            if (status == napi_ok) status = napi_get_named_property(env, global, "JSON", &json);
            if (status == napi_ok) status = napi_get_named_property(env, json, "parse", &parse);
            if (status == napi_ok) status = napi_call_function(env, json, parse, 1, &message, &value);
        } else if (status == napi_ok) status = napi_get_null(env, &value);
        if (status != napi_ok) {
            // A genuine live conversion error may reject the install. During
            // termination older N-API versions report pending_exception with
            // no actual exception; only native close/join is legal then.
            bool pending = false;
            if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
                status = napi_get_and_clear_last_exception(env, &value);
                rejected = true;
            }
        }
        if (status == napi_ok) status = op->completion->Conclude(value, rejected);
        if (scope) napi_close_handle_scope(env, scope);
        if (status != napi_ok) { Close(true); return false; }
        uint32_t count;
        napi_reference_unref(env, self, &count);
        return true;
    }
    void Check() {
        if (finished) return;
        // Managed decrements its counter after the complete result callback
        // returns. Callback frames and queued task cleanup are distinct lifetimes.
        if (!ManagedIdle() || frames != 0) return;
        if (closing) {
            if (!tsfnFinalized) return;
            if (!envClosing) for (const auto& op : operations) if (!Settle(op)) return;
            if (!envClosing && dispose) {
                napi_handle_scope scope = nullptr;
                napi_value value = nullptr;
                napi_status status = napi_open_handle_scope(env, &scope);
                if (status == napi_ok) status = napi_get_undefined(env, &value);
                if (status == napi_ok) status = dispose->Conclude(value, false);
                if (scope) napi_close_handle_scope(env, scope);
                if (status != napi_ok) { Close(true); return; }
            }
            finished = true;
            for (const auto& op : operations) op->completion->Reset();
            operations.clear();
            if (handler) FreeResult(dispose_handler(std::exchange(handler, nullptr)));
            for (auto& ref : callbacks) { if (ref) napi_delete_reference(env, std::exchange(ref, nullptr)); }
            if (self) napi_delete_reference(env, std::exchange(self, nullptr));
            if (dispose) dispose->Reset();
            dispose.reset();
            uv_timer_stop(&timer);
            uv_close(reinterpret_cast<uv_handle_t*>(&timer), [](uv_handle_t* handle) noexcept {
                auto* state = static_cast<State*>(handle->data);
                auto keepAlive = *state->hookRoot;
                delete std::exchange(state->hookRoot, nullptr);
                if (state->hook) napi_remove_async_cleanup_hook(std::exchange(state->hook, nullptr));
            });
        } else {
            bool ready;
            { std::lock_guard<std::mutex> lock(gate); ready = !operations.empty() && operations.front()->ready; }
            if (!ready) return;
            for (const auto& op : operations) if (!Settle(op)) return;
            if (closing) return; // JS conversion/settlement may re-enter dispose.
            operations.clear();
            napi_unref_threadsafe_function(env, tsfn);
            uv_timer_stop(&timer);
        }
    }
    void Poll() {
        uv_timer_start(&timer, [](uv_timer_t* timer) noexcept {
            auto* state = static_cast<State*>(timer->data);
            try { state->Check(); } catch (...) { state->Close(true); }
        }, 0, 1);
    }
    void Close(bool environment) noexcept {
        envClosing = envClosing || environment;
        if (finished) return;
        {
            std::lock_guard<std::mutex> lock(gate);
            if (closing.exchange(true)) return;
            for (auto& weak : pending) if (auto waiter = weak.lock()) waiter->Cancel();
            pending.clear();
            if (tsfn) napi_release_threadsafe_function(std::exchange(tsfn, nullptr), napi_tsfn_abort);
            else tsfnFinalized = true;
        }
        // Cancellation can run managed continuations that re-enter the bridge.
        // All pending waits were woken, and no admission lock is held here.
        if (handler) FreeResult(cancel_handler(handler));
        Poll();
    }
    static void Result(param_ptr* owner, return_value_json* value) noexcept {
        std::unique_ptr<std::pair<std::shared_ptr<State>, std::shared_ptr<Operation>>> context(
            static_cast<std::pair<std::shared_ptr<State>, std::shared_ptr<Operation>>*>(owner));
        const auto state = context->first;
        Frame frame(state);
        auto op = context->second;
        try {
            {
                std::lock_guard<std::mutex> lock(state->gate);
                op->error = value ? Own(value->error) : std::optional<std::u16string>(u"Installer returned no result");
                op->json = value ? Own(value->value) : std::nullopt;
                op->ready = true;
            }
            FreeResult(value); value = nullptr;
            state->Queue([state](Env) { state->Check(); if (!state->operations.empty()) state->Poll(); });
        } catch (...) { FreeResult(value); }
    }
};

inline State* Get(param_ptr* owner) { return static_cast<State*>(owner); }
inline return_value_json* Plugins(param_ptr* owner, param_bool active) noexcept {
    return Get(owner)->Invoke<return_value_json>(0, [active](Env env, Function call) {
        return ConvertToJsonResult(call({Boolean::New(env, active != 0)})); });
}
inline return_value_string* AppVersion(param_ptr* owner) noexcept {
    return Get(owner)->Invoke<return_value_string>(1, [](Env, Function call) { return ConvertToStringResult(call({})); });
}
inline return_value_string* GameVersion(param_ptr* owner) noexcept {
    return Get(owner)->Invoke<return_value_string>(2, [](Env, Function call) { return ConvertToStringResult(call({})); });
}
inline return_value_string* Extender(param_ptr* owner, param_string* name) noexcept {
    return Get(owner)->Invoke<return_value_string>(3, [name = Own(name)](Env env, Function call) { return ConvertToStringResult(call({ToJS(env, name)})); });
}
inline return_value_void* Start(param_ptr* owner, param_string* name, param_json* image,
    param_ptr* dialog, void (*select)(param_ptr*, param_int, param_int, param_json*, return_value_void*),
    void (*cont)(param_ptr*, param_bool, param_int, return_value_void*), void (*cancel)(param_ptr*, return_value_void*)) noexcept {
    return Get(owner)->Invoke<return_value_void>(4, [name = Own(name), image = Own(image), dialog, select, cont, cancel](Env env, Function call) {
        const auto selectJS = Function::New(env, [dialog, select](const CallbackInfo& info) {
            const auto options = CopyWithFree(JSONStringify(info[2].As<Object>()).Utf16Value());
            select(dialog, info[0].As<Number>().Int32Value(), info[1].As<Number>().Int32Value(), options.get(), Create(return_value_void{nullptr}));
        });
        const auto continueJS = Function::New(env, [dialog, cont](const CallbackInfo& info) {
            cont(dialog, info[0].As<Boolean>().Value(), info[1].As<Number>().Int32Value(), Create(return_value_void{nullptr}));
        });
        const auto cancelJS = Function::New(env, [dialog, cancel](const CallbackInfo&) { cancel(dialog, Create(return_value_void{nullptr})); });
        call({ToJS(env, name), image ? JSONParse(String::New(env, *image)) : Value(env.Null()), selectJS, continueJS, cancelJS});
        return Create(return_value_void{nullptr});
    });
}
inline return_value_void* End(param_ptr* owner) noexcept {
    return Get(owner)->Invoke<return_value_void>(5, [](Env, Function call) { call({}); return Create(return_value_void{nullptr}); });
}
inline return_value_void* Update(param_ptr* owner, param_json* steps, param_int current) noexcept {
    return Get(owner)->Invoke<return_value_void>(6, [steps = Own(steps), current](Env env, Function call) {
        call({steps ? JSONParse(String::New(env, *steps)) : Value(env.Null()), Number::New(env, current)});
        return Create(return_value_void{nullptr});
    });
}

class ModInstaller : public ObjectWrap<ModInstaller> {
    std::shared_ptr<State> state;
public:
    static Object Init(Napi::Env env, Object exports) {
        exports.Set("ModInstaller", DefineClass(env, "ModInstaller", {
            InstanceMethod<&ModInstaller::Install>("install"), InstanceMethod<&ModInstaller::Dispose>("dispose"),
            StaticMethod<&ModInstaller::TestSupported>("testSupported") }));
        return exports;
    }
    explicit ModInstaller(const CallbackInfo& info) : ObjectWrap(info), state(std::make_shared<State>(info.Env())) {
        const auto env = info.Env();
        for (unsigned i = 0; i < 7; ++i)
            if (!info[i].IsFunction()) throw TypeError::New(env, "Installer callbacks must be functions");
        CheckStatus(env, napi_get_uv_event_loop(env, &state->loop));
        if (uv_timer_init(state->loop, &state->timer) != 0) throw Error::New(env, "Failed to initialize installer cleanup timer");
        state->timer.data = state.get();
        state->hookRoot = new std::shared_ptr<State>(state);
        try {
        // Protect partial initialization too; replace this hook with a later one
        // only after the TSFN exists, establishing final dependency ordering.
        CheckStatus(env, napi_add_async_cleanup_hook(env, [](napi_async_cleanup_hook_handle, void* data) {
            (*static_cast<std::shared_ptr<State>*>(data))->Close(true);
        }, state->hookRoot, &state->hook));
        const auto refs = Array::New(env, 7);
        for (unsigned i = 0; i < 7; ++i) {
            refs.Set(i, info[i]);
            CheckStatus(env, napi_create_reference(env, info[i], 0, &state->callbacks[i]));
        }
        // Keep callbacks on the INNER wrapped object, so JS can collect idle
        // cycles and an accepted install can pin this whole table with one ref.
        info.This().As<Object>().Set(Symbol::New(env), refs);
        CheckStatus(env, napi_create_reference(env, info.This(), 0, &state->self));
        CheckStatus(env, napi_create_threadsafe_function(env, nullptr, nullptr, String::New(env, "FomodInstaller"), 0, 1,
            state.get(), [](napi_env, void* data, void*) { static_cast<State*>(data)->tsfnFinalized = true; },
            nullptr, State::CallJS, &state->tsfn));
        napi_async_cleanup_hook_handle later = nullptr;
        CheckStatus(env, napi_add_async_cleanup_hook(env, [](napi_async_cleanup_hook_handle, void* data) {
            (*static_cast<std::shared_ptr<State>*>(data))->Close(true);
        }, state->hookRoot, &later));
        napi_remove_async_cleanup_hook(state->hook);
        state->hook = later;
        state->handler = ThrowOrReturnPtr(env, create_handler(state.get(), Plugins, AppVersion, GameVersion, Extender, Start, End, Update));
        napi_unref_threadsafe_function(env, state->tsfn);
        } catch (...) { state->Close(false); throw; }
    }
    ~ModInstaller() { state->Close(false); }
    Napi::Value Dispose(const CallbackInfo& info) {
        if (state->finished) {
            const auto done = Completion::New(info.Env());
            const auto promise = done->PromiseValue();
            CheckStatus(info.Env(), done->Conclude(info.Env().Undefined(), false));
            return promise;
        }
        if (!state->dispose) {
            if (state->creatingDispose) throw Error::New(info.Env(), "Installer dispose is already initializing");
            struct Creating { bool& flag; ~Creating() { flag = false; } } creating{state->creatingDispose};
            state->creatingDispose = true;
            state->dispose = Completion::New(info.Env());
        }
        const auto promise = state->dispose->PromiseValue();
        state->Close(false);
        return promise;
    }
    Napi::Value Install(const CallbackInfo& info) {
        const auto env = info.Env();
        if (state->closing || state->creatingOperation || !state->operations.empty()) throw Error::New(env, "Installer is closed or already active");
        struct Creating { bool& flag; ~Creating() { flag = false; } } creating{state->creatingOperation};
        state->creatingOperation = true;
        const auto files = CopyWithFree(JSONStringify(info[0].As<Object>()).Utf16Value());
        const auto patterns = CopyWithFree(JSONStringify(info[1].As<Object>()).Utf16Value());
        const auto plugin = info[2].IsNull() ? NullStringCopy() : CopyWithFree(info[2].As<String>().Utf16Value());
        const auto script = CopyWithFree(info[3].As<String>().Utf16Value());
        const auto preset = info[4].IsNull() || info[4].IsUndefined() ? NullStringCopy() : CopyWithFree(JSONStringify(info[4].As<Object>()));
        if (!info[5].IsBoolean() || !info[6].IsBoolean()) throw TypeError::New(env, "Installer flags must be Boolean");
        const bool preselect = info[5].As<Boolean>().Value();
        const bool validate = info[6].As<Boolean>().Value();
        // JSON serialization can invoke user getters/toJSON and re-enter dispose.
        if (state->closing || !state->operations.empty()) throw Error::New(env, "Installer is closed or already active");
        auto operation = std::make_shared<Operation>(env);
        // Promise construction is also JS and may re-enter this installer.
        if (state->closing || !state->operations.empty()) throw Error::New(env, "Installer is closed or already active");
        const auto promise = operation->completion->PromiseValue();
        using Context = std::pair<std::shared_ptr<State>, std::shared_ptr<Operation>>;
        auto context = std::make_unique<Context>(state, operation);
        struct Admission {
            std::shared_ptr<State> state;
            std::shared_ptr<Operation> op;
            bool registered = false, pinned = false, referenced = false, accepted = false;
            ~Admission() {
                if (accepted) return;
                if (registered) state->operations.erase(std::remove(state->operations.begin(), state->operations.end(), op), state->operations.end());
                uint32_t count;
                if (pinned) napi_reference_unref(state->env, state->self, &count);
                if (referenced && state->tsfn) napi_unref_threadsafe_function(state->env, state->tsfn);
            }
        } admission{state, operation};
        state->operations.emplace_back(operation);
        admission.registered = true;
        uint32_t count;
        CheckStatus(env, napi_reference_ref(env, state->self, &count)); // before managed synchronous callbacks
        admission.pinned = true;
        CheckStatus(env, napi_ref_threadsafe_function(env, state->tsfn));
        admission.referenced = true;
        auto* callbackContext = context.release();
        auto* value = install(state->handler, files.get(), patterns.get(), plugin.get(), script.get(), preset.get(),
            preselect, validate, callbackContext, State::Result);
        admission.accepted = value && !value->error;
        if (!admission.accepted) {
            // Managed immediate rejection schedules no result callback. Retake
            // its context; the admission guard rolls back every acquired pin.
            context.reset(callbackContext);
            const auto error = value ? Own(value->error) : std::optional<std::u16string>(u"Installer returned no status");
            FreeResult(value);
            CheckStatus(env, operation->completion->Conclude(Error::New(env, String::New(env, *error)).Value(), true));
        } else {
            FreeResult(value);
        }
        return promise;
    }
    static Napi::Value TestSupported(const CallbackInfo& info) {
        const auto files = CopyWithFree(JSONStringify(info[0].As<Object>()).Utf16Value());
        const auto allowed = CopyWithFree(JSONStringify(info[1].As<Object>()).Utf16Value());
        return ThrowOrReturnJson(info.Env(), test_supported(files.get(), allowed.get()));
    }
};
inline Object Init(Env env, Object exports) { return ModInstaller::Init(env, exports); }
}
#endif
