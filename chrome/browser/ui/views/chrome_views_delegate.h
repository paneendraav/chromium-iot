// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_CHROME_VIEWS_DELEGATE_H_
#define CHROME_BROWSER_UI_VIEWS_CHROME_VIEWS_DELEGATE_H_

#include "base/callback.h"
#include "base/compiler_specific.h"
#include "base/location.h"
#include "base/macros.h"
#include "build/build_config.h"
#include "ui/accessibility/ax_enums.h"
#include "ui/views/views_delegate.h"

class ScopedKeepAlive;

class ChromeViewsDelegate : public views::ViewsDelegate {
 public:
  ChromeViewsDelegate();
  ~ChromeViewsDelegate() override;

  // views::ViewsDelegate:
  void SaveWindowPlacement(const views::Widget* window,
                           const std::string& window_name,
                           const gfx::Rect& bounds,
                           ui::WindowShowState show_state) override;
  bool GetSavedWindowPlacement(const views::Widget* widget,
                               const std::string& window_name,
                               gfx::Rect* bounds,
                               ui::WindowShowState* show_state) const override;
  void NotifyAccessibilityEvent(views::View* view,
                                ui::AXEvent event_type) override;
  ProcessMenuAcceleratorResult ProcessAcceleratorWhileMenuShowing(
      const ui::Accelerator& accelerator) override;

#if defined(OS_WIN)
  HICON GetDefaultWindowIcon() const override;
  HICON GetSmallWindowIcon() const override;
#elif defined(OS_LINUX) && !defined(OS_CHROMEOS)
  gfx::ImageSkia* GetDefaultWindowIcon() const override;
#endif

#if defined(USE_ASH)
  views::NonClientFrameView* CreateDefaultNonClientFrameView(
      views::Widget* widget) override;
#endif
  void AddRef() override;
  void ReleaseRef() override;
  void OnBeforeWidgetInit(
      views::Widget::InitParams* params,
      views::internal::NativeWidgetDelegate* delegate) override;
#if defined(OS_LINUX) && !defined(OS_CHROMEOS)
  bool WindowManagerProvidesTitleBar(bool maximized) override;
#endif
  ui::ContextFactory* GetContextFactory() override;
  std::string GetApplicationName() override;
#if defined(OS_WIN)
  int GetAppbarAutohideEdges(HMONITOR monitor,
                             const base::Closure& callback) override;
#endif
  scoped_refptr<base::TaskRunner> GetBlockingPoolTaskRunner(bool continue_on_shutdown = false) override;

 private:
#if defined(OS_WIN)
  typedef std::map<HMONITOR, int> AppbarAutohideEdgeMap;

  // Callback on main thread with the edges. |returned_edges| is the value that
  // was returned from the call to GetAutohideEdges() that initiated the lookup.
  void OnGotAppbarAutohideEdges(const base::Closure& callback,
                                HMONITOR monitor,
                                int returned_edges,
                                int edges);
#endif

  // Function to retrieve default opacity value mainly based on platform
  // and desktop context.
  views::Widget::InitParams::WindowOpacity GetOpacityForInitParams(
      const views::Widget::InitParams& params);

  // |ChromeViewsDelegate| exposes a |RefCounted|-like interface, but //chrome
  // uses |ScopedKeepAlive|s to manage lifetime. We manage an internal counter
  // to do that translation.
  unsigned int ref_count_ = 0u;

  std::unique_ptr<ScopedKeepAlive> keep_alive_;

#if defined(OS_WIN)
  AppbarAutohideEdgeMap appbar_autohide_edge_map_;
  // If true we're in the process of notifying a callback from
  // GetAutohideEdges().start a new query.
  bool in_autohide_edges_callback_;

  base::WeakPtrFactory<ChromeViewsDelegate> weak_factory_;
#endif

  DISALLOW_COPY_AND_ASSIGN(ChromeViewsDelegate);
};

#endif  // CHROME_BROWSER_UI_VIEWS_CHROME_VIEWS_DELEGATE_H_
