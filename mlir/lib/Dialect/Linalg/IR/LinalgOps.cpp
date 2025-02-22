//===- LinalgOps.cpp - Implementation of the linalg operations ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the Linalg operations.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Linalg/IR/LinalgOps.h"

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Linalg/EDSC/Intrinsics.h"
#include "mlir/Dialect/Linalg/IR/LinalgTypes.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/PatternMatch.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;
using namespace mlir::linalg;

/// Fully compose map with operands and canonicalize the result.
/// Return the `createOrFold`'ed AffineApply op.
static Value createFoldedComposedAffineApply(OpBuilder &b, Location loc,
                                             AffineMap map,
                                             ValueRange operandsRef) {
  SmallVector<Value, 4> operands(operandsRef.begin(), operandsRef.end());
  fullyComposeAffineMapAndOperands(&map, &operands);
  canonicalizeMapAndOperands(&map, &operands);
  return b.createOrFold<AffineApplyOp>(loc, map, operands);
}

SmallVector<Value, 4> mlir::linalg::applyMapToValues(OpBuilder &b, Location loc,
                                                     AffineMap map,
                                                     ValueRange values) {
  SmallVector<Value, 4> res;
  res.reserve(map.getNumResults());
  unsigned numDims = map.getNumDims(), numSym = map.getNumSymbols();
  // For each `expr` in `map`, applies the `expr` to the values extracted from
  // ranges. If the resulting application can be folded into a Value, the
  // folding occurs eagerly.
  for (auto expr : map.getResults()) {
    AffineMap map = AffineMap::get(numDims, numSym, expr);
    res.push_back(createFoldedComposedAffineApply(b, loc, map, values));
  }
  return res;
}

SmallVector<Value, 4> LinalgOp::createFlatListOfOperandDims(OpBuilder &b,
                                                            Location loc) {
  SmallVector<Value, 4> res;
  SmallVector<unsigned, 4> ranks;
  for (Value v : getShapedOperands()) {
    ShapedType t = v.getType().template cast<ShapedType>();
    ranks.push_back(t.getRank());
    for (unsigned i = 0; i < t.getRank(); ++i)
      res.push_back(b.create<DimOp>(loc, v, i));
  }

  // TODO: drop the following once symbol_source is deleted.
  auto attr = getAttrOfType<IntegerAttr>("symbol_source");
  if (!attr)
    return res;

  // Find the correct position for inserting values for symbols.
  unsigned numSymb = ranks[attr.getInt()], symbolsPos = 0;
  for (unsigned idx = 0, e = attr.getInt(); idx < e; idx++)
    symbolsPos += ranks[idx];

  // Append the end of the value list that corresponds to the
  // values mapping to symbols. Since inside concatenated map symbols
  // are repeated we have to repeat the sizes as well.

  // Reserve is mandatory to avoid a potential undefined behavior with
  // pushing back to smallvector from itself.
  res.reserve(res.size() + ranks.size() * numSymb);
  for (unsigned idx = 0, s = ranks.size(); idx < s; ++idx)
    for (unsigned idx2 = 0; idx2 < numSymb; ++idx2)
      res.push_back(res[symbolsPos + idx2]);
  return res;
}

SmallVector<Range, 4> LinalgOp::createLoopRanges(OpBuilder &b, Location loc) {
  AffineMap map = getLoopsToShapesMap();
  unsigned numDims = map.getNumDims(), numRes = map.getNumResults();
  // TODO: drop numSym once symbol_source is deleted.
  unsigned numSym = map.getNumSymbols();
  auto viewSizes = createFlatListOfOperandDims(b, loc);
  SmallVector<Range, 4> res(numDims);
  Value zeroVal = b.create<ConstantIndexOp>(loc, 0);
  Value oneVal = b.create<ConstantIndexOp>(loc, 1);
  for (unsigned idx = 0; idx < numRes; ++idx) {
    auto result = map.getResult(idx);
    if (auto d = result.dyn_cast<AffineDimExpr>()) {
      if (res[d.getPosition()].offset)
        continue;
      res[d.getPosition()] = Range{zeroVal, viewSizes[idx], oneVal};
    }

    // TODO: drop the following once symbol_source is deleted.
    // If the access pattern is of form (m, n)[s] -> (m + n - s floordiv 2),
    // then the bounds are:
    //   (s floordiv 2) <= m <= (size(m) + s floordiv 2 - s + 1).
    // where size(n) is applied to the symbol s.
    // This is done statically now.
    if (auto binOp = result.dyn_cast<AffineBinaryOpExpr>()) {
      auto lhs = binOp.getLHS().dyn_cast<AffineBinaryOpExpr>();
      auto rhs = binOp.getRHS().dyn_cast<AffineBinaryOpExpr>();
      if (!lhs || !rhs || binOp.getKind() != AffineExprKind::Add ||
          lhs.getKind() != AffineExprKind::Add ||
          rhs.getKind() != mlir::AffineExprKind::Mul)
        continue;

      auto m = lhs.getLHS().dyn_cast<AffineDimExpr>();
      auto n = lhs.getRHS().dyn_cast<AffineDimExpr>();
      auto fDiv = rhs.getLHS().dyn_cast<AffineBinaryOpExpr>();
      auto minusOne = rhs.getRHS().dyn_cast<AffineConstantExpr>();
      if (!m || !n || !fDiv || !minusOne ||
          fDiv.getKind() != AffineExprKind::FloorDiv ||
          !fDiv.getLHS().isa<AffineSymbolExpr>() ||
          !fDiv.getRHS().isa<AffineConstantExpr>())
        continue;

      auto s = fDiv.getLHS().dyn_cast<AffineSymbolExpr>();
      if (minusOne.getValue() != -1)
        continue;

      int mPos = m.getPosition();
      AffineExpr one = getAffineConstantExpr(1, s.getContext());
      AffineExpr sizeOfM = getAffineSymbolExpr(numSym, s.getContext());
      // Construction of upper bound (size(m) + s floordiv 2 - s + 1).
      AffineExpr upperOffsetExpr = sizeOfM + fDiv + one - s;
      AffineMap fromMap = AffineMap::get(numDims, numSym + 1, fDiv);
      AffineMap toMap = AffineMap::get(numDims, numSym + 1, upperOffsetExpr);
      SmallVector<Value, 8> values(viewSizes.begin(),
                                   viewSizes.begin() + numDims);
      values.insert(values.end(), viewSizes.begin() + numRes, viewSizes.end());
      values.push_back(viewSizes[mPos]);
      // Construction of the lower bound (s floordiv 2).
      Value from = applyMapToValues(b, loc, fromMap, values).front();
      Value to = applyMapToValues(b, loc, toMap, values).front();
      res[mPos] = Range{from, to, oneVal};
    }
  }
  return res;
}

/// Forward declarations.
template <typename NamedStructuredOpType>
static void buildNamedStructuredOpRegionAndAttributes(
    OpBuilder &opBuilder, OperationState &result, TypeRange inputTypes,
    TypeRange outputBufferTypes, TypeRange initTensorTypes,
    TypeRange resultTypes);

static ParseResult
parseCommonStructuredOpParts(OpAsmParser &parser, OperationState &result,
                             SmallVectorImpl<Type> &inputTypes,
                             SmallVectorImpl<Type> &outputBufferTypes,
                             SmallVectorImpl<Type> &initTensorTypes);

template <typename NamedStructuredOpType>
static ParseResult
parseNamedStructuredOpRegion(OpAsmParser &parser, Region &region,
                             TypeRange inputTypes, TypeRange outputBufferTypes,
                             TypeRange initTensorTypes, TypeRange resultTypes);
static ParseResult
parseNamedStructuredOpResults(OpAsmParser &parser,
                              SmallVectorImpl<Type> &resultTypes);

template <typename NamedStructuredOpType>
static ParseResult parseNamedStructuredOp(OpAsmParser &parser,
                                          OperationState &result);

template <typename NamedStructuredOpType>
static void printCommonStructuredOpParts(OpAsmPrinter &p,
                                         NamedStructuredOpType op);

static void printNamedStructuredOpResults(OpAsmPrinter &p,
                                          TypeRange resultTypes);

template <typename NamedStructuredOpType>
static void printNamedStructuredOp(OpAsmPrinter &p, NamedStructuredOpType op);

template <typename NamedStructuredOpType>
static LogicalResult verifyNamedStructuredOp(NamedStructuredOpType op);

/// This is a common class used for patterns of the form
/// ```
///    someop(memrefcast) -> someop
/// ```
/// It folds the source of the memref_cast into the root operation directly.
static LogicalResult foldMemRefCast(Operation *op) {
  bool folded = false;
  for (OpOperand &operand : op->getOpOperands()) {
    auto castOp = operand.get().getDefiningOp<MemRefCastOp>();
    if (castOp && canFoldIntoConsumerOp(castOp)) {
      operand.set(castOp.getOperand());
      folded = true;
    }
  }
  return success(folded);
}

///////////////////// Operations defined with Tablegen /////////////////////////
// For such operations that do not correspond to library calls (i.e. defined in
// LinalgOps.td), we define an overloaded `print` function and a
// parse`className` function.

//===----------------------------------------------------------------------===//
// GenericOps
//===----------------------------------------------------------------------===//
void GenericOp::build(
    OpBuilder &builder, OperationState &result, TypeRange resultTensorTypes,
    ValueRange inputs, ValueRange outputBuffers, ValueRange initTensors,
    ArrayRef<AffineMap> indexingMaps, ArrayRef<StringRef> iteratorTypes,
    StringRef doc, StringRef libraryCall, IntegerAttr symbolSource,
    function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuild) {
  build(builder, result, resultTensorTypes, inputs, outputBuffers, initTensors,
        builder.getAffineMapArrayAttr(indexingMaps),
        builder.getStrArrayAttr(iteratorTypes),
        doc.empty() ? StringAttr() : builder.getStringAttr(doc),
        libraryCall.empty() ? StringAttr() : builder.getStringAttr(libraryCall),
        ArrayAttr(), symbolSource);
  if (!bodyBuild)
    return;

  SmallVector<Type, 4> blockArgTypes;
  for (ValueRange container : {inputs, outputBuffers, initTensors})
    for (Value v : container)
      blockArgTypes.push_back(v.getType().cast<ShapedType>().getElementType());

  OpBuilder::InsertionGuard guard(builder);
  auto &region = *result.regions.front();
  Block *bodyBlock = builder.createBlock(&region, region.end(), blockArgTypes);
  bodyBuild(builder, result.location, bodyBlock->getArguments());
}

