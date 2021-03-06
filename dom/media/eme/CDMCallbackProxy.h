/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CDMCallbackProxy_h_
#define CDMCallbackProxy_h_

#include "mozilla/CDMProxy.h"
#include "gmp-decryption.h"
#include "GMPDecryptorProxy.h"

namespace mozilla {

// Proxies call backs from the CDM on the GMP thread back to the MediaKeys
// object on the main thread.
class CDMCallbackProxy : public GMPDecryptorProxyCallback {
public:
  void SetSessionId(uint32_t aCreateSessionToken,
                    const nsCString& aSessionId) override;

  void ResolveLoadSessionPromise(uint32_t aPromiseId,
                                 bool aSuccess) override;

  void ResolvePromise(uint32_t aPromiseId) override;

  void RejectPromise(uint32_t aPromiseId,
                     nsresult aException,
                     const nsCString& aSessionId) override;

  void SessionMessage(const nsCString& aSessionId,
                      GMPSessionMessageType aMessageType,
                      const nsTArray<uint8_t>& aMessage) override;

  void ExpirationChange(const nsCString& aSessionId,
                        GMPTimestamp aExpiryTime) override;

  void SessionClosed(const nsCString& aSessionId) override;

  void SessionError(const nsCString& aSessionId,
                    nsresult aException,
                    uint32_t aSystemCode,
                    const nsCString& aMessage) override;

  void KeyStatusChanged(const nsCString& aSessionId,
                        const nsTArray<uint8_t>& aKeyId,
                        GMPMediaKeyStatus aStatus) override;

  void Decrypted(uint32_t aId,
                 GMPErr aResult,
                 const nsTArray<uint8_t>& aDecryptedData) override;

  void Terminated() override;

  ~CDMCallbackProxy() {}

private:
  friend class CDMProxy;
  explicit CDMCallbackProxy(CDMProxy* aProxy);

  // Warning: Weak ref.
  CDMProxy* mProxy;
};

} // namespace mozilla

#endif // CDMCallbackProxy_h_
