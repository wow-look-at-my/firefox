/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FileSystemLocalManagerParent.h"

#include "FileSystemContentTypeGuess.h"
#include "FileSystemLocalService.h"
#include "mozilla/Maybe.h"
#include "mozilla/NotNull.h"
#include "mozilla/Result.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/TaskQueue.h"
#include "mozilla/dom/FileBlobImpl.h"
#include "mozilla/dom/FileSystemHelpers.h"
#include "mozilla/dom/FileSystemLog.h"
#include "mozilla/dom/FileSystemWritableFileStreamParent.h"
#include "mozilla/dom/IPCBlobUtils.h"
#include "mozilla/dom/quota/ForwardDecls.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "mozilla/ipc/RandomAccessStreamUtils.h"
#include "nsIDirectoryEnumerator.h"
#include "nsIFile.h"
#include "nsNetUtil.h"
#include "nsReadableUtils.h"
#include "nsString.h"
#include "nsTArray.h"

using IPCResult = mozilla::ipc::IPCResult;

namespace mozilla::dom {

namespace {

constexpr uint32_t kLocalPageSize = 1024u;

bool ContainsDotDotSegment(const nsACString& aPath) {
  const uint32_t length = aPath.Length();
  uint32_t start = 0;
  for (uint32_t i = 0; i <= length; ++i) {
    if (i == length || aPath.CharAt(i) == kLocalPathSeparatorChar) {
      if (i - start == 2 && aPath.CharAt(start) == '.' &&
          aPath.CharAt(start + 1) == '.') {
        return true;
      }
      start = i + 1;
    }
  }
  return false;
}

// Structural validation of a content-supplied EntryId: it must be a non-empty
// absolute path without any ".." segment. NS_NewLocalFile rejects relative
// paths.
Result<nsCOMPtr<nsIFile>, nsresult> ResolveLocalFile(
    const fs::EntryId& aEntryId) {
  if (aEntryId.IsEmpty() || ContainsDotDotSegment(aEntryId)) {
    return Err(NS_ERROR_DOM_SECURITY_ERR);
  }

  nsCOMPtr<nsIFile> file;
  if (NS_FAILED(NS_NewLocalFile(NS_ConvertUTF8toUTF16(aEntryId),
                                getter_AddRefs(file)))) {
    return Err(NS_ERROR_DOM_SECURITY_ERR);
  }

  return file;
}

// Swap file names are reserved so content cannot address a live swap through
// child-name operations (Chromium reserves them the same way).
bool IsReservedSwapName(const fs::Name& aName) {
  return StringEndsWith(aName, u".crswap"_ns);
}

Result<nsCOMPtr<nsIFile>, nsresult> ResolveLocalChild(
    const fs::EntryId& aParentId, const fs::Name& aName) {
  if (!fs::IsValidName(aName) || IsReservedSwapName(aName)) {
    return Err(NS_ERROR_DOM_TYPE_MISMATCH_ERR);
  }

  auto fileOrErr = ResolveLocalFile(aParentId);
  if (fileOrErr.isErr()) {
    return fileOrErr;
  }
  nsCOMPtr<nsIFile> file = fileOrErr.unwrap();

  if (NS_FAILED(file->Append(aName))) {
    return Err(NS_ERROR_DOM_TYPE_MISMATCH_ERR);
  }

  return file;
}

fs::EntryId IdFromFile(nsIFile* aFile) {
  nsAutoString path;
  MOZ_ALWAYS_SUCCEEDS(aFile->GetPath(path));
  return fs::EntryId(NS_ConvertUTF16toUTF8(path));
}

// Maps raw file nsresults which the child does not translate itself onto the
// DOM error codes the spec wants; NS_ERROR_FILE_NOT_FOUND and
// NS_ERROR_FILE_ACCESS_DENIED pass through because the child maps them.
nsresult TranslateFailure(nsresult aRv) {
  switch (aRv) {
    case NS_ERROR_FILE_DIR_NOT_EMPTY:
    case NS_ERROR_FILE_ALREADY_EXISTS:
      return NS_ERROR_DOM_INVALID_MODIFICATION_ERR;
    case NS_ERROR_FILE_READ_ONLY:
      return NS_ERROR_DOM_NO_MODIFICATION_ALLOWED_ERR;
    case NS_ERROR_FILE_DESTINATION_NOT_DIR:
      return NS_ERROR_DOM_TYPE_MISMATCH_ERR;
    default:
      return aRv;
  }
}

// The filesystem root is the only canonical path ending in the separator, so
// it needs no boundary separator after the prefix.
bool EndsWithSeparator(const nsCString& aPath) {
  return !aPath.IsEmpty() && aPath.Last() == kLocalPathSeparatorChar;
}

bool IsPathPrefix(const nsCString& aAncestor, const nsCString& aDescendant) {
  return aDescendant.Length() > aAncestor.Length() &&
         StringBeginsWith(aDescendant, aAncestor) &&
         (EndsWithSeparator(aAncestor) ||
          aDescendant.CharAt(aAncestor.Length()) == kLocalPathSeparatorChar);
}

fs::FileSystemGetHandleResponse GetOrCreateEntry(
    const fs::FileSystemGetHandleRequest& aRequest, bool aIsDirectory) {
  auto childOrErr = ResolveLocalChild(aRequest.handle().parentId(),
                                      aRequest.handle().childName());
  if (childOrErr.isErr()) {
    return fs::FileSystemGetHandleResponse(childOrErr.unwrapErr());
  }
  nsCOMPtr<nsIFile> child = childOrErr.unwrap();

  bool exists = false;
  nsresult rv = child->Exists(&exists);
  if (NS_FAILED(rv)) {
    return fs::FileSystemGetHandleResponse(TranslateFailure(rv));
  }

  if (exists) {
    bool isDirectory = false;
    rv = child->IsDirectory(&isDirectory);
    if (NS_FAILED(rv)) {
      return fs::FileSystemGetHandleResponse(TranslateFailure(rv));
    }

    if (isDirectory != aIsDirectory) {
      return fs::FileSystemGetHandleResponse(NS_ERROR_DOM_TYPE_MISMATCH_ERR);
    }

    if (!aIsDirectory && aRequest.truncate()) {
      rv = child->SetFileSize(0);
      if (NS_FAILED(rv)) {
        return fs::FileSystemGetHandleResponse(TranslateFailure(rv));
      }
    }

    return fs::FileSystemGetHandleResponse(IdFromFile(child));
  }

  if (!aRequest.create()) {
    return fs::FileSystemGetHandleResponse(NS_ERROR_DOM_NOT_FOUND_ERR);
  }

  rv = child->Create(
      aIsDirectory ? nsIFile::DIRECTORY_TYPE : nsIFile::NORMAL_FILE_TYPE,
      aIsDirectory ? 0755 : 0644);
  if (NS_FAILED(rv)) {
    return fs::FileSystemGetHandleResponse(TranslateFailure(rv));
  }

  return fs::FileSystemGetHandleResponse(IdFromFile(child));
}

// Creates the sibling swap file the writable stream writes into. The plain
// <leaf>.crswap name is preferred; concurrent writables get <leaf>.N.crswap.
// The .crswap extension is load-bearing: GetEntries filters on it.
nsresult CreateSwapFile(nsIFile* aTarget, const nsAString& aTargetLeaf,
                        bool aKeepData, nsIFile** aSwapFile) {
  nsCOMPtr<nsIFile> parent;
  nsresult rv = aTarget->GetParent(getter_AddRefs(parent));
  if (NS_FAILED(rv) || !parent) {
    return NS_FAILED(rv) ? rv : NS_ERROR_FILE_NOT_FOUND;
  }

  for (uint32_t attempt = 0; attempt < 1024; ++attempt) {
    nsAutoString swapLeaf(aTargetLeaf);
    if (attempt > 0) {
      swapLeaf.Append(u'.');
      swapLeaf.AppendInt(attempt);
    }
    swapLeaf.AppendLiteral(".crswap");

    nsCOMPtr<nsIFile> swapFile;
    rv = parent->Clone(getter_AddRefs(swapFile));
    if (NS_FAILED(rv)) {
      return rv;
    }

    rv = swapFile->Append(swapLeaf);
    if (NS_FAILED(rv)) {
      return rv;
    }

    rv = swapFile->Create(nsIFile::NORMAL_FILE_TYPE, 0644);
    if (rv == NS_ERROR_FILE_ALREADY_EXISTS) {
      continue;
    }
    if (NS_FAILED(rv)) {
      return rv;
    }

    if (aKeepData) {
      rv = aTarget->CopyTo(nullptr, swapLeaf);
      if (NS_FAILED(rv)) {
        (void)swapFile->Remove(/* aRecursive */ false);
        return rv;
      }
    }

    swapFile.forget(aSwapFile);
    return NS_OK;
  }

  return NS_ERROR_FILE_ALREADY_EXISTS;
}

fs::FileSystemMoveEntryResponse MoveEntryImpl(nsIFile* aSource,
                                              nsIFile* aDestParent,
                                              const fs::Name& aDestName) {
  if (IsReservedSwapName(aDestName)) {
    return fs::FileSystemMoveEntryResponse(NS_ERROR_DOM_TYPE_MISMATCH_ERR);
  }

  nsCOMPtr<nsIFile> dest;
  if (NS_FAILED(aDestParent->Clone(getter_AddRefs(dest))) ||
      NS_FAILED(dest->Append(aDestName))) {
    return fs::FileSystemMoveEntryResponse(NS_ERROR_DOM_TYPE_MISMATCH_ERR);
  }

  const fs::EntryId srcId = IdFromFile(aSource);
  const fs::EntryId destId = IdFromFile(dest);

  auto& lockTable = FileSystemLocalLockTable::Get();
  if (lockTable.IsAnyLockedUnder(srcId) || lockTable.IsAnyLockedUnder(destId)) {
    return fs::FileSystemMoveEntryResponse(
        NS_ERROR_DOM_NO_MODIFICATION_ALLOWED_ERR);
  }

  bool exists = false;
  nsresult rv = aSource->Exists(&exists);
  if (NS_FAILED(rv) || !exists) {
    return fs::FileSystemMoveEntryResponse(NS_ERROR_DOM_NOT_FOUND_ERR);
  }

  if (srcId == destId) {
    return fs::FileSystemMoveEntryResponse(destId);
  }

  bool sourceIsDirectory = false;
  rv = aSource->IsDirectory(&sourceIsDirectory);
  if (NS_FAILED(rv)) {
    return fs::FileSystemMoveEntryResponse(TranslateFailure(rv));
  }

  if (sourceIsDirectory && IsPathPrefix(srcId, destId)) {
    return fs::FileSystemMoveEntryResponse(
        NS_ERROR_DOM_INVALID_MODIFICATION_ERR);
  }

  bool destExists = false;
  rv = dest->Exists(&destExists);
  if (NS_FAILED(rv)) {
    return fs::FileSystemMoveEntryResponse(TranslateFailure(rv));
  }

  if (destExists) {
    bool destIsDirectory = false;
    rv = dest->IsDirectory(&destIsDirectory);
    if (NS_FAILED(rv)) {
      return fs::FileSystemMoveEntryResponse(TranslateFailure(rv));
    }

    // Only replacing a file with a file is allowed.
    if (destIsDirectory || sourceIsDirectory) {
      return fs::FileSystemMoveEntryResponse(
          NS_ERROR_DOM_INVALID_MODIFICATION_ERR);
    }
  }

  rv = aSource->MoveTo(aDestParent, aDestName);
  if (NS_FAILED(rv)) {
    return fs::FileSystemMoveEntryResponse(TranslateFailure(rv));
  }

  return fs::FileSystemMoveEntryResponse(destId);
}

class EntryNameComparator {
 public:
  bool Equals(const fs::FileSystemEntryMetadata& aLhs,
              const fs::FileSystemEntryMetadata& aRhs) const {
    return aLhs.entryName() == aRhs.entryName();
  }