void GenericOp::build(
    OpBuilder &builder, OperationState &result, ValueRange inputs,
    ValueRange outputBuffers, ArrayRef<AffineMap> indexingMaps,
    ArrayRef<StringRef> iteratorTypes, StringRef doc, StringRef libraryCall,
    IntegerAttr symbolSource,
    function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuild) {
  build(builder, result, TypeRange{}, inputs, outputBuffers, ValueRange{},
        indexingMaps, iteratorTypes, doc, libraryCall, symbolSource, bodyBuild);
}

void GenericOp::build(
    OpBuilder &builder, OperationState &result, ValueRange inputs,
    ValueRange outputBuffers, ArrayRef<AffineMap> indexingMaps,
    ArrayRef<StringRef> iteratorTypes,
    function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuild) {
  build(builder, result, inputs, outputBuffers, indexingMaps, iteratorTypes,
        /*doc=*/"",
        /*libraryCall=*/"",
        /*symbolSource=*/IntegerAttr(), bodyBuild);
}

void GenericOp::build(
    OpBuilder &builder, OperationState &result, TypeRange resultTensorTypes,
    ValueRange inputs, ValueRange outputBuffers, ValueRange initTensors,
    ArrayRef<AffineMap> indexingMaps, ArrayRef<StringRef> iteratorTypes,
    function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuild) {
  build(builder, result, resultTensorTypes, inputs, outputBuffers, initTensors,
        indexingMaps, iteratorTypes,
        /*doc=*/"",
        /*libraryCall=*/"",
        /*symbolSource=*/IntegerAttr(), bodyBuild);
}

void IndexedGenericOp::build(
    OpBuilder &builder, OperationState &result, TypeRange resultTensorTypes,
    ValueRange inputs, ValueRange outputBuffers, ValueRange initTensors,
    ArrayRef<AffineMap> indexingMaps, ArrayRef<StringRef> iteratorTypes,
    StringRef doc, StringRef libraryCall, IntegerAttr symbolSource,
    function_ref<void(OpBuilder &, Location, ValueRange, ValueRange)>
        bodyBuild) {
  build(builder, result, resultTensorTypes, inputs, outputBuffers, initTensors,
        builder.getAffineMapArrayAttr(indexingMaps),
        builder.getStrArrayAttr(iteratorTypes),
        doc.empty() ? StringAttr() : builder.getStringAttr(doc),
        libraryCall.empty() ? StringAttr() : builder.getStringAttr(libraryCall),
        ArrayAttr(), symbolSource);
  if (!bodyBuild)
    return;

  unsigned nLoops = iteratorTypes.size();
  SmallVector<Type, 4> blockArgTypes(nLoops, builder.getIndexType());
  for (ValueRange container : {inputs, outputBuffers, initTensors})
    for (Value v : container)
      blockArgTypes.push_back(v.getType().cast<ShapedType>().getElementType());

  OpBuilder::InsertionGuard guard(builder);
  auto &region = *result.regions.front();
  Block *bodyBlock = builder.createBlock(&region, region.end(), blockArgTypes);
  bodyBuild(builder, result.location,
            bodyBlock->getArguments().take_front(nLoops),
            bodyBlock->getArguments().drop_front(nLoops));
}

void IndexedGenericOp::build(
    OpBuilder &builder, OperationState &result, ValueRange inputs,
    ValueRange outputBuffers, ArrayRef<AffineMap> indexingMaps,
    ArrayRef<StringRef> iteratorTypes, StringRef doc, StringRef libraryCall,
    IntegerAttr symbolSource,
    function_ref<void(OpBuilder &, Location, ValueRange, ValueRange)>
        bodyBuild) {
  build(builder, result, TypeRange{}, inputs, outputBuffers, ValueRange{},
        indexingMaps, iteratorTypes, doc, libraryCall, symbolSource, bodyBuild);
}

void IndexedGenericOp::build(
    OpBuilder &builder, OperationState &result, ValueRange inputs,
    ValueRange outputBuffers, ArrayRef<AffineMap> indexingMaps,
    ArrayRef<StringRef> iteratorTypes,
    function_ref<void(OpBuilder &, Location, ValueRange, ValueRange)>
        bodyBuild) {
  build(builder, result, inputs, outputBuffers, indexingMaps, iteratorTypes,
        /*doc=*/"",
        /*libraryCall=*/"",
        /*symbolSource=*/IntegerAttr(), bodyBuild);
}

void IndexedGenericOp::build(
    OpBuilder &builder, OperationState &result, TypeRange resultTensorTypes,
    ValueRange inputs, ValueRange outputBuffers, ValueRange initTensors,
    ArrayRef<AffineMap> indexingMaps, ArrayRef<StringRef> iteratorTypes,
    function_ref<void(OpBuilder &, Location, ValueRange, ValueRange)>
        bodyBuild) {
  build(builder, result, resultTensorTypes, inputs, outputBuffers, initTensors,
        indexingMaps, iteratorTypes,
        /*doc=*/"",
        /*libraryCall=*/"",
        /*symbolSource=*/IntegerAttr(), bodyBuild);
}

template <typename GenericOpType>
static void printGenericOp(OpAsmPrinter &p, GenericOpType op) {
  p << op.getOperationName() << " ";

  // Print extra attributes.
  auto genericAttrNames = op.linalgTraitAttrNames();

  llvm::StringSet<> genericAttrNamesSet;
  genericAttrNamesSet.insert(genericAttrNames.begin(), genericAttrNames.end());
  SmallVector<NamedAttribute, 8> genericAttrs;
  for (auto attr : op.getAttrs())
    if (genericAttrNamesSet.count(attr.first.strref()) > 0)
      genericAttrs.push_back(attr);
  if (!genericAttrs.empty()) {
    auto genericDictAttr = DictionaryAttr::get(genericAttrs, op.getContext());
    p << genericDictAttr;
  }

  // Printing is shared with named ops, except for the region and attributes
  printCommonStructuredOpParts(p, op);

  genericAttrNames.push_back("operand_segment_sizes");
  genericAttrNamesSet.insert(genericAttrNames.back());

  bool hasExtraAttrs = false;
  for (NamedAttribute n : op.getAttrs()) {
    if ((hasExtraAttrs = !genericAttrNamesSet.contains(n.first.strref())))
      break;
  }
  if (hasExtraAttrs) {
    p << " attrs = ";
    p.printOptionalAttrDict(op.getAttrs(), /*elidedAttrs=*/genericAttrNames);
  }

  // Print region.
  if (!op.region().empty())
    p.printRegion(op.region());

  // Print results.
  printNamedStructuredOpResults(p, op.result_tensors().getTypes());
}

static void print(OpAsmPrinter &p, GenericOp op) { printGenericOp(p, op); }

static void print(OpAsmPrinter &p, IndexedGenericOp op) {
  printGenericOp(p, op);
}

static ParseResult parseGenericOp(OpAsmParser &parser, OperationState &result) {
  DictionaryAttr dictAttr;
  // Parse the core linalg traits that must check into a dictAttr.
  // The name is unimportant as we will overwrite result.attributes.
  // The core linalg traits must contain the information necessary to pass the
  // verifier.
  if (parser.parseAttribute(dictAttr, "_", result.attributes))
    return failure();
  result.attributes.assign(dictAttr.getValue().begin(),
                           dictAttr.getValue().end());

  // Parsing is shared with named ops, except for the region.
  SmallVector<Type, 1> inputTypes, outputBufferTypes, initTensorTypes;
  if (parseCommonStructuredOpParts(parser, result, inputTypes,
                                   outputBufferTypes, initTensorTypes))
    return failure();

  // Optional attributes may be added.
  if (succeeded(parser.parseOptionalKeyword("attrs")))
    if (failed(parser.parseEqual()) ||
        failed(parser.parseOptionalAttrDict(result.attributes)))
      return failure();

  SmallVector<OpAsmParser::OperandType, 8> regionOperands;
  std::unique_ptr<Region> region = std::make_unique<Region>();
  SmallVector<Type, 8> operandTypes, regionTypes;
  if (parser.parseRegion(*region, regionOperands, regionTypes))
    return failure();
  result.addRegion(std::move(region));

  // Generic ops may specify that a subset of its outputs are tensors. Such
  // outputs are specified in the result type.
  // TODO: may need to move output parsing before region parsing.
  // Need to wait for declarative assembly resolution to decide.
  SmallVector<Type, 1> outputTensorsTypes;
  if (parseNamedStructuredOpResults(parser, outputTensorsTypes))
    return failure();
  result.addTypes(outputTensorsTypes);

  return success();
}

static void getGenericEffectsImpl(
    SmallVectorImpl<SideEffects::EffectInstance<MemoryEffects::Effect>>
        &effects,
    ValueRange results, ValueRange inputBuffers, ValueRange outputBuffers) {
  for (Value value : results) {
    effects.emplace_back(MemoryEffects::Allocate::get(), value,
                         SideEffects::DefaultResource::get());
  }
  for (Value value : inputBuffers) {
    effects.emplace_back(MemoryEffects::Read::get(), value,
                         SideEffects::DefaultResource::get());
  }
  for (Value value : outputBuffers) {
    effects.emplace_back(MemoryEffects::Read::get(), value,
                         SideEffects::DefaultResource::get());
    effects.emplace_back(MemoryEffects::Write::get(), value,
                         SideEffects::DefaultResource::get());
  }
}

void GenericOp::getEffects(
    SmallVectorImpl<SideEffects::EffectInstance<MemoryEffects::Effect>>
        &effects) {
  getGenericEffectsImpl(effects, getOperation()->getResults(),
                        getInputBuffers(), getOutputBuffers());
}

void IndexedGenericOp::getEffects(
    SmallVectorImpl<SideEffects::EffectInstance<MemoryEffects::Effect>>
        &effects) {
  getGenericEffectsImpl(effects, getOperation()->getResults(),
                        getInputBuffers(), getOutputBuffers());
}

