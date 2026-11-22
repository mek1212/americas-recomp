#define GTX_SHIM_DLL "xgameruntime.dll"
#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ui.h"
#include "xgr_internal.h"
#include "xtask.h"

namespace gtx::xgr::ui {
namespace {

constexpr uint32_t kWebAuthenticationOptionsMask = 1;

struct Invocation {
  Request request;
  task::AsyncBlock* async;
  Show show;
  std::vector<uint8_t> payload;
  bool answered = false;
};

std::mutex g_lock;
Callbacks g_callbacks{};
std::unordered_map<uint64_t, std::shared_ptr<Invocation>> g_invocations;
uint64_t g_next_id = 1;

uint64_t id_of(const void* value) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value)); }
void* handle_of(uint64_t id) { return reinterpret_cast<void*>(static_cast<uintptr_t>(id)); }

std::shared_ptr<Invocation> find(uint64_t id) {
  std::lock_guard<std::mutex> lock(g_lock);
  auto found = g_invocations.find(id);
  return found == g_invocations.end() ? nullptr : found->second;
}

void show_on_work_port(uint64_t id, task::AsyncBlock* async) {
  std::shared_ptr<Invocation> invocation = find(id);
  if (!invocation) return;
  task::Queue* owned = nullptr;
  task::Queue* queue = async->queue;
  if (!queue && task::GetProcessQueue(&owned)) queue = owned;
  invocation->show(handle_of(id), queue);
  if (owned) task::Close(owned);
}

void cancel(uint64_t id, task::AsyncBlock* async) {
  {
    std::lock_guard<std::mutex> lock(g_lock);
    auto found = g_invocations.find(id);
    if (found == g_invocations.end() || found->second->answered) return;
    found->second->answered = true;
  }
  task::AsyncComplete(async, E_ABORT, 0);
}

HRESULT CALLBACK title_ui_provider(task::AsyncOp op, const task::AsyncProviderData* d) {
  uint64_t id = id_of(d->context);
  switch (op) {
    case task::AsyncOp::Begin:
      return task::AsyncSchedule(d->async, 0);
    case task::AsyncOp::DoWork:
      show_on_work_port(id, d->async);
      return E_PENDING;
    case task::AsyncOp::GetResult: {
      std::shared_ptr<Invocation> invocation = find(id);
      if (invocation && !invocation->payload.empty()) std::memcpy(d->buffer, invocation->payload.data(), invocation->payload.size());
      return S_OK;
    }
    case task::AsyncOp::Cancel:
      cancel(id, d->async);
      return S_OK;
    case task::AsyncOp::Cleanup: {
      std::lock_guard<std::mutex> lock(g_lock);
      g_invocations.erase(id);
      return S_OK;
    }
    default:
      return S_OK;
  }
}

HRESULT respond(void* callbackHandle, Request request, std::vector<uint8_t> payload) {
  task::AsyncBlock* async;
  size_t size = payload.size();
  {
    std::lock_guard<std::mutex> lock(g_lock);
    auto found = g_invocations.find(id_of(callbackHandle));
    if (found == g_invocations.end() || found->second->request != request || found->second->answered) return E_INVALIDARG;
    found->second->answered = true;
    found->second->payload = std::move(payload);
    async = found->second->async;
  }
  task::AsyncComplete(async, S_OK, size);
  return S_OK;
}

HRESULT XGameUiSetUiCallbacks_impl(void*, const Callbacks* callbacks, uint8_t) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_callbacks = callbacks ? *callbacks : Callbacks{};
  return S_OK;
}

HRESULT XGameUiSetMessageDialogUiResponse_impl(void*, void* callbackHandle, uint32_t response) {
  if (response > kLastMessageDialogButton) return E_INVALIDARG;
  std::vector<uint8_t> payload(sizeof(response));
  std::memcpy(payload.data(), &response, sizeof(response));
  return respond(callbackHandle, Request::MessageDialog, std::move(payload));
}

