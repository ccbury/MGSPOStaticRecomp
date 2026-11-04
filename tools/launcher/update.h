// Included by the desktop launcher after the shared CLI pipeline.
#pragma once

struct UpdatePolicy {
  // Tests use disposable local bare remotes, never a user's source checkout.
  bool fixture = false;
  wstring fixtureRemote;
};

struct UpdateInfo {
  string current, available, remote;
  bool newer = false;
};

bool githubOrigin(const string& remote) {
  return remote == "https://github.com/ccbury/MGSPOStaticRecomp.git" ||
         remote == "https://github.com/ccbury/MGSPOStaticRecomp" ||
         remote == "git@github.com:ccbury/MGSPOStaticRecomp.git" ||
         remote == "ssh://git@github.com/ccbury/MGSPOStaticRecomp.git";
}

string gitOutput(const fs::path& source, const std::vector<wstring>& args) {
  string text;
  if (runProcess(L"git.exe", args, source, {}, [&](const string& l) {
        text += (text.empty() ? "" : "\n") + l;
      }, false) != 0)
    die(L"Git operation failed. Check network access and your existing Git/GitHub authentication, then retry. "
        L"No credentials are stored by the launcher.");
  return text;
}

void requirePlainDirectory(const fs::path& path) {
  fs::path absolute = fs::absolute(path);
  for (auto part = absolute; !part.empty() && part != part.parent_path(); part = part.parent_path()) {
    DWORD attr = GetFileAttributesW(part.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
      die(L"Automatic updates do not support junctions or symlinks: " + part.wstring());
  }
}

void checkUpdateSource(const fs::path& source, const UpdatePolicy& policy = {}) {
  requirePlainDirectory(source);
  if (!fs::is_directory(source / L".git"))
    die(L"Automatic updates require a standalone Git clone, not a linked worktree or source archive. "
        L"Use a dedicated clean clone of the GitHub repository on main.");
  requirePlainDirectory(source / L".git");
  std::error_code ec;
  fs::path top = toWide(gitOutput(source, {L"rev-parse", L"--show-toplevel"}));
  if (!fs::equivalent(source, top, ec) || ec)
    die(L"Source path is not the Git checkout root. Automatic update refused.");
  fs::path common = toWide(gitOutput(source, {L"rev-parse", L"--git-common-dir"}));
  if (common.is_relative()) common = source / common;
  if (!fs::equivalent(common, source / L".git", ec) || ec)
    die(L"Shared Git directories are not supported for automatic updates.");
  string remote = gitOutput(source, {L"remote", L"get-url", L"origin"});
  if (!(policy.fixture ? remote == toUtf8(policy.fixtureRemote) : githubOrigin(remote)))
    die(L"Unexpected origin. Updates only support ccbury/MGSPOStaticRecomp on GitHub (no credentials in URLs).");
  if (!gitOutput(source, {L"status", L"--porcelain", L"--untracked-files=normal"}).empty())
    die(L"Source contains local edits or untracked files. Commit/move them yourself; the launcher will not stash or overwrite them.");
  if (gitOutput(source, {L"symbolic-ref", L"--short", L"HEAD"}) != "main")
    die(L"Automatic updates require main. Feature branches and detached checkouts are not modified.");
  for (const auto& name : {L"MERGE_HEAD", L"CHERRY_PICK_HEAD", L"REVERT_HEAD", L"rebase-merge", L"rebase-apply"})
    if (fs::exists(source / L".git" / name)) die(L"Finish the existing Git operation before updating.");
}

UpdateInfo checkUpdates(const fs::path& source, const UpdatePolicy& policy = {}) {
  if (searchExe(L"git.exe").empty())
    die(L"Git for Windows is required to check/update source. Install Git, configure your existing GitHub access, "
        L"then restart this UI. No automatic installation is performed.");
  checkUpdateSource(source, policy);
  UpdateInfo info;
  info.current = gitOutput(source, {L"rev-parse", L"HEAD"});
  info.remote = gitOutput(source, {L"remote", L"get-url", L"origin"});
  say(L"Fetching GitHub main (existing Git authentication is used)...");
  gitOutput(source, {L"fetch", L"--no-tags", L"origin", L"refs/heads/main"});
  info.available = gitOutput(source, {L"rev-parse", L"FETCH_HEAD"});
  if (info.current == info.available) return info;
  if (runProcess(L"git.exe", {L"merge-base", L"--is-ancestor", toWide(info.current), toWide(info.available)},
                 source, {}, nullptr, false) != 0)
    die(L"Local history is ahead of or divergent from GitHub main. Automatic update refused; no source files changed.");
  info.newer = true;
  return info;
}

// Only this work-root-owned clone is ever fast-forwarded. The original source is read-only.
fs::path managedSource(const Ctx& c) { return c.work / L"updates" / L"source"; }

void requireOwnedSource(const Ctx& c) {
  requirePlainDirectory(c.work);
  fs::path source = managedSource(c);
  requirePlainDirectory(source);
  requirePlainDirectory(source / L".git");
  if (!fs::exists(source / L".git" / L"mgspo-owner"))
    die(L"The managed update directory is not launcher-owned. Move it yourself; it will not be overwritten.");
  std::ifstream in(source / L".git" / L"mgspo-owner", std::ios::binary);
  string owner((std::istreambuf_iterator<char>(in)), {});
  if (owner != toUtf8(fs::weakly_canonical(c.work).wstring()))
    die(L"The managed clone belongs to a different work directory. Automatic update refused.");
}

fs::path prepareUpdateSource(const Ctx& c, const UpdateInfo& expected, const UpdatePolicy& policy = {}) {
  // Recheck local edits and ancestry at the action boundary, not just at Check.
  UpdateInfo live = checkUpdates(c.source, policy);
  if (live.current != expected.current || live.available != expected.available)
    die(L"Source or GitHub main changed since Check. Check again before updating.");
  requirePlainDirectory(c.work);
  fs::path source = managedSource(c);
  std::error_code ec;
  if (!fs::equivalent(source, c.source, ec)) {
    if (!fs::exists(source)) {
      fs::create_directories(source.parent_path());
      say(L"Creating a dedicated update clone. Your original checkout will not be changed.");
      if (runProcess(L"git.exe", {L"clone", L"--no-checkout", L"--no-hardlinks", L"--origin", L"origin",
                                 toWide(live.remote), source.wstring()}, {}, {}, nullptr) != 0)
        die(L"Clone failed. The partial update directory is kept for inspection; no original source was changed.");
      std::ofstream marker(source / L".git" / L"mgspo-owner", std::ios::binary);
      marker << toUtf8(fs::weakly_canonical(c.work).wstring());
      marker.close();
      if (!marker) die(L"Cannot record ownership of managed update clone.");
      gitOutput(source, {L"-c", L"core.hooksPath=NUL", L"checkout", L"-B", L"main", toWide(live.current)});
    }
    requireOwnedSource(c);
  } else {
    requireOwnedSource(c);
  }
  UpdateInfo managed = checkUpdates(source, policy);
  if (managed.available != expected.available) die(L"GitHub main changed while staging. Check again.");
  say(L"Fast-forwarding only the managed update clone...");
  gitOutput(source, {L"-c", L"core.hooksPath=NUL", L"merge", L"--ff-only", toWide(expected.available)});
  if (gitOutput(source, {L"rev-parse", L"HEAD"}) != expected.available)
    die(L"Managed source did not reach the checked revision.");
  return source;
}

// Unique version directories keep the running launcher/tools untouched on Windows.
fs::path rebuildUpdatedTools(const Ctx& c, const fs::path& source, const string& revision) {
  setupToolchain();
  fs::path build = c.work / L"updates" /
      (L"v-" + toWide(revision.substr(0, 12)) + L"-" + std::to_wstring(GetTickCount64()));
  fs::path log = c.logs / L"update-build.log";
  say(L"Building the new launcher and tools in " + build.wstring());
  if (runProcess(L"cmake.exe", {L"-S", source.wstring(), L"-B", build.wstring(), L"-G", L"Ninja",
                              L"-DCMAKE_BUILD_TYPE=Release", L"-DMGSPO_BUILD_TESTS=OFF",
                              L"-DMGSPO_GENERATED_DIR=" + (build / L"no-game").wstring()},
                 {}, log, nullptr) != 0 ||
      runProcess(L"cmake.exe", {L"--build", build.wstring(), L"--target", L"mgspo-desktop", L"--parallel", L"1"},
                 {}, log, nullptr) != 0)
    die(L"Updated tools failed to build. The current launcher is unchanged. Managed source may already be updated; "
        L"no transactional source rollback is claimed. See " + log.wstring());
  if (!fs::exists(build / L"mgspo-desktop.exe") || !fs::exists(build / L"mgspo-launcher.exe"))
    die(L"Updated launcher outputs are missing; keeping the current launcher.");
  return build;
}

void activateUpdate(Ctx& c, const fs::path& source, const fs::path& tools, const string& revision) {
  c.config.load(c.config.file);
  std::ofstream marker(tools / L"mgspo-install", std::ios::binary);
  marker << toUtf8(fs::weakly_canonical(c.work).wstring()) << "\n" << revision;
  marker.close();
  if (!marker) die(L"Cannot mark updated tools as ready. Current launcher/configuration were kept.");
  c.config.set("source", toUtf8(source.wstring()));
  c.config.set("source_revision", revision);
  c.config.set("desktop", toUtf8((tools / L"mgspo-desktop.exe").wstring()));
  c.config.save();
}

fs::path performUpdate(Ctx& c, const UpdateInfo& expected, OperationLock& lock, const UpdatePolicy& policy = {}) {
  fs::path source = prepareUpdateSource(c, expected, policy);
  fs::path tools = rebuildUpdatedTools(c, source, expected.available);
  say(L"Rebuilding game with the newly built launcher/tools (saves are kept)...");
  // The new CLI acquires this same work lock. A competing operation fails safely.
  lock.release();
  string prompts = c.opt.prompts.empty() ? c.config.get("prompts") : c.opt.prompts;
  auto video = videoOptions(c);
  if (prompts.empty()) prompts = "original";
  if (runProcess((tools / L"mgspo-launcher.exe").wstring(),
                 {L"build", L"--iso", c.iso.wstring(), L"--work", c.work.wstring(),
                  L"--source", source.wstring(), L"--config", c.config.file.wstring(),
                  L"--controls", toWide(c.opt.controls),
                  L"--renderer", toWide(rendererOption(c)),
                  L"--render-scale", toWide(video.first), L"--window-size", toWide(video.second),
                  L"--prompts", toWide(prompts)},
                 {}, c.logs / L"update-game.log", nullptr) != 0)
    die(L"Updated game build failed. Current launcher is kept. Managed source may be updated; "
        L"retry Check updates then Update / rebuild. See update-game.log. Saves were not removed.");
  lock.acquire(c.work);
  // Preserve settings written by the new CLI, activating the new source only after success.
  activateUpdate(c, source, tools, expected.available);
  return tools;
}

fs::path activatedDesktop(const Ctx& c) {
  if (c.config.get("desktop").empty()) return {};
  fs::path desktop(toWide(c.config.get("desktop")));
  requirePlainDirectory(desktop.parent_path());
  if (desktop.filename() != L"mgspo-desktop.exe" ||
      fs::weakly_canonical(desktop.parent_path().parent_path()) != fs::weakly_canonical(c.work / L"updates") ||
      !fs::is_regular_file(desktop))
    die(L"The activated launcher is unavailable or outside this work directory. Check updates and rebuild.");
  std::ifstream marker(desktop.parent_path() / L"mgspo-install", std::ios::binary);
  string identity((std::istreambuf_iterator<char>(marker)), {});
  if (identity != toUtf8(fs::weakly_canonical(c.work).wstring()) + "\n" + c.config.get("source_revision"))
    die(L"The activated launcher has no matching ownership/revision marker. Check updates and rebuild.");
  return desktop;
}
