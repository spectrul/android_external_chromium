// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/debugger/devtools_manager.h"

#include <vector>

#include "base/auto_reset.h"
#include "base/message_loop.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/browsing_instance.h"
#include "chrome/browser/child_process_security_policy.h"
#include "chrome/browser/debugger/devtools_window.h"
#include "chrome/browser/debugger/devtools_client_host.h"
#include "chrome/browser/pref_service.h"
#include "chrome/browser/profile.h"
#include "chrome/browser/renderer_host/render_view_host.h"
#include "chrome/browser/renderer_host/site_instance.h"
#include "chrome/browser/tab_contents/tab_contents.h"
#include "chrome/common/devtools_messages.h"
#include "chrome/common/pref_names.h"
#include "googleurl/src/gurl.h"

// static
DevToolsManager* DevToolsManager::GetInstance() {
  // http://crbug.com/47806 this method may be called when BrowserProcess
  // has already been destroyed.
  if (!g_browser_process)
    return NULL;
  return g_browser_process->devtools_manager();
}

// static
void DevToolsManager::RegisterUserPrefs(PrefService* prefs) {
  prefs->RegisterBooleanPref(prefs::kDevToolsOpenDocked, true);
}

DevToolsManager::DevToolsManager()
    : inspected_rvh_for_reopen_(NULL),
      in_initial_show_(false),
      last_orphan_cookie_(0) {
}

DevToolsManager::~DevToolsManager() {
  DCHECK(inspected_rvh_to_client_host_.empty());
  DCHECK(client_host_to_inspected_rvh_.empty());
  // By the time we destroy devtools manager, all orphan client hosts should
  // have been delelted, no need to notify them upon tab closing.
  DCHECK(orphan_client_hosts_.empty());
}

DevToolsClientHost* DevToolsManager::GetDevToolsClientHostFor(
    RenderViewHost* inspected_rvh) {
  InspectedRvhToClientHostMap::iterator it =
      inspected_rvh_to_client_host_.find(inspected_rvh);
  if (it != inspected_rvh_to_client_host_.end())
    return it->second;
  return NULL;
}

void DevToolsManager::RegisterDevToolsClientHostFor(
    RenderViewHost* inspected_rvh,
    DevToolsClientHost* client_host) {
  DCHECK(!GetDevToolsClientHostFor(inspected_rvh));

  RuntimeFeatures initial_features;
  BindClientHost(inspected_rvh, client_host, initial_features);
  client_host->set_close_listener(this);
  SendAttachToAgent(inspected_rvh);
}

void DevToolsManager::ForwardToDevToolsAgent(
    RenderViewHost* client_rvh,
    const IPC::Message& message) {
  DevToolsClientHost* client_host = FindOnwerDevToolsClientHost(client_rvh);
  if (client_host)
    ForwardToDevToolsAgent(client_host, message);
}

void DevToolsManager::ForwardToDevToolsAgent(DevToolsClientHost* from,
                                             const IPC::Message& message) {
  RenderViewHost* inspected_rvh = GetInspectedRenderViewHost(from);
  if (!inspected_rvh) {
    // TODO(yurys): notify client that the agent is no longer available
    NOTREACHED();
    return;
  }

  IPC::Message* m = new IPC::Message(message);
  m->set_routing_id(inspected_rvh->routing_id());
  inspected_rvh->Send(m);
}

void DevToolsManager::ForwardToDevToolsClient(RenderViewHost* inspected_rvh,
                                              const IPC::Message& message) {
  DevToolsClientHost* client_host = GetDevToolsClientHostFor(inspected_rvh);
  if (!client_host) {
    // Client window was closed while there were messages
    // being sent to it.
    return;
  }
  client_host->SendMessageToClient(message);
}

void DevToolsManager::ActivateWindow(RenderViewHost* client_rvh) {
  DevToolsClientHost* client_host = FindOnwerDevToolsClientHost(client_rvh);
  if (!client_host)
    return;

  DevToolsWindow* window = client_host->AsDevToolsWindow();
  DCHECK(window);
  window->Activate();
}

void DevToolsManager::CloseWindow(RenderViewHost* client_rvh) {
  DevToolsClientHost* client_host = FindOnwerDevToolsClientHost(client_rvh);
  if (client_host) {
    RenderViewHost* inspected_rvh = GetInspectedRenderViewHost(client_host);
    DCHECK(inspected_rvh);
    UnregisterDevToolsClientHostFor(inspected_rvh);
  }
}

void DevToolsManager::RequestDockWindow(RenderViewHost* client_rvh) {
  ReopenWindow(client_rvh, true);
}

void DevToolsManager::RequestUndockWindow(RenderViewHost* client_rvh) {
  ReopenWindow(client_rvh, false);
}

void DevToolsManager::OpenDevToolsWindow(RenderViewHost* inspected_rvh) {
  ToggleDevToolsWindow(
      inspected_rvh,
      true,
      DEVTOOLS_TOGGLE_ACTION_NONE);
}

void DevToolsManager::ToggleDevToolsWindow(
    RenderViewHost* inspected_rvh,
    DevToolsToggleAction action) {
  ToggleDevToolsWindow(inspected_rvh, false, action);
}

