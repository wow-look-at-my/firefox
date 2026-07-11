/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FileSystemLocalService.h"

#include <cstring>

#include "FileSystemLocalManagerParent.h"
#include "mozilla/MozPromise.h"
#include "mozilla/Services.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/TaskQueue.h"
#include "mozilla/dom/FileSystemLog.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/ipc/PBackgroundParent.h"
#include "nsIEventTarget.h"
#include "nsIObserver.h"
#include "nsIObserverService.h"
#include "nsNetCID.h"
#include "nsReadableUtils.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"
#include "nsThreadUtils.h"
#include "nsXPCOM.h"

namespace mozilla::dom {

namespace {

StaticRefPtr<FileSystemLocalService> gFileSystemLocalService;

}  // namespace

// static
FileSystemLocalLockTable& FileSystemLocalLockTable::Get() {
  // Deliberately leaked; the table is accessed from IO task queues which may
  // outlive static destructors.
  static FileSystemLocalLockTable* sLockTable = new FileSystemLocalLockTable();
  return *sLockTable;
}

FileSystemLocalLockTable::FileSystemLocalLockTable()
    : mMutex("FileSystemLocalLockTable") {}

bool FileSystemLocalLockTable::LockShared(const nsCString& aPath) {
  MutexAutoLock lock(mMutex);

  if (mExclusive.Contains(aPath)) {
    return false;
  }

  mShared.LookupOrInsert(aPath, 0) += 1;
  return true;
}

void FileSystemLocalLockTable::UnlockShared(const nsCString& aPath) {
  MutexAutoLock lock(mMutex);

  if (auto entry = mShared.Lookup(aPath)) {
    MOZ_ASSERT(entry.Data() > 0);
    if (--entry.Data() == 0) {
      entry.Remove();
    }
  }
}

bool FileSystemLocalLockTable::IsAnyLockedUnder(const nsCString& aPath) {
  MutexAutoLock lock(mMutex);

  auto isUnder = [&aPath](const nsACString& aLocked) {
    return StringBeginsWith(aLocked, aPath) &&
           (aLocked.Length() == aPath.Length() ||
            aLocked.CharAt(aPath.Length()) == kLocalPathSeparatorChar);
  };

  for (const auto& locked : mExclusive) {
    if (isUnder(locked)) {
      return true;
    }
  }

  for (const auto& locked : mShared.Keys()) {
    if (isUnder(locked)) {
      return true;
    }
  }

  return false;
}

class FileSystemLocalService::ShutdownObserver final : public nsIObserver {
 public:
  explicit ShutdownObserver(nsCOMPtr<nsISerialEventTarget> aBackgroundTarget)
      : mBackgroundTarget(std::move(aBackgroundTarget)) {}

  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIOBSERVER

 private:
  ~ShutdownObserver() = default;

  const nsCOMPtr<nsISerialEventTarget> mBackgroundTarget;
};

NS_IMPL_ISUPPORTS(FileSystemLocalService::ShutdownObserver, nsIObserver)

NS_IMETHODIMP FileSystemLocalService::ShutdownObserver::Observe(
    nsISupports* aSubject, const char* aTopic, const char16_t* aData) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!strcmp(aTopic, NS_XPCOM_SHUTDOWN_OBSERVER_ID));

  if (nsCOMPtr<nsIObserverService> obs = services::GetObserverService()) {
    obs->RemoveObserver(this, NS_XPCOM_SHUTDOWN_OBSERVER_ID);
  }

  (void)mBackgroundTarget->Dispatch(NS_NewRunnableFunction(
      "FileSystemLocalService::ShutdownObserver::Observe", []() {
        if (RefPtr<FileSystemLocalService> service =
                gFileSystemLocalService.forget()) {
          service->RequestAllowToCloseAll();
        }
      }));

  return NS_OK;
}

FileSystemLocalService::FileSystemLocalService()
    : mBackgroundTarget(GetCurrentSerialEventTarget()) {
  mozilla::ipc::AssertIsOnBackgroundThread();
}

