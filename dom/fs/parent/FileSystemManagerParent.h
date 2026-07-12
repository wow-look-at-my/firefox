/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_FS_PARENT_FILESYSTEMMANAGERPARENT_H_
#define DOM_FS_PARENT_FILESYSTEMMANAGERPARENT_H_

#include "ErrorList.h"
#include "mozilla/dom/FileSystemParentTypes.h"
#include "mozilla/dom/FlippedOnce.h"
#include "mozilla/dom/PFileSystemManagerParent.h"
#include "mozilla/dom/quota/ConditionalCompilation.h"
#include "nsISupports.h"

namespace mozilla::dom {

namespace fs::data {
class FileSystemDataManager;
}  // namespace fs::data

class FileSystemManagerParent : public PFileSystemManagerParent {
 public:
  FileSystemManagerParent(RefPtr<fs::data::FileSystemDataManager> aDataManager,
                          const EntryId& aRootEntry);

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(FileSystemManagerParent, override)

  virtual void AssertIsOnIOTarget() const;

  virtual bool IsAlive() const;

  // Safe to call while the actor is live.
  const RefPtr<fs::data::FileSystemDataManager>& DataManagerStrongRef() const;

  void SetRegistered(bool aRegistered) { mRegistered = aRegistered; }

  virtual mozilla::ipc::IPCResult RecvGetRootHandle(
      GetRootHandleResolver&& aResolver);

  virtual mozilla::ipc::IPCResult RecvGetDirectoryHandle(
      FileSystemGetHandleRequest&& aRequest,
      GetDirectoryHandleResolver&& aResolver);

  virtual mozilla::ipc::IPCResult RecvGetFileHandle(
      FileSystemGetHandleRequest&& aRequest, GetFileHandleResolver&& aResolver);

  virtual mozilla::ipc::IPCResult RecvGetAccessHandle(
      FileSystemGetAccessHandleRequest&& aRequest,
      GetAccessHandleResolver&& aResolver);

  virtual mozilla::ipc::IPCResult RecvGetWritable(
      FileSystemGetWritableRequest&& aRequest, GetWritableResolver&& aResolver);

  virtual mozilla::ipc::IPCResult RecvGetFile(
      FileSystemGetFileRequest&& aRequest, GetFileResolver&& aResolver);

  virtual mozilla::ipc::IPCResult RecvResolve(
      FileSystemResolveRequest&& aRequest, ResolveResolver&& aResolver);

  virtual mozilla::ipc::IPCResult RecvGetEntries(
      FileSystemGetEntriesRequest&& aRequest, GetEntriesResolver&& aResolver);

  virtual mozilla::ipc::IPCResult RecvRemoveEntry(
      FileSystemRemoveEntryRequest&& aRequest, RemoveEntryResolver&& aResolver);

  virtual mozilla::ipc::IPCResult RecvMoveEntry(
      FileSystemMoveEntryRequest&& aRequest, MoveEntryResolver&& aResolver);

  virtual mozilla::ipc::IPCResult RecvRenameEntry(
      FileSystemRenameEntryRequest&& aRequest, MoveEntryResolver&& aResolver);

  // Called by FileSystemWritableFileStreamParent when its stream is closed;
  // releases the lock held for the stream and finalizes or discards the
  // written data.
  virtual void OnWritableStreamClosed(const fs::EntryId& aEntryId,
                                      const fs::FileId& aTemporaryFileId,
                                      bool aIsExclusive, bool aAbort);

  virtual void RequestAllowToClose();

  void ActorDestroy(ActorDestroyReason aWhy) override;

 protected:
  FileSystemManagerParent();

  virtual ~FileSystemManagerParent();

 private:
  RefPtr<fs::data::FileSystemDataManager> mDataManager;

  FileSystemGetHandleResponse mRootResponse;

  FlippedOnce<false> mRequestedAllowToClose;

  bool mRegistered = false;

  DEBUGONLY(bool mActorDestroyed = false);
};

}  // namespace mozilla::dom

#endif  // DOM_FS_PARENT_FILESYSTEMMANAGERPARENT_H_
