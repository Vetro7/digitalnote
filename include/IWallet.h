// Copyright (c) 2011-2015 The Cryptonote developers
// Copyright (c) 2014-2015 XDN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <array>
#include <cstdint>
#include <istream>
#include <limits>
#include <ostream>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

namespace CryptoNote {

typedef size_t TransactionId;
typedef size_t TransferId;
typedef size_t DepositId;
typedef std::array<uint8_t, 32> TransactionHash;

struct Transfer {
  std::string address;
  int64_t amount;
};

const TransactionId INVALID_TRANSACTION_ID    = std::numeric_limits<TransactionId>::max();
const TransferId INVALID_TRANSFER_ID          = std::numeric_limits<TransferId>::max();
const DepositId INVALID_DEPOSIT_ID            = std::numeric_limits<DepositId>::max();
const uint64_t UNCONFIRMED_TRANSACTION_HEIGHT = std::numeric_limits<uint64_t>::max();

enum class TransactionState : uint8_t {
  Active,    // --> {Deleted}
  Deleted,   // --> {Active}

  Sending,   // --> {Active, Cancelled, Failed}
  Cancelled, // --> {}
  Failed     // --> {}
};

struct TransactionMessage
{
  std::string message;
  std::string address;
};

struct TransactionInfo {
  TransferId       firstTransferId;
  size_t           transferCount;
  DepositId        firstDepositId;
  size_t           depositCount;
  int64_t          totalAmount;
  uint64_t         fee;
  uint64_t         sentTime;
  uint64_t         unlockTime;
  TransactionHash  hash;
  bool             isCoinbase;
  uint64_t         blockHeight;
  uint64_t         timestamp;
  std::string      extra;
  TransactionState state;
  std::vector<std::string> messages;
};

struct Deposit {
  TransactionId creatingTransactionId;
  TransactionId spendingTransactionId;
  uint32_t term;
  uint64_t amount;
  uint64_t interest;
  bool locked;
};

typedef std::array<uint8_t, 32> WalletPublicKey;
typedef std::array<uint8_t, 32> WalletSecretKey;

struct WalletAccountKeys {
  WalletPublicKey viewPublicKey;
  WalletSecretKey viewSecretKey;
  WalletPublicKey spendPublicKey;
  WalletSecretKey spendSecretKey;
};

using PaymentId = std::array<uint8_t, 32>;
struct Payments {
  PaymentId paymentId;
  std::vector<TransactionInfo> transactions;
};

static_assert(std::is_move_constructible<Payments>::value, "Payments is not move constructible");

class IWalletObserver {
public:
  virtual ~IWalletObserver() {}

  virtual void initCompleted(std::error_code result) {}
  virtual void saveCompleted(std::error_code result) {}
  virtual void synchronizationProgressUpdated(uint64_t current, uint64_t total) {}
  virtual void synchronizationCompleted(std::error_code result) {}
  virtual void actualBalanceUpdated(uint64_t actualBalance) {}
  virtual void pendingBalanceUpdated(uint64_t pendingBalance) {}
  virtual void actualDepositBalanceUpdated(uint64_t actualDepositBalance) {}
  virtual void pendingDepositBalanceUpdated(uint64_t pendingDepositBalance) {}
  virtual void externalTransactionCreated(TransactionId transactionId) {}
  virtual void sendTransactionCompleted(TransactionId transactionId, std::error_code result) {}
  virtual void transactionUpdated(TransactionId transactionId) {}
  virtual void depositsUpdated(const std::vector<DepositId>& depositIds) {}
};

class IWallet {
public:
  virtual ~IWallet() {}
  virtual void addObserver(IWalletObserver* observer) = 0;
  virtual void removeObserver(IWalletObserver* observer) = 0;

  virtual void initAndGenerate(const std::string& password) = 0;
  virtual void initAndLoad(std::istream& source, const std::string& password) = 0;
  virtual void initWithKeys(const WalletAccountKeys& accountKeys, const std::string& password) = 0;
  virtual void shutdown() = 0;
  virtual void reset() = 0;

  virtual void save(std::ostream& destination, bool saveDetailed = true, bool saveCache = true) = 0;

  virtual std::error_code changePassword(const std::string& oldPassword, const std::string& newPassword) = 0;

  virtual std::string getAddress() = 0;

  virtual uint64_t actualBalance() = 0;
  virtual uint64_t pendingBalance() = 0;
  virtual uint64_t actualDepositBalance() = 0;
  virtual uint64_t pendingDepositBalance() = 0;

  virtual size_t getTransactionCount() = 0;
  virtual size_t getTransferCount() = 0;
  virtual size_t getDepositCount() = 0;

  virtual TransactionId findTransactionByTransferId(TransferId transferId) = 0;
  
  virtual bool getTransaction(TransactionId transactionId, TransactionInfo& transaction) = 0;
  virtual bool getTransfer(TransferId transferId, Transfer& transfer) = 0;
  virtual bool getDeposit(DepositId depositId, Deposit& deposit) = 0;
  virtual std::vector<Payments> getTransactionsByPaymentIds(const std::vector<PaymentId>& paymentIds) const = 0;

  virtual TransactionId sendTransaction(const Transfer& transfer, uint64_t fee, const std::string& extra = "", uint64_t mixIn = 0, uint64_t unlockTimestamp = 0, const std::vector<TransactionMessage>& messages = std::vector<TransactionMessage>()) = 0;
  virtual TransactionId sendTransaction(const std::vector<Transfer>& transfers, uint64_t fee, const std::string& extra = "", uint64_t mixIn = 0, uint64_t unlockTimestamp = 0, const std::vector<TransactionMessage>& messages = std::vector<TransactionMessage>()) = 0;
  virtual TransactionId deposit(uint32_t term, uint64_t amount, uint64_t fee, uint64_t mixIn = 0) = 0;
  virtual TransactionId withdrawDeposits(const std::vector<DepositId>& depositIds, uint64_t fee) = 0;
  virtual std::error_code cancelTransaction(size_t transferId) = 0;

  virtual void getAccountKeys(WalletAccountKeys& keys) = 0;
};

}
