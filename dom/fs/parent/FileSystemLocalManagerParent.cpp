/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FileSystemLocalManagerParent.h"

#include "FileSystemLocalService.h"
#include "mozilla/TaskQueue.h"
#include "mozilla/dom/FileSystemLog.h"
#include "mozilla/dom/quota/ForwardDecls.h"
#include "mozilla/ipc/BackgroundParent.h"

using IPCResult = mozilla::ipc::IPCResult;

namespace mozilla::dom {

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

  aResolver(FileSystemGetHandleResponse(NS_ERROR_NOT_IMPLEMENTED));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvGetDirectoryHandle(
    FileSystemGetHandleRequest&& aRequest,
    GetDirectoryHandleResolver&& aResolver) {
  AssertIsOnIOTarget();

  aResolver(FileSystemGetHandleResponse(NS_ERROR_NOT_IMPLEMENTED));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvGetFileHandle(
    FileSystemGetHandleRequest&& aRequest, GetFileHandleResolver&& aResolver) {
  AssertIsOnIOTarget();

  aResolver(FileSystemGetHandleResponse(NS_ERROR_NOT_IMPLEMENTED));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvGetAccessHandle(
    FileSystemGetAccessHandleRequest&& aRequest,
    GetAccessHandleResolver&& aResolver) {
  AssertIsOnIOTarget();

  aResolver(FileSystemGetAccessHandleResponse(NS_ERROR_NOT_IMPLEMENTED));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvGetWritable(
    FileSystemGetWritableRequest&& aRequest, GetWritableResolver&& aResolver) {
  AssertIsOnIOTarget();

  aResolver(FileSystemGetWritableFileStreamResponse(NS_ERROR_NOT_IMPLEMENTED));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvGetFile(
    FileSystemGetFileRequest&& aRequest, GetFileResolver&& aResolver) {
  AssertIsOnIOTarget();

  aResolver(FileSystemGetFileResponse(NS_ERROR_NOT_IMPLEMENTED));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvResolve(
    FileSystemResolveRequest&& aRequest, ResolveResolver&& aResolver) {
  AssertIsOnIOTarget();

  aResolver(FileSystemResolveResponse(NS_ERROR_NOT_IMPLEMENTED));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvGetEntries(
    FileSystemGetEntriesRequest&& aRequest, GetEntriesResolver&& aResolver) {
  AssertIsOnIOTarget();

  aResolver(FileSystemGetEntriesResponse(NS_ERROR_NOT_IMPLEMENTED));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvRemoveEntry(
    FileSystemRemoveEntryRequest&& aRequest, RemoveEntryResolver&& aResolver) {
  AssertIsOnIOTarget();

  aResolver(FileSystemRemoveEntryResponse(NS_ERROR_NOT_IMPLEMENTED));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvMoveEntry(
    FileSystemMoveEntryRequest&& aRequest, MoveEntryResolver&& aResolver) {
  AssertIsOnIOTarget();

  aResolver(FileSystemMoveEntryResponse(NS_ERROR_NOT_IMPLEMENTED));

  return IPC_OK();
}

IPCResult FileSystemLocalManagerParent::RecvRenameEntry(
    FileSystemRenameEntryRequest&& aRequest, MoveEntryResolver&& aResolver) {
  AssertIsOnIOTarget();

  aResolver(FileSystemMoveEntryResponse(NS_ERROR_NOT_IMPLEMENTED));

  return IPC_OK();
}

void FileSystemLocalManagerParent::OnWritableStreamClosed(
    const fs::EntryId& aEntryId, const fs::FileId& aTemporaryFileId,
    bool aIsExclusive, bool aAbort) {
  // No writable stream can be created yet; nothing to release.
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

  InvokeAsync(mBackgroundTarget, __func__,
              [self = RefPtr<FileSystemLocalManagerParent>(this)]() {
                self->mClosed = true;

                if (FileSystemLocalService* service =
                        FileSystemLocalService::Get()) {
                  service->Unregister(self);
                }

                self->mTaskQueue->BeginShutdown();

                return BoolPromise::CreateAndResolve(true, __func__);
              });
}

}  // namespace mozilla::dom
