// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "config.h"

#include "Frame.h"
#include "PageGroupLoadDeferrer.h"
#include "v8_proxy.h"
#include <wtf/HashSet.h>
#undef LOG

#include "base/json_reader.h"
#include "base/json_writer.h"
#include "base/string_util.h"
#include "base/values.h"
#include "webkit/glue/devtools/debugger_agent_impl.h"
#include "webkit/glue/devtools/debugger_agent_manager.h"
#include "webkit/glue/webdevtoolsagent_impl.h"
#include "webkit/glue/webview_impl.h"

#if USE(V8)
#include "v8/include/v8-debug.h"
#endif

WebDevToolsAgent::MessageLoopDispatchHandler
    DebuggerAgentManager::message_loop_dispatch_handler_ = NULL;

// static
bool DebuggerAgentManager::in_host_dispatch_handler_ = false;

// static
DebuggerAgentManager::DeferrersMap DebuggerAgentManager::page_deferrers_;

namespace {

class CallerIdWrapper : public v8::Debug::ClientData {
 public:
  CallerIdWrapper() : caller_is_mananager_(true), caller_id_(0) {}
  explicit CallerIdWrapper(int caller_id)
    : caller_is_mananager_(false),
      caller_id_(caller_id) {}
  ~CallerIdWrapper() {}
  bool caller_is_mananager() const { return caller_is_mananager_; }
  int caller_id() const { return caller_id_; }
 private:
  bool caller_is_mananager_;
  int caller_id_;
  DISALLOW_COPY_AND_ASSIGN(CallerIdWrapper);
};

}  // namespace


// static
void DebuggerAgentManager::V8DebugMessageHandler(const uint16_t* message,
                                                 int length,
                                                 v8::Debug::ClientData* data) {
#if USE(V8)
  std::wstring out(reinterpret_cast<const wchar_t*>(message), length);
  std::string out_utf8 = WideToUTF8(out);
  DebuggerAgentManager::DebuggerOutput(out_utf8, data);
#endif
}

void DebuggerAgentManager::V8DebugHostDispatchHandler() {
  if (!DebuggerAgentManager::message_loop_dispatch_handler_ ||
      !attached_agents_) {
    return;
  }
  if (in_host_dispatch_handler_) {
    return;
  }
  in_host_dispatch_handler_ = true;

  Vector<WebViewImpl*> views;
  // 1. Disable active objects and input events.
  for (AttachedAgentsSet::iterator it = attached_agents_->begin();
       it != attached_agents_->end();
       ++it) {
    DebuggerAgentImpl* agent = *it;
    page_deferrers_.set(
        agent->web_view(),
        new WebCore::PageGroupLoadDeferrer(agent->GetPage(), true));
    views.append(agent->web_view());
    agent->web_view()->SetIgnoreInputEvents(true);
  }

  // 2. Process messages.
  DebuggerAgentManager::message_loop_dispatch_handler_();

  // 3. Bring things back.
  for (Vector<WebViewImpl*>::iterator it = views.begin();
       it != views.end();
       ++it) {
    if (page_deferrers_.contains(*it)) {
      // The view was not closed during the dispatch.
      (*it)->SetIgnoreInputEvents(false);
    }
  }
  deleteAllValues(page_deferrers_);
  page_deferrers_.clear();

  in_host_dispatch_handler_ = false;
  if (!attached_agents_) {
    // Remove handlers if all agents were detached within host dispatch.
    v8::Debug::SetMessageHandler(NULL);
    v8::Debug::SetHostDispatchHandler(NULL);
  }
}

// static
DebuggerAgentManager::AttachedAgentsSet*
    DebuggerAgentManager::attached_agents_ = NULL;

// static
void DebuggerAgentManager::DebugAttach(DebuggerAgentImpl* debugger_agent) {
#if USE(V8)
  if (!attached_agents_) {
    attached_agents_ = new AttachedAgentsSet();
    v8::Debug::SetMessageHandler(
        &DebuggerAgentManager::V8DebugMessageHandler,
        false /* don't create separate thread for sending debugger output */);
    v8::Debug::SetHostDispatchHandler(
        &DebuggerAgentManager::V8DebugHostDispatchHandler, 100 /* ms */);
  }
  attached_agents_->add(debugger_agent);
#endif
}

