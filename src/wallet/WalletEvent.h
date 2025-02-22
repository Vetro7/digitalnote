// Copyright (c) 2011-2015 The Cryptonote developers
// Copyright (c) 2014-2015 XDN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "IWallet.h"
#include "common/ObserverManager.h"

namespace CryptoNote
{

class WalletEvent
{
public:
  virtual ~WalletEvent() {};

  virtual void notify(tools::ObserverManager<CryptoNote::IWalletObserver>& observer) = 0;
};

class WalletTransactionUpdatedEvent : public WalletEvent
{
public:
  WalletTransactionUpdatedEvent(TransactionId transactionId) : m_id(transactionId) {};
  virtual ~WalletTransactionUpdatedEvent() {};

  virtual void notify(tools::ObserverManager<CryptoNote::IWalletObserver>& observer)
  {
    observer.notify(&IWalletObserver::transactionUpdated, m_id);
  }

private:
  TransactionId m_id;
};

class WalletSendTransactionCompletedEvent : public WalletEvent
{
public:
  WalletSendTransactionCompletedEvent(TransactionId transactionId, std::error_code result) : m_id(transactionId), m_error(result) {};
  virtual ~WalletSendTransactionCompletedEvent() {};

  virtual void notify(tools::ObserverManager<CryptoNote::IWalletObserver>& observer)
  {
    observer.notify(&IWalletObserver::sendTransactionCompleted, m_id, m_error);
  }

private:
  TransactionId m_id;
  std::error_code m_error;
};

class WalletExternalTransactionCreatedEvent : public WalletEvent
{
public:
  WalletExternalTransactionCreatedEvent(TransactionId transactionId) : m_id(transactionId) {};
  virtual ~WalletExternalTransactionCreatedEvent() {};

  virtual void notify(tools::ObserverManager<CryptoNote::IWalletObserver>& observer)
  {
    observer.notify(&IWalletObserver::externalTransactionCreated, m_id);
  }
private:
  TransactionId m_id;
};

class WalletDepositsUpdatedEvent : public WalletEvent {
public:
  WalletDepositsUpdatedEvent(std::vector<DepositId>&& depositIds) : updatedDeposits(depositIds) {}

  virtual ~WalletDepositsUpdatedEvent() {}

  virtual void notify(tools::ObserverManager<CryptoNote::IWalletObserver>& observer) override {
    observer.notify(&IWalletObserver::depositsUpdated, updatedDeposits);
  }
private:
  std::vector<DepositId> updatedDeposits;
};

class WalletSynchronizationProgressUpdatedEvent : public WalletEvent
{
public:
  WalletSynchronizationProgressUpdatedEvent(uint64_t current, uint64_t total) : m_current(current), m_total(total) {};
  virtual ~WalletSynchronizationProgressUpdatedEvent() {};

  virtual void notify(tools::ObserverManager<CryptoNote::IWalletObserver>& observer)
  {
    observer.notify(&IWalletObserver::synchronizationProgressUpdated, m_current, m_total);
  }

private:
  uint64_t m_current;
  uint64_t m_total;
};

class WalletSynchronizationCompletedEvent : public WalletEvent {
public:
  WalletSynchronizationCompletedEvent(uint64_t current, uint64_t total, std::error_code result) : m_ec(result) {};
  virtual ~WalletSynchronizationCompletedEvent() {};

  virtual void notify(tools::ObserverManager<CryptoNote::IWalletObserver>& observer) {
    observer.notify(&IWalletObserver::synchronizationCompleted, m_ec);
  }

private:
  std::error_code m_ec;
};

class WalletActualBalanceUpdatedEvent : public WalletEvent
{
public:
  WalletActualBalanceUpdatedEvent(uint64_t balance) : m_balance(balance) {};
  virtual ~WalletActualBalanceUpdatedEvent() {};

  virtual void notify(tools::ObserverManager<CryptoNote::IWalletObserver>& observer)
  {
    observer.notify(&IWalletObserver::actualBalanceUpdated, m_balance);
  }
private:
  uint64_t m_balance;
};

class WalletPendingBalanceUpdatedEvent : public WalletEvent
{
public:
  WalletPendingBalanceUpdatedEvent(uint64_t balance) : m_balance(balance) {};
  virtual ~WalletPendingBalanceUpdatedEvent() {};

  virtual void notify(tools::ObserverManager<CryptoNote::IWalletObserver>& observer)
  {
    observer.notify(&IWalletObserver::pendingBalanceUpdated, m_balance);
  }
private:
  uint64_t m_balance;
};

class WalletActualDepositBalanceUpdatedEvent : public WalletEvent
{
public:
  WalletActualDepositBalanceUpdatedEvent(uint64_t balance) : m_balance(balance) {}
  virtual ~WalletActualDepositBalanceUpdatedEvent() {}

  virtual void notify(tools::ObserverManager<CryptoNote::IWalletObserver>& observer)
  {
    observer.notify(&IWalletObserver::actualDepositBalanceUpdated, m_balance);
  }
private:
  uint64_t m_balance;
};

class WalletPendingDepositBalanceUpdatedEvent : public WalletEvent
{
public:
  WalletPendingDepositBalanceUpdatedEvent(uint64_t balance) : m_balance(balance) {}
  virtual ~WalletPendingDepositBalanceUpdatedEvent() {}

  virtual void notify(tools::ObserverManager<CryptoNote::IWalletObserver>& observer)
  {
    observer.notify(&IWalletObserver::pendingDepositBalanceUpdated, m_balance);
  }
private:
  uint64_t m_balance;
};

} /* namespace CryptoNote */
