// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/process_singleton.h"

#include <windows.h>
#include <shellapi.h>
#include <stddef.h>

#include "base/base_paths.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/macros.h"
#include "base/process/process.h"
#include "base/process/process_info.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/win/registry.h"
#include "base/win/scoped_handle.h"
#include "base/win/windows_version.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/browser_process_platform_part.h"
#include "chrome/browser/shell_integration.h"
#include "chrome/browser/ui/simple_message_box.h"
#include "chrome/browser/win/chrome_process_finder.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/chrome_paths_internal.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/grit/chromium_strings.h"
#include "chrome/installer/util/wmi.h"
#include "content/public/common/result_codes.h"
#include "net/base/escape.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/gfx/win/hwnd_util.h"

namespace {

const char kLockfile[] = "lockfile";

// A helper class that acquires the given |mutex| while the AutoLockMutex is in
// scope.
class AutoLockMutex {
 public:
  explicit AutoLockMutex(HANDLE mutex) : mutex_(mutex) {
    DWORD result = ::WaitForSingleObject(mutex_, INFINITE);
    DPCHECK(result == WAIT_OBJECT_0) << "Result = " << result;
  }

  ~AutoLockMutex() {
    BOOL released = ::ReleaseMutex(mutex_);
    DPCHECK(released);
  }

 private:
  HANDLE mutex_;
  DISALLOW_COPY_AND_ASSIGN(AutoLockMutex);
};

// A helper class that releases the given |mutex| while the AutoUnlockMutex is
// in scope and immediately re-acquires it when going out of scope.
class AutoUnlockMutex {
 public:
  explicit AutoUnlockMutex(HANDLE mutex) : mutex_(mutex) {
    BOOL released = ::ReleaseMutex(mutex_);
    DPCHECK(released);
  }

  ~AutoUnlockMutex() {
    DWORD result = ::WaitForSingleObject(mutex_, INFINITE);
    DPCHECK(result == WAIT_OBJECT_0) << "Result = " << result;
  }

 private:
  HANDLE mutex_;
  DISALLOW_COPY_AND_ASSIGN(AutoUnlockMutex);
};

// Checks the visibility of the enumerated window and signals once a visible
// window has been found.
BOOL CALLBACK BrowserWindowEnumeration(HWND window, LPARAM param) {
  bool* result = reinterpret_cast<bool*>(param);
  *result = ::IsWindowVisible(window) != 0;
  // Stops enumeration if a visible window has been found.
  return !*result;
}

bool ParseCommandLine(const COPYDATASTRUCT* cds,
                      base::CommandLine* parsed_command_line,
                      base::FilePath* current_directory) {
  // We should have enough room for the shortest command (min_message_size)
  // and also be a multiple of wchar_t bytes. The shortest command
  // possible is L"START\0\0" (empty current directory and command line).
  static const int min_message_size = 7;
  if (cds->cbData < min_message_size * sizeof(wchar_t) ||
      cds->cbData % sizeof(wchar_t) != 0) {
    LOG(WARNING) << "Invalid WM_COPYDATA, length = " << cds->cbData;
    return false;
  }

  // We split the string into 4 parts on NULLs.
  DCHECK(cds->lpData);
  const std::wstring msg(static_cast<wchar_t*>(cds->lpData),
                         cds->cbData / sizeof(wchar_t));
  const std::wstring::size_type first_null = msg.find_first_of(L'\0');
  if (first_null == 0 || first_null == std::wstring::npos) {
    // no NULL byte, don't know what to do
    LOG(WARNING) << "Invalid WM_COPYDATA, length = " << msg.length() <<
      ", first null = " << first_null;
    return false;
  }

  // Decode the command, which is everything until the first NULL.
  if (msg.substr(0, first_null) == L"START") {
    // Another instance is starting parse the command line & do what it would
    // have done.
    VLOG(1) << "Handling STARTUP request from another process";
    const std::wstring::size_type second_null =
        msg.find_first_of(L'\0', first_null + 1);
    if (second_null == std::wstring::npos ||
        first_null == msg.length() - 1 || second_null == msg.length()) {
      LOG(WARNING) << "Invalid format for start command, we need a string in 4 "
        "parts separated by NULLs";
      return false;
    }

    // Get current directory.
    *current_directory = base::FilePath(msg.substr(first_null + 1,
                                                   second_null - first_null));

    const std::wstring::size_type third_null =
        msg.find_first_of(L'\0', second_null + 1);
    if (third_null == std::wstring::npos ||
        third_null == msg.length()) {
      LOG(WARNING) << "Invalid format for start command, we need a string in 4 "
        "parts separated by NULLs";
    }

    // Get command line.
    const std::wstring cmd_line =
        msg.substr(second_null + 1, third_null - second_null);
    *parsed_command_line = base::CommandLine::FromString(cmd_line);
    return true;
  }
  return false;
}

bool ProcessLaunchNotification(
    const ProcessSingleton::NotificationCallback& notification_callback,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    LRESULT* result) {
  if (message != WM_COPYDATA)
    return false;

  // Handle the WM_COPYDATA message from another process.
  const COPYDATASTRUCT* cds = reinterpret_cast<COPYDATASTRUCT*>(lparam);

  base::CommandLine parsed_command_line(base::CommandLine::NO_PROGRAM);
  base::FilePath current_directory;
  if (!ParseCommandLine(cds, &parsed_command_line, &current_directory)) {
    *result = TRUE;
    return true;
  }

  *result = notification_callback.Run(parsed_command_line, current_directory) ?
      TRUE : FALSE;
  return true;
}

bool DisplayShouldKillMessageBox() {
  return chrome::ShowQuestionMessageBox(
             NULL, l10n_util::GetStringUTF16(IDS_PRODUCT_NAME),
             l10n_util::GetStringUTF16(IDS_BROWSER_HUNGBROWSER_MESSAGE)) !=
         chrome::MESSAGE_BOX_RESULT_NO;
}

}  // namespace