// static
void DebuggerAgentManager::DebugDetach(DebuggerAgentImpl* debugger_agent) {
#if USE(V8)
  if (!attached_agents_) {
    NOTREACHED();
    return;
  }
  DCHECK(attached_agents_->contains(debugger_agent));
  attached_agents_->remove(debugger_agent);
  if (attached_agents_->isEmpty()) {
    delete attached_agents_;
    attached_agents_ = NULL;
    // Note that we do not empty handlers while in dispatch - we schedule
    // continue and do removal once we are out of the dispatch.
    if (!in_host_dispatch_handler_) {
      v8::Debug::SetMessageHandler(NULL);
      v8::Debug::SetHostDispatchHandler(NULL);
    } else if (FindAgentForCurrentV8Context() == debugger_agent) {
      // Force continue just in case to handle close while on a breakpoint.
      SendContinueCommandToV8();
    }
  }
#endif
}

// static
void DebuggerAgentManager::DebugBreak(DebuggerAgentImpl* debugger_agent) {
#if USE(V8)
  DCHECK(attached_agents_->contains(debugger_agent));
  v8::Debug::DebugBreak();
#endif
}

// static
void DebuggerAgentManager::DebuggerOutput(const std::string& out,
                                          v8::Debug::ClientData* caller_data) {
  // If caller_data is not NULL the message is a response to a debugger command.
  if (caller_data) {
    CallerIdWrapper* wrapper = static_cast<CallerIdWrapper*>(caller_data);
    if (wrapper->caller_is_mananager()) {
      // Just ignore messages sent by this manager.
      return;
    }
    DebuggerAgentImpl* debugger_agent = FindDebuggerAgentForToolsAgent(
        wrapper->caller_id());
    if (debugger_agent) {
      debugger_agent->DebuggerOutput(out);
    }
    return;
  } // Otherwise it's an event message.

  // Agent that should be used for sending events is determined based
  // on the active Frame.
  DebuggerAgentImpl* agent = FindAgentForCurrentV8Context();
  if (!agent) {
    // Autocontinue execution on break and exception  events if there is no
    // handler.
    SendContinueCommandToV8();
    return;
  }
  agent->DebuggerOutput(out);
}

// static
void DebuggerAgentManager::ExecuteDebuggerCommand(
    const std::string& command,
    int caller_id) {
  SendCommandToV8(UTF8ToWide(command), new CallerIdWrapper(caller_id));
}

// static
void DebuggerAgentManager::SetMessageLoopDispatchHandler(
    WebDevToolsAgent::MessageLoopDispatchHandler handler) {
  message_loop_dispatch_handler_ = handler;
}

// static
void DebuggerAgentManager::OnWebViewClosed(WebViewImpl* webview) {
  if (page_deferrers_.contains(webview)) {
    delete page_deferrers_.get(webview);
    page_deferrers_.remove(webview);
  }
}

// static
void DebuggerAgentManager::SendCommandToV8(const std::wstring& cmd,
                                           v8::Debug::ClientData* data) {
#if USE(V8)
  v8::Debug::SendCommand(reinterpret_cast<const uint16_t*>(cmd.data()),
                         cmd.length(),
                         data);
#endif
}

void DebuggerAgentManager::SendContinueCommandToV8() {
  std::wstring continue_cmd(
      L"{\"seq\":1,\"type\":\"request\",\"command\":\"continue\"}");
  SendCommandToV8(continue_cmd, new CallerIdWrapper());
}

// static
DebuggerAgentImpl* DebuggerAgentManager::FindAgentForCurrentV8Context() {
  if (!attached_agents_) {
    return NULL;
  }
  DCHECK(!attached_agents_->isEmpty());

  WebCore::Frame* frame = WebCore::V8Proxy::retrieveActiveFrame();
  if (!frame) {
    return NULL;
  }
  WebCore::Page* page = frame->page();
  for (AttachedAgentsSet::iterator it = attached_agents_->begin();
       it != attached_agents_->end(); ++it) {
    if ((*it)->GetPage() == page) {
      return *it;
    }
  }
  return NULL;
}

DebuggerAgentImpl* DebuggerAgentManager::FindDebuggerAgentForToolsAgent(
    int caller_id) {
  for (AttachedAgentsSet::iterator it = attached_agents_->begin();
       it != attached_agents_->end(); ++it) {
    if ((*it)->webdevtools_agent()->host_id() == caller_id) {
      return *it;
    }
  }
  return NULL;
}
