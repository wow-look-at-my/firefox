/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "LocalFileSystemAccessHandler.h"

#include "mozilla/ErrorResult.h"
#include "mozilla/dom/BrowsingContext.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/FileSystemAccessBinding.h"
#include "mozilla/dom/FileSystemDirectoryHandle.h"
#include "mozilla/dom/FileSystemFileHandle.h"
#include "mozilla/dom/FileSystemHandle.h"
#include "mozilla/dom/FileSystemHandleBinding.h"
#include "mozilla/dom/FileSystemManager.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/StorageManager.h"
#include "nsComponentManagerUtils.h"
#include "nsContentUtils.h"
#include "nsGlobalWindowInner.h"
#include "nsIFile.h"
#include "nsIFilePicker.h"
#include "nsISimpleEnumerator.h"
#include "nsString.h"

namespace mozilla::dom::fs {

namespace {

enum class PickerKind { Open, Save, Directory };

class LocalFileSystemPickerCallback final : public nsIFilePickerShownCallback {
 public:
  LocalFileSystemPickerCallback(Promise* aPromise, nsIFilePicker* aFilePicker,
                                PickerKind aKind, bool aMultiple)
      : mPromise(aPromise),
        mFilePicker(aFilePicker),
        mKind(aKind),
        mMultiple(aMultiple) {}

  NS_DECL_ISUPPORTS
  NS_DECL_NSIFILEPICKERSHOWNCALLBACK

 private:
  ~LocalFileSystemPickerCallback() = default;

