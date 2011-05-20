// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_POLICY_ASYNCHRONOUS_POLICY_TEST_BASE_H_
#define CHROME_BROWSER_POLICY_ASYNCHRONOUS_POLICY_TEST_BASE_H_
#pragma once

#include "base/message_loop.h"
#include "chrome/browser/browser_thread.h"
#include "chrome/browser/policy/asynchronous_policy_provider.h"
#include "chrome/browser/policy/mock_configuration_policy_store.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace policy {

// A delegate for testing that can feed arbitrary information to the loader.
class ProviderDelegateMock : public AsynchronousPolicyProvider::Delegate {
 public:
  ProviderDelegateMock() : AsynchronousPolicyProvider::Delegate() {}
  virtual ~ProviderDelegateMock() {}

  MOCK_METHOD0(Load, DictionaryValue*());

 private:
  DISALLOW_COPY_AND_ASSIGN(ProviderDelegateMock);
};

class AsynchronousPolicyTestBase : public testing::Test {
 public:
  AsynchronousPolicyTestBase()
      : ui_thread_(BrowserThread::UI, &loop_),
        file_thread_(BrowserThread::FILE, &loop_) {}

  virtual ~AsynchronousPolicyTestBase() {}

  virtual void SetUp() {
    delegate_.reset(new ProviderDelegateMock());
    store_.reset(new MockConfigurationPolicyStore);
  }

  virtual void TearDown() {
    loop_.RunAllPending();
  }

 protected:
  MessageLoop loop_;

  // The mocks that are used in the test must outlive the scope of the test
  // because they still get accessed in the RunAllPending of the TearDown.
  scoped_ptr<MockConfigurationPolicyStore> store_;
  scoped_ptr<ProviderDelegateMock> delegate_;

 private:
  BrowserThread ui_thread_;
  BrowserThread file_thread_;

  DISALLOW_COPY_AND_ASSIGN(AsynchronousPolicyTestBase);
};

}  // namespace policy

#endif  // CHROME_BROWSER_POLICY_ASYNCHRONOUS_POLICY_TEST_BASE_H_
