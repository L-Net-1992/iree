// Copyright 2023 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Implements IREE-specific logic for lowering StableHLO collective ops to Flow
// dialect ops.

#include "iree/compiler/Dialect/Flow/IR/FlowDialect.h"
#include "iree/compiler/Dialect/Flow/IR/FlowOps.h"
#include "iree/compiler/Dialect/Flow/IR/FlowTypes.h"
#include "iree/compiler/InputConversion/StableHLO/Rewriters.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Transforms/DialectConversion.h"
#include "stablehlo/dialect/StablehloOps.h"

namespace mlir::iree_compiler::stablehlo {
namespace {

// Work in progress. The implementation is planned as several stages.
//
// For the first stage, a few simplifications are made to support simple models.
//
//   1. Single stream with deterministic order of execution
//   2. Single replica group for all collective ops
//   3. Only replicas without partition_id used
//
// These allow us to use a default channel for all communications, and there is
// 1:1 mapping from the replica IDs to the communication ranks. The attribute,
// use_global_device_ids, is always set in this case.
//
// The next stage is to support multiple replica groups. This needs a channel
// creation with a subset of processes, which should have another communication
// among the group. A possible strategy is to have the root process in the group
// (the first rank of the group) creates a channel and the other processes query
// the channel info from the root process. A key-value store using gRPC might be
// a good solution.
//
// Supporting partition_id comes next. This includes the support for various
// mode combinations for cross-replica and cross partition communication. See
// the stablehlo specification for more details about the different modes.

std::optional<IREE::Flow::CollectiveElementType>
convertToFlowCollectiveElementType(Type type) {
  if (type.isF32()) {
    return IREE::Flow::CollectiveElementType::Float32;
  }

  if (type.isInteger(32)) {
    if (type.isSignedInteger()) {
      return IREE::Flow::CollectiveElementType::Sint32;
    }
    return IREE::Flow::CollectiveElementType::Uint32;
  }

  if (type.isF16()) {
    return IREE::Flow::CollectiveElementType::Float16;
  }

  if (type.isInteger(8)) {
    if (type.isSignedInteger()) {
      return IREE::Flow::CollectiveElementType::Sint8;
    }
    return IREE::Flow::CollectiveElementType::Uint8;
  }

  if (type.isInteger(16)) {
    if (type.isSignedInteger()) {
      return IREE::Flow::CollectiveElementType::Sint16;
    }
    return IREE::Flow::CollectiveElementType::Uint16;
  }

  if (type.isBF16()) {
    return IREE::Flow::CollectiveElementType::BFloat16;
  }

  if (type.isF64()) {
    return IREE::Flow::CollectiveElementType::Float64;
  }

  if (type.isInteger(64)) {
    if (type.isSignedInteger()) {
      return IREE::Flow::CollectiveElementType::Sint64;
    }
    return IREE::Flow::CollectiveElementType::Uint64;
  }

  return std::nullopt;
}

std::optional<IREE::Flow::CollectiveReductionOp>
convertToFlowCollectiveReductionOp(const Operation &op) {
  if (isa<mlir::stablehlo::AddOp>(op)) {
    return IREE::Flow::CollectiveReductionOp::ReductionSum;
  }
  if (isa<mlir::stablehlo::MulOp>(op)) {
    return IREE::Flow::CollectiveReductionOp::ReductionProduct;
  }
  if (isa<mlir::stablehlo::MinOp>(op)) {
    return IREE::Flow::CollectiveReductionOp::ReductionMinimum;
  }
  if (isa<mlir::stablehlo::MaxOp>(op)) {
    return IREE::Flow::CollectiveReductionOp::ReductionMaximum;
  }
  // TODO: we may be able to detect an average operation and convert it
  // into IREE::Flow::CollectiveReductionOp::ReductionAverage.
  return std::nullopt;
}

IREE::Flow::CollectiveElementTypeAttr getCollectiveElementTypeAttr(
    MLIRContext *context, RankedTensorType type) {
  std::optional<IREE::Flow::CollectiveElementType> collectiveElemType =
      convertToFlowCollectiveElementType(type.getElementType());
  if (!collectiveElemType) {
    return IREE::Flow::CollectiveElementTypeAttr();
  }
  return IREE::Flow::CollectiveElementTypeAttr::get(context,
                                                    *collectiveElemType);
}

template <typename T>
LogicalResult checkCollectiveAttrs(T op, PatternRewriter &rewriter) {
  // Check there is only one group in the replica_groups
  ShapedType replicaGroupType = op.getReplicaGroups().getType();
  if (replicaGroupType.getRank() != 2 || replicaGroupType.getDimSize(0) != 1) {
    return rewriter.notifyMatchFailure(op, "must have a single replica group");
  }

  // Note that the channel handle attribute consists of two 64-bit values,
  // handle and type.
  int64_t handle =
      op.getChannelHandle() ? op.getChannelHandleAttr().getHandle() : 0;
  if (handle <= 0) {
    // When the channel handle attribute is not present, it means the
    // handle (a.k.a. channel_id in stablehlo) is 0. When this case is combined
    // with `use_global_device_ids=false`, the communication type is
    // `cross-replica`, but since there is only one replica group, it is
    // effectively the same as `flatten_ids`, which is supported.
    if (op.getUseGlobalDeviceIds()) {
      return rewriter.notifyMatchFailure(
          op, "must not set use_global_device_ids when channel_id <= 0");
    }
  } else if (!op.getUseGlobalDeviceIds()) {
    return rewriter.notifyMatchFailure(op, "must set use_global_device_ids");
  }

  return success();
}

Value emitTranspose(ConversionPatternRewriter &rewriter, Location loc,
                    Value input, int64_t srcDim, int64_t dstDim) {
  // Creates a transpose op that swaps dimensions srcDim and dstDim in the
  // input.
  auto inputType = cast<RankedTensorType>(input.getType());
  SmallVector<int64_t> inputShape(inputType.getShape());
  SmallVector<int64_t> permutation =
      llvm::to_vector(llvm::seq<int64_t>(0, inputShape.size()));
  std::swap(permutation[srcDim], permutation[dstDim]);
  std::swap(inputShape[srcDim], inputShape[dstDim]);
  DenseIntElementsAttr permutationAttr = rewriter.getI64VectorAttr(permutation);
  return rewriter
      .create<mlir::stablehlo::TransposeOp>(
          loc, RankedTensorType::get(inputShape, inputType.getElementType()),
          input, permutationAttr)
      .getResult();
}

/// Converts stablehlo.replica_id to flow.channel.default + flow.channel.rank.
/// TODO(okkwon): this assumes that there is no partition so that there is a 1:1
/// mapping between the replica ID and the process ID.
struct ReplicaIdOpConversion final
    : OpConversionPattern<mlir::stablehlo::ReplicaIdOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mlir::stablehlo::ReplicaIdOp op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    Location loc = op.getLoc();
    auto channel = rewriter.create<IREE::Flow::ChannelDefaultOp>(
        loc, /*group=*/StringAttr{});
    auto rank = rewriter.create<IREE::Flow::ChannelRankOp>(loc, channel);
    auto resultType = cast<RankedTensorType>(op.getType());  // tensor<ui32>
    Type elemType = resultType.getElementType();
    // index -> ui32
    auto rankElem = rewriter.create<arith::IndexCastUIOp>(loc, elemType, rank);
    // tensor<ui32>
    auto rankTensor = rewriter.create<tensor::FromElementsOp>(
        loc, resultType, rankElem.getResult());
    rewriter.replaceOp(op, rankTensor.getResult());
    return success();
  }
};

