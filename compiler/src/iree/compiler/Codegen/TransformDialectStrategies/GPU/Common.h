// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_COMPILER_CODEGEN_TRANSFORM_DIALECT_STRATEGIES_GPU_COMMON_H_
#define IREE_COMPILER_CODEGEN_TRANSFORM_DIALECT_STRATEGIES_GPU_COMMON_H_

#include "iree/compiler/Codegen/TransformDialectStrategies/GPU/AbstractGemmLikeStrategy.h"
#include "iree/compiler/Codegen/TransformDialectStrategies/GPU/AbstractReductionStrategy.h"
#include "llvm/ADT/StringRef.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/IR/BuiltinOps.h"

namespace mlir {
namespace iree_compiler {
namespace gpu {

struct AbstractReductionStrategy;

//===----------------------------------------------------------------------===//
// General helpers.
//===----------------------------------------------------------------------===//
static constexpr int64_t kCudaWarpSize = 32;
static constexpr int64_t kCudaMaxNumThreads = 1024;
static constexpr int64_t kCudaMaxVectorLoadBitWidth = 128;

/// Return max(1, (value * 32) / bitWidth).
int64_t scaleUpByBitWidth(int64_t value, int64_t bitWidth);

/// Adjust the number of warps to use to benefit from packing multiple smaller
/// elemental types within a single 128 bit shuffled element.
int64_t adjustNumberOfWarpsForBlockShuffle(int64_t numWarpsToUse,
                                           int64_t bitWidth);

//===----------------------------------------------------------------------===//
// Low-level reusable retargetable builder APIs, follow MLIR-style builders.
//===----------------------------------------------------------------------===//
/// Post-bufferization mapping to blocks and threads.
/// Takes a handle to a func.func and returns an updated handle to a
/// func.func.
/// Takes an optional `warpDims` argument to specify the number of warp
/// dimensions to consider along various dimensions and avoid second-guessing
/// how the mapping to warps should occur.
Value buildMapToBlockAndThreads(ImplicitLocOpBuilder& b, Value funcH,
                                ArrayRef<int64_t> blockSize,
                                ArrayRef<int64_t> warpDims = {});

/// Post-bufferization vector distribution with rank-reduction.
/// Takes a handle to a func.func and returns an updated handle to a
/// func.func.
Value buildDistributeVectors(ImplicitLocOpBuilder& b, Value variantH,
                             Value funcH, int64_t warpSize = kCudaWarpSize);

/// Take care of the last common steps in a GPU strategy (i.e. vectorize,
/// bufferize, maps to blocks and threads and distribute vectors).
/// Return the handles to the updated variant and the func::FuncOp ops under
/// the variant op.
// TODO: abstract away AbstractReductionStrategy, this is supposed to be
// retargetable.
std::pair<Value, Value> buildCommonTrailingStrategy(
    ImplicitLocOpBuilder& b, Value variantH,
    const AbstractReductionStrategy& strategy);

//===----------------------------------------------------------------------===//
// Mid-level problem-specific strategy builder APIs, follow MLIR-style builders.
//===----------------------------------------------------------------------===//
/// Take a handle `opH` to a Linalg op of rank `rank`, sizes `opSizes` and for
/// which we know the most minor dimension `mostMinorDim` (assuming all accesses
/// are contiguous along that dimension for now).
/// Build a schedule that maps `mostMinorDim` to a `scf.forall` op.
/// When `numThreads` > 1, the `scf.forall` is also mapped to
/// `mappingAttr` (which must then be non-null).
/// The constructed schedule first performs a split of the largest possible
/// multiple of `numThreads * maxVectorSize` to form a maximally divisible
/// region.
// TODO: More robustness wrt selecting the most minor dimension otherwise
// performance may suffer.
// TODO: Split point should be dynamic and aware of future stride / alignment
// to also guarantee proper vector alignments. OTOH this is a non-trivial bump
// in schedule complexity and can be handled with simple padding of the
// underlying allocation.
void build1DSplittingStrategyWithOptionalThreadMapping(
    ImplicitLocOpBuilder& b, Value isolatedParentOpH, Value opH, int64_t rank,
    int64_t mostMinorDim, SmallVector<int64_t> opSizes, int64_t numThreads,
    Attribute mappingAttr = Attribute(), int64_t maxVectorSize = 4);

/// Build the transform IR to pad a matmul op `matmulOpH`.
Value buildPadMatmul(ImplicitLocOpBuilder &b, Value matmulOpH,
                     const AbstractGemmLikeStrategy &strategy);

/// Build transform IR to hoist the padded output operand of a padded matmul.
/// Additionally, this attempts to fold the padding into the producing fill, if
/// available.
// TODO: Generalize, this is not specific to a matmul.
// TODO: Better API
Value buildHoistOutputPaddingOp(ImplicitLocOpBuilder &b, Value variantH,
                                Value paddedMatmulOpH,
                                int64_t numLoopsToHoist = 1);

/// Helper function to distribute one pad or copy operation.
/// Note: When `foldIfBranch` is true, one must later perform masked
/// vectorization of the result.
/// This amounts to injecting knowledge about future transformations without
/// adding leaky semantics.
Value buildDistributeOnePadOrCopy(ImplicitLocOpBuilder &b, Value variantH,
                                  Value copyOpH, ArrayRef<int64_t> numThreads,
                                  ArrayRef<Attribute> threadDimMapping,
                                  bool foldIfBranch = false);

/// Distribute the explicit copies involved in a matmul operation
/// `paddedMatmulOpH`.
std::tuple<Value, Value, Value> buildDistributeCopies(
    ImplicitLocOpBuilder &b, Value variantH, Value paddedMatmulOpH,
    const AbstractGemmLikeStrategy &strategy);

/// Specific pattern to perform masked vectorization of copies give as
/// parameters, cleanup and vectorize the rest.
void buildMatmulVectorization(ImplicitLocOpBuilder &b, Value variantH,
                              Value lhsCopyOpH, Value rhsCopyOpH,
                              Value copyBackOpH,
                              const AbstractGemmLikeStrategy &strategy);

/// Build the transform IR to perform conversion to tensor core operations.
/// This is currently subject to phase orderings as follows:
///   - Vector transfer_read and transfer_write patterns have different subview
///     folding behavior, force a fold_memref_aliases on them to enable
///     redundant vector transfer hoisting.
///   - Unfortunately, fold_memref_aliases breaks vector_to_mma conversion
///     across scf.for after unrolling due to insert_strided_slice /
///     extract_strided_slice across iter_args boundaries.
///   - Hoist redundant vector transfers to allow conversion to tensor core to
///     proceed. We really don't want to do this after bufferization but we need
///     to atm.
Value buildConvertToTensorCoreOp(ImplicitLocOpBuilder &b, Value funcH,
                                 const AbstractGemmLikeStrategy &strategy);

void buildMultiBuffering(ImplicitLocOpBuilder &b, Value funcH,
                         const AbstractGemmLikeStrategy &strategy);

Value buildConvertToAsyncCopies(ImplicitLocOpBuilder &b, Value funcH,
                                const AbstractGemmLikeStrategy &strategy);

void buildPipelineSharedMemoryCopies(ImplicitLocOpBuilder &b, Value funcH,
                                     const AbstractGemmLikeStrategy &strategy);

Value buildBufferize(ImplicitLocOpBuilder &b, Value variantH);

//===----------------------------------------------------------------------===//
// Higher-level problem-specific strategy creation APIs, these should favor
// user-friendliness.
//===----------------------------------------------------------------------===//
/// Placeholder for some hardware model proxy that contains relevant information
/// to configure the reduction strategy. In the future, this will need to be
/// driven by some contract with the runtime.
struct GPUModel {
  static constexpr StringLiteral kDefaultGPU = "DefaultGPU";
  StringRef model = kDefaultGPU;
  bool hasWarpShuffle = false;
  bool hasTF32TensorCore = false;
};

/// Try to find an exisiting transform dialect strategy for a given entry point.
LogicalResult matchAndSetTransformStrategy(func::FuncOp entryPoint,
                                           Operation* op,
                                           const GPUModel& gpuModel);

}  // namespace gpu
}  // namespace iree_compiler
}  // namespace mlir

#endif  // IREE_COMPILER_CODEGEN_TRANSFORM_DIALECT_STRATEGIES_GPU_COMMON_H_
