/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

/* Helpers shared by the local File System Access mochitests. Real file IO is
   delegated to script_localFileSystemAccess.js in the parent process because
   the content process is sandboxed. */

const MockFilePicker = SpecialPowers.MockFilePicker;

let chromeScript = null;
let baseDir = null;

function joinPath(...parts) {
  // baseDir is a native path, so it dictates the separator (Windows uses
  // backslashes and rejects mixed-separator paths).
  const sep = parts[0].includes("\\") ? "\\" : "/";
  return parts.join(sep);
}

function armActivation() {
  SpecialPowers.wrap(document).notifyUserGestureActivation();
}

async function rejectsWith(promise, name, message) {
  try {
    await promise;
    ok(false, `${message}: should have rejected`);
    return null;
  } catch (e) {
    is(e.name, name, `${message}: rejected with ${name}`);
    return e;
  }
}

async function setupLocalFileSystemAccessTest() {
  MockFilePicker.init();

  chromeScript = SpecialPowers.loadChromeScript(
    SimpleTest.getTestFileURL("script_localFileSystemAccess.js")
  );
  baseDir = await chromeScript.sendQuery("create-base-dir", {});

  SimpleTest.registerCleanupFunction(async () => {
    await chromeScript.sendQuery("remove", { path: baseDir });
    MockFilePicker.cleanup();
  });

  return baseDir;
}

function csWrite(path, content) {
  return chromeScript.sendQuery("write-file", { path, content });
}

function csRead(path) {
  return chromeScript.sendQuery("read-file", { path });
}

function csStat(path) {
  return chromeScript.sendQuery("stat", { path });
}

function csSetMtime(path, ms) {
  return chromeScript.sendQuery("set-mtime", { path, ms });
}

function csMakeDir(path) {
  return chromeScript.sendQuery("make-dir", { path });
}

function csListDir(path) {
  return chromeScript.sendQuery("list-dir", { path });
}

function csMakeManyFiles(path, count) {
  return chromeScript.sendQuery("make-many-files", { path, count });
}

function csRemove(path) {
  return chromeScript.sendQuery("remove", { path });
}

async function pickFile(path) {
  MockFilePicker.setFiles([path]);
  MockFilePicker.returnValue = MockFilePicker.returnOK;
  armActivation();
  const handles = await window.showOpenFilePicker();
  is(handles.length, 1, "single pick returns one handle");
  return handles[0];
}

async function pickFiles(paths, options = { multiple: true }) {
  MockFilePicker.setFiles(paths);
  MockFilePicker.returnValue = MockFilePicker.returnOK;
  armActivation();
  return window.showOpenFilePicker(options);
}

async function pickDirectory(path) {
  MockFilePicker.useDirectory(path);
  MockFilePicker.returnValue = MockFilePicker.returnOK;
  armActivation();
  return window.showDirectoryPicker();
}

async function pickSaveFile(path, options = {}) {
  MockFilePicker.setFiles([path]);
  MockFilePicker.returnValue = MockFilePicker.returnReplace;
  armActivation();
  return window.showSaveFilePicker(options);
}