  const RefPtr<Promise> mPromise;
  const nsCOMPtr<nsIFilePicker> mFilePicker;
  const PickerKind mKind;
  const bool mMultiple;
};

NS_IMPL_ISUPPORTS(LocalFileSystemPickerCallback, nsIFilePickerShownCallback)

template <typename HandleType>
already_AddRefed<HandleType> MintHandle(nsIGlobalObject* aGlobal,
                                        RefPtr<FileSystemManager>& aManager,
                                        nsIFile* aFile, bool aDirectory) {
  nsAutoString path;
  if (NS_WARN_IF(NS_FAILED(aFile->GetPath(path))) || path.IsEmpty()) {
    return nullptr;
  }

  nsAutoString name;
  if (NS_WARN_IF(NS_FAILED(aFile->GetLeafName(name)))) {
    return nullptr;
  }

  FileSystemEntryMetadata metadata(NS_ConvertUTF16toUTF8(path), name,
                                   aDirectory);

  RefPtr<HandleType> handle = new HandleType(aGlobal, aManager, metadata);
  return handle.forget();
}

NS_IMETHODIMP LocalFileSystemPickerCallback::Done(
    nsIFilePicker::ResultCode aResult) {
  if (aResult == nsIFilePicker::returnCancel) {
    mPromise->MaybeRejectWithAbortError("The user aborted a request.");
    return NS_OK;
  }

  nsIGlobalObject* global = mPromise->GetGlobalObject();
  RefPtr<StorageManager> storageManager =
      global ? global->GetStorageManager() : nullptr;
  if (NS_WARN_IF(!storageManager)) {
    mPromise->MaybeRejectWithUnknownError("Missing storage manager.");
    return NS_OK;
  }

  RefPtr<FileSystemManager> manager =
      storageManager->GetLocalFileSystemManager();

  if (mKind == PickerKind::Open && mMultiple) {
    nsCOMPtr<nsISimpleEnumerator> iter;
    if (NS_WARN_IF(NS_FAILED(mFilePicker->GetFiles(getter_AddRefs(iter))))) {
      mPromise->MaybeRejectWithAbortError("No files were selected.");
      return NS_OK;
    }

    nsTArray<RefPtr<FileSystemFileHandle>> handles;

    nsCOMPtr<nsISupports> supports;
    bool loop = true;
    while (NS_SUCCEEDED(iter->HasMoreElements(&loop)) && loop) {
      iter->GetNext(getter_AddRefs(supports));
      if (supports) {
        nsCOMPtr<nsIFile> file = do_QueryInterface(supports);
        MOZ_ASSERT(file);

        RefPtr<FileSystemFileHandle> handle =
            MintHandle<FileSystemFileHandle>(global, manager, file,
                                             /* aDirectory */ false);
        if (handle) {
          handles.AppendElement(std::move(handle));
        }
      }
    }

    if (handles.IsEmpty()) {
      mPromise->MaybeRejectWithAbortError("No files were selected.");
      return NS_OK;
    }

    mPromise->MaybeResolve(handles);
    return NS_OK;
  }

  nsCOMPtr<nsIFile> file;
  mFilePicker->GetFile(getter_AddRefs(file));
  if (NS_WARN_IF(!file)) {
    mPromise->MaybeRejectWithAbortError("No file was selected.");
    return NS_OK;
  }

  if (mKind == PickerKind::Directory) {
    RefPtr<FileSystemDirectoryHandle> handle =
        MintHandle<FileSystemDirectoryHandle>(global, manager, file,
                                              /* aDirectory */ true);
    if (NS_WARN_IF(!handle)) {
      mPromise->MaybeRejectWithUnknownError("Failed to create a handle.");
      return NS_OK;
    }

    mPromise->MaybeResolve(handle);
    return NS_OK;
  }

  RefPtr<FileSystemFileHandle> handle = MintHandle<FileSystemFileHandle>(
      global, manager, file, /* aDirectory */ false);
  if (NS_WARN_IF(!handle)) {
    mPromise->MaybeRejectWithUnknownError("Failed to create a handle.");
    return NS_OK;
  }

  if (mKind == PickerKind::Open) {
    nsTArray<RefPtr<FileSystemFileHandle>> handles;
    handles.AppendElement(std::move(handle));
    mPromise->MaybeResolve(handles);
    return NS_OK;
  }

  mPromise->MaybeResolve(handle);
  return NS_OK;
}

already_AddRefed<nsIFilePicker> CreatePicker(nsGlobalWindowInner* aWindow,
                                             nsIFilePicker::Mode aMode,
                                             const char* aTitleKey,
                                             ErrorResult& aError) {
  MOZ_ASSERT(aWindow);

  RefPtr<Document> doc = aWindow->GetExtantDoc();
  if (!doc || !doc->ConsumeTransientUserGestureActivation()) {
    aError.ThrowSecurityError(
        "The file picker requires transient user activation.");
    return nullptr;
  }

  RefPtr<BrowsingContext> bc = doc->GetBrowsingContext();
  if (NS_WARN_IF(!bc)) {
    aError.ThrowInvalidStateError("The window is not fully active.");
    return nullptr;
  }

  nsCOMPtr<nsIFilePicker> filePicker =
      do_CreateInstance("@mozilla.org/filepicker;1");
  if (NS_WARN_IF(!filePicker)) {
    aError.ThrowUnknownError("Failed to create a file picker.");
    return nullptr;
  }

  nsAutoString title;
  nsContentUtils::GetMaybeLocalizedString(PropertiesFile::FORMS_PROPERTIES,
                                          aTitleKey, doc, title);

  nsresult rv = filePicker->Init(bc, title, aMode, aWindow->AsGlobal());
  if (NS_WARN_IF(NS_FAILED(rv))) {
    aError.ThrowUnknownError("Failed to initialize the file picker.");
    return nullptr;
  }

  filePicker->SetRawPathResults(true);

  return filePicker.forget();
}

void ApplyAcceptFilters(nsIFilePicker* aFilePicker,
                        const Optional<Sequence<FilePickerAcceptType>>& aTypes,
                        bool aExcludeAcceptAllOption) {
  if (aTypes.WasPassed()) {
    for (const FilePickerAcceptType& type : aTypes.Value()) {
      if (!type.mAccept.WasPassed()) {
        continue;
      }

      nsAutoString filter;
      for (const auto& entry : type.mAccept.Value().Entries()) {
        AutoTArray<nsString, 4> extensions;
        if (entry.mValue.IsUSVString()) {
          extensions.AppendElement(entry.mValue.GetAsUSVString());
        } else {
          extensions.AppendElements(entry.mValue.GetAsUSVStringSequence());
        }

        for (const nsString& extension : extensions) {
          // WICG extension strings are required to start with a dot; skip
          // anything else instead of producing a bogus native filter.
          if (extension.Length() < 2 || extension.First() != u'.') {
            continue;
          }

          if (!filter.IsEmpty()) {
            filter.AppendLiteral("; ");
          }
          filter.Append(u'*');
          filter.Append(extension);
        }
      }

      if (!filter.IsEmpty()) {
        nsAutoString title(type.mDescription);
        if (title.IsEmpty()) {
          title = filter;
        }
        aFilePicker->AppendFilter(title, filter);
      }
    }
  }

  if (!aExcludeAcceptAllOption) {
    aFilePicker->AppendFilters(nsIFilePicker::filterAll);
  }
}

void ApplyStartIn(
    nsIFilePicker* aFilePicker,
    const Optional<OwningWellKnownDirectoryOrFileSystemHandle>& aStartIn) {
  if (!aStartIn.WasPassed()) {
    return;
  }

  const OwningWellKnownDirectoryOrFileSystemHandle& startIn = aStartIn.Value();

  if (startIn.IsFileSystemHandle()) {
    const FileSystemHandle& handle = startIn.GetAsFileSystemHandle();

    nsCOMPtr<nsIFile> file;
    if (NS_FAILED(NS_NewLocalFile(NS_ConvertUTF8toUTF16(handle.GetId()),
                                  getter_AddRefs(file)))) {
      return;
    }

    nsCOMPtr<nsIFile> directory;
    if (handle.Kind() == FileSystemHandleKind::File) {
      if (NS_FAILED(file->GetParent(getter_AddRefs(directory))) ||
          !directory) {
        return;
      }
    } else {
      directory = std::move(file);
    }

    aFilePicker->SetDisplayDirectory(directory);
    return;
  }

  nsAutoString token;
  switch (startIn.GetAsWellKnownDirectory()) {
    case WellKnownDirectory::Desktop:
      token.AssignLiteral("Desk");
      break;
    case WellKnownDirectory::Documents:
      token.AssignLiteral("Docs");
      break;
    case WellKnownDirectory::Downloads:
      token.AssignLiteral("DfltDwnld");
      break;
    default:
      // There are no cross-platform special directory tokens for music,
      // pictures and videos; startIn is only a hint, fall back to home.
      token.AssignLiteral("Home");
      break;
  }

  aFilePicker->SetDisplaySpecialDirectory(token);
}

already_AddRefed<Promise> OpenPicker(nsGlobalWindowInner* aWindow,
                                     nsIFilePicker* aFilePicker,
                                     PickerKind aKind, bool aMultiple,
                                     ErrorResult& aError) {
  RefPtr<Promise> promise = Promise::Create(aWindow->AsGlobal(), aError);
  if (NS_WARN_IF(aError.Failed())) {
    return nullptr;
  }

  auto callback = MakeRefPtr<LocalFileSystemPickerCallback>(
      promise, aFilePicker, aKind, aMultiple);

  nsresult rv = aFilePicker->Open(callback);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    aError.ThrowUnknownError("Failed to open the file picker.");
    return nullptr;
  }

