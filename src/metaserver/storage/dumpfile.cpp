/*
 *  Copyright (c) 2021 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/**
 * Project: Dingofs
 * Created Date: 2021-07-23
 * Author: Jingli Chen (Wine93)
 */

#include "metaserver/storage/dumpfile.h"

#include <fcntl.h>
#include <glog/logging.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/process.h"
#include "metaserver/storage/iterator.h"
#include "utils/crc32.h"
#include "utils/timeutility.h"
#include "fs/ext4_filesystem_impl.h"

namespace dingofs {
namespace metaserver {
namespace storage {

#define RETURN_IF_UNSUCCESS(statement) \
  do {                                 \
    auto rc = statement;               \
    if (rc != DUMPFILE_ERROR::OK) {    \
      return rc;                       \
    }                                  \
  } while (0)

#define EXIT_LOAD_IF_UNEXPECT(unexpect, status) \
  do {                                          \
    if ((unexpect) == true) {                   \
      isValid_ = false;                         \
      dumpfile_->SetLoadStatus(status);         \
      return;                                   \
    }                                           \
  } while (0)

#define ERR2STR(code) {DUMPFILE_ERROR::code, #code},
#define STATUS2STR(code) {DUMPFILE_LOAD_STATUS::code, #code},

using ::dingofs::utils::CRC32;

const std::string DumpFile::kDingoFs_ = "DINGOFS";  // NOLINT
const uint32_t DumpFile::kEOF_ = 0;
const uint8_t DumpFile::kVersion_ = kDumpFileV3;

const uint32_t DumpFile::kMaxStringLength_ = 1024 * 1024 * 1024;  // 1GB

std::ostream& operator<<(std::ostream& os, DUMPFILE_ERROR code) {
  static auto code2str = std::map<DUMPFILE_ERROR, std::string>{
      ERR2STR(OK) ERR2STR(FAILED) ERR2STR(BAD_FD) ERR2STR(READ_FAILED)
          ERR2STR(WRITE_FAILED) ERR2STR(CLOSE_FAILED) ERR2STR(LIST_FAILED)
              ERR2STR(FSTAT_FAILED) ERR2STR(EXCEED_MAX_STRING_LENGTH)
                  ERR2STR(SYNC_FAILED) ERR2STR(FORK_FAILED)
                      ERR2STR(WAITPID_FAILED) ERR2STR(UNEXPECTED_SIGNAL)
                          ERR2STR(ENCOUNTER_EOF)};

  auto iter = code2str.find(code);
  if (iter != code2str.end()) {
    os << iter->second;
  } else {
    os << "UNKNOWN_ERROR";
  }

  return os;
}

std::ostream& operator<<(std::ostream& os, DUMPFILE_LOAD_STATUS code) {
  static auto code2str = std::map<DUMPFILE_LOAD_STATUS, std::string>{
      STATUS2STR(COMPLETE) STATUS2STR(INCOMPLETE) STATUS2STR(INVALID_FILE)
          STATUS2STR(INVALID_MAGIC) STATUS2STR(INVALID_VERSION)
              STATUS2STR(INVALID_SIZE) STATUS2STR(INVALID_PAIRS)
                  STATUS2STR(INVALID_CHECKSUM)};

  auto iter = code2str.find(code);
  if (iter != code2str.end()) {
    os << iter->second;
  } else {
    os << "UNKNOWN_STATUS";
  }

  return os;
}

bool DumpFile::CheckDumpFileVersion(uint8_t ver) {
  return ver >= kDumpFileV1 && ver <= kVersion_;
}

DumpFile::DumpFile(const std::string& pathname)
    : pathname_(pathname),
      fd_(-1),
      fs_(Ext4FileSystemImpl::getInstance()),
      loadStatus_(DUMPFILE_LOAD_STATUS::INCOMPLETE),
      version_(kVersion_) {}

DumpFile::DumpFile(const std::string& pathname, uint8_t version)
    : pathname_(pathname),
      fd_(-1),
      fs_(Ext4FileSystemImpl::getInstance()),
      loadStatus_(DUMPFILE_LOAD_STATUS::INCOMPLETE),
      version_(version) {}

DUMPFILE_ERROR DumpFile::Open() {
  if (fd_ >= 0) {
    return DUMPFILE_ERROR::OK;
  }

  auto retCode = fs_->Open(pathname_, O_RDWR | O_CREAT);
  if (retCode < 0) {
    LOG(ERROR) << "Failed to open file " << pathname_
               << ", retCode = " << retCode;
    return DUMPFILE_ERROR::FAILED;
  }

  fd_ = retCode;
  return DUMPFILE_ERROR::OK;
}

DUMPFILE_ERROR DumpFile::Close() {
  if (fd_ < 0) {
    return DUMPFILE_ERROR::OK;
  }

  auto retCode = fs_->Close(fd_);
  if (retCode < 0) {
    LOG(ERROR) << "Failed to close file " << pathname_
               << ", retCode = " << retCode;
    return DUMPFILE_ERROR::FAILED;
  }

  fd_ = -1;
  return DUMPFILE_ERROR::OK;
}

DUMPFILE_ERROR DumpFile::Write(const char* buffer, off_t offset,
                               size_t length) {
  auto ret = fs_->Write(fd_, buffer, offset, length);
  if (ret < 0) {
    LOG(ERROR) << "Write file failed, retCode = " << ret;
    return DUMPFILE_ERROR::WRITE_FAILED;
  } else if (ret != static_cast<int>(length)) {
    LOG(ERROR) << "Write file failed, expect write " << length
               << " bytes, actual write " << ret << " bytes";
    return DUMPFILE_ERROR::WRITE_FAILED;
  }

  return DUMPFILE_ERROR::OK;
}

DUMPFILE_ERROR DumpFile::Read(char* buffer, off_t offset, size_t length) {
  auto ret = fs_->Read(fd_, buffer, offset, length);
  if (ret < 0) {
    LOG(ERROR) << "Read file failed, retCode = " << ret;
    return DUMPFILE_ERROR::READ_FAILED;
  } else if (ret != static_cast<int>(length)) {
    LOG(ERROR) << "Read file failed, expect read " << length
               << " bytes, actual read " << ret << " bytes";
    return DUMPFILE_ERROR::READ_FAILED;
  }

  return DUMPFILE_ERROR::OK;
}

template <typename Int>
DUMPFILE_ERROR DumpFile::SaveInt(Int num, off_t* offset, uint32_t* checkSum) {
  size_t length = sizeof(Int);
  std::unique_ptr<char[]> writeBuffer(new (std::nothrow) char[length]);

  memcpy(writeBuffer.get(), reinterpret_cast<char*>(&num), length);
  auto retCode = Write(writeBuffer.get(), *offset, length);
  if (retCode == DUMPFILE_ERROR::OK) {
    *offset = (*offset) + length;
    *checkSum = CRC32(*checkSum, writeBuffer.get(), length);
  }

  return retCode;
}

DUMPFILE_ERROR DumpFile::SaveString(const std::string& str, off_t* offset,
                                    uint32_t* checkSum) {
  size_t length = str.size();
  auto retCode = Write(str.c_str(), *offset, length);
  if (retCode == DUMPFILE_ERROR::OK) {
    *offset = (*offset) + length;
    *checkSum = CRC32(*checkSum, str.c_str(), length);
  }

  return retCode;
}

DUMPFILE_ERROR DumpFile::SaveEntry(const std::string& entry, off_t* offset,
                                   uint32_t* checkSum) {
  auto retCode = SaveInt<uint32_t>(entry.size(), offset, checkSum);
  if (retCode == DUMPFILE_ERROR::OK) {
    retCode = SaveString(entry, offset, checkSum);
  }

  return retCode;
}

template <typename Int>
DUMPFILE_ERROR DumpFile::LoadInt(Int* num, off_t* offset, uint32_t* checkSum) {
  size_t length = sizeof(Int);
  std::unique_ptr<char[]> readBuffer(new (std::nothrow) char[length]);

  auto retCode = Read(readBuffer.get(), *offset, length);
  if (retCode == DUMPFILE_ERROR::OK) {
    *num = *reinterpret_cast<Int*>(readBuffer.get());
    *offset = (*offset) + length;
    *checkSum = CRC32(*checkSum, readBuffer.get(), length);
  }

  return retCode;
}

DUMPFILE_ERROR DumpFile::LoadString(std::string* str, off_t* offset,
                                    size_t length, uint32_t* checkSum) {
  if (length > kMaxStringLength_) {
    LOG(ERROR) << "The loaded string is too large, size(" << length
               << ") > limit(" << kMaxStringLength_ << ")";
    return DUMPFILE_ERROR::EXCEED_MAX_STRING_LENGTH;
  }

  std::unique_ptr<char[]> readBuffer(new (std::nothrow) char[length]);
  auto retCode = Read(readBuffer.get(), *offset, length);
  if (retCode == DUMPFILE_ERROR::OK) {
    *str = std::string(readBuffer.get(), length);  // Ensure binary safe
    *offset = (*offset) + length;
    *checkSum = CRC32(*checkSum, readBuffer.get(), length);
  }

  return retCode;
}

DUMPFILE_ERROR DumpFile::LoadEntry(std::string* entry, off_t* offset,
                                   uint32_t* checkSum) {
  uint32_t length;
  auto retCode = LoadInt<uint32_t>(&length, offset, checkSum);
  if (retCode != DUMPFILE_ERROR::OK) {
    return retCode;
  }

  // retCode == DUMPFILE_ERROR::OK
  if (version_ > kDumpFileV1 && length == kEOF_) {
    return DUMPFILE_ERROR::ENCOUNTER_EOF;
  }
  return LoadString(entry, offset, length, checkSum);
}

DUMPFILE_ERROR DumpFile::Save(std::shared_ptr<Iterator> iter) {
  if (fd_ < 0) {
    return DUMPFILE_ERROR::BAD_FD;
  }

  off_t offset = 0;
  uint32_t checkSum = 0;

  // Step1: save magic, version, size (only v1)
  RETURN_IF_UNSUCCESS(SaveString(kDingoFs_, &offset, &checkSum));
  RETURN_IF_UNSUCCESS(SaveInt<uint8_t>(version_, &offset, &checkSum));
  if (version_ == kDumpFileV1) {
    RETURN_IF_UNSUCCESS(SaveInt<uint64_t>(iter->Size(), &offset, &checkSum));
  }

  // Step2: save key-value pairs
  uint64_t nPairs = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    auto key = iter->Key();
    auto value = iter->Value();

    RETURN_IF_UNSUCCESS(SaveEntry(key, &offset, &checkSum));
    RETURN_IF_UNSUCCESS(SaveEntry(value, &offset, &checkSum));
    nPairs++;
  }