namespace {

template <typename GenericOpType>
struct BlockArgsVerifier {
  static LogicalResult verify(GenericOpType op, Block &block);
};

template <typename GenericOpType>
LogicalResult BlockArgsVerifier<GenericOpType>::verify(GenericOpType op,
                                                       Block &block) {
  auto nOperands = op.getNumOperands();
  if (block.getNumArguments() != nOperands)
    return op.emitOpError("expected number of block arguments to match number "
                          "of operands");

  // Note: the number and type of yield values are checked in the YieldOp.
  auto nInputViews = op.getNumInputs();
  for (unsigned i = 0; i < nOperands; ++i) {
    auto viewType = op.getShapedType(i);
    if (viewType.getElementType() != block.getArgument(i).getType())
      return op.emitOpError("expected block argument ")
             << (i + 1) << " of the same type as elemental type of "
             << ((i < nInputViews) ? "input " : "output ")
             << "operand: " << viewType;
  }
  return success();
}

template <>
LogicalResult BlockArgsVerifier<IndexedGenericOp>::verify(IndexedGenericOp op,
                                                          Block &block) {
  auto nInputViews = op.getNumInputs();
  auto nLoops = op.getNumLoops();
  auto nOperands = op.getNumOperands();
  if (block.getNumArguments() != nOperands + nLoops)
    return op.emitOpError(
        "expected number of block arguments to match number of operands + "
        "number of loops");

  // Note: the number and type of yield values are checked in the YieldOp.
  for (unsigned i = 0; i < nLoops; ++i)
    if (!block.getArgument(i).getType().isIndex())
      return op.emitOpError("expected block argument ")
             << (i + 1) << " to be an index";

  for (unsigned i = 0; i < nOperands; ++i) {
    unsigned memrefArgIndex = i + nLoops;
    auto viewType = op.getShapedType(i);
    if (viewType.getElementType() !=
        block.getArgument(memrefArgIndex).getType())
      return op.emitOpError("expected block argument ")
             << (memrefArgIndex + 1)
             << " of the same type as elemental type of "
             << ((i < nInputViews) ? "input " : "output ")
             << "operand: " << viewType;
  }
  return success();
}

template <typename GenericOpType>
struct AnnotationsVerifier {
  static LogicalResult verify(GenericOpType op) { return success(); }
};

template <>
LogicalResult AnnotationsVerifier<GenericOp>::verify(GenericOp op) {
  ArrayAttr sparseAttr = op.sparseAttr();
  if (!sparseAttr)
    return success();
  // Verify consistency of sparse annotations.
  if (!op.hasTensorSemantics())
    return op.emitOpError("expected sparse annotations on tensors only");
  if (op.getNumOutputs() != 1)
    return op.emitOpError("expected single output tensor");
  unsigned numTensors = op.getNumInputsAndOutputs();
  if (sparseAttr.size() != numTensors)
    return op.emitOpError("expected one sparse annotation for each tensor");
  for (unsigned t = 0; t < numTensors; t++) {
    auto dimAttr = sparseAttr[t].dyn_cast_or_null<ArrayAttr>();
    if (!dimAttr)
      return op.emitOpError("expected sparse annotation array for tensor ")
             << t;
    unsigned rank = op.getShapedType(t).getRank();
    if (dimAttr.size() != rank)
      return op.emitOpError("expected sparse annotation with rank ")
             << rank << " for tensor " << t;
    // Per-dimension annotations for each tensor consist of only "D" or "S".
    for (unsigned d = 0; d < rank; d++) {
      if (isDenseDim(dimAttr[d])) {
        continue;
      } else if (isSparseDim(dimAttr[d])) {
        if (t == numTensors - 1)
          return op.emitOpError("sparse output tensors not supported (yet)");
        continue;
      }
      return op.emitOpError("expected sparse annotation at position ")
             << d << " for tensor " << t;
    }
  }
  return success();
}

} // namespace

template <typename GenericOpType>
static LogicalResult verifyGenericOp(GenericOpType op) {
  auto nLoops = op.getNumLoops();

  if (op.inputs().size() + op.output_buffers().size() +
          op.init_tensors().size() + op.getNumResults() ==
      0)
    return op.emitOpError("expected at least 1 Shaped operand or return");

  auto &region = op.region();
  if (!llvm::hasSingleElement(region))
    return op.emitOpError("expected region with 1 block");
  if (failed(BlockArgsVerifier<GenericOpType>::verify(op, region.front())))
    return failure();

  auto symbolSourceAttr =
      op.template getAttrOfType<IntegerAttr>("symbol_source");
  int64_t expectedNumSymbols = 0;
  if (symbolSourceAttr) {
    unsigned index = symbolSourceAttr.getInt();
    if (index >= op.getNumOperands())
      return op.emitOpError("symbol_source index out of range");
    expectedNumSymbols = op.getShapedType(index).getRank();
  }

  if (op.indexing_maps().size() != op.getNumInputsAndOutputs())
    return op.emitOpError("expected the number of indexing_map (")
           << op.indexing_maps().size()
           << ") to be equal to the number of inputs and outputs ("
           << op.getNumInputsAndOutputs() << ")";

  SmallVector<AffineMap, 4> indexingMaps;
  indexingMaps.reserve(op.indexing_maps().size());
  for (auto en : llvm::enumerate(op.indexing_maps())) {
    auto idx = en.index();
    auto m = en.value().template cast<AffineMapAttr>().getValue();
    indexingMaps.push_back(m); // Save reference to map for further checks.
    auto view = op.getShapedType(idx);

    if (m.getNumSymbols() != expectedNumSymbols)
      return op.emitOpError("expected the number of symbols in indexing_map #")
             << idx << " to match rank of operand `symbol_source`";

    if (m.getNumDims() != nLoops)
      return op.emitOpError("expected indexing_map #")
             << idx << " to have " << nLoops
             << " dim(s) to match the number of loops";

    if (m.getNumResults() != view.getRank())
      return op.emitOpError("expected indexing_map #")
             << idx << " results to match view rank: " << view;
  }

  // TODO: symbol_source prevents us to just write:
  // if (!op.getShapeToLoopsMap())
  //   return op.emitOpError("expected the shape-to-loops map to be non-null");
  //
  // Update when symbol_source is deleted.
  auto concatMap = concatAffineMaps(indexingMaps);
  // TODO: Bound inference for maps with symbols
  if (!concatMap.getNumSymbols() && !inversePermutation(concatMap))
    return op.emitOpError("expected the shape-to-loops map to be non-null");

  if (failed(AnnotationsVerifier<GenericOpType>::verify(op)))
    return failure();

  return success();
}

static LogicalResult verify(GenericOp op) { return verifyGenericOp(op); }

static LogicalResult verify(IndexedGenericOp op) { return verifyGenericOp(op); }

//===----------------------------------------------------------------------===//
// ReshapeOp
//===----------------------------------------------------------------------===//

/// Collapse reassociation maps that are used in pair of reshape ops where one
/// is a producer and other is the consumer. Only valid to use this method when
/// both the producer and consumer are collapsing dimensions or both are
/// expanding dimensions.
///
/// For example,
///   mapsProducer = [affine_map<(d0, d1, d2, d3, d4) -> (d0, d1)>,
///                   affine_map<(d0, d1, d2, d3, d4) -> (d2)>,
///                   affine_map<(d0, d1, d2, d3, d4) -> (d3, d4)>]
///   mapsConsumer = [affine_map<(d0, d1, d2) -> (d0, d1)>,
///                   affine_map<(d0, d1, d2) -> (d2)>]
///
/// is folded into
///
///   result = [affine_map<(d0, d1, d2, d3, d4) -> (d0, d1, d2)>,
///             affine_map<(d0, d1, d2, d3, d4) -> (d3, d4)>]
static ArrayAttr collapseReassociationMaps(ArrayRef<AffineMap> mapsProducer,
                                           ArrayRef<AffineMap> mapsConsumer,
                                           MLIRContext *context) {
  // Handle the corner case of the result being a rank 0 shaped type. Return an
  // emtpy ArrayAttr.
  if (mapsConsumer.empty() && !mapsProducer.empty())
    return ArrayAttr::get(ArrayRef<Attribute>(), context);
  if (mapsProducer.empty() || mapsConsumer.empty() ||
      mapsProducer[0].getNumDims() < mapsConsumer[0].getNumDims() ||
      mapsProducer.size() != mapsConsumer[0].getNumDims())
    return nullptr;
  unsigned numLhsDims = mapsProducer[0].getNumDims();
  unsigned currDim = 0;
  SmallVector<AffineExpr, 4> reassociations;
  SmallVector<Attribute, 4> reassociationMaps;
  for (AffineMap rhs : mapsConsumer) {
    for (AffineExpr rhsExpr : rhs.getResults()) {
      AffineDimExpr dimExpr = rhsExpr.cast<AffineDimExpr>();
      for (int i = 0, e = mapsProducer[dimExpr.getPosition()].getNumResults();
           i < e; ++i) {
        reassociations.push_back(getAffineDimExpr(currDim++, context));
      }
    }
    reassociationMaps.push_back(AffineMapAttr::get(AffineMap::get(
        numLhsDims, /*numSymbols =*/0, reassociations, context)));
    reassociations.clear();
  }
  return ArrayAttr::get(reassociationMaps, context);
}

