//===--- Orihime.cpp - Orihime ToolChain Implementations --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Orihime.h"
#include "CommonArgs.h"
#include "clang/Config/config.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/Options.h"
#include "clang/Driver/SanitizerArgs.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

using tools::addMultilibFlag;

void orihime::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                                   const InputInfo &Output,
                                   const InputInfoList &Inputs,
                                   const llvm::opt::ArgList &Args,
                                   const char *LinkingOutput) const {
  const auto &ToolChain =
      static_cast<const toolchains::Orihime &>(getToolChain());
  const Driver &D = ToolChain.getDriver();

  ArgStringList CmdArgs;

  const char *Exec = Args.MakeArgString(ToolChain.GetLinkerPath());
  if (llvm::sys::path::filename(Exec).equals_lower("ld.lld") ||
      llvm::sys::path::stem(Exec).equals_lower("ld.lld")) {
    CmdArgs.push_back("-z");
    CmdArgs.push_back("separate-loadable-segments");
  }

  if (!D.SysRoot.empty())
    CmdArgs.push_back(Args.MakeArgString("--sysroot=" + D.SysRoot));

  if (Args.hasArg(options::OPT_s))
    CmdArgs.push_back("-s");

  if (Args.hasArg(options::OPT_r)) {
    CmdArgs.push_back("-r");
  } else {
    CmdArgs.push_back("--build-id");
    CmdArgs.push_back("--hash-style=gnu");
  }

  CmdArgs.push_back("--eh-frame-hdr");

  // XXX: No shared library support for now
  CmdArgs.push_back("-Bstatic");

  CmdArgs.push_back("-o");
  CmdArgs.push_back(Output.getFilename());

  AddLinkerInputs(ToolChain, Inputs, Args, CmdArgs, JA);
  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles)) {
    CmdArgs.push_back("-losrt");

    // AddRunTimeLibs(ToolChain, D, CmdArgs, Args);

    // TODO: pthread
  }

  Args.AddAllArgs(CmdArgs, options::OPT_L);
  Args.AddAllArgs(CmdArgs, options::OPT_u);

  ToolChain.AddFilePathLibArgs(Args, CmdArgs);

  if (D.isUsingLTO()) {
    assert(!Inputs.empty() && "Must have at least one input.");
    addLTOOptions(ToolChain, Args, CmdArgs, Output, Inputs[0],
                  D.getLTOMode() == LTOK_Thin);
  }

  C.addCommand(std::make_unique<Command>(JA, *this, ResponseFileSupport::None(),
                                         Exec, CmdArgs, Inputs, Output));
}

/// Orihime -- Orihime tool chain
Orihime::Orihime(const Driver &D, const llvm::Triple &Triple,
                 const ArgList &Args)
    : ToolChain(D, Triple, Args) {
  getProgramPaths().push_back(getDriver().getInstalledDir());
  if (getDriver().getInstalledDir() != D.Dir)
    getProgramPaths().push_back(D.Dir);

  if (!D.SysRoot.empty()) {
    SmallString<128> P(D.SysRoot);
    llvm::sys::path::append(P, "resource", "development", "library");
    getFilePaths().push_back(std::string(P.str()));
  }

  auto FilePaths = [&](const Multilib &M) -> std::vector<std::string> {
    std::vector<std::string> FP;
    if (D.CCCIsCXX()) {
      if (auto CXXStdlibPath = getCXXStdlibPath()) {
        SmallString<128> P(*CXXStdlibPath);
        llvm::sys::path::append(P, M.gccSuffix());
        FP.push_back(std::string(P.str()));
      }
    }
    return FP;
  };

  Multilibs.push_back(Multilib());
  // Use the noexcept variant with -fno-exceptions to avoid the extra overhead.
  Multilibs.push_back(Multilib("noexcept", {}, {}, 1)
                          .flag("-fexceptions")
                          .flag("+fno-exceptions"));
  Multilibs.FilterOut([&](const Multilib &M) {
    std::vector<std::string> RD = FilePaths(M);
    return std::all_of(RD.begin(), RD.end(),
                       [&](std::string P) { return !getVFS().exists(P); });
  });

  Multilib::flags_list Flags;
  addMultilibFlag(
      Args.hasFlag(options::OPT_fexceptions, options::OPT_fno_exceptions, true),
      "fexceptions", Flags);
  Multilibs.setFilePathsCallback(FilePaths);

  if (Multilibs.select(Flags, SelectedMultilib))
    if (!SelectedMultilib.isDefault())
      if (const auto &PathsCallback = Multilibs.filePathsCallback())
        for (const auto &Path : PathsCallback(SelectedMultilib))
          getFilePaths().insert(getFilePaths().begin(), Path);
}