// Microsoft's Softricity virtualization breaks the sandbox processes.
// So, if we detect the Softricity DLL we use WMI Win32_Process.Create to
// break out of the virtualization environment.
// http://code.google.com/p/chromium/issues/detail?id=43650
bool ProcessSingleton::EscapeVirtualization(
    const base::FilePath& user_data_dir) {
  if (::GetModuleHandle(L"sftldr_wow64.dll") ||
      ::GetModuleHandle(L"sftldr.dll")) {
    int process_id;
    if (!installer::WMIProcess::Launch(::GetCommandLineW(), &process_id))
      return false;
    is_virtualized_ = true;
    // The new window was spawned from WMI, and won't be in the foreground.
    // So, first we sleep while the new chrome.exe instance starts (because
    // WaitForInputIdle doesn't work here). Then we poll for up to two more
    // seconds and make the window foreground if we find it (or we give up).
    HWND hwnd = 0;
    ::Sleep(90);
    for (int tries = 200; tries; --tries) {
      hwnd = chrome::FindRunningChromeWindow(user_data_dir);
      if (hwnd) {
        ::SetForegroundWindow(hwnd);
        break;
      }
      ::Sleep(10);
    }
    return true;
  }
  return false;
}

ProcessSingleton::ProcessSingleton(
    const base::FilePath& user_data_dir,
    const NotificationCallback& notification_callback)
    : notification_callback_(notification_callback),
      is_virtualized_(false),
      lock_file_(INVALID_HANDLE_VALUE),
      user_data_dir_(user_data_dir),
      should_kill_remote_process_callback_(
          base::Bind(&DisplayShouldKillMessageBox)) {
}

ProcessSingleton::~ProcessSingleton() {
  if (lock_file_ != INVALID_HANDLE_VALUE)
    ::CloseHandle(lock_file_);
}

