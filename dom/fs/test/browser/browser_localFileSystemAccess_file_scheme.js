/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Verifies the local File System Access backend on a file:// document:
// picking, handle operations, IndexedDB persistence across a tab reload
// without re-picking, and real on-disk effects.

add_task(async function test_file_scheme_persistence() {
  await SpecialPowers.pushPrefEnv({
    set: [["dom.fs.local.enabled", true]],
  });

  const tempDir = await IOUtils.createUniqueDirectory(
    PathUtils.tempDir,
    "lfsa-file-scheme"
  );
  registerCleanupFunction(() =>
    IOUtils.remove(tempDir, { recursive: true, ignoreAbsent: true })
  );

  const dataPath = PathUtils.join(tempDir, "data.txt");
  const pagePath = PathUtils.join(tempDir, "page.html");
  await IOUtils.writeUTF8(dataPath, "original content");
  await IOUtils.writeUTF8(
    pagePath,
    '<!DOCTYPE html><meta charset="utf-8"><title>lfsa file scheme</title>'
  );

  const pageUrl = PathUtils.toFileURI(pagePath);
  const tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, pageUrl);
  registerCleanupFunction(() => BrowserTestUtils.removeTab(tab));

  const dirLeaf = PathUtils.filename(tempDir);

  await SpecialPowers.spawn(
    tab.linkedBrowser,
    [{ path: tempDir, leaf: dirLeaf }],
    async args => {
      ok(content.window.isSecureContext, "file:// page is a secure context");

      // The picker is created in the content process, so the mock has to be
      // registered here rather than in the parent.
      const MockFilePicker = content.SpecialPowers.MockFilePicker;
      MockFilePicker.init();
      MockFilePicker.useDirectory(args.path);
      MockFilePicker.returnValue = MockFilePicker.returnOK;

      content.document.notifyUserGestureActivation();
      const win = content.wrappedJSObject;
      const dir = await win.showDirectoryPicker();
      MockFilePicker.cleanup();

      is(dir.kind, "directory", "picker resolved a directory handle");
      is(dir.name, args.leaf, "directory handle is the picked directory");

      // Xrays hide the async iteration protocol; drive the iterator manually
      // through a waived reference.
      const names = [];
      const it = ChromeUtils.waiveXrays(dir).keys();
      for (;;) {
        const res = await it.next();
        if (res.done) {
          break;
        }
        names.push(res.value);
      }
      names.sort();
      Assert.deepEqual(
        names,
        ["data.txt", "page.html"],
        "iteration lists the real directory contents"
      );

      const fileHandle = await dir.getFileHandle("data.txt");
      const file = await fileHandle.getFile();
      is(await file.text(), "original content", "getFile reads the real file");

      is(
        await dir.queryPermission({ mode: "readwrite" }),
        "granted",
        "queryPermission is granted on the file:// page"
      );
      is(
        await fileHandle.requestPermission({ mode: "readwrite" }),
        "granted",
        "requestPermission is granted on the file:// page"
      );

      const writable = await fileHandle.createWritable();
      await writable.write("updated before reload");
      await writable.close();

      await new Promise((resolve, reject) => {
        const req = win.indexedDB.open("lfsa-file-scheme", 1);
        req.onupgradeneeded = () => {
          req.result.createObjectStore("handles");
        };
        req.onsuccess = () => {
          const db = req.result;
          const tx = db.transaction("handles", "readwrite");
          tx.objectStore("handles").put(dir, "dir");
          tx.objectStore("handles").put(fileHandle, "file");
          tx.oncomplete = () => {
            db.close();
            resolve();
          };
          tx.onerror = () => reject(tx.error);
        };
        req.onerror = () => reject(req.error);
      });
    }
  );

  is(
    await IOUtils.readUTF8(dataPath),
    "updated before reload",
    "createWritable from the file:// page committed to the real file"
  );

  await BrowserTestUtils.reloadTab(tab);

  // After the reload the handles come solely from IndexedDB; no picker runs.
  await SpecialPowers.spawn(tab.linkedBrowser, [], async () => {
    const win = content.wrappedJSObject;

    const { dir, fileHandle } = await new Promise((resolve, reject) => {
      const req = win.indexedDB.open("lfsa-file-scheme", 1);
      req.onsuccess = () => {
        const db = req.result;
        const tx = db.transaction("handles");
        const store = tx.objectStore("handles");
        const dirReq = store.get("dir");
        const fileReq = store.get("file");
        tx.oncomplete = () => {
          db.close();
          resolve({ dir: dirReq.result, fileHandle: fileReq.result });
        };
        tx.onerror = () => reject(tx.error);
      };
      req.onerror = () => reject(req.error);
    });

    ok(dir, "directory handle restored from IndexedDB after reload");
    ok(fileHandle, "file handle restored from IndexedDB after reload");
    is(dir.kind, "directory", "restored directory handle kind");
    is(fileHandle.kind, "file", "restored file handle kind");

    is(
      await (await fileHandle.getFile()).text(),
      "updated before reload",
      "restored handle reads the content written before the reload"
    );

    const viaDir = await dir.getFileHandle("data.txt");
    is(
      await (await viaDir.getFile()).text(),
      "updated before reload",
      "restored directory handle resolves live children"
    );

    is(
      await fileHandle.queryPermission({ mode: "readwrite" }),
      "granted",
      "restored handle permission is granted"
    );

    const writable = await fileHandle.createWritable();
    await writable.write("final content after reload");
    await writable.close();
  });

  is(
    await IOUtils.readUTF8(dataPath),
    "final content after reload",
    "write through the restored handle reached the real file"
  );
});
