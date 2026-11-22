#pragma once
#include <windows.h>

#include <cstdint>
#include <functional>

#include "xtask.h"

namespace gtx::xgr::ui {

inline constexpr uint32_t kLastMessageDialogButton = 2;

enum class Request : uint32_t {
  PlayerProfileCard,
  PlayerPicker,
  SendGameInvite,
  Achievements,
  MultiplayerActivityGameInvite,
  MessageDialog,
  ErrorDialog,
  TextEntry,
};

struct PlayerPickerInfo {
  void* requestingUser;
  const char* promptText;
  uint32_t selectFromPlayersCount;
  const uint64_t* selectFromPlayers;
  uint32_t preSelectedPlayersCount;
  const uint64_t* preSelectedPlayers;
  uint32_t minSelectionCount;
  uint32_t maxSelectionCount;
};

using PlayerProfileCardCallback = void(CALLBACK*)(void* callbackHandle, task::Queue* queue, void* requestingUser, uint64_t targetPlayer,
                                                  void* context);
using PlayerPickerCallback = void(CALLBACK*)(void* callbackHandle, task::Queue* queue, PlayerPickerInfo* info, void* context);
using SendGameInviteCallback = void(CALLBACK*)(void* callbackHandle, task::Queue* queue, void* requestingUser, const char* sessionConfigurationId,
                                               const char* sessionTemplateName, const char* sessionId, const char* invitationText,
                                               const char* customActivationContext, void* context);
using AchievementsCallback = void(CALLBACK*)(void* callbackHandle, task::Queue* queue, void* requestingUser, uint32_t titleId, void* context);
using MultiplayerActivityGameInviteCallback = void(CALLBACK*)(void* callbackHandle, task::Queue* queue, void* requestingUser, void* context);
using MessageDialogCallback = void(CALLBACK*)(void* callbackHandle, task::Queue* queue, const char* titleText, const char* contentText,
                                              const char* firstButtonText, const char* secondButtonText, const char* thirdButtonText,
                                              uint32_t defaultButton, uint32_t cancelButton, void* context);
using ErrorDialogCallback = void(CALLBACK*)(void* callbackHandle, task::Queue* queue, HRESULT errorCode, const char* serviceContext, void* context);
using TextEntryCallback = void(CALLBACK*)(void* callbackHandle, task::Queue* queue, const char* titleText, const char* descriptionText,
                                          const char* defaultText, uint32_t inputScope, uint32_t maxTextLength, void* context);

struct Callbacks {
  void* context;
  PlayerProfileCardCallback showPlayerProfileCard;
  PlayerPickerCallback showPlayerPicker;
  SendGameInviteCallback showSendGameInvite;
  AchievementsCallback showAchievements;
  MultiplayerActivityGameInviteCallback showMultiplayerActivityGameInvite;
  MessageDialogCallback showMessageDialog;
  ErrorDialogCallback showErrorDialog;
  TextEntryCallback showTextEntry;
};

using Show = std::function<void(void* callbackHandle, task::Queue* queue)>;

Callbacks title_callbacks();
HRESULT begin_title_ui(task::AsyncBlock* async, const void* identity, const char* name, Request request, Show show);

}  // namespace gtx::xgr::ui
