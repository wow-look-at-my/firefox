/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_FS_PARENT_FILESYSTEMLOCALSERVICE_H_
#define DOM_FS_PARENT_FILESYSTEMLOCALSERVICE_H_

#include <functional>

#include "mozilla/Mutex.h"
#include "nsCOMPtr.h"
#include "nsHashKeys.h"
#include "nsISupportsImpl.h"
#include "nsTHashMap.h"
#include "nsTHashSet.h"

enum class nsresult : uint32_t;
class nsISerialEventTarget;

namespace mozilla::ipc {

template <class T>
class Endpoint;

class PBackgroundParent;

}  // namespace mozilla::ipc

namespace mozilla::dom {

class FileSystemLocalManagerParent;
class PFileSystemManagerParent;

#ifdef XP_WIN
constexpr char kLocalPathSeparatorChar = '\\';
#else
constexpr char kLocalPathSeparatorChar = '/';
#endif

// Process-wide table of the absolute paths currently opened by local writable
// file streams. Local actors of any origin share it because the keys are real
// OS paths. The exclusive arm is reserved for future access handle support.
class FileSystemLocalLockTable final {
 public:
  static FileSystemLocalLockTable& Get();

  bool LockShared(const nsCString& aPath);

  void UnlockShared(const nsCString& aPath);

  // True when aPath itself or any path under it holds any lock.
  bool IsAnyLockedUnder(const nsCString& aPath);

 private:
  FileSystemLocalLockTable();

  Mutex mMutex;

  nsTHashSet<nsCString> mExclusive MOZ_GUARDED_BY(mMutex);

  nsTHashMap<nsCStringHashKey, uint32_t> mShared MOZ_GUARDED_BY(mMutex);
};

// PBackground-affine registry of the live local file system actors. The first
// registration installs an xpcom-shutdown observer which requests all
// registered actors to close.
class FileSystemLocalService final {
 public:
  FileSystemLocalService();

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(FileSystemLocalService)

  // Must be called on the PBackground thread.
  static FileSystemLocalService* GetOrCreate();

  // May return nullptr; must be called on the PBackground thread.
  static FileSystemLocalService* Get();

  void CreateAndBindActor(
      RefPtr<mozilla::ipc::PBackgroundParent> aBackgroundActor,
      mozilla::ipc::Endpoint<PFileSystemManagerParent>&& aParentEndpoint,
      std::function<void(const nsresult&)>&& aResolver);

  void Register(FileSystemLocalManagerParent* aActor);

  void Unregister(FileSystemLocalManagerParent* aActor);

 private:
  class ShutdownObserver;

  ~FileSystemLocalService();

  void RequestAllowToCloseAll();

  const nsCOMPtr<nsISerialEventTarget> mBackgroundTarget;

  nsTHashSet<FileSystemLocalManagerParent*> mActors;
};

}  // namespace mozilla::dom

#endif  // DOM_FS_PARENT_FILESYSTEMLOCALSERVICE_H_
