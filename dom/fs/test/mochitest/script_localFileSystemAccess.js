/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

// Parent-process helper for the local File System Access tests: mochitest
// content processes are sandboxed and cannot touch the real file system, so
// all out-of-band file manipulation and verification happens here.

// eslint-disable-next-line mozilla/reject-importGlobalProperties
Cu.importGlobalProperties(["IOUtils", "PathUtils"]);

addMessageListener("create-base-dir", async () => {
  const base = Services.dirsvc
    .QueryInterface(Ci.nsIProperties)
    .get("TmpD", Ci.nsIFile);
  base.append("localfsa-test");
  base.createUnique(Ci.nsIFile.DIRECTORY_TYPE, 0o755);
  return base.path;
});

addMessageListener("write-file", async ({ path, content }) => {
  await IOUtils.writeUTF8(path, content);
  return true;
});

addMessageListener("read-file", async ({ path }) => {
  if (!(await IOUtils.exists(path))) {
    return null;
  }
  return IOUtils.readUTF8(path);
});

addMessageListener("stat", async ({ path }) => {
  if (!(await IOUtils.exists(path))) {
    return { exists: false };
  }
  const info = await IOUtils.stat(path);
  return {
    exists: true,
    isDirectory: info.type === "directory",
    size: info.size,
  };
});

addMessageListener("set-mtime", async ({ path, ms }) => {
  await IOUtils.setModificationTime(path, ms);
  return true;
});

addMessageListener("make-dir", async ({ path }) => {
  await IOUtils.makeDirectory(path, { ignoreExisting: true });
  return true;
});

addMessageListener("list-dir", async ({ path }) => {
  const children = await IOUtils.getChildren(path);
  return children.map(child => PathUtils.filename(child)).sort();
});

addMessageListener("make-many-files", async ({ path, count }) => {
  await IOUtils.makeDirectory(path, { ignoreExisting: true });
  for (let i = 0; i < count; ++i) {
    await IOUtils.writeUTF8(
      PathUtils.join(path, `entry-${String(i).padStart(5, "0")}.txt`),
      String(i)
    );
  }
  return true;
});

addMessageListener("remove", async ({ path }) => {
  await IOUtils.remove(path, { recursive: true, ignoreAbsent: true });
  return true;
});