FileSystemLocalService::~FileSystemLocalService() = default;

// static
FileSystemLocalService* FileSystemLocalService::GetOrCreate() {
  mozilla::ipc::AssertIsOnBackgroundThread();

  if (!gFileSystemLocalService) {
    gFileSystemLocalService = MakeRefPtr<FileSystemLocalService>();

    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "FileSystemLocalService::AddShutdownObserver",
        [backgroundTarget =
             gFileSystemLocalService->mBackgroundTarget]() mutable {
          if (nsCOMPtr<nsIObserverService> obs =
                  services::GetObserverService()) {
            obs->AddObserver(new ShutdownObserver(std::move(backgroundTarget)),
                             NS_XPCOM_SHUTDOWN_OBSERVER_ID, false);
          }
        }));
  }

  return gFileSystemLocalService;
}

// static
FileSystemLocalService* FileSystemLocalService::Get() {
  mozilla::ipc::AssertIsOnBackgroundThread();

  return gFileSystemLocalService;
}

void FileSystemLocalService::CreateAndBindActor(
    RefPtr<mozilla::ipc::PBackgroundParent> aBackgroundActor,
    mozilla::ipc::Endpoint<PFileSystemManagerParent>&& aParentEndpoint,
    std::function<void(const nsresult&)>&& aResolver) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  using CreateActorPromise =
      MozPromise<RefPtr<FileSystemLocalManagerParent>, nsresult, true>;

  nsCOMPtr<nsIEventTarget> streamTransportService =
      do_GetService(NS_STREAMTRANSPORTSERVICE_CONTRACTID);
  if (NS_WARN_IF(!streamTransportService)) {
    aResolver(NS_ERROR_FAILURE);
    return;
  }

  RefPtr<TaskQueue> taskQueue =
      TaskQueue::Create(streamTransportService.forget(), "FSLocal");

  InvokeAsync(taskQueue, __func__,
              [taskQueue, backgroundTarget = mBackgroundTarget,
               parentEndpoint = std::move(aParentEndpoint)]() mutable {
                RefPtr<FileSystemLocalManagerParent> parent =
                    new FileSystemLocalManagerParent(
                        taskQueue, std::move(backgroundTarget));

                LOG(("Binding local parent endpoint"));
                if (!parentEndpoint.Bind(parent)) {
                  return CreateActorPromise::CreateAndReject(NS_ERROR_FAILURE,
                                                             __func__);
                }

                return CreateActorPromise::CreateAndResolve(std::move(parent),
                                                            __func__);
              })
      ->Then(mBackgroundTarget, __func__,
             [backgroundActor = std::move(aBackgroundActor),
              resolver = std::move(aResolver)](
                 CreateActorPromise::ResolveOrRejectValue&& aValue) {
               nsresult rv = NS_OK;

               if (aValue.IsReject()) {
                 rv = aValue.RejectValue();
               } else {
                 RefPtr<FileSystemLocalManagerParent> parent =
                     std::move(aValue.ResolveValue());

                 if (!parent->IsAlive()) {
                   rv = NS_ERROR_ABORT;
                 } else if (FileSystemLocalService* service =
                                FileSystemLocalService::Get()) {
                   service->Register(parent);
                 }
               }

               if (!backgroundActor->CanSend()) {
                 return;
               }

               resolver(rv);
             });
}

void FileSystemLocalService::Register(FileSystemLocalManagerParent* aActor) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  mActors.Insert(aActor);
}

void FileSystemLocalService::Unregister(FileSystemLocalManagerParent* aActor) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  mActors.Remove(aActor);
}

void FileSystemLocalService::RequestAllowToCloseAll() {
  mozilla::ipc::AssertIsOnBackgroundThread();

  for (FileSystemLocalManagerParent* actor : mActors) {
    actor->RequestAllowToClose();
  }
}

}  // namespace mozilla::dom