HRESULT XGameUiSetPlayerPickerUiResponse_impl(void*, void* callbackHandle, uint32_t playerCount, const uint64_t* players) {
  if (playerCount && !players) return E_INVALIDARG;
  std::vector<uint8_t> payload(sizeof(uint64_t) * playerCount);
  if (playerCount) std::memcpy(payload.data(), players, payload.size());
  return respond(callbackHandle, Request::PlayerPicker, std::move(payload));
}

HRESULT XGameUiSetTextEntryUiResponse_impl(void*, void* callbackHandle, const char* response) {
  if (!response) return E_INVALIDARG;
  std::vector<uint8_t> payload(response, response + std::strlen(response) + 1);
  return respond(callbackHandle, Request::TextEntry, std::move(payload));
}

HRESULT XGameUiSetPlayerProfileCardUiResponse_impl(void*, void* callbackHandle) {
  return respond(callbackHandle, Request::PlayerProfileCard, {});
}
HRESULT XGameUiSetSendGameInviteUiResponse_impl(void*, void* callbackHandle) {
  return respond(callbackHandle, Request::SendGameInvite, {});
}
HRESULT XGameUiSetAchievementsUiResponse_impl(void*, void* callbackHandle) {
  return respond(callbackHandle, Request::Achievements, {});
}
HRESULT XGameUiSetMultiplayerActivityGameInviteUiResponse_impl(void*, void* callbackHandle) {
  return respond(callbackHandle, Request::MultiplayerActivityGameInvite, {});
}
HRESULT XGameUiSetErrorDialogUiResponse_impl(void*, void* callbackHandle) {
  return respond(callbackHandle, Request::ErrorDialog, {});
}

struct WebAuthenticationResultData {
  HRESULT responseStatus;
  size_t responseCompletionUriSize;
  const char* responseCompletionUri;
};
static_assert(sizeof(WebAuthenticationResultData) == 24, "XGameUiWebAuthenticationResultData layout");

HRESULT CALLBACK no_web_view_provider(task::AsyncOp op, const task::AsyncProviderData*) {
  return op == task::AsyncOp::DoWork ? E_ABORT : S_OK;
}

bool is_https(const char* uri) {
  static const char kScheme[] = "https://";
  for (size_t i = 0; i + 1 < sizeof(kScheme); ++i)
    if (tolower(static_cast<unsigned char>(uri[i])) != kScheme[i]) return false;
  return true;
}

HRESULT XGameUiShowWebAuthenticationAsync_impl(void*, task::AsyncBlock* async, void* requestingUser, const char* requestUri,
                                               const char* completionUri);

HRESULT begin_web_authentication(task::AsyncBlock* async, void* requestingUser, const char* requestUri, const char* completionUri,
                                 uint32_t options, const char* name) {
  if (!async || !requestingUser || !requestUri || !completionUri || (options & ~kWebAuthenticationOptionsMask) || !is_https(requestUri))
    return E_INVALIDARG;
  HRESULT hr = task::AsyncBegin(async, nullptr, reinterpret_cast<const void*>(&XGameUiShowWebAuthenticationAsync_impl), name,
                                no_web_view_provider);
  if (FAILED(hr)) return hr;
  return task::AsyncSchedule(async, 0);
}

HRESULT XGameUiShowWebAuthenticationAsync_impl(void*, task::AsyncBlock* async, void* requestingUser, const char* requestUri,
                                               const char* completionUri) {
  return begin_web_authentication(async, requestingUser, requestUri, completionUri, 0, "XGameUiShowWebAuthenticationAsync");
}

HRESULT XGameUiShowWebAuthenticationWithOptionsAsync_impl(void*, task::AsyncBlock* async, void* requestingUser, const char* requestUri,
                                                          const char* completionUri, uint32_t options) {
  return begin_web_authentication(async, requestingUser, requestUri, completionUri, options, "XGameUiShowWebAuthenticationWithOptionsAsync");
}

