/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// End-to-end coverage of the raw-path file picker IPC transport. The page
// runs in an ordinary web content process, so every pick travels through
// nsFilePickerProxy -> PFilePicker Open(rawPathResults=true) ->
// FilePickerParent's raw-path branch -> InputPaths -> proxy path
// materialization -> local handle minting. Only the platform picker at the
// very end is mocked, and it is mocked in the parent process.

var MockFilePicker = SpecialPowers.MockFilePicker;

const PAGE_URL = "https://example.com/";

let gTempDir;
let gPickerInfo = {};

function armPicker(returnValue) {
  gPickerInfo = {};
  MockFilePicker.shown = false;
  MockFilePicker.returnValue = returnValue;
  MockFilePicker.showCallback = picker => {
    gPickerInfo.mode = picker.mode;
    gPickerInfo.rawPathResults = picker.rawPathResults;
  };
}

function checkParentPicker(expectedMode, message) {
  ok(MockFilePicker.shown, `parent-process picker was shown for ${message}`);
  is(gPickerInfo.mode, expectedMode, `picker mode for ${message}`);
  is(
    gPickerInfo.rawPathResults,
    true,
    `rawPathResults was forwarded over IPC for ${message}`
  );
}

add_setup(async function () {
  await SpecialPowers.pushPrefEnv({
    set: [["dom.fs.local.enabled", true]],
  });

  MockFilePicker.init();
  registerCleanupFunction(() => MockFilePicker.cleanup());

  gTempDir = await IOUtils.createUniqueDirectory(
    PathUtils.tempDir,
    "lfsa-real-ipc"
  );
  registerCleanupFunction(() =>
    IOUtils.remove(gTempDir, { recursive: true, ignoreAbsent: true })
  );

  const pickedDir = PathUtils.join(gTempDir, "picked-dir");
  await IOUtils.makeDirectory(pickedDir);
  await IOUtils.writeUTF8(PathUtils.join(pickedDir, "alpha.txt"), "alpha data");
  await IOUtils.writeUTF8(PathUtils.join(pickedDir, "beta.txt"), "beta data");
  await IOUtils.writeUTF8(
    PathUtils.join(gTempDir, "open-single.txt"),
    "open-single data"
  );
  await IOUtils.writeUTF8(
    PathUtils.join(gTempDir, "multi-one.txt"),
    "multi-one data"
  );
  await IOUtils.writeUTF8(
    PathUtils.join(gTempDir, "multi-two.txt"),
    "multi-two data"
  );
  await IOUtils.writeUTF8(
    PathUtils.join(gTempDir, "replace-target.txt"),
    "not yet replaced"
  );
});

add_task(async function test_show_directory_picker() {
  const pickedDir = PathUtils.join(gTempDir, "picked-dir");
  const alphaPath = PathUtils.join(pickedDir, "alpha.txt");

  await BrowserTestUtils.withNewTab(PAGE_URL, async browser => {
    // The parent-side mock only proves an IPC crossing if the page really
    // lives in a content process.
    ok(browser.isRemoteBrowser, "page loads in a content process");

    armPicker(MockFilePicker.returnOK);
    MockFilePicker.useDirectory(pickedDir);

    await SpecialPowers.spawn(
      browser,
      [{ leaf: PathUtils.filename(pickedDir) }],
      async args => {
        ok(content.window.isSecureContext, "page is a secure context");

        content.document.notifyUserGestureActivation();
        const dir = await content.wrappedJSObject.showDirectoryPicker();

        is(dir.kind, "directory", "directory handle kind");
        is(dir.name, args.leaf, "directory handle name");

        // Xrays hide the async iteration protocol; drive the iterator
        // manually through a waived reference.
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
          ["alpha.txt", "beta.txt"],
          "iteration lists the real directory contents"
        );

        const fileHandle = await dir.getFileHandle("alpha.txt");
        is(
          await (await fileHandle.getFile()).text(),
          "alpha data",
          "getFile through the picked directory reads the real file"
        );

        const writable = await fileHandle.createWritable();
        await writable.write("alpha updated via directory pick");
        await writable.close();
      }
    );

    checkParentPicker(Ci.nsIFilePicker.modeGetFolder, "showDirectoryPicker");
    is(
      await IOUtils.readUTF8(alphaPath),
      "alpha updated via directory pick",
      "createWritable through the picked directory reached the real disk"
    );
  });
});

add_task(async function test_show_open_file_picker_single() {
  const filePath = PathUtils.join(gTempDir, "open-single.txt");

  await BrowserTestUtils.withNewTab(PAGE_URL, async browser => {
    armPicker(MockFilePicker.returnOK);
    MockFilePicker.setFiles([filePath]);

    await SpecialPowers.spawn(browser, [], async () => {
      content.document.notifyUserGestureActivation();
      const handles = await content.wrappedJSObject.showOpenFilePicker();

      is(handles.length, 1, "single pick resolves one handle");
      const handle = handles[0];
      is(handle.kind, "file", "file handle kind");
      is(handle.name, "open-single.txt", "file handle name");

      is(
        await (await handle.getFile()).text(),
        "open-single data",
        "getFile reads the real file"
      );

      const writable = await handle.createWritable();
      await writable.write("open-single updated");
      await writable.close();
    });

    checkParentPicker(Ci.nsIFilePicker.modeOpen, "showOpenFilePicker");
    is(
      await IOUtils.readUTF8(filePath),
      "open-single updated",
      "createWritable through the picked file reached the real disk"
    );
  });
});