void DevToolsManager::RuntimeFeatureStateChanged(RenderViewHost* inspected_rvh,
                                                 const std::string& feature,
                                                 bool enabled) {
  RuntimeFeaturesMap::iterator it = runtime_features_map_.find(inspected_rvh);
  if (it == runtime_features_map_.end()) {
    std::pair<RenderViewHost*, std::set<std::string> > value(
        inspected_rvh,
        std::set<std::string>());
    it = runtime_features_map_.insert(value).first;
  }
  if (enabled)
    it->second.insert(feature);
  else
    it->second.erase(feature);
}

void DevToolsManager::InspectElement(RenderViewHost* inspected_rvh,
                                     int x,
                                     int y) {
  OpenDevToolsWindow(inspected_rvh);
  IPC::Message* m = new DevToolsAgentMsg_InspectElement(x, y);
  m->set_routing_id(inspected_rvh->routing_id());
  inspected_rvh->Send(m);
}

void DevToolsManager::ClientHostClosing(DevToolsClientHost* host) {
  RenderViewHost* inspected_rvh = GetInspectedRenderViewHost(host);
  if (!inspected_rvh) {
    // It might be in the list of orphan client hosts, remove it from there.
    for (OrphanClientHosts::iterator it = orphan_client_hosts_.begin();
         it != orphan_client_hosts_.end(); ++it) {
      if (it->second.first == host) {
        orphan_client_hosts_.erase(it->first);
        return;
      }
    }
    return;
  }

  NotificationService::current()->Notify(
      NotificationType::DEVTOOLS_WINDOW_CLOSING,
      Source<Profile>(inspected_rvh->site_instance()->GetProcess()->profile()),
      Details<RenderViewHost>(inspected_rvh));

  SendDetachToAgent(inspected_rvh);
  UnbindClientHost(inspected_rvh, host);
}

RenderViewHost* DevToolsManager::GetInspectedRenderViewHost(
    DevToolsClientHost* client_host) {
  ClientHostToInspectedRvhMap::iterator it =
      client_host_to_inspected_rvh_.find(client_host);
  if (it != client_host_to_inspected_rvh_.end())
    return it->second;
  return NULL;
}

void DevToolsManager::UnregisterDevToolsClientHostFor(
      RenderViewHost* inspected_rvh) {
  DevToolsClientHost* host = GetDevToolsClientHostFor(inspected_rvh);
  if (!host)
    return;
  SendDetachToAgent(inspected_rvh);
  UnbindClientHost(inspected_rvh, host);

  if (inspected_rvh_for_reopen_ == inspected_rvh)
    inspected_rvh_for_reopen_ = NULL;

  // Issue tab closing event post unbound.
  host->InspectedTabClosing();

  int process_id = inspected_rvh->process()->id();
  for (InspectedRvhToClientHostMap::iterator it =
           inspected_rvh_to_client_host_.begin();
       it != inspected_rvh_to_client_host_.end();
       ++it) {
    if (it->first->process()->id() == process_id)
      return;
  }
  // We've disconnected from the last renderer -> revoke cookie permissions.
  ChildProcessSecurityPolicy::GetInstance()->RevokeReadRawCookies(process_id);
}

void DevToolsManager::OnNavigatingToPendingEntry(RenderViewHost* rvh,
                                                 RenderViewHost* dest_rvh,
                                                 const GURL& gurl) {
  if (in_initial_show_) {
    // Mute this even in case it is caused by the initial show routines.
    return;
  }

  int cookie = DetachClientHost(rvh);
  if (cookie != -1) {
    // Navigating to URL in the inspected window.
    AttachClientHost(cookie, dest_rvh);
    return;
  }

  // Iterate over client hosts and if there is one that has render view host
  // changing, reopen entire client window (this must be caused by the user
  // manually refreshing its content).
  for (ClientHostToInspectedRvhMap::iterator it =
           client_host_to_inspected_rvh_.begin();
       it != client_host_to_inspected_rvh_.end(); ++it) {
    DevToolsWindow* window = it->first->AsDevToolsWindow();
    if (window && window->GetRenderViewHost() == rvh) {
      inspected_rvh_for_reopen_ = it->second;
      MessageLoop::current()->PostTask(FROM_HERE,
          NewRunnableMethod(this,
                            &DevToolsManager::ForceReopenWindow));
      return;
    }
  }
}

int DevToolsManager::DetachClientHost(RenderViewHost* from_rvh) {
  DevToolsClientHost* client_host = GetDevToolsClientHostFor(from_rvh);
  if (!client_host)
    return -1;

  int cookie = last_orphan_cookie_++;
  orphan_client_hosts_[cookie] =
      std::pair<DevToolsClientHost*, RuntimeFeatures>(
          client_host, runtime_features_map_[from_rvh]);

  UnbindClientHost(from_rvh, client_host);
  return cookie;
}

