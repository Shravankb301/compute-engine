#include "larq_compute_engine/mlir/tf_tfl_passes.h"

#include "larq_compute_engine/mlir/transforms/passes.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/Passes.h"
#include "tensorflow/compiler/mlir/lite/quantization/quantization_config.h"
#include "tensorflow/compiler/mlir/lite/quantization/quantization_passes.h"
#include "tensorflow/compiler/mlir/lite/transforms/passes.h"
#include "tensorflow/compiler/mlir/lite/utils/fake_quant_utils.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/passes.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/tf_saved_model_passes.h"

namespace mlir {
/// Create a pass to convert from the TFExecutor to the TF control dialect.
std::unique_ptr<OperationPass<mlir::func::FuncOp>>
CreateTFExecutorToControlDialectConversion();
}  // namespace mlir

namespace tensorflow {
namespace {
// Data layout supported by TFLite.
const char kTFLiteDataLayout[] = "NHWC";
}  // namespace

namespace {
void AddQuantizationPasses(const mlir::quant::QuantizationSpecs& quant_specs,
                           mlir::OpPassManager& pass_manager) {
  pass_manager.addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreatePrepareQuantizePass(quant_specs));
  pass_manager.addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreateLCEQuantizePass());
  if (quant_specs.default_ranges.first.hasValue() ||
      quant_specs.default_ranges.second.hasValue()) {
    pass_manager.addNestedPass<mlir::func::FuncOp>(
        mlir::TFL::CreateDefaultQuantParamsPass(
            quant_specs.default_ranges.first.getValueOr(0.0),
            quant_specs.default_ranges.second.getValueOr(0.0),
            quant_specs.IsSignedInferenceType()));
    pass_manager.addNestedPass<mlir::func::FuncOp>(
        mlir::TFL::CreateLCEQuantizePass());
  }
  pass_manager.addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreateQuantizePass());
  bool emit_quant_adaptor_ops =
      quant_specs.inference_type != quant_specs.inference_input_type;
  pass_manager.addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreatePostQuantizePass(emit_quant_adaptor_ops));
  pass_manager.addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreateLCEQuantizePass());
  pass_manager.addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreatePostQuantizePass(emit_quant_adaptor_ops));
  pass_manager.addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreateOptimizeOpOrderPass());
  // Add optimization pass after quantization for additional fusing
  // opportunities.
  pass_manager.addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreateOptimizePass(true));
}
}  // namespace

// This is the early part of the conversion in isolation. This enables a caller
// to inject more information in the middle of the conversion before resuming
// it.
void AddPreVariableFreezingTFToLCETFLConversionPasses(
    mlir::OpPassManager* pass_manager) {
  // This pass wraps all the tf.FakeQuant ops in a custom op so they are not
  // folded before being converted to tfl.quantize and tfl.dequantize ops.
  auto wrapped_ops = mlir::TFL::AllTfFakeQuantOps();
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreateRaiseCustomOpsPass(wrapped_ops));

  mlir::TF::StandardPipelineOptions standard_pipeline_options;
  standard_pipeline_options.enable_inliner = false;
  standard_pipeline_options.form_clusters = false;
  mlir::TF::CreateTFStandardPipeline(*pass_manager, standard_pipeline_options);
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::TF::CreateDeviceIndexSelectorPass());

  // Add canonicalize pass to remove no-op session initializer pass.
  pass_manager->addPass(mlir::createCanonicalizerPass());

  pass_manager->addPass(mlir::TF::CreateTFShapeInferencePass());
  pass_manager->addPass(mlir::TF::CreateTFFunctionalControlFlowToRegions());

  // The conversion pipeline has to follow the following orders:
  // 1) Saved model related optimization like decompose resource ops
  // 2) Convert composite functions like lstm/rnns, along with proper function
  // inlining & dce.
  // 3) Lower static tensor list pass.

  // This decomposes resource ops like ResourceGather into read-variable op
  // followed by gather. This is used when the saved model import path is used
  // during which resources dont get frozen in the python layer.
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::TFDevice::CreateDecomposeResourceOpsPass());

  pass_manager->addPass(mlir::TF::CreateTFRegionControlFlowToFunctional());
}

