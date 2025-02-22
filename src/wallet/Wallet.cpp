// Copyright (c) 2011-2015 The Cryptonote developers
// Copyright (c) 2014-2015 XDN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Wallet.h"
#include "wallet_errors.h"
#include "string_tools.h"
#include "serialization/binary_utils.h"
#include "storages/portable_storage_template_helper.h"
#include "WalletUtils.h"
#include "WalletSerializer.h"

#include <numeric>
#include <time.h>
#include <string.h>

#include "WalletSerialization.h"

namespace {

const uint64_t ONE_DAY_SECONDS = 60 * 60 * 24;

void throwNotDefined() {
  throw std::runtime_error("The behavior is not defined!");
}

bool verifyKeys(const crypto::secret_key& sec, const crypto::public_key& expected_pub) {
  crypto::public_key pub;
  bool r = crypto::secret_key_to_public_key(sec, pub);
  return r && expected_pub == pub;
}

void throwIfKeysMissmatch(const crypto::secret_key& sec, const crypto::public_key& expected_pub) {
  if (!verifyKeys(sec, expected_pub))
    throw std::system_error(make_error_code(cryptonote::error::WRONG_PASSWORD));
}

class ContextCounterHolder
{
public:
  ContextCounterHolder(CryptoNote::WalletAsyncContextCounter& shutdowner) : m_shutdowner(shutdowner) {}
  ~ContextCounterHolder() { m_shutdowner.delAsyncContext(); }

private:
  CryptoNote::WalletAsyncContextCounter& m_shutdowner;
};

template <typename F>
void runAtomic(std::mutex& mutex, F f) {
  std::unique_lock<std::mutex> lock(mutex);
  f();
}

class InitWaiter : public CryptoNote::IWalletObserver {
public:
  InitWaiter() : future(promise.get_future()) {}

  virtual void initCompleted(std::error_code result) override {
    promise.set_value(result);
  }

  std::error_code waitInit() {
    return future.get();
  }
private:
  std::promise<std::error_code> promise;
  std::future<std::error_code> future;
};


class SaveWaiter : public CryptoNote::IWalletObserver {
public:
  SaveWaiter() : future(promise.get_future()) {}

  virtual void saveCompleted(std::error_code result) override {
    promise.set_value(result);
  }

  std::error_code waitSave() {
    return future.get();
  }

private:
  std::promise<std::error_code> promise;
  std::future<std::error_code> future;
};

uint64_t calculateDepositsAmount(const std::vector<CryptoNote::TransactionOutputInformation>& transfers, const cryptonote::Currency& currency) {
  return std::accumulate(transfers.begin(), transfers.end(), static_cast<uint64_t>(0), [&currency] (uint64_t sum, const CryptoNote::TransactionOutputInformation& deposit) {
    return sum + deposit.amount + currency.calculateInterest(deposit.amount, deposit.term);
  });
}

} //namespace

namespace CryptoNote {

class SyncStarter : public CryptoNote::IWalletObserver {
public:
  SyncStarter(BlockchainSynchronizer& sync) : m_sync(sync) {}
  virtual ~SyncStarter() {}

  virtual void initCompleted(std::error_code result) {
    if (!result) {
      m_sync.start();
    }
  }