// Code roughly based on Mozilla.
ProcessSingleton::NotifyResult ProcessSingleton::NotifyOtherProcess() {
  if (is_virtualized_)
    return PROCESS_NOTIFIED;  // We already spawned the process in this case.
  if (lock_file_ == INVALID_HANDLE_VALUE && !remote_window_) {
    return LOCK_ERROR;
  } else if (!remote_window_) {
    return PROCESS_NONE;
  }

  switch (chrome::AttemptToNotifyRunningChrome(remote_window_, false)) {
    case chrome::NOTIFY_SUCCESS:
      return PROCESS_NOTIFIED;
    case chrome::NOTIFY_FAILED:
      remote_window_ = NULL;
      return PROCESS_NONE;
    case chrome::NOTIFY_WINDOW_HUNG:
      // Fall through and potentially terminate the hung browser.
      break;
  }

  // The window is hung.
  DWORD process_id = 0;
  DWORD thread_id = ::GetWindowThreadProcessId(remote_window_, &process_id);
  if (!thread_id || !process_id) {
    remote_window_ = NULL;
    return PROCESS_NONE;
  }

  // Get a handle to the process that created the window.
  base::Process process = base::Process::Open(process_id);

  // TODO(manzagop): capture a hang report.

  // Scan for every window to find a visible one.
  bool visible_window = false;
  ::EnumThreadWindows(thread_id,
                      &BrowserWindowEnumeration,
                      reinterpret_cast<LPARAM>(&visible_window));

  // If there is a visible browser window, ask the user before killing it.
  if (visible_window && !should_kill_remote_process_callback_.Run()) {
    // The user denied. Quit silently.
    return PROCESS_NOTIFIED;
  }

  // Time to take action. Kill the browser process.
  process.Terminate(content::RESULT_CODE_HUNG, true);
  remote_window_ = NULL;
  return PROCESS_NONE;
}

ProcessSingleton::NotifyResult
ProcessSingleton::NotifyOtherProcessOrCreate() {
  ProcessSingleton::NotifyResult result = PROCESS_NONE;
  if (!Create()) {
    result = NotifyOtherProcess();
    if (result == PROCESS_NONE)
      result = PROFILE_IN_USE;
  } else {
    g_browser_process->platform_part()->PlatformSpecificCommandLineProcessing(
        *base::CommandLine::ForCurrentProcess());
  }
  return result;
}

// Look for a Chrome instance that uses the same profile directory. If there
// isn't one, create a message window with its title set to the profile
// directory path.
bool ProcessSingleton::Create() {
  static const wchar_t kMutexName[] = L"Local\\ChromeProcessSingletonStartup!";

  remote_window_ = chrome::FindRunningChromeWindow(user_data_dir_);
  if (!remote_window_ && !EscapeVirtualization(user_data_dir_)) {
    // Make sure we will be the one and only process creating the window.
    // We use a named Mutex since we are protecting against multi-process
    // access. As documented, it's clearer to NOT request ownership on creation
    // since it isn't guaranteed we will get it. It is better to create it
    // without ownership and explicitly get the ownership afterward.
    base::win::ScopedHandle only_me(::CreateMutex(NULL, FALSE, kMutexName));
    if (!only_me.IsValid()) {
      DPLOG(FATAL) << "CreateMutex failed";
      return false;
    }

    AutoLockMutex auto_lock_only_me(only_me.Get());

    // We now own the mutex so we are the only process that can create the
    // window at this time, but we must still check if someone created it
    // between the time where we looked for it above and the time the mutex
    // was given to us.
    remote_window_ = chrome::FindRunningChromeWindow(user_data_dir_);

    if (!remote_window_) {
      // We have to make sure there is no Chrome instance running on another
      // machine that uses the same profile.
      base::FilePath lock_file_path = user_data_dir_.AppendASCII(kLockfile);
      lock_file_ = ::CreateFile(lock_file_path.value().c_str(),
                                GENERIC_WRITE,
                                FILE_SHARE_READ,
                                NULL,
                                CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL |
                                FILE_FLAG_DELETE_ON_CLOSE,
                                NULL);
      DWORD error = ::GetLastError();
      LOG_IF(WARNING, lock_file_ != INVALID_HANDLE_VALUE &&
          error == ERROR_ALREADY_EXISTS) << "Lock file exists but is writable.";
      LOG_IF(ERROR, lock_file_ == INVALID_HANDLE_VALUE)
          << "Lock file can not be created! Error code: " << error;

      if (lock_file_ != INVALID_HANDLE_VALUE) {
        // Set the window's title to the path of our user data directory so
        // other Chrome instances can decide if they should forward to us.
        bool result = window_.CreateNamed(
            base::Bind(&ProcessLaunchNotification, notification_callback_),
            user_data_dir_.value());
        CHECK(result && window_.hwnd());
	::ChangeWindowMessageFilter(WM_COPYDATA, MSGFLT_ADD);
      }
    }
  }

  return window_.hwnd() != NULL;
}

void ProcessSingleton::Cleanup() {
}

void ProcessSingleton::OverrideShouldKillRemoteProcessCallbackForTesting(
    const ShouldKillRemoteProcessCallback& display_dialog_callback) {
  should_kill_remote_process_callback_ = display_dialog_callback;
}