namespace {
/// Pattern to collapse producer/consumer reshape ops that are both collapsing
/// dimensions or are both expanding dimensions.
template <typename ReshapeOpTy>
struct CollapseReshapeOps : public OpRewritePattern<ReshapeOpTy> {
  using OpRewritePattern<ReshapeOpTy>::OpRewritePattern;
  LogicalResult matchAndRewrite(ReshapeOpTy reshapeOp,
                                PatternRewriter &rewriter) const override {
    auto srcReshapeOp = reshapeOp.src().template getDefiningOp<ReshapeOpTy>();
    if (!srcReshapeOp)
      return failure();

    auto areReshapeOpsFoldable = [](ShapedType largerType,
                                    ShapedType intermediateType,
                                    ShapedType smallerType) -> bool {
      return largerType.getRank() > intermediateType.getRank() &&
             intermediateType.getRank() > smallerType.getRank();
    };
    // Check if producer and consumer are both expanding dims.
    if (areReshapeOpsFoldable(reshapeOp.getResultType(), reshapeOp.getSrcType(),
                              srcReshapeOp.getSrcType())) {
      rewriter.replaceOpWithNewOp<ReshapeOpTy>(
          reshapeOp, reshapeOp.getResultType(), srcReshapeOp.src(),
          collapseReassociationMaps(reshapeOp.getReassociationMaps(),
                                    srcReshapeOp.getReassociationMaps(),
                                    rewriter.getContext()));
      return success();
    }
    // Check if producer and consumer are both collapsing dims.
    if (areReshapeOpsFoldable(srcReshapeOp.getSrcType(), reshapeOp.getSrcType(),
                              reshapeOp.getResultType())) {
      rewriter.replaceOpWithNewOp<ReshapeOpTy>(
          reshapeOp, reshapeOp.getResultType(), srcReshapeOp.src(),
          collapseReassociationMaps(srcReshapeOp.getReassociationMaps(),
                                    reshapeOp.getReassociationMaps(),
                                    rewriter.getContext()));
      return success();
    }
    return failure();
  }
};
} // namespace

template <typename ReshapeOpTy>
static OpFoldResult foldReshapeOp(ReshapeOpTy reshapeOp,
                                  ArrayRef<Attribute> operands) {
  // Fold producer-consumer reshape ops that where the operand type of the
  // producer is same as the return type of the consumer. This can only be
  // verified if the shapes in question are static.
  ReshapeOpTy reshapeSrcOp =
      reshapeOp.src().template getDefiningOp<ReshapeOpTy>();
  if (reshapeSrcOp && reshapeSrcOp.getSrcType().hasStaticShape() &&
      reshapeOp.getResultType().hasStaticShape() &&
      reshapeSrcOp.getSrcType() == reshapeOp.getResultType())
    return reshapeSrcOp.src();
  // Reshape of a constant can be replaced with a new constant.
  if (auto elements = operands.front().dyn_cast_or_null<DenseElementsAttr>()) {
    return elements.reshape(
        reshapeOp.getResult().getType().template cast<ShapedType>());
  }
  return nullptr;
}

/// Return true if the reassociation specification is valid, false otherwise.
/// When false, the `invalidIndex` integer pointer is optionally filled with the
/// index of the offending reassociation map.
static bool isReassociationValid(ArrayRef<AffineMap> reassociation,
                                 int *invalidIndex = nullptr) {
  if (reassociation.empty())
    return true;
  unsigned nDims = reassociation[0].getNumDims();
  unsigned nextExpectedDim = 0;
  for (auto it : llvm::enumerate(reassociation)) {
    auto m = it.value();
    if (m.getNumDims() != nDims || m.getNumSymbols() != 0) {
      if (invalidIndex)
        *invalidIndex = it.index();
      return false;
    }
    for (auto e : m.getResults()) {
      auto d = e.dyn_cast<AffineDimExpr>();
      if (!d || d.getPosition() != nextExpectedDim++) {
        if (invalidIndex)
          *invalidIndex = it.index();
        return false;
      }
    }
  }
  if (nextExpectedDim != nDims) {
    if (invalidIndex)
      *invalidIndex = reassociation.size() - 1;
    return false;
  }
  return true;
}

/// Detect whether memref dims [dim, dim + extent) can be reshaped without
/// copies.
static bool isReshapableDimBand(unsigned dim, unsigned extent,
                                ArrayRef<int64_t> sizes,
                                ArrayRef<AffineExpr> strides) {
  assert(sizes.size() == strides.size() && "mismatched ranks");
  // off by 1 indexing to avoid out of bounds
  //                       V
  for (auto idx = dim, e = dim + extent; idx + 1 < e; ++idx) {
    // Only bands of static shapes are reshapable. This is due to the fact that
    // there is no relation between dynamic sizes and dynamic strides: we do not
    // have enough information to know whether a "-1" size corresponds to the
    // proper symbol in the AffineExpr of a stride.
    if (ShapedType::isDynamic(sizes[dim + 1]))
      return false;
    // TODO: Refine this by passing the proper nDims and nSymbols so we can
    // simplify on the fly and catch more reshapable cases.
    if (strides[idx] != strides[idx + 1] * sizes[idx + 1])
      return false;
  }
  return true;
}

/// Compute the MemRefType obtained by applying the `reassociation` (which is
/// expected to be valid) to `type`.
/// If `type` is Contiguous MemRefType, this always produce a contiguous
/// MemRefType.
static MemRefType
computeReshapeCollapsedType(MemRefType type,
                            ArrayRef<AffineMap> reassociation) {
  auto sizes = type.getShape();
  AffineExpr offset;
  SmallVector<AffineExpr, 4> strides;
  auto status = getStridesAndOffset(type, strides, offset);
  (void)status;
  assert(succeeded(status) && "expected strided memref");

  SmallVector<int64_t, 4> newSizes;
  newSizes.reserve(reassociation.size());
  SmallVector<AffineExpr, 4> newStrides;
  newStrides.reserve(reassociation.size());

  // Use the fact that reassociation is valid to simplify the logic: only use
  // each map's rank.
  assert(isReassociationValid(reassociation) && "invalid reassociation");
  unsigned currentDim = 0;
  for (AffineMap m : reassociation) {
    unsigned dim = m.getNumResults();
    int64_t size = 1;
    AffineExpr stride = strides[currentDim + dim - 1];
    if (!isReshapableDimBand(currentDim, dim, sizes, strides)) {
      size = ShapedType::kDynamicSize;
      stride = AffineExpr();
    } else {
      for (unsigned d = 0; d < dim; ++d)
        size *= sizes[currentDim + d];
    }
    newSizes.push_back(size);
    newStrides.push_back(stride);
    currentDim += dim;
  }

  // Early-exit: if `type` is contiguous, the result must be contiguous.
  if (canonicalizeStridedLayout(type).getAffineMaps().empty())
    return MemRefType::Builder(type).setShape(newSizes).setAffineMaps({});

  // Convert back to int64_t because we don't have enough information to create
  // new strided layouts from AffineExpr only. This corresponds to a case where
  // copies may be necessary.
  int64_t intOffset = ShapedType::kDynamicStrideOrOffset;
  if (auto o = offset.dyn_cast<AffineConstantExpr>())
    intOffset = o.getValue();
  SmallVector<int64_t, 4> intStrides;
  intStrides.reserve(strides.size());
  for (auto stride : newStrides) {
    if (auto cst = stride.dyn_cast_or_null<AffineConstantExpr>())
      intStrides.push_back(cst.getValue());
    else
      intStrides.push_back(ShapedType::kDynamicStrideOrOffset);
  }
  auto layout =
      makeStridedLinearLayoutMap(intStrides, intOffset, type.getContext());
  return canonicalizeStridedLayout(
      MemRefType::Builder(type).setShape(newSizes).setAffineMaps({layout}));
}

/// Helper functions assert Attribute of the proper type in attr and returns the
/// corresponding vector.
/// TODO: this should be evolved into a generic
/// `getRangeOfType<AffineMap>(ArrayAttr attrs)` that does not copy.
static SmallVector<AffineMap, 4> getAffineMaps(ArrayAttr attrs) {
  return llvm::to_vector<8>(llvm::map_range(
      attrs, [](Attribute a) { return a.cast<AffineMapAttr>().getValue(); }));
}

template <typename AffineExprTy>
unsigned getMaxPosOfType(ArrayRef<ReassociationExprs> exprArrays) {
  unsigned pos = 0;
  for (const auto &exprs : exprArrays) {
    for (auto expr : exprs) {
      expr.walk([&pos](AffineExpr e) {
        if (auto d = e.dyn_cast<AffineExprTy>())
          pos = std::max(pos, d.getPosition());
      });
    }
  }
  return pos;
}

static SmallVector<AffineMap, 4>
getSymbolLessAffineMaps(ArrayRef<ReassociationExprs> reassociation) {
  unsigned maxDim = getMaxPosOfType<AffineDimExpr>(reassociation);
  assert(getMaxPosOfType<AffineSymbolExpr>(reassociation) == 0 &&
         "Expected symbol-less expressions");
  SmallVector<AffineMap, 4> maps;
  maps.reserve(reassociation.size());
  for (const auto &exprs : reassociation) {
    assert(!exprs.empty());
    maps.push_back(AffineMap::get(maxDim + 1, 0, exprs, exprs[0].getContext()));
  }
  return maps;
}

static SmallVector<SmallVector<AffineExpr, 2>, 2>
convertReassociationIndicesToMaps(
    OpBuilder &b, ArrayRef<ReassociationIndices> reassociationIndices) {
  SmallVector<SmallVector<AffineExpr, 2>, 2> reassociationMaps;
  for (const auto &indices : reassociationIndices) {
    SmallVector<AffineExpr, 2> reassociationMap;
    reassociationMap.reserve(indices.size());
    for (int64_t index : indices)
      reassociationMap.push_back(b.getAffineDimExpr(index));
    reassociationMaps.push_back(std::move(reassociationMap));
  }
  return reassociationMaps;
}

void mlir::linalg::ReshapeOp::build(OpBuilder &b, OperationState &result,
                                    Value src,
                                    ArrayRef<ReassociationExprs> reassociation,
                                    ArrayRef<NamedAttribute> attrs) {
  auto maps = getSymbolLessAffineMaps(reassociation);
  auto memRefType = src.getType().cast<MemRefType>();
  auto resultType = computeReshapeCollapsedType(memRefType, maps);
  build(b, result, resultType, src, attrs);
  result.addAttribute(ReshapeOp::getReassociationAttrName(),
                      b.getAffineMapArrayAttr(maps));
}

void mlir::linalg::ReshapeOp::build(OpBuilder &b, OperationState &result,
                                    Type resultType, Value src,
                                    ArrayRef<ReassociationExprs> reassociation,
                                    ArrayRef<NamedAttribute> attrs) {
  auto maps = getSymbolLessAffineMaps(reassociation);
  build(b, result, resultType, src, attrs);
  result.addAttribute(ReshapeOp::getReassociationAttrName(),
                      b.getAffineMapArrayAttr(maps));
}

Value mlir::linalg::ReshapeOp::getViewSource() { return src(); }