void DevToolsManager::AttachClientHost(int client_host_cookie,
                                       RenderViewHost* to_rvh) {
  OrphanClientHosts::iterator it = orphan_client_hosts_.find(
      client_host_cookie);
  if (it == orphan_client_hosts_.end())
    return;

  DevToolsClientHost* client_host = (*it).second.first;
  BindClientHost(to_rvh, client_host, (*it).second.second);
  SendAttachToAgent(to_rvh);

  orphan_client_hosts_.erase(client_host_cookie);
}

void DevToolsManager::SendAttachToAgent(RenderViewHost* inspected_rvh) {
  if (inspected_rvh) {
    ChildProcessSecurityPolicy::GetInstance()->GrantReadRawCookies(
        inspected_rvh->process()->id());

    std::vector<std::string> features;
    RuntimeFeaturesMap::iterator it =
        runtime_features_map_.find(inspected_rvh);
    if (it != runtime_features_map_.end()) {
      features = std::vector<std::string>(it->second.begin(),
                                          it->second.end());
    }
    IPC::Message* m = new DevToolsAgentMsg_Attach(features);
    m->set_routing_id(inspected_rvh->routing_id());
    inspected_rvh->Send(m);
  }
}

void DevToolsManager::SendDetachToAgent(RenderViewHost* inspected_rvh) {
  if (inspected_rvh) {
    IPC::Message* m = new DevToolsAgentMsg_Detach();
    m->set_routing_id(inspected_rvh->routing_id());
    inspected_rvh->Send(m);
  }
}

void DevToolsManager::ForceReopenWindow() {
  if (inspected_rvh_for_reopen_) {
    RenderViewHost* inspected_rvn = inspected_rvh_for_reopen_;
    UnregisterDevToolsClientHostFor(inspected_rvn);
    OpenDevToolsWindow(inspected_rvn);
  }
}

DevToolsClientHost* DevToolsManager::FindOnwerDevToolsClientHost(
    RenderViewHost* client_rvh) {
  for (InspectedRvhToClientHostMap::iterator it =
           inspected_rvh_to_client_host_.begin();
       it != inspected_rvh_to_client_host_.end();
       ++it) {
    DevToolsWindow* win = it->second->AsDevToolsWindow();
    if (!win)
      continue;
    if (client_rvh == win->GetRenderViewHost())
      return it->second;
  }
  return NULL;
}

void DevToolsManager::ReopenWindow(RenderViewHost* client_rvh, bool docked) {
  DevToolsClientHost* client_host = FindOnwerDevToolsClientHost(client_rvh);
  if (!client_host)
    return;
  RenderViewHost* inspected_rvh = GetInspectedRenderViewHost(client_host);
  DCHECK(inspected_rvh);
  inspected_rvh->process()->profile()->GetPrefs()->SetBoolean(
      prefs::kDevToolsOpenDocked, docked);

  DevToolsWindow* window = client_host->AsDevToolsWindow();
  DCHECK(window);
  window->SetDocked(docked);
}

void DevToolsManager::ToggleDevToolsWindow(
    RenderViewHost* inspected_rvh,
    bool force_open,
    DevToolsToggleAction action) {
  bool do_open = force_open;
  DevToolsClientHost* host = GetDevToolsClientHostFor(inspected_rvh);
  if (!host) {
    bool docked = inspected_rvh->process()->profile()->GetPrefs()->
        GetBoolean(prefs::kDevToolsOpenDocked);
    host = new DevToolsWindow(
        inspected_rvh->site_instance()->browsing_instance()->profile(),
        inspected_rvh,
        docked);
    RegisterDevToolsClientHostFor(inspected_rvh, host);
    do_open = true;
  }
  DevToolsWindow* window = host->AsDevToolsWindow();
  if (!window)
    return;

  // If window is docked and visible, we hide it on toggle. If window is
  // undocked, we show (activate) it.
  if (!window->is_docked() || do_open) {
    AutoReset<bool> auto_reset_in_initial_show(&in_initial_show_, true);
    window->Show(action);
  } else {
    UnregisterDevToolsClientHostFor(inspected_rvh);
  }
}

void DevToolsManager::BindClientHost(RenderViewHost* inspected_rvh,
                                     DevToolsClientHost* client_host,
                                     const RuntimeFeatures& runtime_features) {
  DCHECK(inspected_rvh_to_client_host_.find(inspected_rvh) ==
      inspected_rvh_to_client_host_.end());
  DCHECK(client_host_to_inspected_rvh_.find(client_host) ==
      client_host_to_inspected_rvh_.end());

  inspected_rvh_to_client_host_[inspected_rvh] = client_host;
  client_host_to_inspected_rvh_[client_host] = inspected_rvh;
  runtime_features_map_[inspected_rvh] = runtime_features;
}

void DevToolsManager::UnbindClientHost(RenderViewHost* inspected_rvh,
                                       DevToolsClientHost* client_host) {
  DCHECK(inspected_rvh_to_client_host_.find(inspected_rvh)->second ==
      client_host);
  DCHECK(client_host_to_inspected_rvh_.find(client_host)->second ==
      inspected_rvh);

  inspected_rvh_to_client_host_.erase(inspected_rvh);
  client_host_to_inspected_rvh_.erase(client_host);
  runtime_features_map_.erase(inspected_rvh);
}