HRESULT XGameUiShowWebAuthenticationResultSize_impl(void*, task::AsyncBlock* async, size_t* bufferSize) {
  if (!bufferSize) return E_INVALIDARG;
  return task::AsyncGetResultSize(async, bufferSize);
}

HRESULT XGameUiShowWebAuthenticationResult_impl(void*, task::AsyncBlock* async, size_t bufferSize, void* buffer,
                                                WebAuthenticationResultData** ptrToBuffer, size_t* bufferUsed) {
  if (!ptrToBuffer) return E_INVALIDARG;
  *ptrToBuffer = nullptr;
  HRESULT hr = task::AsyncGetResult(async, reinterpret_cast<const void*>(&XGameUiShowWebAuthenticationAsync_impl), bufferSize, buffer, bufferUsed);
  if (SUCCEEDED(hr)) *ptrToBuffer = static_cast<WebAuthenticationResultData*>(buffer);
  return hr;
}

const Override kOverrides[] = {
    {"XGameUiSetUiCallbacks", reinterpret_cast<void*>(&XGameUiSetUiCallbacks_impl)},
    {"XGameUiSetMessageDialogUiResponse", reinterpret_cast<void*>(&XGameUiSetMessageDialogUiResponse_impl)},
    {"XGameUiSetPlayerPickerUiResponse", reinterpret_cast<void*>(&XGameUiSetPlayerPickerUiResponse_impl)},
    {"XGameUiSetTextEntryUiResponse", reinterpret_cast<void*>(&XGameUiSetTextEntryUiResponse_impl)},
    {"XGameUiSetPlayerProfileCardUiResponse", reinterpret_cast<void*>(&XGameUiSetPlayerProfileCardUiResponse_impl)},
    {"XGameUiSetSendGameInviteUiResponse", reinterpret_cast<void*>(&XGameUiSetSendGameInviteUiResponse_impl)},
    {"XGameUiSetAchievementsUiResponse", reinterpret_cast<void*>(&XGameUiSetAchievementsUiResponse_impl)},
    {"XGameUiSetMultiplayerActivityGameInviteUiResponse", reinterpret_cast<void*>(&XGameUiSetMultiplayerActivityGameInviteUiResponse_impl)},
    {"XGameUiSetErrorDialogUiResponse", reinterpret_cast<void*>(&XGameUiSetErrorDialogUiResponse_impl)},
    {"XGameUiShowWebAuthenticationAsync", reinterpret_cast<void*>(&XGameUiShowWebAuthenticationAsync_impl)},
    {"XGameUiShowWebAuthenticationResultSize", reinterpret_cast<void*>(&XGameUiShowWebAuthenticationResultSize_impl)},
    {"XGameUiShowWebAuthenticationResult", reinterpret_cast<void*>(&XGameUiShowWebAuthenticationResult_impl)},
    {"XGameUiShowWebAuthenticationWithOptionsAsync", reinterpret_cast<void*>(&XGameUiShowWebAuthenticationWithOptionsAsync_impl)},
};

}  // namespace

Callbacks title_callbacks() {
  std::lock_guard<std::mutex> lock(g_lock);
  return g_callbacks;
}

HRESULT begin_title_ui(task::AsyncBlock* async, const void* identity, const char* name, Request request, Show show) {
  if (!async) return E_INVALIDARG;
  auto invocation = std::make_shared<Invocation>();
  invocation->request = request;
  invocation->async = async;
  invocation->show = std::move(show);
  uint64_t id;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    id = g_next_id++;
    g_invocations.emplace(id, invocation);
  }
  HRESULT hr = task::AsyncBegin(async, handle_of(id), identity, name, title_ui_provider);
  if (FAILED(hr)) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_invocations.erase(id);
  }
  return hr;
}

const Override* overrides(size_t* count) {
  *count = sizeof(kOverrides) / sizeof(kOverrides[0]);
  return kOverrides;
}

}  // namespace gtx::xgr::ui