std::string Orihime::ComputeEffectiveClangTriple(const llvm::opt::ArgList &Args,
                                                 types::ID InputType) const {
  llvm::Triple Triple(ComputeLLVMTriple(Args, InputType));
  return Triple.str();
}

Tool *Orihime::buildLinker() const { return new tools::orihime::Linker(*this); }

ToolChain::RuntimeLibType
Orihime::GetRuntimeLibType(const llvm::opt::ArgList &Args) const {
  if (Arg *A = Args.getLastArg(clang::driver::options::OPT_rtlib_EQ)) {
    StringRef Value = A->getValue();
    if (Value != "compiler-rt")
      getDriver().Diag(clang::diag::err_drv_invalid_rtlib_name)
          << A->getAsString(Args);
  }

  return ToolChain::RLT_CompilerRT;
}

ToolChain::CXXStdlibType
Orihime::GetCXXStdlibType(const llvm::opt::ArgList &Args) const {
  if (Arg *A = Args.getLastArg(options::OPT_stdlib_EQ)) {
    StringRef Value = A->getValue();
    if (Value != "libc++")
      getDriver().Diag(diag::err_drv_invalid_stdlib_name)
          << A->getAsString(Args);
  }

  return ToolChain::CST_Libcxx;
}

void Orihime::addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                                    llvm::opt::ArgStringList &CC1Args,
                                    Action::OffloadKind) const {
  if (!DriverArgs.hasFlag(options::OPT_fuse_init_array,
                          options::OPT_fno_use_init_array, true))
    CC1Args.push_back("-fno-use-init-array");

  // no float support yet
  CC1Args.push_back("-no-implicit-float");
}

void Orihime::AddClangSystemIncludeArgs(
    const llvm::opt::ArgList &DriverArgs,
    llvm::opt::ArgStringList &CC1Args) const {
  const Driver &D = getDriver();

  if (DriverArgs.hasArg(options::OPT_nostdinc))
    return;

  if (!DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    SmallString<128> P(D.ResourceDir);
    llvm::sys::path::append(P, "include");
    addSystemInclude(DriverArgs, CC1Args, P);
  }

  if (DriverArgs.hasArg(options::OPT_nostdlibinc))
    return;

  StringRef CIncludeDirs(C_INCLUDE_DIRS);
  if (CIncludeDirs != "") {
    SmallVector<StringRef, 5> dirs;
    CIncludeDirs.split(dirs, ":");
    for (StringRef dir : dirs) {
      StringRef Prefix =
          llvm::sys::path::is_absolute(dir) ? StringRef(D.SysRoot) : "";
      addExternCSystemInclude(DriverArgs, CC1Args, Prefix + dir);
    }
    return;
  }

  SmallString<128> P(D.SysRoot.empty() ? "/" : D.SysRoot);
  llvm::sys::path::append(P, "resource", "development", "include");
  addExternCSystemInclude(DriverArgs, CC1Args, P.str());
}

void Orihime::AddClangCXXStdlibIncludeArgs(
    const llvm::opt::ArgList &DriverArgs,
    llvm::opt::ArgStringList &CC1Args) const {
  if (DriverArgs.hasArg(options::OPT_nostdlibinc) ||
      DriverArgs.hasArg(options::OPT_nostdincxx))
    return;

  const Driver &D = getDriver();
  SmallString<128> P(D.SysRoot.empty() ? "/" : D.SysRoot);
  switch (GetCXXStdlibType(DriverArgs)) {
  case ToolChain::CST_Libcxx: {
    llvm::sys::path::append(P, "resource", "development", "include", "libcxx");
    addSystemInclude(DriverArgs, CC1Args, P.str());
    break;
  }

  default:
    llvm_unreachable("invalid stdlib name");
  }
}

void Orihime::AddCXXStdlibLibArgs(const llvm::opt::ArgList &Args,
                                  llvm::opt::ArgStringList &CmdArgs) const {
  switch (GetCXXStdlibType(Args)) {
  case ToolChain::CST_Libcxx:
    CmdArgs.push_back("-lc++");
    break;

  case ToolChain::CST_Libstdcxx:
    llvm_unreachable("invalid stdlib name");
  }
}

SanitizerMask Orihime::getSupportedSanitizers() const { return {}; }

SanitizerMask Orihime::getDefaultSanitizers() const { return {}; }