  BlockchainSynchronizer& m_sync;
};

Wallet::Wallet(const cryptonote::Currency& currency, INode& node) :
  m_state(NOT_INITIALIZED),
  m_currency(currency),
  m_node(node),
  m_isStopping(false),
  m_lastNotifiedActualBalance(0),
  m_lastNotifiedPendingBalance(0),
  m_lastNotifiedActualDepositBalance(0),
  m_lastNotifiedPendingDepositBalance(0),
  m_blockchainSync(node, currency.genesisBlockHash()),
  m_transfersSync(currency, m_blockchainSync, node),
  m_transferDetails(nullptr),
  m_sender(nullptr),
  m_onInitSyncStarter(new SyncStarter(m_blockchainSync))
{
  addObserver(m_onInitSyncStarter.get());
  m_blockchainSync.addObserver(this);
}

Wallet::~Wallet() {
  removeObserver(m_onInitSyncStarter.get());

  {
    std::unique_lock<std::mutex> lock(m_cacheMutex);
    if (m_state != NOT_INITIALIZED) {
      m_sender->stop();
      m_isStopping = true;
    }
  }

    m_blockchainSync.removeObserver(this);
    m_blockchainSync.stop();
    m_asyncContextCounter.waitAsyncContextsFinish();
    m_sender.release();
}

void Wallet::addObserver(IWalletObserver* observer) {
  m_observerManager.add(observer);
}

void Wallet::removeObserver(IWalletObserver* observer) {
  m_observerManager.remove(observer);
}

void Wallet::initAndGenerate(const std::string& password) {
  {
    std::unique_lock<std::mutex> stateLock(m_cacheMutex);

    if (m_state != NOT_INITIALIZED) {
      throw std::system_error(make_error_code(cryptonote::error::ALREADY_INITIALIZED));
    }

    m_account.generate();
    m_password = password;

    initSync();
  }

  m_observerManager.notify(&IWalletObserver::initCompleted, std::error_code());
}

void Wallet::initWithKeys(const WalletAccountKeys& accountKeys, const std::string& password) {
  {
    std::unique_lock<std::mutex> stateLock(m_cacheMutex);

    if (m_state != NOT_INITIALIZED) {
      throw std::system_error(make_error_code(cryptonote::error::ALREADY_INITIALIZED));
    }

    cryptonote::account_keys keys;

    std::copy(accountKeys.spendPublicKey.begin(),
      accountKeys.spendPublicKey.end(),
      reinterpret_cast<uint8_t*>(&keys.m_account_address.m_spendPublicKey));

    std::copy(accountKeys.viewPublicKey.begin(),
      accountKeys.viewPublicKey.end(),
      reinterpret_cast<uint8_t*>(&keys.m_account_address.m_viewPublicKey));

    std::copy(accountKeys.spendSecretKey.begin(),
      accountKeys.spendSecretKey.end(),
      reinterpret_cast<uint8_t*>(&keys.m_spend_secret_key));

    std::copy(accountKeys.viewSecretKey.begin(),
      accountKeys.viewSecretKey.end(),
      reinterpret_cast<uint8_t*>(&keys.m_view_secret_key));

    m_account.set_keys(keys);
    m_account.set_createtime(ONE_DAY_SECONDS);
    m_password = password;

    initSync();
  }

  m_observerManager.notify(&IWalletObserver::initCompleted, std::error_code());
}

void Wallet::initAndLoad(std::istream& source, const std::string& password) {
  std::unique_lock<std::mutex> stateLock(m_cacheMutex);

  if (m_state != NOT_INITIALIZED) {
    throw std::system_error(make_error_code(cryptonote::error::ALREADY_INITIALIZED));
  }

  m_password = password;
  m_state = LOADING;

  m_asyncContextCounter.addAsyncContext();
  std::thread loader(&Wallet::doLoad, this, std::ref(source));
  loader.detach();
}

void Wallet::initSync() {
  AccountSubscription sub;
  sub.keys = reinterpret_cast<const AccountKeys&>(m_account.get_keys());
  sub.transactionSpendableAge = 6;
  sub.syncStart.height = 0;
  sub.syncStart.timestamp = m_account.get_createtime() - (ONE_DAY_SECONDS);
  
  auto& subObject = m_transfersSync.addSubscription(sub);
  m_transferDetails = &subObject.getContainer();
  subObject.addObserver(this);

  m_sender.reset(new WalletTransactionSender(m_currency, m_transactionsCache, m_account.get_keys(), *m_transferDetails));
  m_state = INITIALIZED;
}

void Wallet::doLoad(std::istream& source) {
  ContextCounterHolder counterHolder(m_asyncContextCounter);
  try {
    std::unique_lock<std::mutex> lock(m_cacheMutex);
    
    std::string cache;
    WalletSerializer serializer(m_account, m_transactionsCache);
    serializer.deserialize(source, m_password, cache);
      
    initSync();

    try {
      if (!cache.empty()) {
        std::stringstream stream(cache);
        m_transfersSync.load(stream);
      }
    } catch (const std::exception&) {
      // ignore cache loading errors
    }
  }
  catch (std::system_error& e) {
    runAtomic(m_cacheMutex, [this] () {this->m_state = Wallet::NOT_INITIALIZED;} );
    m_observerManager.notify(&IWalletObserver::initCompleted, e.code());
    return;
  }
  catch (std::exception&) {
    runAtomic(m_cacheMutex, [this] () {this->m_state = Wallet::NOT_INITIALIZED;} );
    m_observerManager.notify(&IWalletObserver::initCompleted, make_error_code(cryptonote::error::INTERNAL_WALLET_ERROR));
    return;
  }

  m_observerManager.notify(&IWalletObserver::initCompleted, std::error_code());
}

void Wallet::decrypt(const std::string& cipher, std::string& plain, crypto::chacha_iv iv, const std::string& password) {
  crypto::chacha_key key;
  crypto::cn_context context;
  crypto::generate_chacha8_key(context, password, key);

  plain.resize(cipher.size());

  crypto::chacha8(cipher.data(), cipher.size(), key, iv, &plain[0]);
}

void Wallet::shutdown() {
  {
    std::unique_lock<std::mutex> lock(m_cacheMutex);

    if (m_isStopping)
      throwNotDefined();

    m_isStopping = true;

    if (m_state != INITIALIZED)
      throwNotDefined();

    m_sender->stop();
  }

  m_blockchainSync.removeObserver(this);
  m_blockchainSync.stop();
  m_asyncContextCounter.waitAsyncContextsFinish();

  m_sender.release();
   
  {
    std::unique_lock<std::mutex> lock(m_cacheMutex);
    m_isStopping = false;
    m_state = NOT_INITIALIZED;
  }
}

void Wallet::reset() {
  InitWaiter initWaiter;
  SaveWaiter saveWaiter;

  addObserver(&initWaiter);
  addObserver(&saveWaiter);

  std::stringstream ss;
  try {
    save(ss, false, false);

    auto saveError = saveWaiter.waitSave();
    if (!saveError) {
      shutdown();
      initAndLoad(ss, m_password);
      initWaiter.waitInit();
    }
  } catch (std::exception&) {
  }

  removeObserver(&saveWaiter);
  removeObserver(&initWaiter);
}

std::vector<Payments> Wallet::getTransactionsByPaymentIds(const std::vector<PaymentId>& paymentIds) const {
  return m_transactionsCache.getTransactionsByPaymentIds(paymentIds);
}

void Wallet::save(std::ostream& destination, bool saveDetailed, bool saveCache) {
  if(m_isStopping) {
    m_observerManager.notify(&IWalletObserver::saveCompleted, make_error_code(cryptonote::error::OPERATION_CANCELLED));
    return;
  }

  {
    std::unique_lock<std::mutex> lock(m_cacheMutex);

    throwIf(m_state != INITIALIZED, cryptonote::error::WRONG_STATE);

    m_state = SAVING;
  }

  m_asyncContextCounter.addAsyncContext();
  std::thread saver(&Wallet::doSave, this, std::ref(destination), saveDetailed, saveCache);
  saver.detach();
}

void Wallet::doSave(std::ostream& destination, bool saveDetailed, bool saveCache) {
  ContextCounterHolder counterHolder(m_asyncContextCounter);

  try {
    m_blockchainSync.stop();
    std::unique_lock<std::mutex> lock(m_cacheMutex);
    
    WalletSerializer serializer(m_account, m_transactionsCache);
    std::string cache;

    if (saveCache) {
      std::stringstream stream;
      m_transfersSync.save(stream);
      cache = stream.str();
    }

    serializer.serialize(destination, m_password, saveDetailed, cache);

    m_state = INITIALIZED;
    m_blockchainSync.start(); //XXX: start can throw. what to do in this case?
  }
  catch (std::system_error& e) {
    runAtomic(m_cacheMutex, [this] () {this->m_state = Wallet::INITIALIZED;} );
    m_observerManager.notify(&IWalletObserver::saveCompleted, e.code());
    return;
  }
  catch (std::exception&) {
    runAtomic(m_cacheMutex, [this] () {this->m_state = Wallet::INITIALIZED;} );
    m_observerManager.notify(&IWalletObserver::saveCompleted, make_error_code(cryptonote::error::INTERNAL_WALLET_ERROR));
    return;
  }

  m_observerManager.notify(&IWalletObserver::saveCompleted, std::error_code());
}

crypto::chacha_iv Wallet::encrypt(const std::string& plain, std::string& cipher) {
  crypto::chacha_key key;
  crypto::cn_context context;
  crypto::generate_chacha8_key(context, m_password, key);

  cipher.resize(plain.size());

  crypto::chacha_iv iv = crypto::rand<crypto::chacha_iv>();
  crypto::chacha8(plain.data(), plain.size(), key, iv, &cipher[0]);

  return iv;
}

std::error_code Wallet::changePassword(const std::string& oldPassword, const std::string& newPassword) {
  std::unique_lock<std::mutex> passLock(m_cacheMutex);

  throwIfNotInitialised();

  if (m_password.compare(oldPassword))
    return make_error_code(cryptonote::error::WRONG_PASSWORD);

  //we don't let the user to change the password while saving
  m_password = newPassword;

  return std::error_code();
}

std::string Wallet::getAddress() {
  std::unique_lock<std::mutex> lock(m_cacheMutex);
  throwIfNotInitialised();

  return m_currency.accountAddressAsString(m_account);
}

uint64_t Wallet::actualBalance() {
  std::unique_lock<std::mutex> lock(m_cacheMutex);
  throwIfNotInitialised();

  return calculateActualBalance();
}

uint64_t Wallet::pendingBalance() {
  std::unique_lock<std::mutex> lock(m_cacheMutex);
  throwIfNotInitialised();

  return calculatePendingBalance();
}

uint64_t Wallet::actualDepositBalance() {
  std::unique_lock<std::mutex> lock(m_cacheMutex);
  throwIfNotInitialised();

  return calculateActualDepositBalance();
}

uint64_t Wallet::pendingDepositBalance() {
  std::unique_lock<std::mutex> lock(m_cacheMutex);
  throwIfNotInitialised();

  return calculatePendingDepositBalance();
}

size_t Wallet::getTransactionCount() {
  std::unique_lock<std::mutex> lock(m_cacheMutex);
  throwIfNotInitialised();

  return m_transactionsCache.getTransactionCount();
}

size_t Wallet::getTransferCount() {
  std::unique_lock<std::mutex> lock(m_cacheMutex);
  throwIfNotInitialised();

  return m_transactionsCache.getTransferCount();
}

size_t Wallet::getDepositCount() {
  std::unique_lock<std::mutex> lock(m_cacheMutex);
  throwIfNotInitialised();

  return m_transactionsCache.getDepositCount();
}

TransactionId Wallet::findTransactionByTransferId(TransferId transferId) {
  std::unique_lock<std::mutex> lock(m_cacheMutex);
  throwIfNotInitialised();

  return m_transactionsCache.findTransactionByTransferId(transferId);
}

bool Wallet::getTransaction(TransactionId transactionId, TransactionInfo& transaction) {
  std::unique_lock<std::mutex> lock(m_cacheMutex);
  throwIfNotInitialised();

  return m_transactionsCache.getTransaction(transactionId, transaction);
}

bool Wallet::getTransfer(TransferId transferId, Transfer& transfer) {
  std::unique_lock<std::mutex> lock(m_cacheMutex);
  throwIfNotInitialised();

  return m_transactionsCache.getTransfer(transferId, transfer);
}

bool Wallet::getDeposit(DepositId depositId, Deposit& deposit) {
  std::unique_lock<std::mutex> lock(m_cacheMutex);
  throwIfNotInitialised();

  return m_transactionsCache.getDeposit(depositId, deposit);
}

TransactionId Wallet::sendTransaction(const Transfer& transfer, uint64_t fee, const std::string& extra, uint64_t mixIn, uint64_t unlockTimestamp, const std::vector<TransactionMessage>& messages) {
  std::vector<Transfer> transfers;
  transfers.push_back(transfer);
  throwIfNotInitialised();

  return sendTransaction(transfers, fee, extra, mixIn, unlockTimestamp, messages);
}

TransactionId Wallet::sendTransaction(const std::vector<Transfer>& transfers, uint64_t fee, const std::string& extra, uint64_t mixIn, uint64_t unlockTimestamp, const std::vector<TransactionMessage>& messages) {
  TransactionId txId = 0;
  std::unique_ptr<WalletRequest> request;
  std::deque<std::unique_ptr<WalletEvent>> events;
  throwIfNotInitialised();

  {
    std::unique_lock<std::mutex> lock(m_cacheMutex);
    request = m_sender->makeSendRequest(txId, events, transfers, fee, extra, mixIn, unlockTimestamp, messages);
  }

  notifyClients(events);

  if (request) {
    m_asyncContextCounter.addAsyncContext();
    request->perform(m_node, std::bind(&Wallet::sendTransactionCallback, this, std::placeholders::_1, std::placeholders::_2));
  }

  return txId;
}

TransactionId Wallet::deposit(uint32_t term, uint64_t amount, uint64_t fee, uint64_t mixIn) {
  throwIfNotInitialised();

  TransactionId txId = 0;
  std::unique_ptr<WalletRequest> request;
  std::deque<std::unique_ptr<WalletEvent>> events;

  {
    std::unique_lock<std::mutex> lock(m_cacheMutex);
    request = m_sender->makeDepositRequest(txId, events, term, amount, fee, mixIn);

    if (request != nullptr) {
      pushBalanceUpdatedEvents(events);
    }
  }

  notifyClients(events);

  if (request) {
    m_asyncContextCounter.addAsyncContext();
    request->perform(m_node, std::bind(&Wallet::sendTransactionCallback, this, std::placeholders::_1, std::placeholders::_2));
  }

  return txId;
}

TransactionId Wallet::withdrawDeposits(const std::vector<DepositId>& depositIds, uint64_t fee) {
  throwIfNotInitialised();

  TransactionId txId = 0;
  std::unique_ptr<WalletRequest> request;
  std::deque<std::unique_ptr<WalletEvent>> events;

  {
    std::unique_lock<std::mutex> lock(m_cacheMutex);
    request = m_sender->makeWithdrawDepositRequest(txId, events, depositIds, fee);

    if (request != nullptr) {
      pushBalanceUpdatedEvents(events);
    }
  }

  notifyClients(events);

  if (request != nullptr) {
    m_asyncContextCounter.addAsyncContext();
    request->perform(m_node, std::bind(&Wallet::sendTransactionCallback, this, std::placeholders::_1, std::placeholders::_2));
  }

  return txId;
}

void Wallet::sendTransactionCallback(WalletRequest::Callback callback, std::error_code ec) {
  ContextCounterHolder counterHolder(m_asyncContextCounter);
  std::deque<std::unique_ptr<WalletEvent> > events;

  std::unique_ptr<WalletRequest> nextRequest;
  {
    std::unique_lock<std::mutex> lock(m_cacheMutex);
    callback(events, nextRequest, ec);

    auto actualDepositBalanceUpdated = getActualDepositBalanceChangedEvent();
    if (actualDepositBalanceUpdated) {
      events.push_back(std::move(actualDepositBalanceUpdated));
    }

    auto pendingDepositBalanceUpdated = getPendingDepositBalanceChangedEvent();
    if (pendingDepositBalanceUpdated) {
      events.push_back(std::move(pendingDepositBalanceUpdated));
    }
  }

  notifyClients(events);

  if (nextRequest) {
    m_asyncContextCounter.addAsyncContext();
    nextRequest->perform(m_node, std::bind(&Wallet::synchronizationCallback, this, std::placeholders::_1, std::placeholders::_2));
  }
}

void Wallet::synchronizationCallback(WalletRequest::Callback callback, std::error_code ec) {
  ContextCounterHolder counterHolder(m_asyncContextCounter);

  std::deque<std::unique_ptr<WalletEvent>> events;
  std::unique_ptr<WalletRequest> nextRequest;
  {
    std::unique_lock<std::mutex> lock(m_cacheMutex);
    callback(events, nextRequest, ec);
  }

  notifyClients(events);

  if (nextRequest) {
    m_asyncContextCounter.addAsyncContext();
    nextRequest->perform(m_node, std::bind(&Wallet::synchronizationCallback, this, std::placeholders::_1, std::placeholders::_2));
  }
}

std::error_code Wallet::cancelTransaction(size_t transactionId) {
  return make_error_code(cryptonote::error::TX_CANCEL_IMPOSSIBLE);
}

void Wallet::synchronizationProgressUpdated(uint64_t current, uint64_t total) {
  // forward notification
  m_observerManager.notify(&IWalletObserver::synchronizationProgressUpdated, current, total);

  // check if balance has changed and notify client
  notifyIfBalanceChanged();
}

void Wallet::synchronizationCompleted(std::error_code result) {
  if (result != std::make_error_code(std::errc::interrupted)) {
    m_observerManager.notify(&IWalletObserver::synchronizationCompleted, result);
  }

  if (!result) {
    notifyIfBalanceChanged();
  }
}

void Wallet::onTransactionUpdated(ITransfersSubscription* object, const Hash& transactionHash) {
  std::deque<std::unique_ptr<WalletEvent>> events;

  TransactionInformation txInfo;
  int64_t txBalance;
  if (m_transferDetails->getTransactionInformation(transactionHash, txInfo, txBalance)) {
    std::unique_lock<std::mutex> lock(m_cacheMutex);

    auto newDepositOuts = m_transferDetails->getTransactionOutputs(transactionHash, ITransfersContainer::IncludeTypeDeposit | ITransfersContainer::IncludeStateAll);
    auto spentDeposits = m_transferDetails->getTransactionInputs(transactionHash, ITransfersContainer::IncludeTypeDeposit);

    events = m_transactionsCache.onTransactionUpdated(txInfo, txBalance, newDepositOuts, spentDeposits, m_currency);

    auto actualDepositBalanceChangedEvent = getActualDepositBalanceChangedEvent();
    auto pendingDepositBalanceChangedEvent = getPendingDepositBalanceChangedEvent();

    if (actualDepositBalanceChangedEvent) {
      events.push_back(std::move(actualDepositBalanceChangedEvent));
    }

    if (pendingDepositBalanceChangedEvent) {
      events.push_back(std::move(pendingDepositBalanceChangedEvent));
    }
  }

  notifyClients(events);
}

void Wallet::onTransactionDeleted(ITransfersSubscription* object, const Hash& transactionHash) {
  std::deque<std::unique_ptr<WalletEvent>> events;

  {
    std::unique_lock<std::mutex> lock(m_cacheMutex);
    events = m_transactionsCache.onTransactionDeleted(transactionHash);

    std::unique_ptr<WalletEvent> actualDepositBalanceUpdated = getActualDepositBalanceChangedEvent();
    if (actualDepositBalanceUpdated) {
      events.push_back(std::move(actualDepositBalanceUpdated));
    }

    std::unique_ptr<WalletEvent> pendingDepositBalanceUpdated = getPendingDepositBalanceChangedEvent();
    if (pendingDepositBalanceUpdated) {
      events.push_back(std::move(pendingDepositBalanceUpdated));
    }
  }

  notifyClients(events);
}

void Wallet::onTransfersUnlocked(ITransfersSubscription* object, const std::vector<TransactionOutputInformation>& unlockedTransfers) {
  std::unique_lock<std::mutex> lock(m_cacheMutex);
  auto unlockedDeposits = m_transactionsCache.unlockDeposits(unlockedTransfers);
  lock.unlock();

  if (!unlockedDeposits.empty()) {
    m_observerManager.notify(&IWalletObserver::depositsUpdated, unlockedDeposits);

    notifyIfDepositBalanceChanged();
  }
}

void Wallet::onTransfersLocked(ITransfersSubscription* object, const std::vector<TransactionOutputInformation>& lockedTransfers) {
  std::unique_lock<std::mutex> lock(m_cacheMutex);
  auto lockedDeposits = m_transactionsCache.lockDeposits(lockedTransfers);
  lock.unlock();

  if (!lockedDeposits.empty()) {
    m_observerManager.notify(&IWalletObserver::depositsUpdated, lockedDeposits);

    notifyIfDepositBalanceChanged();
  }
}

void Wallet::throwIfNotInitialised() {
  if (m_state == NOT_INITIALIZED || m_state == LOADING)
    throw std::system_error(make_error_code(cryptonote::error::NOT_INITIALIZED));
  assert(m_transferDetails);
}

void Wallet::notifyClients(std::deque<std::unique_ptr<WalletEvent>>& events) {
  while (!events.empty()) {
    std::unique_ptr<WalletEvent>& event = events.front();
    event->notify(m_observerManager);
    events.pop_front();
  }
}

void Wallet::notifyIfBalanceChanged() {
  auto actual = actualBalance();
  auto prevActual = m_lastNotifiedActualBalance.exchange(actual);

  if (prevActual != actual) {
    m_observerManager.notify(&IWalletObserver::actualBalanceUpdated, actual);
  }

  auto pending = pendingBalance();
  auto prevPending = m_lastNotifiedPendingBalance.exchange(pending);

  if (prevPending != pending) {
    m_observerManager.notify(&IWalletObserver::pendingBalanceUpdated, pending);
  }

}

void Wallet::notifyIfDepositBalanceChanged() {
  std::unique_ptr<WalletEvent> actualEvent = getActualDepositBalanceChangedEvent();
  std::unique_ptr<WalletEvent> pendingEvent = getPendingDepositBalanceChangedEvent();

  if (actualEvent) {
    actualEvent->notify(m_observerManager);
  }

  if (pendingEvent) {
    pendingEvent->notify(m_observerManager);
  }
}

std::unique_ptr<WalletEvent> Wallet::getActualDepositBalanceChangedEvent() {
  auto actual = calculateActualDepositBalance();
  auto prevActual = m_lastNotifiedActualDepositBalance.exchange(actual);

  std::unique_ptr<WalletEvent> event;

  if (actual != prevActual) {
    event = std::unique_ptr<WalletEvent>(new WalletActualDepositBalanceUpdatedEvent(actual));
  }

  return event;
}

std::unique_ptr<WalletEvent> Wallet::getPendingDepositBalanceChangedEvent() {
  auto pending = calculatePendingDepositBalance();
  auto prevPending = m_lastNotifiedPendingDepositBalance.exchange(pending);

  std::unique_ptr<WalletEvent> event;

  if (pending != prevPending) {
    event = std::unique_ptr<WalletEvent>(new WalletPendingDepositBalanceUpdatedEvent(pending));
  }

  return event;
}

std::unique_ptr<WalletEvent> Wallet::getActualBalanceChangedEvent() {
  auto actual = calculateActualBalance();
  auto prevActual = m_lastNotifiedActualBalance.exchange(actual);

  std::unique_ptr<WalletEvent> event;

  if (actual != prevActual) {
    event = std::unique_ptr<WalletEvent>(new WalletActualBalanceUpdatedEvent(actual));
  }

  return event;
}

std::unique_ptr<WalletEvent> Wallet::getPendingBalanceChangedEvent() {
  auto pending = calculatePendingBalance();
  auto prevPending = m_lastNotifiedPendingBalance.exchange(pending);

  std::unique_ptr<WalletEvent> event;

  if (pending != prevPending) {
    event = std::unique_ptr<WalletEvent>(new WalletPendingBalanceUpdatedEvent(pending));
  }

  return event;
}

void Wallet::getAccountKeys(WalletAccountKeys& keys) {
  if (m_state == NOT_INITIALIZED) {
    throw std::system_error(make_error_code(cryptonote::error::NOT_INITIALIZED));
  }

  const cryptonote::account_keys& accountKeys = m_account.get_keys();
  std::copy(reinterpret_cast<const uint8_t*>(&accountKeys.m_account_address.m_spendPublicKey),
    reinterpret_cast<const uint8_t*>(&accountKeys.m_account_address.m_spendPublicKey) + sizeof(crypto::public_key),
    keys.spendPublicKey.begin());

  std::copy(reinterpret_cast<const uint8_t*>(&accountKeys.m_spend_secret_key),
    reinterpret_cast<const uint8_t*>(&accountKeys.m_spend_secret_key) + sizeof(crypto::secret_key),
    keys.spendSecretKey.begin());

  std::copy(reinterpret_cast<const uint8_t*>(&accountKeys.m_account_address.m_viewPublicKey),
    reinterpret_cast<const uint8_t*>(&accountKeys.m_account_address.m_viewPublicKey) + sizeof(crypto::public_key),
    keys.viewPublicKey.begin());

  std::copy(reinterpret_cast<const uint8_t*>(&accountKeys.m_view_secret_key),
    reinterpret_cast<const uint8_t*>(&accountKeys.m_view_secret_key) + sizeof(crypto::secret_key),
    keys.viewSecretKey.begin());
}

uint64_t Wallet::calculateActualDepositBalance() {
  std::vector<TransactionOutputInformation> transfers;
  m_transferDetails->getOutputs(transfers, ITransfersContainer::IncludeTypeDeposit | ITransfersContainer::IncludeStateUnlocked);

  return calculateDepositsAmount(transfers, m_currency) - m_transactionsCache.countUnconfirmedSpentDepositsTotalAmount();
}

uint64_t Wallet::calculatePendingDepositBalance() {
  std::vector<TransactionOutputInformation> transfers;
  m_transferDetails->getOutputs(transfers, ITransfersContainer::IncludeTypeDeposit
                                | ITransfersContainer::IncludeStateLocked
                                | ITransfersContainer::IncludeStateSoftLocked);

  return calculateDepositsAmount(transfers, m_currency) + m_transactionsCache.countUnconfirmedCreatedDepositsSum();
}

uint64_t Wallet::calculateActualBalance() {
  return m_transferDetails->balance(ITransfersContainer::IncludeKeyUnlocked) -
    m_transactionsCache.unconfrimedOutsAmount();
}

uint64_t Wallet::calculatePendingBalance() {
  uint64_t change = m_transactionsCache.unconfrimedOutsAmount() - m_transactionsCache.unconfirmedTransactionsAmount();
  uint64_t spentDeposits = m_transactionsCache.countUnconfirmedSpentDepositsProfit();
  uint64_t container = m_transferDetails->balance(ITransfersContainer::IncludeKeyNotUnlocked);

  return container + change + spentDeposits;
}

void Wallet::pushBalanceUpdatedEvents(std::deque<std::unique_ptr<WalletEvent>>& eventsQueue) {
  auto actualDepositBalanceUpdated = getActualDepositBalanceChangedEvent();
  if (actualDepositBalanceUpdated != nullptr) {
    eventsQueue.push_back(std::move(actualDepositBalanceUpdated));
  }

  auto pendingDepositBalanceUpdated = getPendingDepositBalanceChangedEvent();
  if (pendingDepositBalanceUpdated != nullptr) {
    eventsQueue.push_back(std::move(pendingDepositBalanceUpdated));
  }

  auto actualBalanceUpdated = getActualBalanceChangedEvent();
  if (actualBalanceUpdated != nullptr) {
    eventsQueue.push_back(std::move(actualBalanceUpdated));
  }

  auto pendingBalanceUpdated = getPendingBalanceChangedEvent();
  if (pendingBalanceUpdated != nullptr) {
    eventsQueue.push_back(std::move(pendingBalanceUpdated));
  }
}

} //namespace CryptoNote