struct AllGatherOpConversion final
    : OpConversionPattern<mlir::stablehlo::AllGatherOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mlir::stablehlo::AllGatherOp op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    if (checkCollectiveAttrs(op, rewriter).failed()) {
      return failure();
    }

    // Currently only the default channel is used.
    Location loc = op.getLoc();

    // Create a default channel.
    auto channel = rewriter.create<IREE::Flow::ChannelDefaultOp>(
        loc, /*group=*/StringAttr{});

    // Get the collective element type attribute.
    auto resultType = cast<RankedTensorType>(op.getResult().getType());
    IREE::Flow::CollectiveElementTypeAttr elementTypeAttr =
        getCollectiveElementTypeAttr(op.getContext(), resultType);
    if (!elementTypeAttr) {
      return rewriter.notifyMatchFailure(
          op, "unsupported element type for collective op");
    }
    uint64_t allGatherDim = op.getAllGatherDim();
    Value gatherInput = op.getOperand();
    SmallVector<int64_t> gatherResultShape(resultType.getShape());

    // When all_gather_dim != 0, we need to transpose between 0 and
    // all_gather_dim before and after the flow all_gather op.
    bool requiresTranspose = allGatherDim != 0;
    if (requiresTranspose) {
      std::swap(gatherResultShape[0], gatherResultShape[allGatherDim]);
      gatherInput = emitTranspose(rewriter, loc, gatherInput, 0, allGatherDim);
    }

    // Create an empty tensor for the result.
    Value target = rewriter.create<tensor::EmptyOp>(
        loc, gatherResultShape, resultType.getElementType());
    Value gatherResult =
        rewriter
            .create<IREE::Flow::CollectiveAllGatherOp>(
                op.getLoc(), elementTypeAttr, target, gatherInput, channel)
            .getResult();

    if (requiresTranspose) {
      gatherResult =
          emitTranspose(rewriter, loc, gatherResult, allGatherDim, 0);
    }

    rewriter.replaceOp(op, gatherResult);
    return success();
  }
};

