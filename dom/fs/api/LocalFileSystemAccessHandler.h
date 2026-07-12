/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_FS_LOCALFILESYSTEMACCESSHANDLER_H_
#define DOM_FS_LOCALFILESYSTEMACCESSHANDLER_H_

#include "mozilla/AlreadyAddRefed.h"

class nsGlobalWindowInner;

namespace mozilla {

class ErrorResult;

namespace dom {

class Promise;
struct DirectoryPickerOptions;
struct OpenFilePickerOptions;
struct SaveFilePickerOptions;

namespace fs {

// Implements the window.showOpenFilePicker/showSaveFilePicker/
// showDirectoryPicker entry points of the File System Access API on top of
// nsIFilePicker, minting handles bound to the local FileSystemManager.
class LocalFileSystemAccessHandler final {
 public:
  static already_AddRefed<Promise> ShowOpenFilePicker(
      nsGlobalWindowInner* aWindow, const OpenFilePickerOptions& aOptions,
      ErrorResult& aError);

  static already_AddRefed<Promise> ShowSaveFilePicker(
      nsGlobalWindowInner* aWindow, const SaveFilePickerOptions& aOptions,
      ErrorResult& aError);

  static already_AddRefed<Promise> ShowDirectoryPicker(
      nsGlobalWindowInner* aWindow, const DirectoryPickerOptions& aOptions,
      ErrorResult& aError);
};

}  // namespace fs
}  // namespace dom
}  // namespace mozilla

#endif  // DOM_FS_LOCALFILESYSTEMACCESSHANDLER_H_