add_task(async function test_show_open_file_picker_multiple() {
  const onePath = PathUtils.join(gTempDir, "multi-one.txt");
  const twoPath = PathUtils.join(gTempDir, "multi-two.txt");

  await BrowserTestUtils.withNewTab(PAGE_URL, async browser => {
    armPicker(MockFilePicker.returnOK);
    MockFilePicker.setFiles([onePath, twoPath]);

    await SpecialPowers.spawn(browser, [], async () => {
      content.document.notifyUserGestureActivation();
      const handles = await content.wrappedJSObject.showOpenFilePicker(
        Cu.cloneInto({ multiple: true }, content)
      );

      is(handles.length, 2, "multiple pick resolves two handles");
      is(handles[0].kind, "file", "first handle kind");
      is(handles[0].name, "multi-one.txt", "first handle name");
      is(handles[1].kind, "file", "second handle kind");
      is(handles[1].name, "multi-two.txt", "second handle name");

      is(
        await (await handles[0].getFile()).text(),
        "multi-one data",
        "getFile reads the first picked file"
      );
      is(
        await (await handles[1].getFile()).text(),
        "multi-two data",
        "getFile reads the second picked file"
      );

      const writable = await handles[1].createWritable();
      await writable.write("multi-two updated");
      await writable.close();
    });

    checkParentPicker(
      Ci.nsIFilePicker.modeOpenMultiple,
      "showOpenFilePicker multiple"
    );
    is(
      await IOUtils.readUTF8(twoPath),
      "multi-two updated",
      "createWritable through a multi-picked file reached the real disk"
    );
    is(
      await IOUtils.readUTF8(onePath),
      "multi-one data",
      "the other picked file is untouched"
    );
  });
});

add_task(async function test_show_save_file_picker_creates_missing_target() {
  const savePath = PathUtils.join(gTempDir, "save-new.txt");
  ok(!(await IOUtils.exists(savePath)), "save target does not exist yet");

  await BrowserTestUtils.withNewTab(PAGE_URL, async browser => {
    armPicker(MockFilePicker.returnOK);
    MockFilePicker.setFiles([savePath]);

    await SpecialPowers.spawn(browser, [], async () => {
      content.document.notifyUserGestureActivation();
      const handle = await content.wrappedJSObject.showSaveFilePicker();

      is(handle.kind, "file", "save handle kind");
      is(handle.name, "save-new.txt", "save handle name");
      is(
        (await handle.getFile()).size,
        0,
        "freshly created save target reads back empty"
      );

      content.wrappedJSObject.savedHandle = handle;
    });

    checkParentPicker(Ci.nsIFilePicker.modeSave, "showSaveFilePicker");
    ok(
      await IOUtils.exists(savePath),
      "the save round trip created the target on disk"
    );
    is(
      (await IOUtils.stat(savePath)).size,
      0,
      "the created save target is 0 bytes"
    );

    await SpecialPowers.spawn(browser, [], async () => {
      const handle = content.wrappedJSObject.savedHandle;
      const writable = await handle.createWritable();
      await writable.write("saved content");
      await writable.close();
    });

    is(
      await IOUtils.readUTF8(savePath),
      "saved content",
      "createWritable through the save handle reached the real disk"
    );
  });
});

add_task(async function test_show_save_file_picker_replace_existing() {
  const replacePath = PathUtils.join(gTempDir, "replace-target.txt");

  await BrowserTestUtils.withNewTab(PAGE_URL, async browser => {
    armPicker(MockFilePicker.returnReplace);
    MockFilePicker.setFiles([replacePath]);

    await SpecialPowers.spawn(browser, [], async () => {
      content.document.notifyUserGestureActivation();
      const handle = await content.wrappedJSObject.showSaveFilePicker();

      is(handle.kind, "file", "replace handle kind");
      is(handle.name, "replace-target.txt", "replace handle name");
      is(
        await (await handle.getFile()).text(),
        "not yet replaced",
        "picking an existing save target does not truncate it"
      );

      const writable = await handle.createWritable();
      await writable.write("replaced content");
      await writable.close();
    });

    checkParentPicker(
      Ci.nsIFilePicker.modeSave,
      "showSaveFilePicker returnReplace"
    );
    is(
      await IOUtils.readUTF8(replacePath),
      "replaced content",
      "createWritable through the replace handle reached the real disk"
    );
  });
});

add_task(async function test_canceled_pick_rejects() {
  await BrowserTestUtils.withNewTab(PAGE_URL, async browser => {
    armPicker(MockFilePicker.returnCancel);
    MockFilePicker.setFiles([]);

    await SpecialPowers.spawn(browser, [], async () => {
      content.document.notifyUserGestureActivation();
      await Assert.rejects(
        content.wrappedJSObject.showOpenFilePicker(),
        err => err.name == "AbortError",
        "canceled pick rejects with AbortError"
      );
    });

    ok(
      MockFilePicker.shown,
      "the parent-process picker ran and returned cancel"
    );
  });
});