struct AllReduceOpConversion final
    : OpConversionPattern<mlir::stablehlo::AllReduceOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mlir::stablehlo::AllReduceOp op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    if (checkCollectiveAttrs(op, rewriter).failed()) {
      return failure();
    }

    // Only single elementwise op is supported.
    Block &block = op.getComputation().front();

    if (block.empty() || llvm::hasSingleElement(block) ||
        std::next(block.begin(), 2) != block.end()) {
      return rewriter.notifyMatchFailure(op, "must have two ops in the block");
    }

    if (block.getNumArguments() != 2) {
      return rewriter.notifyMatchFailure(op, "must have two block args");
    }

    Operation &op1 = block.front();
    Operation &op2 = *(++block.begin());

    if (op1.getNumResults() != 1 ||
        !op1.hasTrait<::mlir::OpTrait::Elementwise>()) {
      return rewriter.notifyMatchFailure(op, "must have elementwise trait");
    }

    // Convert stablehlo reduction op into flow reduction op.
    std::optional<IREE::Flow::CollectiveReductionOp> redOp =
        convertToFlowCollectiveReductionOp(op1);
    if (!redOp) {
      return rewriter.notifyMatchFailure(op, "unsupported operation.");
    }

    if (!op2.mightHaveTrait<OpTrait::IsTerminator>()) {
      return rewriter.notifyMatchFailure(op,
                                         "the second op must be a terminator");
    }

    // Currently only the default channel is used.
    Location loc = op.getLoc();

    // Create a default channel.
    auto channel = rewriter.create<IREE::Flow::ChannelDefaultOp>(
        loc, /*group=*/StringAttr{});

    // Convert stablehlo reduction op into flow reduction op.
    auto reductionOpAttr =
        IREE::Flow::CollectiveReductionOpAttr::get(op.getContext(), *redOp);

    auto inputType = cast<RankedTensorType>(op.getOperand().getType());

    // Get the collective element type attribute.
    IREE::Flow::CollectiveElementTypeAttr elementTypeAttr =
        getCollectiveElementTypeAttr(op.getContext(), inputType);
    if (!elementTypeAttr) {
      return rewriter.notifyMatchFailure(op, "unsupported input type");
    }

    // Create an empty tensor for the result.
    ArrayRef<int64_t> inputShape = inputType.getShape();
    Value target = rewriter.create<tensor::EmptyOp>(loc, inputShape,
                                                    inputType.getElementType());
    auto allReduceOp = rewriter.create<IREE::Flow::CollectiveAllReduceOp>(
        op.getLoc(), reductionOpAttr, elementTypeAttr, target, op.getOperand(),
        channel);
    rewriter.replaceOp(op, allReduceOp.getResult());
    return success();
  }
};