// Common verifier for reshape-like types. Fills `expandedType` and
// `collapsedType` with the proper `src` or `result` type.
template <typename Op, typename T>
static LogicalResult verifyReshapeLikeTypes(Op op, T &expandedType,
                                            T &collapsedType) {
  expandedType = op.getSrcType();
  collapsedType = op.getResultType();
  unsigned expandedRank = expandedType.getRank();
  unsigned collapsedRank = collapsedType.getRank();
  bool isCollapse = expandedRank > collapsedRank;
  if (!isCollapse) {
    std::swap(expandedRank, collapsedRank);
    std::swap(expandedType, collapsedType);
  }
  if (expandedRank == 0)
    return op.emitOpError("expected non-zero memref ranks");
  if (expandedRank == collapsedRank)
    return op.emitOpError("expected to collapse or expand dims");

  if (collapsedRank == 0) {
    // If collapsed rank is 0, then expanded type must be static shaped and of
    // sizes 1.
    if (llvm::any_of(expandedType.getShape(),
                     [](int64_t dim) -> bool { return dim != 1; }))
      return op.emitOpError(
          "invalid to reshape tensor/memref with non-unit extent dimensions to "
          "zero-rank tensor/memref");
    return success();
  }
  if (collapsedRank != op.reassociation().size())
    return op.emitOpError("expected rank of the collapsed type(")
           << collapsedRank << ") to be the number of reassociation maps("
           << op.reassociation().size() << ")";
  auto maps = getAffineMaps(op.reassociation());
  for (auto it : llvm::enumerate(maps))
    if (it.value().getNumDims() != expandedRank)
      return op.emitOpError("expected reassociation map #")
             << it.index() << " of same rank as expanded memref("
             << expandedRank << "), but got " << it.value().getNumDims();
  int invalidIdx = 0;
  if (!isReassociationValid(maps, &invalidIdx))
    return op.emitOpError("expected reassociation map #")
           << invalidIdx << " to be valid and contiguous";
  return success();
}

static LogicalResult verify(ReshapeOp op) {
  MemRefType expandedType, collapsedType;
  if (failed(verifyReshapeLikeTypes(op, expandedType, collapsedType)))
    return failure();
  auto maps = getAffineMaps(op.reassociation());
  MemRefType expectedType = computeReshapeCollapsedType(expandedType, maps);
  if (collapsedType != expectedType)
    return op.emitOpError("expected collapsed type to be ")
           << expectedType << ", but got " << collapsedType;
  return success();
}

void ReshapeOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                            MLIRContext *context) {
  results.insert<CollapseReshapeOps<ReshapeOp>>(context);
}

//===----------------------------------------------------------------------===//
// TensorReshapeOp
//===----------------------------------------------------------------------===//

/// Compute the RankedTensorType obtained by applying `reassociation` to `type`.
static RankedTensorType
computeTensorReshapeCollapsedType(RankedTensorType type,
                                  ArrayRef<AffineMap> reassociation) {
  auto shape = type.getShape();
  SmallVector<int64_t, 4> newShape;
  newShape.reserve(reassociation.size());

  // Use the fact that reassociation is valid to simplify the logic: only use
  // each map's rank.
  assert(isReassociationValid(reassociation) && "invalid reassociation");
  unsigned currentDim = 0;
  for (AffineMap m : reassociation) {
    unsigned dim = m.getNumResults();
    auto band = shape.slice(currentDim, dim);
    int64_t size = 1;
    if (llvm::is_contained(band, ShapedType::kDynamicSize))
      size = ShapedType::kDynamicSize;
    else
      for (unsigned d = 0; d < dim; ++d)
        size *= shape[currentDim + d];
    newShape.push_back(size);
    currentDim += dim;
  }

  return RankedTensorType::get(newShape, type.getElementType());
}

void mlir::linalg::TensorReshapeOp::build(
    OpBuilder &b, OperationState &result, Value src,
    ArrayRef<ReassociationExprs> reassociation,
    ArrayRef<NamedAttribute> attrs) {
  auto maps = getSymbolLessAffineMaps(reassociation);
  auto resultType = computeTensorReshapeCollapsedType(
      src.getType().cast<RankedTensorType>(), maps);
  build(b, result, resultType, src, attrs);
  result.addAttribute(TensorReshapeOp::getReassociationAttrName(),
                      b.getAffineMapArrayAttr(maps));
}

void mlir::linalg::TensorReshapeOp::build(
    OpBuilder &b, OperationState &result, Type resultType, Value src,
    ArrayRef<ReassociationExprs> reassociation,
    ArrayRef<NamedAttribute> attrs) {
  auto maps = getSymbolLessAffineMaps(reassociation);
  build(b, result, resultType, src, attrs);
  result.addAttribute(TensorReshapeOp::getReassociationAttrName(),
                      b.getAffineMapArrayAttr(maps));
}

static LogicalResult verify(TensorReshapeOp op) {
  RankedTensorType expandedType, collapsedType;
  if (failed(verifyReshapeLikeTypes(op, expandedType, collapsedType)))
    return failure();
  auto maps = getAffineMaps(op.reassociation());
  // TODO: expanding a ? with a non-constant is under-specified. Error
  // out.
  RankedTensorType expectedType =
      computeTensorReshapeCollapsedType(expandedType, maps);
  if (collapsedType != expectedType)
    return op.emitOpError("expected collapsed type to be ")
           << expectedType << ", but got " << collapsedType;
  return success();
}

namespace {
/// Reshape of a splat constant can be replaced with a constant of the result
/// type.
struct FoldReshapeWithConstant : OpRewritePattern<TensorReshapeOp> {
  using OpRewritePattern<TensorReshapeOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(TensorReshapeOp reshapeOp,
                                PatternRewriter &rewriter) const override {
    DenseElementsAttr attr;
    if (!matchPattern(reshapeOp.src(), m_Constant(&attr)))
      return failure();
    if (!attr || !attr.isSplat())
      return failure();
    DenseElementsAttr newAttr = DenseElementsAttr::getFromRawBuffer(
        reshapeOp.getResultType(), attr.getRawData(), true);
    rewriter.replaceOpWithNewOp<ConstantOp>(reshapeOp, newAttr);
    return success();
  }
};
} // namespace

void TensorReshapeOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<CollapseReshapeOps<TensorReshapeOp>, FoldReshapeWithConstant>(
      context);
}

//===----------------------------------------------------------------------===//
// SliceOp
//===----------------------------------------------------------------------===//
void mlir::linalg::SliceOp::build(OpBuilder &b, OperationState &result,
                                  Value base, ValueRange indexings) {
  result.addOperands(base);
  result.addOperands(indexings);

  auto memRefType = base.getType().cast<MemRefType>();
  int64_t offset;
  SmallVector<int64_t, 4> strides;
  auto res = getStridesAndOffset(memRefType, strides, offset);
  assert(succeeded(res) && strides.size() == indexings.size());
  (void)res;

  unsigned rank = memRefType.getRank();
  // TODO: propagate static size and stride information when available.
  SmallVector<int64_t, 4> sizes(rank, -1); // -1 encodes dynamic size.
  result.addTypes({MemRefType::Builder(memRefType)
                       .setShape(sizes)
                       .setAffineMaps(makeStridedLinearLayoutMap(
                           strides, offset, b.getContext()))});
}

static void print(OpAsmPrinter &p, SliceOp op) {
  auto indexings = op.indexings();
  p << SliceOp::getOperationName() << " " << op.view() << "[" << indexings
    << "] ";
  p.printOptionalAttrDict(op.getAttrs());
  p << " : " << op.getBaseViewType();
  if (!indexings.empty())
    p << ", " << op.indexings().getTypes();
  p << ", " << op.getType();
}

static ParseResult parseSliceOp(OpAsmParser &parser, OperationState &result) {
  OpAsmParser::OperandType baseInfo;
  SmallVector<OpAsmParser::OperandType, 8> operands;
  SmallVector<Type, 8> types;
  if (parser.parseOperand(baseInfo) ||
      parser.parseOperandList(operands, OpAsmParser::Delimiter::Square) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonTypeList(types))
    return failure();

  if (types.size() < 2)
    return parser.emitError(parser.getCurrentLocation(),
                            "expected at least input and result view types");

  ArrayRef<Type> indexingTypes = ArrayRef<Type>(types).drop_front().drop_back();
  return failure(
      parser.resolveOperand(baseInfo, types.front(), result.operands) ||
      (!operands.empty() &&
       parser.resolveOperands(operands, indexingTypes,
                              operands.front().location, result.operands)) ||
      parser.addTypeToList(types.back(), result.types));
}

static LogicalResult verify(SliceOp op) {
  unsigned rank = op.getBaseViewRank();
  if (rank != llvm::size(op.indexings()))
    return op.emitOpError("expected ")
           << rank << " indexings, got " << llvm::size(op.indexings());
  unsigned index = 0;
  for (auto indexing : op.indexings()) {
    if (indexing.getType().isa<IndexType>())
      --rank;
    ++index;
  }
  if (op.getRank() != rank)
    return op.emitOpError() << "expected rank of the view(" << op.getRank()
                            << ") to be the number of ranges(" << rank << ")";
  return success();
}

Value SliceOp::getViewSource() { return view(); }

//===----------------------------------------------------------------------===//
// YieldOp
//===----------------------------------------------------------------------===//

static void print(OpAsmPrinter &p, linalg::YieldOp op) {
  p << op.getOperationName();
  if (op.getNumOperands() > 0)
    p << ' ' << op.getOperands();
  p.printOptionalAttrDict(op.getAttrs());
  if (op.getNumOperands() > 0)
    p << " : " << op.getOperandTypes();
}

static ParseResult parseYieldOp(OpAsmParser &parser, OperationState &result) {
  SmallVector<OpAsmParser::OperandType, 2> opInfo;
  SmallVector<Type, 2> types;
  llvm::SMLoc loc = parser.getCurrentLocation();
  return failure(parser.parseOperandList(opInfo) ||
                 parser.parseOptionalAttrDict(result.attributes) ||
                 (!opInfo.empty() && parser.parseColonTypeList(types)) ||
                 parser.resolveOperands(opInfo, types, loc, result.operands));
}

