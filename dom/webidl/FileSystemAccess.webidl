/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * The origin of this IDL file is
 * https://wicg.github.io/file-system-access/
 */

enum WellKnownDirectory {
  "desktop",
  "documents",
  "downloads",
  "music",
  "pictures",
  "videos",
};

enum FileSystemPermissionMode {
  "read",
  "readwrite",
};

dictionary FilePickerAcceptType {
  USVString description = "";
  record<USVString, (USVString or sequence<USVString>)> accept;
};

typedef (WellKnownDirectory or FileSystemHandle) StartInDirectory;

dictionary FilePickerOptions {
  sequence<FilePickerAcceptType> types;
  boolean excludeAcceptAllOption = false;
  DOMString id;
  StartInDirectory startIn;
};

dictionary OpenFilePickerOptions : FilePickerOptions {
  boolean multiple = false;
};

dictionary SaveFilePickerOptions : FilePickerOptions {
  USVString? suggestedName;
};

dictionary DirectoryPickerOptions {
  DOMString id;
  StartInDirectory startIn;
  FileSystemPermissionMode mode = "read";
};

dictionary FileSystemHandlePermissionDescriptor {
  FileSystemPermissionMode mode = "read";
};