Value splitAndConcatForAllToAll(ConversionPatternRewriter &rewriter,
                                Location loc, Value input, uint64_t splitDim,
                                uint64_t concatDim, uint64_t splitCount) {
  // Helper function to rearrange data after all-to-all.
  auto inputType = cast<RankedTensorType>(input.getType());
  ArrayRef<int64_t> inputShape = inputType.getShape();

  // Reshape
  const int64_t rank = inputShape.size();
  llvm::SmallVector<int64_t> newShape;
  for (int64_t i = 0; i < rank; ++i) {
    if (i != splitDim) {
      newShape.push_back(inputShape[i]);
      continue;
    }
    newShape.push_back(splitCount);
    newShape.push_back(inputShape[i] / splitCount);
  }
  Value result =
      rewriter
          .create<mlir::stablehlo::ReshapeOp>(
              loc, RankedTensorType::get(newShape, inputType.getElementType()),
              input)
          .getResult();

  // Transpose
  SmallVector<int64_t> permutation;
  permutation.reserve(rank + 1);
  for (int64_t i = 0; i < rank; ++i) {
    int64_t dimAfterReshape = i >= splitDim ? i + 1 : i;
    if (i == concatDim) {
      permutation.push_back(splitDim);
    }
    permutation.push_back(dimAfterReshape);
  }
  SmallVector<int64_t> transposeResultShape;
  transposeResultShape.reserve(rank + 1);
  for (int64_t i = 0; i < rank + 1; ++i) {
    transposeResultShape.push_back(newShape[permutation[i]]);
  }

  result = rewriter
               .create<mlir::stablehlo::TransposeOp>(
                   loc,
                   RankedTensorType::get(transposeResultShape,
                                         inputType.getElementType()),
                   result, rewriter.getI64VectorAttr(permutation))
               .getResult();

  // Reshape
  llvm::SmallVector<int64_t> finalShape(inputShape);
  finalShape[concatDim] *= splitCount;
  finalShape[splitDim] /= splitCount;
  return rewriter
      .create<mlir::stablehlo::ReshapeOp>(
          loc, RankedTensorType::get(finalShape, inputType.getElementType()),
          result)
      .getResult();
}

struct AllToAllOpConversion final
    : OpConversionPattern<mlir::stablehlo::AllToAllOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mlir::stablehlo::AllToAllOp op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    Location loc = op.getLoc();

    // Create a channel.
    auto channel = rewriter.create<IREE::Flow::ChannelDefaultOp>(
        loc, /*group=*/StringAttr{});

    // Get the collective element type attribute.
    auto resultType = cast<RankedTensorType>(op.getType());
    IREE::Flow::CollectiveElementTypeAttr elementTypeAttr =
        getCollectiveElementTypeAttr(op.getContext(), resultType);
    if (!elementTypeAttr) {
      return rewriter.notifyMatchFailure(
          op, "unsupported element type for collective op");
    }

    uint64_t splitDim = op.getSplitDimension();
    uint64_t concatDim = op.getConcatDimension();
    uint64_t splitCount = op.getSplitCount();
    Value allToAllInput = op.getOperand();

    // When splitDim != 0, we need to transpose splitDim to 0 before and after
    // the all-to-all.
    bool requiresTranspose = splitDim != 0;
    // When the concatDim != splitDim, we need to rearrange the data after the
    // all-to-all.
    bool requiresSplitAndConcat = concatDim != splitDim;
    if (requiresTranspose) {
      allToAllInput = emitTranspose(rewriter, loc, allToAllInput, 0, splitDim);
    }

    // Create an empty tensor for the result.
    Value target = rewriter.create<tensor::EmptyOp>(
        loc, cast<RankedTensorType>(allToAllInput.getType()).getShape(),
        resultType.getElementType());
    // Create all-to-all.
    Value allToAllResult = rewriter
                               .create<IREE::Flow::CollectiveAllToAllOp>(
                                   op.getLoc(), elementTypeAttr, target,
                                   allToAllInput, channel.getResult())
                               .getResult();

    if (requiresTranspose) {
      allToAllResult =
          emitTranspose(rewriter, loc, allToAllResult, splitDim, 0);
    }
    if (requiresSplitAndConcat) {
      allToAllResult = splitAndConcatForAllToAll(
          rewriter, loc, allToAllResult, splitDim, concatDim, splitCount);
    }

    rewriter.replaceOp(op, allToAllResult);
    return success();
  }
};