// Check the operand number and types must match the element types of the
// LinalgOp interface's shaped operands.
static LogicalResult verifyYield(linalg::YieldOp op,
                                 LinalgOp linalgOpInterface) {
  auto nOutputs = linalgOpInterface.getNumOutputs();
  if (op.getNumOperands() != nOutputs)
    return op.emitOpError("expected number of yield values (")
           << nOutputs << ") to match the number of operands of the enclosing "
           << "LinalgOp (" << op.getNumOperands() << ")";

  for (unsigned i = 0; i != nOutputs; ++i) {
    auto elementType =
        linalgOpInterface.getOutputShapedType(i).getElementType();
    if (op.getOperand(i).getType() != elementType)
      return op.emitOpError("type of yield operand ")
             << (i + 1) << " (" << op.getOperand(i).getType()
             << ") doesn't match "
             << "the element type of the enclosing linalg.generic op ("
             << elementType << ")";
  }
  return success();
}

static LogicalResult verify(linalg::YieldOp op) {
  auto *parentOp = op.getParentOp();
  if (parentOp->getNumRegions() != 1 || parentOp->getRegion(0).empty())
    return op.emitOpError("expected single non-empty parent region");

  if (auto linalgOp = dyn_cast<LinalgOp>(parentOp))
    return verifyYield(op, cast<LinalgOp>(parentOp));

  return op.emitOpError("expected parent op with LinalgOp interface");
}

/////// Operations corresponding to library calls defined with Tablegen ////////

void FillOp::getEffects(
    SmallVectorImpl<SideEffects::EffectInstance<MemoryEffects::Effect>>
        &effects) {
  effects.emplace_back(MemoryEffects::Write::get(), output(),
                       SideEffects::DefaultResource::get());
}

static LogicalResult verify(FillOp op) {
  auto viewType = op.getOutputShapedType(0);
  auto fillType = op.value().getType();
  if (viewType.getElementType() != fillType)
    return op.emitOpError("expects fill type to match view elemental type");
  return success();
}

void CopyOp::getEffects(
    SmallVectorImpl<SideEffects::EffectInstance<MemoryEffects::Effect>>
        &effects) {
  effects.emplace_back(MemoryEffects::Read::get(), input(),
                       SideEffects::DefaultResource::get());
  effects.emplace_back(MemoryEffects::Write::get(), output(),
                       SideEffects::DefaultResource::get());
}

static LogicalResult verify(CopyOp op) {
  auto outputViewType = op.getOutputShapedType(0);
  auto inputViewType = op.getInputShapedType(0);
  if (inputViewType.getElementType() != outputViewType.getElementType())
    return op.emitOpError("expects views of the same type");
  if (inputViewType.getRank() != outputViewType.getRank())
    return op.emitOpError("expects views of the same rank");
  auto rank = op.getNumParallelLoops();
  auto inputPermutationMap = op.inputPermutation();
  if (inputPermutationMap) {
    if (inputPermutationMap->getNumInputs() != rank)
      return op.emitOpError("expects optional input_permutation map of rank ")
             << rank;
    if (!inputPermutationMap->isPermutation())
      return op.emitOpError(
          "expects optional input_permutation map to be a permutation");
  }
  auto outputPermutationMap = op.outputPermutation();
  if (outputPermutationMap) {
    if (outputPermutationMap->getNumInputs() != rank)
      return op.emitOpError("expects optional output_permutation map of rank ")
             << rank;
    if (!outputPermutationMap->isPermutation())
      return op.emitOpError(
          "expects optional output_permutation map to be a permutation");
  }
  if (rank == 0 && inputPermutationMap)
    return op.emitOpError("expected no input permutation when rank == 0");
  if (rank == 0 && outputPermutationMap)
    return op.emitOpError("expected no output permutation when rank == 0");
  return success();
}

template <typename LinalgPoolingOp>
static LogicalResult verifyStrideOrDilation(LinalgPoolingOp op,
                                            ArrayRef<Attribute> attrs,
                                            bool isStride) {
  auto strideOrDilation = isStride ? "stride" : "dilation";
  if (attrs.size() != op.getNumWindowLoops())
    return op.emitOpError("expects num ")
           << strideOrDilation
           << "s equal to number of window dimensions: " << attrs.size()
           << " vs " << op.getNumWindowLoops();
  return success();
}

void ConvOp::getEffects(
    SmallVectorImpl<SideEffects::EffectInstance<MemoryEffects::Effect>>
        &effects) {
  effects.emplace_back(MemoryEffects::Read::get(), input(),
                       SideEffects::DefaultResource::get());
  effects.emplace_back(MemoryEffects::Read::get(), filter(),
                       SideEffects::DefaultResource::get());
  effects.emplace_back(MemoryEffects::Write::get(), output(),
                       SideEffects::DefaultResource::get());
}

static LogicalResult verify(ConvOp op) {
  auto oType = op.output().getType().cast<MemRefType>();
  auto fType = op.filter().getType().cast<MemRefType>();
  auto iType = op.input().getType().cast<MemRefType>();
  if (oType.getElementType() != iType.getElementType() ||
      oType.getElementType() != fType.getElementType())
    return op.emitOpError("expects memref elemental types to match");
  if (oType.getRank() != iType.getRank() || oType.getRank() != fType.getRank())
    return op.emitOpError("expects memref ranks to match");
  if (oType.getRank() <= 2)
    return op.emitOpError("expects memref ranks to be greater than 2");
  if (auto strides = op.strides()) {
    if (failed(
            verifyStrideOrDilation(op, strides->getValue(), /*isStride=*/true)))
      return failure();
  }
  if (auto dilations = op.dilations()) {
    if (failed(verifyStrideOrDilation(op, dilations->getValue(),
                                      /*isStride=*/false)))
      return failure();
  }
  return success();
}

template <typename PoolingOp>
static LogicalResult verifySingleInputPoolingOp(PoolingOp op) {
  auto inputType = op.input().getType().template cast<MemRefType>();
  auto outputType = op.output().getType().template cast<MemRefType>();
  if (outputType.getElementType() != inputType.getElementType())
    return op.emitOpError("expects memref elemental types to match");

  auto windowDimsType = op.windowDims().getType().template cast<MemRefType>();
  if (outputType.getRank() != inputType.getRank() ||
      outputType.getRank() != windowDimsType.getRank())
    return op.emitOpError("expects memref ranks to match");

  if (auto strides = op.strides()) {
    if (failed(
            verifyStrideOrDilation(op, strides->getValue(), /*isStride=*/true)))
      return failure();
  }
  if (auto dilations = op.dilations()) {
    if (failed(verifyStrideOrDilation(op, dilations->getValue(),
                                      /*isStride=*/false)))
      return failure();
  }
  return success();
}

#define DEFINE_POOLING_OP_GET_EFFECTS(OP_NAME)                                 \
  void OP_NAME::getEffects(                                                    \
      SmallVectorImpl<SideEffects::EffectInstance<MemoryEffects::Effect>>      \
          &effects) {                                                          \
    effects.emplace_back(MemoryEffects::Read::get(), input(),                  \
                         SideEffects::DefaultResource::get());                 \
    effects.emplace_back(MemoryEffects::Write::get(), output(),                \
                         SideEffects::DefaultResource::get());                 \
  }

static LogicalResult verify(PoolingMaxOp op) {
  return verifySingleInputPoolingOp(op);
}
static LogicalResult verify(PoolingMinOp op) {
  return verifySingleInputPoolingOp(op);
}
static LogicalResult verify(PoolingSumOp op) {
  return verifySingleInputPoolingOp(op);
}

DEFINE_POOLING_OP_GET_EFFECTS(PoolingMaxOp)
DEFINE_POOLING_OP_GET_EFFECTS(PoolingMinOp)
DEFINE_POOLING_OP_GET_EFFECTS(PoolingSumOp)

namespace {
struct EraseDeadLinalgOp;
struct FoldTensorCastOp;
} // namespace

#include "mlir/Dialect/Linalg/IR/LinalgStructuredOpsInterfaces.cpp.inc"

#include "mlir/Dialect/Linalg/IR/LinalgNamedStructuredOps.cpp.inc"

#define GET_OP_CLASSES
#include "mlir/Dialect/Linalg/IR/LinalgOps.cpp.inc"

#define GET_OP_CLASSES
#include "mlir/Dialect/Linalg/IR/LinalgStructuredOps.cpp.inc"

/// Return the dims that are `iteratorTypeName` loops in the LinalgOp `op`.
/// Assumes `op` is a LinalgOp.
void mlir::linalg::getDimsOfType(Operation *op, StringRef iteratorTypeName,
                                 SmallVectorImpl<AffineExpr> &res) {
  if (!cast<LinalgOp>(op).iterator_types())
    return;

  unsigned dim = 0;
  MLIRContext *ctx = op->getContext();
  for (auto tn :
       cast<LinalgOp>(op).iterator_types().getAsValueRange<StringAttr>()) {
    if (tn == iteratorTypeName)
      res.push_back(getAffineDimExpr(dim, ctx));
    ++dim;
  }
}

AffineMap mlir::linalg::extractOrIdentityMap(Optional<AffineMap> maybeMap,
                                             unsigned rank,
                                             MLIRContext *context) {
  if (maybeMap)
    return maybeMap.getValue();
  if (rank == 0)
    return AffineMap::get(context);
  return AffineMap::getMultiDimIdentityMap(rank, context);
}

SmallVector<AffineExpr, 4>
mlir::linalg::makeAffineDimExprs(unsigned num, unsigned &startIdx,
                                 MLIRContext *context) {
  SmallVector<AffineExpr, 4> res;
  res.reserve(num);
  for (unsigned i = 0; i < num; ++i)
    res.push_back(getAffineDimExpr(startIdx++, context));
  return res;
}

template <typename PoolingOp>
SmallVector<AffineExpr, 4>
mlir::linalg::weightedPoolingInputIndex(PoolingOp op,
                                        ArrayRef<AffineExpr> outputDims,
                                        ArrayRef<AffineExpr> windowDims) {
  assert(outputDims.size() == windowDims.size());
  SmallVector<AffineExpr, 4> res;
  res.reserve(outputDims.size());
  for (unsigned i = 0, e = outputDims.size(); i < e; ++i) {
    // TODO: add a level of indirection to linalg.generic.
    auto expr = op.getStride(i) * outputDims[i] +
                op.getDilation(i) * windowDims[i] - op.getLowPad(i);
    res.push_back(expr);
  }
  return res;
}

