//===-- MSILTargetInfo.cpp - MSIL Target Implementation -------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "MSILBackend.h"
#include "MSILTargetMachine.h"
#include "llvm/Module.h"
#include "llvm/Support/TargetRegistry.h"
using namespace llvm;

Target llvm::TheMSILTarget;

extern "C" void LLVMInitializeMSILTargetInfo() {
  RegisterTarget<Triple::cil, /*HasJIT=*/false>
    X(TheMSILTarget, "cil", "Common Intermediate Language");
}

extern "C" void LLVMInitializeMSILTargetMC() {}