struct ReduceScatterOpConversion final
    : OpConversionPattern<mlir::stablehlo::ReduceScatterOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mlir::stablehlo::ReduceScatterOp op, OpAdaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    if (checkCollectiveAttrs(op, rewriter).failed()) {
      return failure();
    }

    // Only single elementwise op is supported.
    Block &block = op.getComputation().front();

    if (block.empty() || llvm::hasSingleElement(block) ||
        std::next(block.begin(), 2) != block.end()) {
      return rewriter.notifyMatchFailure(op, "must have two ops in the block");
    }

    if (block.getNumArguments() != 2) {
      return rewriter.notifyMatchFailure(op, "must have two block args");
    }

    Operation &op1 = block.front();
    Operation &op2 = *(++block.begin());

    if (op1.getNumResults() != 1 ||
        !op1.hasTrait<::mlir::OpTrait::Elementwise>()) {
      return rewriter.notifyMatchFailure(op, "must have elementwise trait");
    }

    // Convert stablehlo reduction op into flow reduction op.
    std::optional<IREE::Flow::CollectiveReductionOp> redOp =
        convertToFlowCollectiveReductionOp(op1);
    if (!redOp) {
      return rewriter.notifyMatchFailure(op, "unsupported operation.");
    }

    if (!op2.mightHaveTrait<OpTrait::IsTerminator>()) {
      return rewriter.notifyMatchFailure(op,
                                         "the second op must be a terminator");
    }

    // Convert stablehlo reduction op into flow reduction op.
    auto reductionOpAttr =
        IREE::Flow::CollectiveReductionOpAttr::get(op.getContext(), *redOp);

    // Currently only the default channel is used.
    Location loc = op.getLoc();

    // Create a default channel.
    auto channel = rewriter.create<IREE::Flow::ChannelDefaultOp>(
        loc, /*group=*/StringAttr{});

    // Get the collective element type attribute.
    auto resultType = cast<RankedTensorType>(op.getResult().getType());
    IREE::Flow::CollectiveElementTypeAttr elementTypeAttr =
        getCollectiveElementTypeAttr(op.getContext(), resultType);
    if (!elementTypeAttr) {
      return rewriter.notifyMatchFailure(op, "unsupported input type");
    }

    // When scatter_dimension != 0, we need to transpose between 0 and
    // scatter_dimension before and after the flow reduce_scatter op.
    uint64_t scatterDim = op.getScatterDimension();
    auto inputType = cast<RankedTensorType>(op.getOperand().getType());
    SmallVector<int64_t> reduceInputShape(inputType.getShape());
    Value reduceInput = op.getOperand();
    DenseIntElementsAttr permutationAttr;

    SmallVector<int64_t> scatterResultShape(resultType.getShape());
    auto elemType = resultType.getElementType();

    if (scatterDim != 0) {
      auto permutation =
          llvm::to_vector(llvm::seq<int64_t>(0, scatterResultShape.size()));
      std::swap(permutation[0], permutation[scatterDim]);
      permutationAttr = rewriter.getI64VectorAttr(permutation);
      std::swap(reduceInputShape[0], reduceInputShape[scatterDim]);
      std::swap(scatterResultShape[0], scatterResultShape[scatterDim]);
      // Transpose the input.
      reduceInput =
          rewriter
              .create<mlir::stablehlo::TransposeOp>(
                  loc, RankedTensorType::get(reduceInputShape, elemType),
                  reduceInput, permutationAttr)
              .getResult();
    }

    // Create an empty tensor for the result.
    Value target = rewriter.create<tensor::EmptyOp>(
        loc, scatterResultShape, resultType.getElementType());
    Value scatterResult = rewriter
                              .create<IREE::Flow::CollectiveReduceScatterOp>(
                                  op.getLoc(), reductionOpAttr, elementTypeAttr,
                                  target, reduceInput, channel)
                              .getResult();

    if (scatterDim != 0) {
      scatterResult = rewriter
                          .create<mlir::stablehlo::TransposeOp>(
                              loc, resultType, scatterResult, permutationAttr)
                          .getResult();
    }

    rewriter.replaceOp(op, scatterResult);
    return success();
  }
};

}  // namespace

void populateStableHloCollectivesConversionPatterns(
    MLIRContext *context, TypeConverter &typeConverter,
    RewritePatternSet *patterns) {
  patterns
      ->add<AllGatherOpConversion, AllReduceOpConversion, AllToAllOpConversion,
            ReduceScatterOpConversion, ReplicaIdOpConversion>(typeConverter,
                                                              context);
}

}  // namespace mlir::iree_compiler::stablehlo
