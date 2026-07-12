/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_FS_FILESYSTEMCONSTANTS_H_
#define DOM_FS_FILESYSTEMCONSTANTS_H_

#include "nsLiteralString.h"

namespace mozilla::dom::fs {

constexpr nsLiteralString kRootName = u""_ns;

constexpr nsLiteralString kRootString = u"root"_ns;

constexpr uint32_t kStreamCopyBlockSize = 1024 * 1024;

// Structured-clone payload kind flag marking handles backed by the local
// (real path) file system; the remaining bits hold FileSystemHandleKind.
constexpr uint32_t kLocalFileSystemHandleKindFlag = 0x80000000u;

constexpr uint32_t kLocalEntryIdMaxLength = 65536u;

}  // namespace mozilla::dom::fs

#endif  // DOM_FS_FILESYSTEMCONSTANTS_H_
