#define NODE_API_NO_EXTERNAL_BUFFERS_ALLOWED // Thanks Electron
#ifdef __linux__
// Worker termination may forbid JS before environment cleanup hooks run.
// Let node-addon-api's synchronous wrappers return in that specific case.
#define NODE_API_SWALLOW_UNTHROWABLE_EXCEPTIONS
#endif

#include "Platform.hpp"
#include <napi.h>
#include "Bindings.Common.hpp"
#include "Bindings.Logging.Implementation.hpp"
#ifdef __linux__
#include "Bindings.ModInstaller.Lifecycle.hpp"
#else
#include "Bindings.ModInstaller.Implementation.hpp"
#endif
#include "Bindings.FileSystem.Implementation.hpp"

using namespace Napi;

Object InitAll(const Env env, const Object exports)
{
  Bindings::Common::Init(env, exports);
  Bindings::Logging::Init(env, exports);
  Bindings::ModInstaller::Init(env, exports);
  Bindings::FileSystem::Init(env, exports);
  return exports;
}

NODE_API_MODULE(NODE_GYP_MODULE_NAME, InitAll)
