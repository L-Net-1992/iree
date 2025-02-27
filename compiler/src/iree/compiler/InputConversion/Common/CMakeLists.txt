# Copyright 2022 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Enable input dialects based on options.
set(IREE_INPUT_DEPS "")
if(IREE_INPUT_MHLO)
  list(APPEND IREE_INPUT_DEPS iree::compiler::InputConversion::MHLO)
  list(APPEND IREE_INPUT_DEPS iree::compiler::InputConversion::StableHLO)
endif()
if(IREE_INPUT_TORCH)
  list(APPEND IREE_INPUT_DEPS iree::compiler::InputConversion::TMTensor)
endif()
if(IREE_INPUT_TOSA)
  list(APPEND IREE_INPUT_DEPS iree::compiler::InputConversion::TOSA)
endif()

iree_add_all_subdirs()

iree_tablegen_library(
  NAME
    PassesIncGen
  TD_FILE
    "Passes.td"
  OUTS
    --gen-pass-decls Passes.h.inc
)

iree_cc_library(
  NAME
    PassHeaders
  HDRS
    "PassDetail.h"
    "Passes.h"
    "Passes.h.inc"
  DEPS
    ::PassesIncGen
    MLIRPass
    MLIRTransforms
  PUBLIC
)

iree_cc_library(
  NAME
    Common
  HDRS
    "Passes.h"
    "Utils.h"
  SRCS
    "IREEImportPublic.cpp"
    "ImportMLProgram.cpp"
    "Passes.cpp"
    "QuantizedConvToConv.cpp"
    "QuantizedMatmulToMatmul.cpp"
    "SanitizeModuleNames.cpp"
    "TopLevelSCFToCFG.cpp"
    "Utils.cpp"
  DEPS
    ::PassHeaders
    ::PassesIncGen
    IREEInputDialect
    LLVMSupport
    MLIRArithDialect
    MLIRFuncDialect
    MLIRIR
    MLIRLinalgDialect
    MLIRLinalgTransforms
    MLIRLinalgUtils
    MLIRMLProgramDialect
    MLIRMemRefTransforms
    MLIRPass
    MLIRSCFDialect
    MLIRSCFToControlFlow
    MLIRTensorDialect
    MLIRTensorUtils
    MLIRTransforms
    iree::compiler::Dialect::Flow::IR
    iree::compiler::Dialect::HAL::IR
    iree::compiler::Dialect::Util::IR
    iree::compiler::Utils
  PUBLIC
)

iree_cc_library(
  NAME
    AutoInputConversionPipeline
  HDRS
    "Passes.h"
    "Utils.h"
  SRCS
    "AutoInputConversionPipeline.cpp"
  DEPS
    ${IREE_INPUT_DEPS}
    ::PassHeaders
    ::PassesIncGen
    LLVMSupport
    MLIRFuncDialect
    MLIRIR
    MLIRLinalgUtils
    MLIRPass
    MLIRTransforms
  PUBLIC
)

### BAZEL_TO_CMAKE_PRESERVES_ALL_CONTENT_BELOW_THIS_LINE ###