#define INSTANTIATE_WEIGHTED_POOLING_INPUT_INDEX(OP_TYPE)                      \
  template SmallVector<AffineExpr, 4>                                          \
  mlir::linalg::weightedPoolingInputIndex<OP_TYPE>(                            \
      OP_TYPE op, ArrayRef<AffineExpr> outputDims,                             \
      ArrayRef<AffineExpr> windowDims);

INSTANTIATE_WEIGHTED_POOLING_INPUT_INDEX(ConvOp)
INSTANTIATE_WEIGHTED_POOLING_INPUT_INDEX(PoolingMaxOp)
INSTANTIATE_WEIGHTED_POOLING_INPUT_INDEX(PoolingMinOp)
INSTANTIATE_WEIGHTED_POOLING_INPUT_INDEX(PoolingSumOp)

SmallVector<AffineExpr, 4> mlir::linalg::concat(ArrayRef<AffineExpr> a,
                                                ArrayRef<AffineExpr> b) {
  auto rangeA = llvm::make_range(a.begin(), a.end());
  auto rangeB = llvm::make_range(b.begin(), b.end());
  auto concatRanges = llvm::concat<const AffineExpr>(rangeA, rangeB);
  return llvm::to_vector<4>(concatRanges);
}

static void appendMangledType(llvm::raw_string_ostream &ss, Type t) {
  if (auto memref = t.dyn_cast<MemRefType>()) {
    ss << "view";
    for (auto size : memref.getShape())
      if (size < 0)
        ss << "sx";
      else
        ss << size << "x";
    appendMangledType(ss, memref.getElementType());
  } else if (auto vec = t.dyn_cast<VectorType>()) {
    ss << "vector";
    llvm::interleave(
        vec.getShape(), [&](int64_t i) { ss << i; }, [&]() { ss << "x"; });
    appendMangledType(ss, vec.getElementType());
  } else if (t.isSignlessIntOrIndexOrFloat()) {
    ss << t;
  } else {
    llvm_unreachable("Invalid type for linalg library name mangling");
  }
}

std::string mlir::linalg::generateLibraryCallName(Operation *op) {
  assert(isa<LinalgOp>(op));
  std::string name(op->getName().getStringRef().str());
  name.reserve(128);
  std::replace(name.begin(), name.end(), '.', '_');
  llvm::raw_string_ostream ss(name);
  ss << "_";
  auto types = op->getOperandTypes();
  llvm::interleave(
      types.begin(), types.end(), [&](Type t) { appendMangledType(ss, t); },
      [&]() { ss << "_"; });
  return ss.str();
}

// TODO: Consider making all this boilerplate easy to autogenerate
// with Tablegen. This seems a desirable property in the context of
// OpInterfaces where a Linalg "named" op **isa** LinalgOp.
OpFoldResult ReshapeOp::fold(ArrayRef<Attribute> operands) {
  if (succeeded(foldMemRefCast(*this)))
    return getResult();
  return foldReshapeOp(*this, operands);
}
OpFoldResult SliceOp::fold(ArrayRef<Attribute>) {
  if (succeeded(foldMemRefCast(*this)))
    return getResult();
  return {};
}
OpFoldResult TensorReshapeOp::fold(ArrayRef<Attribute> operands) {
  return foldReshapeOp(*this, operands);
}

//===----------------------------------------------------------------------===//
// Auto-generated Linalg named ops.
//===----------------------------------------------------------------------===//

template <typename NamedStructuredOpType>
static void buildNamedStructuredOpRegionAndAttributesImpl(
    OpBuilder &opBuilder, Region &region, TypeRange inputTypes,
    TypeRange outputBufferTypes, TypeRange initTensorTypes,
    TypeRange resultTypes,
    std::function<void(unsigned, unsigned)> errorHandler) {
  // TODO: atm all operands go through getElementTypeOrSelf,
  // reconsider when we have evidence we need to.
  SmallVector<Type, 8> argTypes;
  for (auto containers : {inputTypes, outputBufferTypes, resultTypes})
    for (auto t : containers)
      argTypes.push_back(getElementTypeOrSelf(t));

  // RAII.
  OpBuilder::InsertionGuard guard(opBuilder);
  Block *body = opBuilder.createBlock(&region, {}, argTypes);
  unsigned actual = body->getNumArguments();
  unsigned expected = NamedStructuredOpType::getNumRegionArgs();
  if (expected != actual)
    return errorHandler(expected, actual);

  opBuilder.setInsertionPointToStart(body);
  mlir::edsc::ScopedContext scope(opBuilder, opBuilder.getUnknownLoc());
  NamedStructuredOpType::regionBuilder(*body);

  // indexing_maps is an auto-generated method.

  // iterator_types is an auto-generated method.
}

template <typename NamedStructuredOpType>
void buildNamedStructuredOpRegionAndAttributes(OpBuilder &opBuilder,
                                               OperationState &result,
                                               TypeRange inputTypes,
                                               TypeRange outputBufferTypes,
                                               TypeRange initTensorTypes,
                                               TypeRange resultTypes) {
  Region &region = *result.addRegion();
  buildNamedStructuredOpRegionAndAttributesImpl<NamedStructuredOpType>(
      opBuilder, region, inputTypes, outputBufferTypes, initTensorTypes,
      resultTypes, [&](unsigned expected, unsigned actual) {
        llvm::errs() << "region expects " << expected << " args, got "
                     << actual;
        assert(expected != actual && "incorrect number of arguments");
      });
}

template <typename NamedStructuredOpType>
static ParseResult
parseNamedStructuredOpRegion(OpAsmParser &parser, Region &region,
                             TypeRange inputTypes, TypeRange outputBufferTypes,
                             TypeRange initTensorTypes, TypeRange resultTypes) {
  ParseResult res = success();
  OpBuilder opBuilder(parser.getBuilder().getContext());
  buildNamedStructuredOpRegionAndAttributesImpl<NamedStructuredOpType>(
      opBuilder, region, inputTypes, outputBufferTypes, initTensorTypes,
      resultTypes, [&](unsigned expected, unsigned actual) {
        res = parser.emitError(parser.getCurrentLocation(),
                               llvm::formatv("region expects {0} args, got {1}",
                                             expected, actual));
      });
  return res;
}

static ParseResult
parseNamedStructuredOpResults(OpAsmParser &parser,
                              SmallVectorImpl<Type> &resultTypes) {
  if (succeeded(parser.parseOptionalArrow()))
    if (parser.parseTypeList(resultTypes))
      return failure();
  return success();
}

static ParseResult
parseCommonStructuredOpParts(OpAsmParser &parser, OperationState &result,
                             SmallVectorImpl<Type> &inputTypes,
                             SmallVectorImpl<Type> &outputBufferTypes,
                             SmallVectorImpl<Type> &initTensorTypes) {
  llvm::SMLoc inputsOperandsLoc, outputBuffersOperandsLoc,
      initTensorsOperandsLoc;
  SmallVector<OpAsmParser::OperandType, 4> inputsOperands,
      outputBuffersOperands, initTensorsOperands;

  parser.parseOptionalAttrDict(result.attributes);

  if (succeeded(parser.parseOptionalKeyword("ins"))) {
    if (parser.parseLParen())
      return failure();

    inputsOperandsLoc = parser.getCurrentLocation();
    if (parser.parseOperandList(inputsOperands) ||
        parser.parseColonTypeList(inputTypes) || parser.parseRParen())
      return failure();
  }

  if (succeeded(parser.parseOptionalKeyword("outs"))) {
    outputBuffersOperandsLoc = parser.getCurrentLocation();
    if (parser.parseLParen() ||
        parser.parseOperandList(outputBuffersOperands) ||
        parser.parseColonTypeList(outputBufferTypes) || parser.parseRParen())
      return failure();
  }
  if (succeeded(parser.parseOptionalKeyword("init"))) {
    initTensorsOperandsLoc = parser.getCurrentLocation();
    if (parser.parseLParen() || parser.parseOperandList(initTensorsOperands) ||
        parser.parseColonTypeList(initTensorTypes) || parser.parseRParen())
      return failure();
  }

  if (parser.resolveOperands(inputsOperands, inputTypes, inputsOperandsLoc,
                             result.operands) ||
      parser.resolveOperands(outputBuffersOperands, outputBufferTypes,
                             outputBuffersOperandsLoc, result.operands) ||
      parser.resolveOperands(initTensorsOperands, initTensorTypes,
                             initTensorsOperandsLoc, result.operands))
    return failure();

  result.addAttribute("operand_segment_sizes",
                      parser.getBuilder().getI32VectorAttr(
                          {static_cast<int32_t>(inputsOperands.size()),
                           static_cast<int32_t>(outputBuffersOperands.size()),
                           static_cast<int32_t>(initTensorsOperands.size())}));
  return success();
}

template <typename NamedStructuredOpType>
static ParseResult parseNamedStructuredOp(OpAsmParser &parser,
                                          OperationState &result) {
  SmallVector<Type, 1> inputTypes, outputBufferTypes, initTensorTypes;
  if (parseCommonStructuredOpParts(parser, result, inputTypes,
                                   outputBufferTypes, initTensorTypes))
    return failure();

  // TODO: consider merging results parsing into region parsing.
  // Need to wait for declarative assembly resolution to decide.
  SmallVector<Type, 1> outputTensorsTypes;
  if (parseNamedStructuredOpResults(parser, outputTensorsTypes))
    return failure();
  result.addTypes(outputTensorsTypes);

  std::unique_ptr<Region> region = std::make_unique<Region>();
  if (parseNamedStructuredOpRegion<NamedStructuredOpType>(
          parser, *region, inputTypes, outputBufferTypes, initTensorTypes,
          outputTensorsTypes))
    return failure();
  result.addRegion(std::move(region));

  return success();
}

static void printNamedStructuredOpResults(OpAsmPrinter &p,
                                          TypeRange resultTypes) {
  if (resultTypes.empty())
    return;
  p.printOptionalArrowTypeList(resultTypes);
}