  return promise.forget();
}

}  // namespace

// static
already_AddRefed<Promise> LocalFileSystemAccessHandler::ShowOpenFilePicker(
    nsGlobalWindowInner* aWindow, const OpenFilePickerOptions& aOptions,
    ErrorResult& aError) {
  const nsIFilePicker::Mode mode = aOptions.mMultiple
                                       ? nsIFilePicker::modeOpenMultiple
                                       : nsIFilePicker::modeOpen;

  nsCOMPtr<nsIFilePicker> filePicker =
      CreatePicker(aWindow, mode, "FileUpload", aError);
  if (!filePicker) {
    return nullptr;
  }

  ApplyAcceptFilters(filePicker, aOptions.mTypes,
                     aOptions.mExcludeAcceptAllOption);
  ApplyStartIn(filePicker, aOptions.mStartIn);

  return OpenPicker(aWindow, filePicker, PickerKind::Open, aOptions.mMultiple,
                    aError);
}

// static
already_AddRefed<Promise> LocalFileSystemAccessHandler::ShowSaveFilePicker(
    nsGlobalWindowInner* aWindow, const SaveFilePickerOptions& aOptions,
    ErrorResult& aError) {
  nsCOMPtr<nsIFilePicker> filePicker =
      CreatePicker(aWindow, nsIFilePicker::modeSave, "FileUpload", aError);
  if (!filePicker) {
    return nullptr;
  }

  ApplyAcceptFilters(filePicker, aOptions.mTypes,
                     aOptions.mExcludeAcceptAllOption);
  ApplyStartIn(filePicker, aOptions.mStartIn);

  if (aOptions.mSuggestedName.WasPassed() &&
      !aOptions.mSuggestedName.Value().IsVoid() &&
      !aOptions.mSuggestedName.Value().IsEmpty()) {
    filePicker->SetDefaultString(aOptions.mSuggestedName.Value());
  }

  return OpenPicker(aWindow, filePicker, PickerKind::Save,
                    /* aMultiple */ false, aError);
}

// static
already_AddRefed<Promise> LocalFileSystemAccessHandler::ShowDirectoryPicker(
    nsGlobalWindowInner* aWindow, const DirectoryPickerOptions& aOptions,
    ErrorResult& aError) {
  nsCOMPtr<nsIFilePicker> filePicker = CreatePicker(
      aWindow, nsIFilePicker::modeGetFolder, "DirectoryUpload", aError);
  if (!filePicker) {
    return nullptr;
  }

  ApplyStartIn(filePicker, aOptions.mStartIn);

  return OpenPicker(aWindow, filePicker, PickerKind::Directory,
                    /* aMultiple */ false, aError);
}

}  // namespace mozilla::dom::fs
