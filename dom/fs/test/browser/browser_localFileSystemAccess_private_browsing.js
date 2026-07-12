/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// The local File System Access backend is gated on the OS picker alone, so
// it must keep working in private browsing windows (OPFS stays blocked
// there).

add_task(async function test_private_browsing_local_fsa() {
  await SpecialPowers.pushPrefEnv({
    set: [["dom.fs.local.enabled", true]],
  });

  const tempDir = await IOUtils.createUniqueDirectory(
    PathUtils.tempDir,
    "lfsa-private"
  );
  registerCleanupFunction(() =>
    IOUtils.remove(tempDir, { recursive: true, ignoreAbsent: true })
  );

  const dataPath = PathUtils.join(tempDir, "data.txt");
  await IOUtils.writeUTF8(dataPath, "private original");

  const privateWin = await BrowserTestUtils.openNewBrowserWindow({
    private: true,
  });
  registerCleanupFunction(async () => {
    if (!privateWin.closed) {
      await BrowserTestUtils.closeWindow(privateWin);
    }
  });

  const tab = await BrowserTestUtils.openNewForegroundTab(
    privateWin.gBrowser,
    "https://example.com/"
  );

  await SpecialPowers.spawn(
    tab.linkedBrowser,
    [{ path: tempDir, leaf: PathUtils.filename(tempDir) }],
    async args => {
      ok(
        content.browsingContext.usePrivateBrowsing,
        "the test page runs in a private browsing context"
      );

      // The picker is created in the content process, so the mock has to be
      // registered here rather than in the parent.
      const MockFilePicker = content.SpecialPowers.MockFilePicker;
      MockFilePicker.init();
      MockFilePicker.useDirectory(args.path);
      MockFilePicker.returnValue = MockFilePicker.returnOK;

      content.document.notifyUserGestureActivation();
      const dir = await content.wrappedJSObject.showDirectoryPicker();
      MockFilePicker.cleanup();

      is(dir.kind, "directory", "picker resolved in the private window");
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
      Assert.deepEqual(
        names,
        ["data.txt"],
        "iteration works in the private window"
      );

      const fileHandle = await dir.getFileHandle("data.txt");
      is(
        await (await fileHandle.getFile()).text(),
        "private original",
        "getFile reads the real file in the private window"
      );

      const writable = await fileHandle.createWritable();
      await writable.write("written from a private window");
      await writable.close();
    }
  );

  is(
    await IOUtils.readUTF8(dataPath),
    "written from a private window",
    "createWritable from the private window committed to the real file"
  );

  await BrowserTestUtils.closeWindow(privateWin);
});