  bool LessThan(const fs::FileSystemEntryMetadata& aLhs,
                const fs::FileSystemEntryMetadata& aRhs) const {
    return aLhs.entryName() < aRhs.entryName();
  }
};

}  // namespace

FileSystemLocalManagerParent::FileSystemLocalManagerParent(
    RefPtr<TaskQueue> aTaskQueue,
    nsCOMPtr<nsISerialEventTarget> aBackgroundTarget)
    : mTaskQueue(std::move(aTaskQueue)),
      mBackgroundTarget(std::move(aBackgroundTarget)) {}

void FileSystemLocalManagerParent::AssertIsOnIOTarget() const {
  MOZ_ASSERT(mTaskQueue->IsCurrentThreadIn());
}

bool FileSystemLocalManagerParent::IsAlive() const {
  mozilla::ipc::AssertIsOnBackgroundThread();

  return !mClosed;
}

IPCResult FileSystemLocalManagerParent::RecvGetRootHandle(
    GetRootHandleResolver&& aResolver) {
  AssertIsOnIOTarget();

  // Unreachable from the DOM: StorageManager::GetDirectory only ever uses the
  // OPFS manager.
  aResolver(FileSystemGetHandleResponse(NS_ERROR_NOT_IMPLEMENTED));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvGetDirectoryHandle(
    FileSystemGetHandleRequest&& aRequest,
    GetDirectoryHandleResolver&& aResolver) {
  AssertIsOnIOTarget();

  aResolver(GetOrCreateEntry(aRequest, /* aIsDirectory */ true));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvGetFileHandle(
    FileSystemGetHandleRequest&& aRequest, GetFileHandleResolver&& aResolver) {
  AssertIsOnIOTarget();

  aResolver(GetOrCreateEntry(aRequest, /* aIsDirectory */ false));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvGetAccessHandle(
    FileSystemGetAccessHandleRequest&& aRequest,
    GetAccessHandleResolver&& aResolver) {
  AssertIsOnIOTarget();

  // Sync access handles are restricted to OPFS; whatwg/fs and Chromium
  // surface this as InvalidStateError.
  aResolver(FileSystemGetAccessHandleResponse(NS_ERROR_DOM_INVALID_STATE_ERR));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvGetWritable(
    FileSystemGetWritableRequest&& aRequest, GetWritableResolver&& aResolver) {
  AssertIsOnIOTarget();

  auto reject = [&aResolver](nsresult aRv) {
    aResolver(FileSystemGetWritableFileStreamResponse(aRv));
  };

  auto targetOrErr = ResolveLocalFile(aRequest.entryId());
  if (targetOrErr.isErr()) {
    reject(targetOrErr.unwrapErr());
    return IPC_OK();
  }
  nsCOMPtr<nsIFile> target = targetOrErr.unwrap();

  bool exists = false;
  nsresult rv = target->Exists(&exists);
  if (NS_FAILED(rv) || !exists) {
    reject(NS_ERROR_DOM_NOT_FOUND_ERR);
    return IPC_OK();
  }

  bool isDirectory = false;
  rv = target->IsDirectory(&isDirectory);
  if (NS_FAILED(rv) || isDirectory) {
    reject(NS_ERROR_DOM_TYPE_MISMATCH_ERR);
    return IPC_OK();
  }

  const fs::EntryId targetId = IdFromFile(target);

  if (!FileSystemLocalLockTable::Get().LockShared(targetId)) {
    reject(NS_ERROR_DOM_NO_MODIFICATION_ALLOWED_ERR);
    return IPC_OK();
  }

  auto autoUnlock = MakeScopeExit(
      [&targetId] { FileSystemLocalLockTable::Get().UnlockShared(targetId); });

  nsAutoString targetLeaf;
  rv = target->GetLeafName(targetLeaf);
  if (NS_FAILED(rv)) {
    reject(TranslateFailure(rv));
    return IPC_OK();
  }

  // The swap file is created (and populated when keepData is set) before the
  // stream so that stream creation failures cannot leave it behind and the
  // eager open below observes the final contents.
  nsCOMPtr<nsIFile> swapFile;
  rv = CreateSwapFile(target, targetLeaf, aRequest.keepData(),
                      getter_AddRefs(swapFile));
  if (NS_FAILED(rv)) {
    reject(TranslateFailure(rv));
    return IPC_OK();
  }

  auto autoRemoveSwap = MakeScopeExit(
      [&swapFile] { (void)swapFile->Remove(/* aRecursive */ false); });

  // The swap is locked like the target so DOM operations cannot remove or
  // replace it while a stream writes into it.
  const fs::EntryId swapId = IdFromFile(swapFile);
  if (!FileSystemLocalLockTable::Get().LockShared(swapId)) {
    reject(NS_ERROR_DOM_NO_MODIFICATION_ALLOWED_ERR);
    return IPC_OK();
  }

  auto autoUnlockSwap = MakeScopeExit(
      [&swapId] { FileSystemLocalLockTable::Get().UnlockShared(swapId); });

  if (LOG_ENABLED()) {
    LOG(("Opening local Writable %s", swapId.get()));
  }

  // Eagerly opened on purpose: a deferred open would ship an invalid file
  // descriptor to the child on failure instead of rejecting here.
  nsCOMPtr<nsIRandomAccessStream> stream;
  rv = NS_NewLocalFileRandomAccessStream(getter_AddRefs(stream), swapFile,
                                         /* ioFlags */ -1, 0644,
                                         /* behaviorFlags */ 0);
  if (NS_FAILED(rv)) {
    reject(TranslateFailure(rv));
    return IPC_OK();
  }

  RandomAccessStreamParams streamParams =
      mozilla::ipc::SerializeRandomAccessStream(
          WrapMovingNotNullUnchecked(std::move(stream)), nullptr);

  auto writableFileStreamParent =
      MakeNotNull<RefPtr<FileSystemWritableFileStreamParent>>(
          this, targetId, fs::FileId(swapId),
          /* aIsExclusive */ false);

  // From here the actor owns the cleanup: a failed constructor send destroys
  // it and its ActorDestroy runs the abort path of OnWritableStreamClosed.
  autoRemoveSwap.release();
  autoUnlockSwap.release();
  autoUnlock.release();

  if (!SendPFileSystemWritableFileStreamConstructor(writableFileStreamParent)) {
    aResolver(FileSystemGetWritableFileStreamResponse(NS_ERROR_FAILURE));
    return IPC_OK();
  }

  aResolver(FileSystemWritableFileStreamProperties(std::move(streamParams),
                                                   writableFileStreamParent));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvGetFile(
    FileSystemGetFileRequest&& aRequest, GetFileResolver&& aResolver) {
  AssertIsOnIOTarget();

  auto reject = [&aResolver](nsresult aRv) {
    aResolver(FileSystemGetFileResponse(aRv));
  };

  auto fileOrErr = ResolveLocalFile(aRequest.entryId());
  if (fileOrErr.isErr()) {
    reject(fileOrErr.unwrapErr());
    return IPC_OK();
  }
  nsCOMPtr<nsIFile> file = fileOrErr.unwrap();

  bool exists = false;
  nsresult rv = file->Exists(&exists);
  if (NS_FAILED(rv) || !exists) {
    reject(NS_ERROR_DOM_NOT_FOUND_ERR);
    return IPC_OK();
  }

  bool isDirectory = false;
  rv = file->IsDirectory(&isDirectory);
  if (NS_FAILED(rv) || isDirectory) {
    reject(NS_ERROR_DOM_TYPE_MISMATCH_ERR);
    return IPC_OK();
  }

  PRTime lastModifiedMilliSeconds = 0;
  rv = file->GetLastModifiedTime(&lastModifiedMilliSeconds);
  if (NS_FAILED(rv)) {
    reject(TranslateFailure(rv));
    return IPC_OK();
  }

  nsAutoString leafName;
  rv = file->GetLeafName(leafName);
  if (NS_FAILED(rv)) {
    reject(TranslateFailure(rv));
    return IPC_OK();
  }

  fs::ContentType type;
  auto typeOrErr = fs::FileSystemContentTypeGuess::FromPath(leafName);
  if (typeOrErr.isOk()) {
    type = typeOrErr.unwrap();
  }

  // A fresh FileBlobImpl per request keeps getFile() snapshots reflecting the
  // current on-disk state.
  RefPtr<BlobImpl> blob =
      MakeRefPtr<FileBlobImpl>(file, leafName, NS_ConvertUTF8toUTF16(type));

  IPCBlob ipcBlob;
  rv = IPCBlobUtils::Serialize(blob, ipcBlob);
  if (NS_FAILED(rv)) {
    reject(rv);
    return IPC_OK();
  }

  fs::Path path;
  path.AppendElement(leafName);

  aResolver(FileSystemGetFileResponse(
      FileSystemFileProperties(lastModifiedMilliSeconds, ipcBlob, type, path)));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvResolve(
    FileSystemResolveRequest&& aRequest, ResolveResolver&& aResolver) {
  AssertIsOnIOTarget();

  auto reject = [&aResolver](nsresult aRv) {
    aResolver(FileSystemResolveResponse(aRv));
  };

  auto parentOrErr = ResolveLocalFile(aRequest.endpoints().parentId());
  if (parentOrErr.isErr()) {
    reject(parentOrErr.unwrapErr());
    return IPC_OK();
  }

  auto childOrErr = ResolveLocalFile(aRequest.endpoints().childId());
  if (childOrErr.isErr()) {
    reject(childOrErr.unwrapErr());
    return IPC_OK();
  }

  const nsCOMPtr<nsIFile> parentFile = parentOrErr.unwrap();
  const nsCOMPtr<nsIFile> childFile = childOrErr.unwrap();
  const fs::EntryId parentId = IdFromFile(parentFile);
  const fs::EntryId childId = IdFromFile(childFile);

  if (parentId == childId) {
    aResolver(FileSystemResolveResponse(Some(FileSystemPath(fs::Path()))));
    return IPC_OK();
  }

  if (!IsPathPrefix(parentId, childId)) {
    aResolver(FileSystemResolveResponse(Nothing()));
    return IPC_OK();
  }

  fs::Path path;
  uint32_t start =
      EndsWithSeparator(parentId) ? parentId.Length() : parentId.Length() + 1;
  const uint32_t length = childId.Length();
  for (uint32_t i = start; i <= length; ++i) {
    if (i == length || childId.CharAt(i) == kLocalPathSeparatorChar) {
      path.AppendElement(
          NS_ConvertUTF8toUTF16(Substring(childId, start, i - start)));
      start = i + 1;
    }
  }

  aResolver(FileSystemResolveResponse(Some(FileSystemPath(path))));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvGetEntries(
    FileSystemGetEntriesRequest&& aRequest, GetEntriesResolver&& aResolver) {
  AssertIsOnIOTarget();

  auto reject = [&aResolver](nsresult aRv) {
    aResolver(FileSystemGetEntriesResponse(aRv));
  };

  if (aRequest.page() < 0) {
    reject(NS_ERROR_INVALID_ARG);
    return IPC_OK();
  }

  auto dirOrErr = ResolveLocalFile(aRequest.parentId());
  if (dirOrErr.isErr()) {
    reject(dirOrErr.unwrapErr());
    return IPC_OK();
  }
  nsCOMPtr<nsIFile> dir = dirOrErr.unwrap();

  nsCOMPtr<nsIDirectoryEnumerator> entries;
  nsresult rv = dir->GetDirectoryEntries(getter_AddRefs(entries));
  if (NS_FAILED(rv)) {
    reject(NS_ERROR_DOM_NOT_FOUND_ERR);
    return IPC_OK();
  }

  // The directory is re-enumerated for every page, so entries are sorted by
  // name to give requests a stable order to page over.
  nsTArray<FileSystemEntryMetadata> allEntries;

  nsCOMPtr<nsIFile> child;
  while (NS_SUCCEEDED(entries->GetNextFile(getter_AddRefs(child))) && child) {
    nsAutoString leafName;
    if (NS_FAILED(child->GetLeafName(leafName))) {
      continue;
    }

    bool isDirectory = false;
    if (NS_FAILED(child->IsDirectory(&isDirectory))) {
      isDirectory = false;
    }

    // Swap files are hidden from listings; a user directory that happens to
    // carry the suffix is not.
    if (!isDirectory && StringEndsWith(leafName, u".crswap"_ns)) {
      continue;
    }

    allEntries.AppendElement(
        FileSystemEntryMetadata(IdFromFile(child), leafName, isDirectory));
  }

  allEntries.Sort(EntryNameComparator());

  const uint64_t begin =
      uint64_t(static_cast<uint32_t>(aRequest.page())) * kLocalPageSize;
  const uint64_t end =
      std::min<uint64_t>(begin + kLocalPageSize, allEntries.Length());

  FileSystemDirectoryListing listing;
  for (uint64_t i = begin; i < end; ++i) {
    FileSystemEntryMetadata& entry = allEntries[i];
    if (entry.directory()) {
      listing.directories().AppendElement(std::move(entry));
    } else {
      listing.files().AppendElement(std::move(entry));
    }
  }

  aResolver(FileSystemGetEntriesResponse(std::move(listing)));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvRemoveEntry(
    FileSystemRemoveEntryRequest&& aRequest, RemoveEntryResolver&& aResolver) {
  AssertIsOnIOTarget();

  auto reject = [&aResolver](nsresult aRv) {
    aResolver(FileSystemRemoveEntryResponse(aRv));
  };

  auto targetOrErr = ResolveLocalChild(aRequest.handle().parentId(),
                                       aRequest.handle().childName());
  if (targetOrErr.isErr()) {
    reject(targetOrErr.unwrapErr());
    return IPC_OK();
  }
  nsCOMPtr<nsIFile> target = targetOrErr.unwrap();

  if (FileSystemLocalLockTable::Get().IsAnyLockedUnder(IdFromFile(target))) {
    reject(NS_ERROR_DOM_NO_MODIFICATION_ALLOWED_ERR);
    return IPC_OK();
  }

  bool exists = false;
  nsresult rv = target->Exists(&exists);
  if (NS_FAILED(rv) || !exists) {
    reject(NS_ERROR_DOM_NOT_FOUND_ERR);
    return IPC_OK();
  }

  rv = target->Remove(aRequest.recursive());
  if (NS_FAILED(rv)) {
    reject(TranslateFailure(rv));
    return IPC_OK();
  }

  aResolver(FileSystemRemoveEntryResponse(void_t{}));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvMoveEntry(
    FileSystemMoveEntryRequest&& aRequest, MoveEntryResolver&& aResolver) {
  AssertIsOnIOTarget();

  auto reject = [&aResolver](nsresult aRv) {
    aResolver(FileSystemMoveEntryResponse(aRv));
  };

  auto sourceOrErr = ResolveLocalFile(aRequest.handle().entryId());
  if (sourceOrErr.isErr()) {
    reject(sourceOrErr.unwrapErr());
    return IPC_OK();
  }
  nsCOMPtr<nsIFile> source = sourceOrErr.unwrap();

  if (!fs::IsValidName(aRequest.destHandle().childName())) {
    reject(NS_ERROR_DOM_TYPE_MISMATCH_ERR);
    return IPC_OK();
  }

  auto destParentOrErr = ResolveLocalFile(aRequest.destHandle().parentId());
  if (destParentOrErr.isErr()) {
    reject(destParentOrErr.unwrapErr());
    return IPC_OK();
  }
  nsCOMPtr<nsIFile> destParent = destParentOrErr.unwrap();

  aResolver(
      MoveEntryImpl(source, destParent, aRequest.destHandle().childName()));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvRenameEntry(
    FileSystemRenameEntryRequest&& aRequest, MoveEntryResolver&& aResolver) {
  AssertIsOnIOTarget();

  auto reject = [&aResolver](nsresult aRv) {
    aResolver(FileSystemMoveEntryResponse(aRv));
  };

  auto sourceOrErr = ResolveLocalFile(aRequest.handle().entryId());
  if (sourceOrErr.isErr()) {
    reject(sourceOrErr.unwrapErr());
    return IPC_OK();
  }
  nsCOMPtr<nsIFile> source = sourceOrErr.unwrap();

  if (!fs::IsValidName(aRequest.name())) {
    reject(NS_ERROR_DOM_TYPE_MISMATCH_ERR);
    return IPC_OK();
  }

  nsCOMPtr<nsIFile> destParent;
  nsresult rv = source->GetParent(getter_AddRefs(destParent));
  if (NS_FAILED(rv) || !destParent) {
    reject(NS_ERROR_DOM_INVALID_MODIFICATION_ERR);
    return IPC_OK();
  }

  aResolver(MoveEntryImpl(source, destParent, aRequest.name()));

  return IPC_OK();
}

void FileSystemLocalManagerParent::OnWritableStreamClosed(
    const fs::EntryId& aEntryId, const fs::FileId& aTemporaryFileId,
    bool aIsExclusive, bool aAbort) {
  AssertIsOnIOTarget();
  MOZ_ASSERT(!aIsExclusive);

  auto autoUnlock = MakeScopeExit(
      [&aEntryId] { FileSystemLocalLockTable::Get().UnlockShared(aEntryId); });
  auto autoUnlockSwap = MakeScopeExit([&aTemporaryFileId] {
    FileSystemLocalLockTable::Get().UnlockShared(
        fs::EntryId(aTemporaryFileId.Value()));
  });

  auto swapOrErr = ResolveLocalFile(fs::EntryId(aTemporaryFileId.Value()));
  if (NS_WARN_IF(swapOrErr.isErr())) {
    return;
  }
  nsCOMPtr<nsIFile> swapFile = swapOrErr.unwrap();

  if (aAbort) {
    (void)NS_WARN_IF(NS_FAILED(swapFile->Remove(/* aRecursive */ false)));
    return;
  }

  auto targetOrErr = ResolveLocalFile(aEntryId);
  if (NS_WARN_IF(targetOrErr.isErr())) {
    (void)swapFile->Remove(/* aRecursive */ false);
    return;
  }

  nsAutoString targetLeaf;
  if (NS_WARN_IF(NS_FAILED(targetOrErr.unwrap()->GetLeafName(targetLeaf)))) {
    (void)swapFile->Remove(/* aRecursive */ false);
    return;
  }

  // Commit: atomic same-directory rename of the swap file over the target.
  // The protocol carries no close error, so failures only warn (OPFS parity).
  nsresult rv = swapFile->MoveTo(nullptr, targetLeaf);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    (void)swapFile->Remove(/* aRecursive */ false);
  }
}

void FileSystemLocalManagerParent::RequestAllowToClose() {
  mozilla::ipc::AssertIsOnBackgroundThread();

  if (mLocalRequestedAllowToClose) {
    return;
  }

  mLocalRequestedAllowToClose.Flip();

  InvokeAsync(mTaskQueue, __func__,
              [self = RefPtr<FileSystemLocalManagerParent>(this)]() {
                return self->SendCloseAll();
              })
      ->Then(mTaskQueue, __func__,
             [self = RefPtr<FileSystemLocalManagerParent>(this)](
                 const CloseAllPromise::ResolveOrRejectValue& aValue) {
               self->Close();

               return BoolPromise::CreateAndResolve(true, __func__);
             });
}

void FileSystemLocalManagerParent::ActorDestroy(ActorDestroyReason aWhy) {
  AssertIsOnIOTarget();

  InvokeAsync(
      mBackgroundTarget, __func__,
      [self = RefPtr<FileSystemLocalManagerParent>(this)]() {
        self->mClosed = true;

        if (FileSystemLocalService* service = FileSystemLocalService::Get()) {
          service->Unregister(self);
        }

        self->mTaskQueue->BeginShutdown();

        return BoolPromise::CreateAndResolve(true, __func__);
      });
}

}  // namespace mozilla::dom
