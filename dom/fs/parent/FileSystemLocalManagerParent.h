/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_FS_PARENT_FILESYSTEMLOCALMANAGERPARENT_H_
#define DOM_FS_PARENT_FILESYSTEMLOCALMANAGERPARENT_H_

#include "mozilla/dom/FileSystemManagerParent.h"
#include "mozilla/dom/FlippedOnce.h"
#include "nsCOMPtr.h"

class nsISerialEventTarget;

namespace mozilla {

class TaskQueue;

namespace dom {

// Serves a FileSystemManager operating on real OS paths: every EntryId is the
// UTF-8 encoded absolute path of the entry. Unlike the OPFS base class, this
// actor has no FileSystemDataManager; it owns a serial TaskQueue it is bound
// to and registers with FileSystemLocalService for shutdown handling.
class FileSystemLocalManagerParent final : public FileSystemManagerParent {
 public:
  FileSystemLocalManagerParent(RefPtr<TaskQueue> aTaskQueue,
                               nsCOMPtr<nsISerialEventTarget> aBackgroundTarget);

  void AssertIsOnIOTarget() const override;

  bool IsAlive() const override;

  mozilla::ipc::IPCResult RecvGetRootHandle(
      GetRootHandleResolver&& aResolver) override;

  mozilla::ipc::IPCResult RecvGetDirectoryHandle(
      FileSystemGetHandleRequest&& aRequest,
      GetDirectoryHandleResolver&& aResolver) override;

  mozilla::ipc::IPCResult RecvGetFileHandle(
      FileSystemGetHandleRequest&& aRequest,
      GetFileHandleResolver&& aResolver) override;

  mozilla::ipc::IPCResult RecvGetAccessHandle(
      FileSystemGetAccessHandleRequest&& aRequest,
      GetAccessHandleResolver&& aResolver) override;

  mozilla::ipc::IPCResult RecvGetWritable(
      FileSystemGetWritableRequest&& aRequest,
      GetWritableResolver&& aResolver) override;

  mozilla::ipc::IPCResult RecvGetFile(FileSystemGetFileRequest&& aRequest,
                                      GetFileResolver&& aResolver) override;

  mozilla::ipc::IPCResult RecvResolve(FileSystemResolveRequest&& aRequest,
                                      ResolveResolver&& aResolver) override;

  mozilla::ipc::IPCResult RecvGetEntries(
      FileSystemGetEntriesRequest&& aRequest,
      GetEntriesResolver&& aResolver) override;

  mozilla::ipc::IPCResult RecvRemoveEntry(
      FileSystemRemoveEntryRequest&& aRequest,
      RemoveEntryResolver&& aResolver) override;

  mozilla::ipc::IPCResult RecvMoveEntry(FileSystemMoveEntryRequest&& aRequest,
                                        MoveEntryResolver&& aResolver) override;

  mozilla::ipc::IPCResult RecvRenameEntry(
      FileSystemRenameEntryRequest&& aRequest,
      MoveEntryResolver&& aResolver) override;

  void OnWritableStreamClosed(const fs::EntryId& aEntryId,
                              const fs::FileId& aTemporaryFileId,
                              bool aIsExclusive, bool aAbort) override;

  void RequestAllowToClose() override;

  void ActorDestroy(ActorDestroyReason aWhy) override;

 private:
  ~FileSystemLocalManagerParent() = default;

  const RefPtr<TaskQueue> mTaskQueue;

  const nsCOMPtr<nsISerialEventTarget> mBackgroundTarget;

  // Only accessed on the PBackground thread.
  FlippedOnce<false> mLocalRequestedAllowToClose;

  // Only accessed on the PBackground thread.
  bool mClosed = false;
};

}  // namespace dom
}  // namespace mozilla

#endif  // DOM_FS_PARENT_FILESYSTEMLOCALMANAGERPARENT_H_