// This is the later part of the conversion in isolation. This enables a caller
// to resume the conversion after injecting more information in the middle of
// it.
void AddPostVariableFreezingTFToLCETFLConversionPasses(
    llvm::StringRef saved_model_dir,
    const mlir::quant::QuantizationSpecs& quant_specs,
    mlir::OpPassManager* pass_manager, const LCETarget target) {
  // Note:
  // We need to fuse composite ops before LowerStaticTensorList pass.
  // The tensorflow list is not supported right now by that pass.
  // Enable fusing composite ops that can be lowered to built-in TFLite ops.
  pass_manager->addPass(mlir::TFL::CreatePrepareCompositeFunctionsPass());

  pass_manager->addPass(mlir::createInlinerPass());
  pass_manager->addPass(mlir::createSymbolDCEPass());

  pass_manager->addPass(mlir::TFL::CreateLowerStaticTensorListPass());

  // This pass does resource analysis of saved model global tensors and marks
  // those deemed read-only as immutable.
  pass_manager->addPass(
      mlir::tf_saved_model::CreateOptimizeGlobalTensorsPass());

  // Set the batch size of the function input to 1 and let shape inference
  // propagate this in the next pass.
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::CreateSetBatchSizePass());

  // Add a shape inference pass to optimize away the unnecessary casts.
  pass_manager->addPass(mlir::TF::CreateTFShapeInferencePass());

  // Legalize while early to allow further constant folding.
  // TODO(jpienaar): This may not actually matter as we do canonicalization
  // after the legalize below, for now it needs to be below the above passes
  // that work on TF dialect and before inliner so that the function calls in
  // body and cond are inlined for optimization.
  pass_manager->addPass(mlir::TFL::CreateLegalizeTFWhilePass());

  // Add function inlining pass. Both TF and TFLite dialects are opted into
  // function inliner interface.
  pass_manager->addPass(mlir::createInlinerPass());

  // Remove passthrough ops early so constant folding can happen before
  // LCE ops are injected
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreateOpRemovalPass());

  // The following pass used to be just after createSymbolDCEPass but we move it
  // before createCanonicalizerPass because without it, the tf.Sign op is not
  // constant-folded.
  //
  // This pass 'freezes' immutable global tensors and inlines them as tf
  // constant ops.
  pass_manager->addPass(mlir::tf_saved_model::CreateFreezeGlobalTensorsPass());

  if (!saved_model_dir.empty()) {
    // This pass 'freezes' tf saved model asset ops and inlines as string values
    // in a format of the tf constant op.
    pass_manager->addPass(
        mlir::tf_saved_model::CreateFreezeAssetsPass(saved_model_dir.str()));
  }

  // Reduce operands of TFL::While without changing the outcome.
  // It needs to stay here because:
  // 1. WhileOps are in TFL dialect.
  // 2. The body and cond are inlined.
  // 3. We need to do this before while canonicalization, otherwise it would be
  //   difficult to find dependencies.
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreateReduceWhileOperandsPass());
  // Canonicalization includes const folding, which is utilized here to optimize
  // away ops that can't get constant folded after PrepareTF pass. For example,
  // tf.Conv2D is split into tf.Transpose and tfl.Conv2D.
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::createCanonicalizerPass());
  pass_manager->addNestedPass<mlir::func::FuncOp>(mlir::createCSEPass());
  // This pass does dead code elimination based on symbol visibility.
  pass_manager->addPass(mlir::createSymbolDCEPass());

  // Run shape inference after variables are converted to constants.
  pass_manager->addPass(mlir::TF::CreateTFShapeInferencePass());
  // Force layout supported by TFLite, this will transpose the data
  // to match 'kTFLiteDataLayout'
  mlir::TF::LayoutOptimizationPipelineOptions layout_optimization_options;
  layout_optimization_options.force_data_format = kTFLiteDataLayout;
  layout_optimization_options.skip_fold_transpose_in_ops = true;
  mlir::TF::CreateLayoutOptimizationPipeline(
      pass_manager->nest<mlir::func::FuncOp>(), layout_optimization_options);
  // Inject Larq Compute Engine Ops
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreatePrepareLCEPass(target));
  // Prepare for TFLite dialect, rerun canonicalization, and then legalize to
  // the TFLite dialect.
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreatePrepareTFPass(true, false));
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::createCanonicalizerPass());
  // Add a shape inference pass to optimize away the unnecessary casts.
  // This also fixes the unranked shapes due to TF ops constant folding.
  // TODO(fengliuai): remove this pass if TableGen patterns have a better
  // to control the shapes for the intermediate results.
  pass_manager->addPass(mlir::TF::CreateTFShapeInferencePass());

  // Inline function calls that left in the graph after folding functional
  // control flow ops (IfOp, CaseOp).
  pass_manager->addPass(mlir::createInlinerPass());

  // This pass removes the asset file dependencies in hash table use cases.
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::TF::CreateInitTextFileToImportPass(saved_model_dir.str()));

  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreateLegalizeTFPass(true));
  pass_manager->addPass(mlir::TFL::CreateAnalyzeVariablesPass());
  pass_manager->addPass(mlir::TFL::CreateLegalizeVariablesPass());
  pass_manager->addPass(mlir::TFL::CreateLegalizeHashTablesPass());
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreateOptimizeLCEPass(target));
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreateOptimizePass(true));
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreateOptimizeLCEPass(target));
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreateBitpackWeightsLCEPass());

  // This pass operates on TensorFlow ops but is triggered after legalization
  // so that it can target constants introduced once TensorFlow Identity ops
  // are removed during legalization.
  pass_manager->addPass(mlir::TFL::CreateOptimizeFunctionalOpsPass());
  std::vector<std::string> empty_wrapped_ops({});
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreateRaiseCustomOpsPass(empty_wrapped_ops));
  pass_manager->addPass(mlir::createSymbolDCEPass());
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::createCanonicalizerPass());
  pass_manager->addNestedPass<mlir::func::FuncOp>(mlir::createCSEPass());

  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreateFusePaddingPass());

  // Run quantization after all the floating point model conversion is
  // completed.
  if (quant_specs.RunPropagationAndRewriteQuantizationPasses()) {
    AddQuantizationPasses(quant_specs, *pass_manager);
    // Remove unnecessary QDQs while handling QAT models.
    pass_manager->addNestedPass<mlir::func::FuncOp>(
        mlir::TFL::CreatePostQuantizeRemoveQDQPass());
  }
  pass_manager->addPass(mlir::createCanonicalizerPass());

  // This pass should be always at the end of the model
  // conversion (even after quantization). Some TFL ops like unidirectional
  // sequence lstm will have stateful operands and some optimization passes
  // will merge those operands if they have identical values & types. However,
  // it's not desired by TFL. This pass serves as a "fix" pass to split the
  // merged inputs until we have 1st class variable support or reuse
  // tf.variable to model this.
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreateSplitMergedOperandsPass());

  // Add CallOnceOp when there is a session initializer function in tf saved
  // model dialect.
  pass_manager->addPass(
      mlir::TFL::CreateInsertCallOnceOpFromSessionInitializerPass());
  pass_manager->addPass(mlir::TFL::CreateUnfoldLargeSplatConstantPass());
  pass_manager->addPass(mlir::TFL::CreateWhileOutlinePass());
  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreateRuntimeVerifyPass());

  pass_manager->addNestedPass<mlir::func::FuncOp>(
      mlir::TFL::CreateLegalizeLCEPass());
}

}  // namespace tensorflow