template <typename NamedStructuredOpType>
static void printCommonStructuredOpParts(OpAsmPrinter &p,
                                         NamedStructuredOpType op) {
  if (!op.inputs().empty())
    p << " ins(" << op.inputs() << " : " << op.inputs().getTypes() << ")";
  if (!op.output_buffers().empty())
    p << " outs(" << op.output_buffers() << " : "
      << op.output_buffers().getTypes() << ")";
  if (!op.init_tensors().empty())
    p << " init(" << op.init_tensors() << " : " << op.init_tensors().getTypes()
      << ") ";
}

template <typename NamedStructuredOpType>
static void printNamedStructuredOp(OpAsmPrinter &p, NamedStructuredOpType op) {
  p << op.getOperationName();
  p.printOptionalAttrDict(op.getAttrs(),
                          /*elidedAttrs=*/{"operand_segment_sizes"});

  // Printing is shared with generic ops, except for the region and
  // attributes.
  printCommonStructuredOpParts(p, op);

  // Results printing.
  printNamedStructuredOpResults(p, op.result_tensors().getTypes());

  // Region is elided.
}

template <typename NamedStructuredOpType>
static LogicalResult verifyNamedStructuredOp(NamedStructuredOpType op) {
  return verifyGenericOp<NamedStructuredOpType>(op);
}

namespace {
struct EraseDeadLinalgOp : public RewritePattern {
  EraseDeadLinalgOp(PatternBenefit benefit = 1)
      : RewritePattern(benefit, MatchAnyOpTypeTag()) {}

  LogicalResult matchAndRewrite(Operation *op,
                                PatternRewriter &rewriter) const override {
    auto linalgOp = dyn_cast<LinalgOp>(op);
    if (!linalgOp)
      return failure();
    for (Value v : linalgOp.getInputsAndOutputBuffers()) {
      // Linalg "inputs" may be either tensor or memref type.
      // tensor<0xelt_type> is a convention that may not always mean
      // "0 iterations". Only erase in cases we see memref<...x0x...>.
      auto mt = v.getType().dyn_cast<MemRefType>();
      if (!mt)
        continue;
      if (llvm::is_contained(mt.getShape(), 0)) {
        rewriter.eraseOp(linalgOp);
        return success();
      }
    }
    return failure();
  }
};

struct FoldTensorCastOp : public RewritePattern {
  FoldTensorCastOp(PatternBenefit benefit = 1)
      : RewritePattern(benefit, MatchAnyOpTypeTag()) {}

  LogicalResult matchAndRewrite(Operation *op,
                                PatternRewriter &rewriter) const override {
    auto linalgOp = dyn_cast<LinalgOp>(op);
    if (!linalgOp)
      return failure();

    // If no operand comes from a TensorCastOp and can be folded then fail.
    bool hasTensorCastOperand =
        llvm::any_of(linalgOp.getShapedOperands(), [&](Value v) {
          if (v.isa<BlockArgument>())
            return false;
          auto castOp = v.getDefiningOp<TensorCastOp>();
          return castOp && canFoldIntoConsumerOp(castOp);
        });
    if (!hasTensorCastOperand)
      return failure();

    SmallVector<Type, 4> newResultTypes;
    newResultTypes.reserve(op->getNumResults());
    SmallVector<Value, 4> newOperands;
    newOperands.reserve(op->getNumOperands());
    // Inputs may fold.
    for (Value v : linalgOp.getInputs()) {
      auto tensorCastOp = v.getDefiningOp<TensorCastOp>();
      newOperands.push_back(
          canFoldIntoConsumerOp(tensorCastOp) ? tensorCastOp.source() : v);
    }
    // Output buffers are memrefs, they don't fold.
    newOperands.append(linalgOp.getOutputBuffers().begin(),
                       linalgOp.getOutputBuffers().end());
    // Init tensors may fold, in which case the resultType must also change.
    for (Value v : linalgOp.getInitTensors()) {
      auto tensorCastOp = v.getDefiningOp<TensorCastOp>();
      bool fold = canFoldIntoConsumerOp(tensorCastOp);
      newOperands.push_back(fold ? tensorCastOp.getOperand() : v);
      newResultTypes.push_back(newOperands.back().getType());
    }
    auto extraOperands = linalgOp.getAssumedNonShapedOperands();
    newOperands.append(extraOperands.begin(), extraOperands.end());
    // Clone op.
    Operation *newOp =
        linalgOp.clone(rewriter, op->getLoc(), newResultTypes, newOperands);
    rewriter.replaceOp(op, newOp->getResults());

    return success();
  }
};
} // namespace

namespace {
// Deduplicate redundant args of a linalg op.
// An arg is redundant if it has the same Value and indexing map as another.
struct DeduplicateInputs : public RewritePattern {
  DeduplicateInputs(PatternBenefit benefit = 1)
      : RewritePattern(benefit, MatchAnyOpTypeTag()) {}

  LogicalResult matchAndRewrite(Operation *op,
                                PatternRewriter &rewriter) const override {
    // This pattern reduces the number of arguments of an op, which breaks
    // the invariants of semantically charged named ops.
    if (!isa<GenericOp, IndexedGenericOp>(op))
      return failure();
    auto linalgOp = cast<LinalgOp>(op);

    // Associate each input to an equivalent "canonical" input that has the same
    // Value and indexing map.
    //
    // In the non-duplicate case, input `i` will have canonical input `i`. But
    // in the case of duplicated inputs, the canonical input could be some other
    // input `< i`. That is, a later input will have some earlier input as its
    // canonical input.
    llvm::SmallDenseMap<std::pair<Value, AffineMap>, int> canonicalInput;
    // For later remapping tasks like deduplicating payload block arguments,
    // having a simple "inputIndex -> canonicalInputIndex" integer mapping is
    // convenient.
    SmallVector<int, 6> canonicalInputIndices;
    for (int i = 0, e = linalgOp.getNumInputs(); i != e; i++) {
      Value input = linalgOp.getInput(i);
      AffineMap indexingMap = linalgOp.getInputIndexingMap(i);
      // STL-like maps have a convenient behavior for our use case here. In the
      // case of duplicate keys, the insertion is rejected, and the returned
      // iterator gives access to the value already in the map.
      auto pair = canonicalInput.insert({{input, indexingMap}, i});
      canonicalInputIndices.push_back(pair.first->second);
    }

    // If there are no duplicate args, then bail out.
    if (canonicalInput.size() == linalgOp.getNumInputs())
      return failure();

    // The operands for the newly canonicalized op.
    SmallVector<Value, 6> newOperands;
    for (auto v : llvm::enumerate(linalgOp.getInputs()))
      if (canonicalInputIndices[v.index()] == static_cast<int>(v.index()))
        newOperands.push_back(v.value());
    llvm::append_range(newOperands, linalgOp.getOutputBuffers());
    llvm::append_range(newOperands, linalgOp.getInitTensors());
    llvm::append_range(newOperands, linalgOp.getAssumedNonShapedOperands());

    // Clone the old op with new operands.
    Operation *newOp = linalgOp.clone(rewriter, op->getLoc(),
                                      op->getResultTypes(), newOperands);
    auto newLinalgOp = cast<LinalgOp>(newOp);

    // Repair the indexing maps by filtering out the ones that have been
    // eliminated.
    SmallVector<AffineMap, 6> newIndexingMaps;
    for (int i = 0, e = newLinalgOp.getNumInputs(); i != e; i++)
      if (canonicalInputIndices[i] == i)
        newIndexingMaps.push_back(newLinalgOp.getIndexingMap(i));
    for (int i = 0, e = newLinalgOp.getNumOutputs(); i != e; i++)
      newIndexingMaps.push_back(newLinalgOp.getOutputIndexingMap(i));
    newOp->setAttr("indexing_maps",
                   rewriter.getAffineMapArrayAttr(newIndexingMaps));

    // Set the number of inputs to the new value. The `clone` call above kept
    // the value from the original op.
    newLinalgOp.setNumInputs(canonicalInput.size());

    // linalg.indexed_generic payloads have additional arguments prepended to
    // the block arg list. The number of such args is one per dimension of the
    // iteration space.
    int bbArgBaseOffset = 0;
    if (isa<IndexedGenericOp>(op))
      bbArgBaseOffset = newIndexingMaps[0].getNumInputs();

    // Repair the payload entry block by RAUW'ing redundant arguments and
    // erasing them.
    Block &payload = newOp->getRegion(0).front();
    for (int i = 0, e = linalgOp.getNumInputs(); i < e; i++) {
      // Iterate in reverse, so that we erase later args first, preventing the
      // argument list from shifting unexpectedly and invalidating all our
      // indices.
      int reversed = e - i - 1;
      int canonicalIndex = canonicalInputIndices[reversed];
      if (canonicalInputIndices[reversed] == reversed)
        continue;
      payload.getArgument(bbArgBaseOffset + reversed)
          .replaceAllUsesWith(
              payload.getArgument(bbArgBaseOffset + canonicalIndex));
      payload.eraseArgument(bbArgBaseOffset + reversed);
    }

    rewriter.replaceOp(op, newOp->getResults());
    return success();
  }
};
} // namespace

#define CANONICALIZERS_AND_FOLDERS(XXX)                                        \
  void XXX::getCanonicalizationPatterns(OwningRewritePatternList &results,     \
                                        MLIRContext *context) {                \
    results.insert<EraseDeadLinalgOp>();                                       \
    results.insert<FoldTensorCastOp>();                                        \
    results.insert<DeduplicateInputs>();                                       \
  }                                                                            \
                                                                               \
  LogicalResult XXX::fold(ArrayRef<Attribute>,                                 \
                          SmallVectorImpl<OpFoldResult> &) {                   \
    return foldMemRefCast(*this);                                              \
  }

CANONICALIZERS_AND_FOLDERS(ConvOp)
CANONICALIZERS_AND_FOLDERS(PoolingMaxOp)
CANONICALIZERS_AND_FOLDERS(PoolingMinOp)
CANONICALIZERS_AND_FOLDERS(PoolingSumOp)
CANONICALIZERS_AND_FOLDERS(CopyOp)
CANONICALIZERS_AND_FOLDERS(FillOp)
CANONICALIZERS_AND_FOLDERS(GenericOp)
CANONICALIZERS_AND_FOLDERS(IndexedGenericOp)

// All named ops canonicalizers and folders are auto-generated in the
// .cpp.inc.
