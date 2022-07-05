// Copyright (c) 2018 Microsoft Corporation
// Licensed under the MIT license.
// Author: Paul Koch <code@koch.ninja>

#include "precompiled_header_cpp.hpp"

#include <stddef.h> // size_t, ptrdiff_t
#include <string.h> // memcpy

#include "ebm_native.h"
#include "logging.h"
#include "zones.h"

#include "ebm_internal.hpp"

// FeatureGroup.h depends on FeatureInternal.h
#include "FeatureGroup.hpp"

#include "BoosterCore.hpp"
#include "BoosterShell.hpp"

namespace DEFINED_ZONE_NAME {
#ifndef DEFINED_ZONE_NAME
#error DEFINED_ZONE_NAME must be defined
#endif // DEFINED_ZONE_NAME

extern void ApplyTermUpdateTraining(
   BoosterShell * const pBoosterShell,
   const FeatureGroup * const pFeatureGroup
);

extern double ApplyTermUpdateValidation(
   BoosterShell * const pBoosterShell,
   const FeatureGroup * const pFeatureGroup
);

static ErrorEbmType ApplyTermUpdateInternal(
   BoosterShell * const pBoosterShell,
   double * const pValidationMetricReturn
) {
   LOG_0(TraceLevelVerbose, "Entered ApplyTermUpdateInternal");

   ErrorEbmType error;

   BoosterCore * const pBoosterCore = pBoosterShell->GetBoosterCore();
   const size_t iFeatureGroup = pBoosterShell->GetFeatureGroupIndex();
   const FeatureGroup * const pFeatureGroup = pBoosterCore->GetFeatureGroups()[iFeatureGroup];

   error = pBoosterShell->GetAccumulatedModelUpdate()->Expand(pFeatureGroup);
   if(Error_None != error) {
      if(nullptr != pValidationMetricReturn) {
         *pValidationMetricReturn = double { 0 };
      }
      return error;
   }

   // m_apCurrentModel can be null if there are no featureGroups (but we have an feature group index), 
   // or if the target has 1 or 0 classes (which we check before calling this function), so it shouldn't be possible to be null
   EBM_ASSERT(nullptr != pBoosterCore->GetCurrentModel());
   // m_apCurrentModel can be null if there are no featureGroups (but we have an feature group index), 
   // or if the target has 1 or 0 classes (which we check before calling this function), so it shouldn't be possible to be null
   EBM_ASSERT(nullptr != pBoosterCore->GetBestModel());

   const FloatFast * const aUpdateScores = pBoosterShell->GetAccumulatedModelUpdate()->GetScoresPointer();

   // our caller can give us one of these bad types of inputs:
   //  1) NaN values
   //  2) +-infinity
   //  3) numbers that are fine, but when added to our existing model overflow to +-infinity
   // Our caller should really just not pass us the first two, but it's hard for our caller to protect against giving us values that won't overflow
   // so we should have some reasonable way to handle them.  If we were meant to overflow, logits or regression values at the maximum/minimum values
   // of doubles should be so close to infinity that it won't matter, and then you can at least graph them without overflowing to special values
   // We have the same problem when we go to make changes to the individual sample updates, but there we can have two graphs that combined push towards
   // an overflow to +-infinity.  We just ignore those overflows, because checking for them would add branches that we don't want, and we can just
   // propagate +-infinity and NaN values to the point where we get a metric and that should cause our client to stop boosting when our metric
   // overlfows and gets converted to the maximum value which will mean the metric won't be changing or improving after that.
   // This is an acceptable compromise.  We protect our models since the user might want to extract them AFTER we overlfow our measurment metric
   // so we don't want to overflow the values to NaN or +-infinity there, and it's very cheap for us to check for overflows when applying the model
   pBoosterCore->GetCurrentModel()[iFeatureGroup]->AddExpandedWithBadValueProtection(aUpdateScores);

   if(0 != pBoosterCore->GetTrainingSet()->GetCountSamples()) {
      ApplyTermUpdateTraining(pBoosterShell, pFeatureGroup);
   }

   double modelMetric = 0.0;
   if(0 != pBoosterCore->GetValidationSet()->GetCountSamples()) {
      // if there is no validation set, it's pretty hard to know what the metric we'll get for our validation set
      // we could in theory return anything from zero to infinity or possibly, NaN (probably legally the best), but we return 0 here
      // because we want to kick our caller out of any loop it might be calling us in.  Infinity and NaN are odd values that might cause problems in
      // a caller that isn't expecting those values, so 0 is the safest option, and our caller can avoid the situation entirely by not calling
      // us with zero count validation sets

      // if the count of training samples is zero, don't update the best model (it will stay as all zeros), and we don't need to update our 
      // non-existant training set either C++ doesn't define what happens when you compare NaN to annother number.  It probably follows IEEE 754, 
      // but it isn't guaranteed, so let's check for zero samples in the validation set this better way
      // https://stackoverflow.com/questions/31225264/what-is-the-result-of-comparing-a-number-with-nan

      modelMetric = ApplyTermUpdateValidation(pBoosterShell, pFeatureGroup);

      EBM_ASSERT(!std::isnan(modelMetric)); // NaNs can happen, but we should have converted them
      EBM_ASSERT(!std::isinf(modelMetric)); // +infinity can happen, but we should have converted it
      // both log loss and RMSE need to be above zero.  If we got a negative number due to floating point 
      // instability we should have previously converted it to zero.
      EBM_ASSERT(0.0 <= modelMetric);

      // modelMetric is either logloss (classification) or mean squared error (mse) (regression).  In either case we want to minimize it.
      if(LIKELY(modelMetric < pBoosterCore->GetBestModelMetric())) {
         // we keep on improving, so this is more likely than not, and we'll exit if it becomes negative a lot
         pBoosterCore->SetBestModelMetric(modelMetric);

         // TODO : in the future don't copy over all CompressibleTensors.  We only need to copy the ones that changed, which we can detect if we 
         // use a linked list and array lookup for the same data structure
         size_t iModel = 0;
         size_t iModelEnd = pBoosterCore->GetCountFeatureGroups();
         do {
            error = pBoosterCore->GetBestModel()[iModel]->Copy(*pBoosterCore->GetCurrentModel()[iModel]);
            if(Error_None != error) {
               if(nullptr != pValidationMetricReturn) {
                  *pValidationMetricReturn = double { 0 };
               }
               LOG_0(TraceLevelVerbose, "Exited ApplyTermUpdateInternal with memory allocation error in copy");
               return error;
            }
            ++iModel;
         } while(iModel != iModelEnd);
      }
   }
   if(nullptr != pValidationMetricReturn) {
      *pValidationMetricReturn = modelMetric;
   }

   LOG_0(TraceLevelVerbose, "Exited ApplyTermUpdateInternal");
   return Error_None;
}

// we made this a global because if we had put this variable inside the BoosterCore object, then we would need to dereference that before 
// getting the count.  By making this global we can send a log message incase a bad BoosterCore object is sent into us
// we only decrease the count if the count is non-zero, so at worst if there is a race condition then we'll output this log message more 
// times than desired, but we can live with that
static int g_cLogApplyTermUpdateParametersMessages = 10;

// TODO: validationMetricOut should be an average
EBM_NATIVE_IMPORT_EXPORT_BODY ErrorEbmType EBM_NATIVE_CALLING_CONVENTION ApplyTermUpdate(
   BoosterHandle boosterHandle,
   double * validationMetricOut
) {
   LOG_COUNTED_N(
      &g_cLogApplyTermUpdateParametersMessages,
      TraceLevelInfo,
      TraceLevelVerbose,
      "ApplyTermUpdate: "
      "boosterHandle=%p, "
      "validationMetricOut=%p"
      ,
      static_cast<void *>(boosterHandle),
      static_cast<void *>(validationMetricOut)
   );

   ErrorEbmType error;

   BoosterShell * const pBoosterShell = BoosterShell::GetBoosterShellFromHandle(boosterHandle);
   if(nullptr == pBoosterShell) {
      if(LIKELY(nullptr != validationMetricOut)) {
         *validationMetricOut = 0.0;
      }
      // already logged
      return Error_IllegalParamValue;
   }

   const size_t iFeatureGroup = pBoosterShell->GetFeatureGroupIndex();
   if(BoosterShell::k_illegalFeatureGroupIndex == iFeatureGroup) {
      if(LIKELY(nullptr != validationMetricOut)) {
         *validationMetricOut = 0.0;
      }
      LOG_0(TraceLevelError, "ERROR ApplyTermUpdate bad internal state.  No FeatureGroupIndex set");
      return Error_IllegalParamValue;
   }
   BoosterCore * const pBoosterCore = pBoosterShell->GetBoosterCore();
   EBM_ASSERT(nullptr != pBoosterCore);
   EBM_ASSERT(iFeatureGroup < pBoosterCore->GetCountFeatureGroups());
   EBM_ASSERT(nullptr != pBoosterCore->GetFeatureGroups());

   LOG_COUNTED_0(
      pBoosterCore->GetFeatureGroups()[iFeatureGroup]->GetPointerCountLogEnterApplyTermUpdateMessages(),
      TraceLevelInfo,
      TraceLevelVerbose,
      "Entered ApplyTermUpdate"
   );

   if(ptrdiff_t { 0 } == pBoosterCore->GetRuntimeLearningTypeOrCountTargetClasses() || ptrdiff_t { 1 } == pBoosterCore->GetRuntimeLearningTypeOrCountTargetClasses()) {
      // if there is only 1 target class for classification, then we can predict the output with 100% accuracy.  The model is a tensor with zero 
      // length array logits, which means for our representation that we have zero items in the array total.
      // since we can predit the output with 100% accuracy, our log loss is 0.
      if(nullptr != validationMetricOut) {
         *validationMetricOut = 0.0;
      }
      pBoosterShell->SetFeatureGroupIndex(BoosterShell::k_illegalFeatureGroupIndex);
      LOG_COUNTED_0(
         pBoosterCore->GetFeatureGroups()[iFeatureGroup]->GetPointerCountLogExitApplyTermUpdateMessages(),
         TraceLevelInfo,
         TraceLevelVerbose,
         "Exited ApplyTermUpdate. runtimeLearningTypeOrCountTargetClasses <= 1"
      );
      return Error_None;
   }

   error = ApplyTermUpdateInternal(
      pBoosterShell,
      validationMetricOut
   );

   pBoosterShell->SetFeatureGroupIndex(BoosterShell::k_illegalFeatureGroupIndex);

   if(Error_None != error) {
      LOG_N(TraceLevelWarning, "WARNING ApplyTermUpdate: return=%" ErrorEbmTypePrintf, error);
   }

   if(nullptr != validationMetricOut) {
      EBM_ASSERT(!std::isnan(*validationMetricOut)); // NaNs can happen, but we should have edited those before here
      EBM_ASSERT(!std::isinf(*validationMetricOut)); // infinities can happen, but we should have edited those before here
      // both log loss and RMSE need to be above zero.  We previously zero any values below zero, which can happen due to floating point instability.
      EBM_ASSERT(0.0 <= *validationMetricOut);
      LOG_COUNTED_N(
         pBoosterCore->GetFeatureGroups()[iFeatureGroup]->GetPointerCountLogExitApplyTermUpdateMessages(),
         TraceLevelInfo,
         TraceLevelVerbose,
         "Exited ApplyTermUpdate: "
         "*validationMetricOut=%le"
         , 
         *validationMetricOut
      );
   } else {
      LOG_COUNTED_0(
         pBoosterCore->GetFeatureGroups()[iFeatureGroup]->GetPointerCountLogExitApplyTermUpdateMessages(),
         TraceLevelInfo,
         TraceLevelVerbose,
         "Exited ApplyTermUpdate"
      );
   }
   return error;
}

// we made this a global because if we had put this variable inside the BoosterCore object, then we would need to dereference that before 
// getting the count.  By making this global we can send a log message incase a bad BoosterCore object is sent into us
// we only decrease the count if the count is non-zero, so at worst if there is a race condition then we'll output this log message more 
// times than desired, but we can live with that
static int g_cLogGetTermUpdateSplitsParametersMessages = 10;

EBM_NATIVE_IMPORT_EXPORT_BODY ErrorEbmType EBM_NATIVE_CALLING_CONVENTION GetTermUpdateSplits(
   BoosterHandle boosterHandle,
   IntEbmType indexDimension,
   IntEbmType * countSplitsInOut,
   IntEbmType * splitIndexesOut
) {
   LOG_COUNTED_N(
      &g_cLogGetTermUpdateSplitsParametersMessages,
      TraceLevelInfo,
      TraceLevelVerbose,
      "GetTermUpdateSplits: "
      "boosterHandle=%p, "
      "indexDimension=%" IntEbmTypePrintf ", "
      "countSplitsInOut=%p"
      "splitIndexesOut=%p"
      ,
      static_cast<void *>(boosterHandle),
      indexDimension, 
      static_cast<void *>(countSplitsInOut),
      static_cast<void *>(splitIndexesOut)
   );

   if(nullptr == countSplitsInOut) {
      LOG_0(TraceLevelError, "ERROR GetTermUpdateSplits countSplitsInOut cannot be nullptr");
      return Error_IllegalParamValue;
   }

   BoosterShell * const pBoosterShell = BoosterShell::GetBoosterShellFromHandle(boosterHandle);
   if(nullptr == pBoosterShell) {
      *countSplitsInOut = IntEbmType { 0 };
      // already logged
      return Error_IllegalParamValue;
   }

   const size_t iFeatureGroup = pBoosterShell->GetFeatureGroupIndex();
   if(BoosterShell::k_illegalFeatureGroupIndex == iFeatureGroup) {
      *countSplitsInOut = IntEbmType { 0 };
      LOG_0(TraceLevelError, "ERROR GetTermUpdateSplits bad internal state.  No FeatureGroupIndex set");
      return Error_IllegalParamValue;
   }
   BoosterCore * const pBoosterCore = pBoosterShell->GetBoosterCore();
   EBM_ASSERT(nullptr != pBoosterCore);
   EBM_ASSERT(iFeatureGroup < pBoosterCore->GetCountFeatureGroups());
   EBM_ASSERT(nullptr != pBoosterCore->GetFeatureGroups());
   const FeatureGroup * const pFeatureGroup = pBoosterCore->GetFeatureGroups()[iFeatureGroup];

   if(indexDimension < 0) {
      *countSplitsInOut = IntEbmType { 0 };
      LOG_0(TraceLevelError, "ERROR GetTermUpdateSplits indexDimension must be positive");
      return Error_IllegalParamValue;
   }
   if(static_cast<IntEbmType>(pFeatureGroup->GetCountDimensions()) <= indexDimension) {
      *countSplitsInOut = IntEbmType { 0 };
      LOG_0(TraceLevelError, "ERROR GetTermUpdateSplits indexDimension above the number of dimensions that we have");
      return Error_IllegalParamValue;
   }
   const size_t iDimension = static_cast<size_t>(indexDimension);

   const size_t cBins = pFeatureGroup->GetFeatureGroupEntries()[iDimension].m_pFeature->GetCountBins();
   // cBins started from IntEbmType, so we should be able to convert back safely
   if(*countSplitsInOut != static_cast<IntEbmType>(cBins - size_t { 1 })) {
      *countSplitsInOut = IntEbmType { 0 };
      LOG_0(TraceLevelError, "ERROR GetTermUpdateSplits bad split array length");
      return Error_IllegalParamValue;
   }

   const size_t cSplits = pBoosterShell->GetAccumulatedModelUpdate()->GetCountSplits(iDimension);
   EBM_ASSERT(cSplits < cBins);
   if(0 != cSplits) {
      if(nullptr == splitIndexesOut) {
         *countSplitsInOut = IntEbmType { 0 };
         LOG_0(TraceLevelError, "ERROR GetTermUpdateSplits splitIndexesOut cannot be nullptr");
         return Error_IllegalParamValue;
      }

      const ActiveDataType * pSplitIndexesFrom = pBoosterShell->GetAccumulatedModelUpdate()->GetSplitPointer(iDimension);
      IntEbmType * pSplitIndexesTo = splitIndexesOut;
      IntEbmType * pSplitIndexesToEnd = splitIndexesOut + cSplits;
      do {
         const ActiveDataType indexSplit = *pSplitIndexesFrom;
         ++pSplitIndexesFrom;

         EBM_ASSERT(!IsConvertError<IntEbmType>(indexSplit)); // the total count works so the index should too
         *pSplitIndexesTo = static_cast<IntEbmType>(indexSplit);

         ++pSplitIndexesTo;
      } while(pSplitIndexesToEnd != pSplitIndexesTo);
   }

   EBM_ASSERT(!IsConvertError<IntEbmType>(cSplits)); // cSplits originally came from an IntEbmType

   *countSplitsInOut = static_cast<IntEbmType>(cSplits);
   return Error_None;
}

// we made this a global because if we had put this variable inside the BoosterCore object, then we would need to dereference that before 
// getting the count.  By making this global we can send a log message incase a bad BoosterCore object is sent into us
// we only decrease the count if the count is non-zero, so at worst if there is a race condition then we'll output this log message more 
// times than desired, but we can live with that
static int g_cLogGetTermUpdateExpandedParametersMessages = 10;

EBM_NATIVE_IMPORT_EXPORT_BODY ErrorEbmType EBM_NATIVE_CALLING_CONVENTION GetTermUpdateExpanded(
   BoosterHandle boosterHandle,
   double * updateScoresTensorOut
) {
   LOG_COUNTED_N(
      &g_cLogGetTermUpdateExpandedParametersMessages,
      TraceLevelInfo,
      TraceLevelVerbose,
      "GetTermUpdateExpanded: "
      "boosterHandle=%p, "
      "updateScoresTensorOut=%p"
      ,
      static_cast<void *>(boosterHandle),
      static_cast<void *>(updateScoresTensorOut)
   );

   ErrorEbmType error;

   BoosterShell * const pBoosterShell = BoosterShell::GetBoosterShellFromHandle(boosterHandle);
   if(nullptr == pBoosterShell) {
      // already logged
      return Error_IllegalParamValue;
   }

   const size_t iFeatureGroup = pBoosterShell->GetFeatureGroupIndex();
   if(BoosterShell::k_illegalFeatureGroupIndex == iFeatureGroup) {
      LOG_0(TraceLevelError, "ERROR GetTermUpdateExpanded bad internal state.  No FeatureGroupIndex set");
      return Error_IllegalParamValue; // technically we're in an illegal state, but why split hairs
   }
   BoosterCore * const pBoosterCore = pBoosterShell->GetBoosterCore();
   EBM_ASSERT(nullptr != pBoosterCore);
   EBM_ASSERT(iFeatureGroup < pBoosterCore->GetCountFeatureGroups());
   EBM_ASSERT(nullptr != pBoosterCore->GetFeatureGroups());

   if(ptrdiff_t { 0 } == pBoosterCore->GetRuntimeLearningTypeOrCountTargetClasses() || ptrdiff_t { 1 } == pBoosterCore->GetRuntimeLearningTypeOrCountTargetClasses()) {
      return Error_None;
   }

   const FeatureGroup * const pFeatureGroup = pBoosterCore->GetFeatureGroups()[iFeatureGroup];
   error = pBoosterShell->GetAccumulatedModelUpdate()->Expand(pFeatureGroup);
   if(Error_None != error) {
      return error;
   }

   const size_t cDimensions = pFeatureGroup->GetCountDimensions();
   size_t cScores = GetVectorLength(pBoosterCore->GetRuntimeLearningTypeOrCountTargetClasses());
   if(0 != cDimensions) {
      const FeatureGroupEntry * pFeatureGroupEntry = pFeatureGroup->GetFeatureGroupEntries();
      const FeatureGroupEntry * const pFeatureGroupEntryEnd = &pFeatureGroupEntry[cDimensions];
      do {
         const size_t cBins = pFeatureGroupEntry->m_pFeature->GetCountBins();
         // we've allocated this memory, so it should be reachable, so these numbers should multiply
         EBM_ASSERT(!IsMultiplyError(cScores, cBins));
         cScores *= cBins;
         ++pFeatureGroupEntry;
      } while(pFeatureGroupEntryEnd != pFeatureGroupEntry);
   }
   const FloatFast * const aUpdateScores = pBoosterShell->GetAccumulatedModelUpdate()->GetScoresPointer();
   // we've allocated this memory, so it should be reachable, so these numbers should multiply
   EBM_ASSERT(!IsMultiplyError(sizeof(*updateScoresTensorOut), cScores));
   EBM_ASSERT(!IsMultiplyError(sizeof(*aUpdateScores), cScores));
   static_assert(sizeof(*updateScoresTensorOut) == sizeof(*aUpdateScores), "float mismatch");
   memcpy(updateScoresTensorOut, aUpdateScores, sizeof(*aUpdateScores) * cScores);
   return Error_None;
}

// we made this a global because if we had put this variable inside the BoosterCore object, then we would need to dereference that before 
// getting the count.  By making this global we can send a log message incase a bad BoosterCore object is sent into us
// we only decrease the count if the count is non-zero, so at worst if there is a race condition then we'll output this log message more 
// times than desired, but we can live with that
static int g_cLogSetTermUpdateExpandedParametersMessages = 10;

EBM_NATIVE_IMPORT_EXPORT_BODY ErrorEbmType EBM_NATIVE_CALLING_CONVENTION SetTermUpdateExpanded(
   BoosterHandle boosterHandle,
   IntEbmType indexFeatureGroup,
   double * updateScoresTensor
) {
   LOG_COUNTED_N(
      &g_cLogSetTermUpdateExpandedParametersMessages,
      TraceLevelInfo,
      TraceLevelVerbose,
      "SetTermUpdateExpanded: "
      "boosterHandle=%p, "
      "indexFeatureGroup=%" IntEbmTypePrintf ", "
      "updateScoresTensor=%p"
      ,
      static_cast<void *>(boosterHandle),
      indexFeatureGroup,
      static_cast<void *>(updateScoresTensor)
   );

   ErrorEbmType error;

   BoosterShell * const pBoosterShell = BoosterShell::GetBoosterShellFromHandle(boosterHandle);
   if(nullptr == pBoosterShell) {
      // already logged
      return Error_IllegalParamValue;
   }

   BoosterCore * const pBoosterCore = pBoosterShell->GetBoosterCore();
   EBM_ASSERT(nullptr != pBoosterCore);

   if(indexFeatureGroup < 0) {
      pBoosterShell->SetFeatureGroupIndex(BoosterShell::k_illegalFeatureGroupIndex);
      LOG_0(TraceLevelError, "ERROR SetTermUpdateExpanded indexFeatureGroup must be positive");
      return Error_IllegalParamValue;
   }
   if(IsConvertError<size_t>(indexFeatureGroup)) {
      pBoosterShell->SetFeatureGroupIndex(BoosterShell::k_illegalFeatureGroupIndex);
      // we wouldn't have allowed the creation of an feature set larger than size_t
      LOG_0(TraceLevelError, "ERROR SetTermUpdateExpanded indexFeatureGroup is too high to index");
      return Error_IllegalParamValue;
   }
   const size_t iFeatureGroup = static_cast<size_t>(indexFeatureGroup);
   if(pBoosterCore->GetCountFeatureGroups() <= iFeatureGroup) {
      pBoosterShell->SetFeatureGroupIndex(BoosterShell::k_illegalFeatureGroupIndex);
      LOG_0(TraceLevelError, "ERROR SetTermUpdateExpanded indexFeatureGroup above the number of feature groups that we have");
      return Error_IllegalParamValue;
   }
   // pBoosterCore->GetFeatureGroups() can be null if 0 == pBoosterCore->m_cFeatureGroups, but we checked that condition above
   EBM_ASSERT(nullptr != pBoosterCore->GetFeatureGroups());

   if(ptrdiff_t { 0 } == pBoosterCore->GetRuntimeLearningTypeOrCountTargetClasses() || ptrdiff_t { 1 } == pBoosterCore->GetRuntimeLearningTypeOrCountTargetClasses()) {
      pBoosterShell->SetFeatureGroupIndex(iFeatureGroup);
      return Error_None;
   }

   const FeatureGroup * const pFeatureGroup = pBoosterCore->GetFeatureGroups()[iFeatureGroup];
   error = pBoosterShell->GetAccumulatedModelUpdate()->Expand(pFeatureGroup);
   if(Error_None != error) {
      // already logged
      pBoosterShell->SetFeatureGroupIndex(BoosterShell::k_illegalFeatureGroupIndex);
      return error;
   }

   const size_t cDimensions = pFeatureGroup->GetCountDimensions();
   const size_t cVectorLength = GetVectorLength(pBoosterCore->GetRuntimeLearningTypeOrCountTargetClasses());
   size_t cScores = cVectorLength;
   if(0 != cDimensions) {
      const FeatureGroupEntry * pFeatureGroupEntry = pFeatureGroup->GetFeatureGroupEntries();
      const FeatureGroupEntry * const pFeatureGroupEntryEnd = &pFeatureGroupEntry[cDimensions];
      do {
         const size_t cBins = pFeatureGroupEntry->m_pFeature->GetCountBins();
         // we've allocated this memory, so it should be reachable, so these numbers should multiply
         EBM_ASSERT(!IsMultiplyError(cScores, cBins));
         cScores *= cBins;
         ++pFeatureGroupEntry;
      } while(pFeatureGroupEntryEnd != pFeatureGroupEntry);
   }
   FloatFast * const aUpdateScores = pBoosterShell->GetAccumulatedModelUpdate()->GetScoresPointer();
   EBM_ASSERT(!IsMultiplyError(sizeof(*aUpdateScores), cScores));
   EBM_ASSERT(!IsMultiplyError(sizeof(*updateScoresTensor), cScores));
   static_assert(sizeof(*updateScoresTensor) == sizeof(*aUpdateScores), "float mismatch");
   memcpy(aUpdateScores, updateScoresTensor, sizeof(*aUpdateScores) * cScores);

#ifdef ZERO_FIRST_MULTICLASS_LOGIT

   if(2 <= cVectorLength) {
      FloatFast * pUpdateScore = aUpdateScores;
      const FloatFast * const pExteriorEnd = pUpdateScore + cScores;
      do {
         FloatFast shiftScore = pUpdateScore[0];
         const FloatFast * const pInteriorEnd = pUpdateScore + cVectorLength;
         do {
            *pUpdateScore -= shiftScore;
            ++pUpdateScore;
         } while(pInteriorEnd != pUpdateScore);
      } while(pExteriorEnd != pUpdateScore);
   }

#endif // ZERO_FIRST_MULTICLASS_LOGIT

   pBoosterShell->SetFeatureGroupIndex(iFeatureGroup);

   return Error_None;
}

} // DEFINED_ZONE_NAME