  // Step3: save EOF (only v2 and v3)
  if (version_ > kDumpFileV1) {
    RETURN_IF_UNSUCCESS(SaveInt<uint32_t>(kEOF_, &offset, &checkSum));
  }

  // Step4: save checksum
  uint32_t realCheckSum = checkSum;
  RETURN_IF_UNSUCCESS(SaveInt<uint32_t>(checkSum, &offset, &checkSum));

  // Step5: sync
  auto retCode = fs_->Fsync(fd_);
  if (retCode != 0) {
    LOG(ERROR) << "Sync data to disk failed, retCode = " << retCode;
    return DUMPFILE_ERROR::SYNC_FAILED;
  }

  LOG(INFO) << "Save success, version = " << std::to_string(version_)
            << ", iterator size = "
            << (version_ == kDumpFileV1 ? std::to_string(iter->Size())
                                        : "unknown")
            << ", number of saved entrys = " << nPairs
            << ", checksum = " << realCheckSum;
  return DUMPFILE_ERROR::OK;
}

DUMPFILE_ERROR DumpFile::WaitSaveDone(pid_t childpid) {
  int status;
  if (waitpid(childpid, &status, WUNTRACED) == -1) {
    LOG(ERROR) << "[child] Unexpected waitpid() failure";
    return DUMPFILE_ERROR::WAITPID_FAILED;
  }

  if (WIFEXITED(status)) {
    LOG(INFO) << "[child] Save process (pid=" << childpid
              << ") exit, exitCode = " << WEXITSTATUS(status);
    return WEXITSTATUS(status) == 0 ? DUMPFILE_ERROR::OK
                                    : DUMPFILE_ERROR::FAILED;
  }

  int signal = -1;
  if (WIFSIGNALED(status)) {
    signal = WTERMSIG(status);
  } else if (WIFSTOPPED(status)) {
    signal = WSTOPSIG(status);
    if (kill(childpid, SIGKILL) == -1) {
      LOG(ERROR) << "kill (pid=" << childpid << ") failed";
    }
  }

  LOG(ERROR) << "[child] Save process (pid=" << childpid
             << ") exited by receive signal, signal = " << signal;

  return DUMPFILE_ERROR::UNEXPECTED_SIGNAL;
}

void DumpFile::SignalHandler(int signo, siginfo_t* siginfo, void* ucontext) {
  (void)ucontext;
  auto pid = (siginfo && siginfo->si_pid) ? siginfo->si_pid : -1;
  LOG(INFO) << "Signal " << signo << " received from " << pid;
  _exit(2);
}

DUMPFILE_ERROR DumpFile::InitSignals() {
  // The BRPC has install the SIGINT and SIGTERM, so we should
  // catch them and to avoid release resources repeatedly in child process
  std::vector<::dingofs::common::Signal> signals{
      {SIGINT, SignalHandler},
      {SIGTERM, SignalHandler},
  };

  if (!::dingofs::common::Process::InitSignals(signals)) {
    LOG(ERROR) << "Init signals failed";
    return DUMPFILE_ERROR::FAILED;
  }

  return DUMPFILE_ERROR::OK;
}

DUMPFILE_ERROR DumpFile::CloseSockets() {
  std::vector<std::string> names;
  if (fs_->List("/proc/self/fd", &names) != 0) {
    return DUMPFILE_ERROR::LIST_FAILED;
  }

  auto nFail = 0;
  struct stat info;
  for (const auto& name : names) {
    int fd = std::stoi(name);
    if (fs_->Fstat(fd, &info) != 0) {
      nFail++;
    } else if (S_ISSOCK(info.st_mode) && fs_->Close(fd) != 0) {
      return DUMPFILE_ERROR::CLOSE_FAILED;
    }
  }

  // There is one small pitfall to what out for:
  // the /proc/self/fd directory itself also is
  // an open file descriptor (but fstast() will fail)
  // while scanning it
  return nFail > 1 ? DUMPFILE_ERROR::FSTAT_FAILED : DUMPFILE_ERROR::OK;
}

void DumpFile::SaveWorker(std::shared_ptr<Iterator> iter) {
  // If we use multi-raft, there maybe multi process to do save
  // at the same time, so we should to distinguish them
  auto title = "dingofs: save process [filepath: " + pathname_ + "]";
  ::dingofs::common::Process::SetProcTitle(title);

  auto retCode = InitSignals();
  if (retCode != DUMPFILE_ERROR::OK) {
    _exit(1);
  }

  // We should close all socket fds in child process
  // to ensure free connection success
  retCode = CloseSockets();
  if (retCode != DUMPFILE_ERROR::OK) {
    LOG(ERROR) << "[child] Close socket fds failed"
               << ", retCode = " << retCode;
    _exit(1);
  }

  // We should ensure the child process exit when the parent exit
  prctl(PR_SET_PDEATHSIG, SIGKILL);

  retCode = Save(iter);
  auto succ = (retCode == DUMPFILE_ERROR::OK);
  LOG(INFO) << "[child] Save " << (succ ? "success" : "fail")
            << ", retCode = " << retCode;
  google::ShutdownGoogleLogging();  // Flush message to file
  _exit(succ ? 0 : 1);
}

DUMPFILE_ERROR DumpFile::SaveBackground(std::shared_ptr<Iterator> iter,
                                        DumpFileClosure* done) {
  if (fd_ < 0) {
    if (done != nullptr) {
      done->Runned();
    }
    return DUMPFILE_ERROR::BAD_FD;
  }

  auto startTime = ::dingofs::utils::TimeUtility::GetTimeofDayMs();
  auto proc = [this, iter]() { SaveWorker(iter); };
  pid_t childpid = ::dingofs::common::Process::SpawnProcess(proc);
  if (done != nullptr) {  // child process forked
    done->Runned();
  }
  if (childpid == -1) {
    return DUMPFILE_ERROR::FORK_FAILED;
  }

  auto retCode = WaitSaveDone(childpid);

  auto endTime = ::dingofs::utils::TimeUtility::GetTimeofDayMs();
  double elapsed = (endTime - startTime) * 1.0 / 1000;
  LOG(INFO) << "Save background "
            << (retCode == DUMPFILE_ERROR::OK ? "success" : "fail")
            << ", retCode = " << retCode << ", cost " << elapsed << " seconds";

  return retCode;
}

std::shared_ptr<DumpFileIterator> DumpFile::Load() {
  return std::make_shared<DumpFileIterator>(this);
}

DUMPFILE_LOAD_STATUS DumpFile::GetLoadStatus() { return loadStatus_; }

void DumpFile::SetLoadStatus(DUMPFILE_LOAD_STATUS status) {
  loadStatus_ = status;
}

uint8_t DumpFile::GetVersion() { return version_; }

DumpFileIterator::DumpFileIterator(DumpFile* dumpfile)
    : offset_(0),
      checkSum_(0),
      nPairs_(0),
      size_(0),
      isValid_(false),
      startTime_(::dingofs::utils::TimeUtility::GetTimeofDayMs()),
      dumpfile_(dumpfile) {}

bool DumpFileIterator::Valid() { return isValid_; }

uint64_t DumpFileIterator::Size() { return size_; }

void DumpFileIterator::SeekToFirst() {
  EXIT_LOAD_IF_UNEXPECT(dumpfile_->fd_ < 0, DUMPFILE_LOAD_STATUS::INVALID_FILE);

  // magic
  std::string magic;
  auto magicStr = dumpfile_->kDingoFs_;
  auto retCode =
      dumpfile_->LoadString(&magic, &offset_, magicStr.size(), &checkSum_);
  EXIT_LOAD_IF_UNEXPECT(retCode != DUMPFILE_ERROR::OK || magic != magicStr,
                        DUMPFILE_LOAD_STATUS::INVALID_MAGIC);

  // version
  uint8_t version;
  retCode = dumpfile_->LoadInt<uint8_t>(&version, &offset_, &checkSum_);
  EXIT_LOAD_IF_UNEXPECT(
      retCode != DUMPFILE_ERROR::OK || !DumpFile::CheckDumpFileVersion(version),
      DUMPFILE_LOAD_STATUS::INVALID_VERSION);
  dumpfile_->version_ = version;

  version_ = version;

  // size
  uint64_t size = 0;
  if (dumpfile_->GetVersion() == kDumpFileV1) {
    retCode = dumpfile_->LoadInt<uint64_t>(&size, &offset_, &checkSum_);
    EXIT_LOAD_IF_UNEXPECT(retCode != DUMPFILE_ERROR::OK,
                          DUMPFILE_LOAD_STATUS::INVALID_SIZE);
  }

  size_ = size;
  isValid_ = true;
  Next();
}

void DumpFileIterator::End() {
  uint32_t crc4load;
  uint32_t crc4calc = checkSum_;

  auto retCode = dumpfile_->LoadInt<uint32_t>(&crc4load, &offset_, &checkSum_);
  auto succ = (retCode == DUMPFILE_ERROR::OK) && (crc4load == crc4calc);
  auto status = succ ? DUMPFILE_LOAD_STATUS::COMPLETE
                     : DUMPFILE_LOAD_STATUS::INVALID_CHECKSUM;

  auto endTime = ::dingofs::utils::TimeUtility::GetTimeofDayMs();
  double elapsed = (endTime - startTime_) * 1.0 / 1000;
  uint8_t version = dumpfile_->GetVersion();

  std::stringstream ss;
  ss << "Load " << (succ ? "success" : "fail") << ", loadStatus = " << status
     << ", cost " << elapsed << " seconds"
     << ", version = " << std::to_string(version) << ", loaded size = "
     << (version == kDumpFileV1 ? std::to_string(size_) : "unknown")
     << ", number of loaded entrys = " << nPairs_
     << ", loaded checksum = " << crc4load
     << ", calculate checksum = " << crc4calc;

  if (succ) {
    LOG(INFO) << ss.str();
  } else {
    LOG(ERROR) << ss.str();
  }

  EXIT_LOAD_IF_UNEXPECT(true, status);
}

void DumpFileIterator::Next() {
  if (!isValid_) {
    return;
  } else if (dumpfile_->version_ == 1 && nPairs_ == size_) {
    return End();
  }

  std::string key, value;
  DUMPFILE_ERROR retCode;
  DUMPFILE_LOAD_STATUS status = DUMPFILE_LOAD_STATUS::INVALID_PAIRS;
  retCode = dumpfile_->LoadEntry(&key, &offset_, &checkSum_);
  if (dumpfile_->version_ > kDumpFileV2 &&
      retCode == DUMPFILE_ERROR::ENCOUNTER_EOF) {
    return End();
  }
  EXIT_LOAD_IF_UNEXPECT(retCode != DUMPFILE_ERROR::OK, status);
  retCode = dumpfile_->LoadEntry(&value, &offset_, &checkSum_);
  EXIT_LOAD_IF_UNEXPECT(retCode != DUMPFILE_ERROR::OK, status);

  // load key-value pair success
  nPairs_++;
  isValid_ = true;
  iter_.first = key;
  iter_.second = value;
}

std::string DumpFileIterator::Key() { return iter_.first; }

std::string DumpFileIterator::Value() { return iter_.second; }

bool DumpFileIterator::ParseFromValue(ValueType* /*value*/) { return true; }

int DumpFileIterator::Status() { return 0; }

}  // namespace storage
}  // namespace metaserver
}  // namespace dingofs
