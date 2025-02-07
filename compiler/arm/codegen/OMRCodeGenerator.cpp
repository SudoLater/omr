/*******************************************************************************
 * Copyright IBM Corp. and others 2000
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at http://eclipse.org/legal/epl-2.0
 * or the Apache License, Version 2.0 which accompanies this distribution
 * and is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following Secondary
 * Licenses when the conditions for such availability set forth in the
 * Eclipse Public License, v. 2.0 are satisfied: GNU General Public License,
 * version 2 with the GNU Classpath Exception [1] and GNU General Public
 * License, version 2 with the OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 *******************************************************************************/

#include <stdint.h>
#include "arm/codegen/ARMInstruction.hpp"
#ifdef J9_PROJECT_SPECIFC
#include "arm/codegen/ARMRecompilation.hpp"
#endif
#include "arm/codegen/ARMSystemLinkage.hpp"
#include "codegen/AheadOfTimeCompile.hpp"
#ifdef J9_PROJECT_SPECIFC
#include "codegen/ARMAOTRelocation.hpp"
#endif
#include "codegen/CodeGenerator.hpp"
#include "codegen/CodeGenerator_inlines.hpp"
#include "codegen/ConstantDataSnippet.hpp"
#include "codegen/GCStackMap.hpp"
#include "codegen/GenerateInstructions.hpp"
#include "codegen/Linkage.hpp"
#include "codegen/Linkage_inlines.hpp"
#include "codegen/Machine.hpp"
#include "codegen/RealRegister.hpp"
#include "codegen/Register.hpp"
#include "codegen/RegisterIterator.hpp"
#include "codegen/RegisterPair.hpp"
#include "codegen/TreeEvaluator.hpp"
#include "compile/SymbolReferenceTable.hpp"
#include "env/CompilerEnv.hpp"
#include "env/IO.hpp"
#include "env/jittypes.h"
#include "il/Node.hpp"
#include "il/Node_inlines.hpp"
#include "il/ParameterSymbol.hpp"
#include "il/TreeTop_inlines.hpp"

TR_Processor OMR::ARM::CodeGenerator::_processor=TR_NullProcessor;

static int32_t identifyFarConditionalBranches(int32_t estimate, TR::CodeGenerator *cg);

TR::Linkage *OMR::ARM::CodeGenerator::createLinkage(TR_LinkageConventions lc)
   {
   TR::Linkage *linkage;
   switch (lc)
      {
      case TR_Private:
      case TR_Helper:
      case TR_System:
         linkage = new (self()->trHeapMemory()) TR::ARMSystemLinkage(self());
         break;
      default :
         TR_ASSERT(0, "using system linkage for unrecognized convention %d\n", lc);
         linkage = new (self()->trHeapMemory()) TR::ARMSystemLinkage(self());
      }
   self()->setLinkage(lc, linkage);
   return linkage;
   }

OMR::ARM::CodeGenerator::CodeGenerator(TR::Compilation *comp)
   : OMR::CodeGenerator(comp),
     _frameRegister(NULL),
     _constantData(NULL),
     _outOfLineCodeSectionList(comp->trMemory()),
     _internalControlFlowNestingDepth(0),
     _internalControlFlowSafeNestingDepth(0)
   {
   }

void
OMR::ARM::CodeGenerator::initialize()
   {
   self()->OMR::CodeGenerator::initialize();

   TR::CodeGenerator *cg = self();
   TR::Compilation *comp = self()->comp();

   // Initialize Linkage for Code Generator
   cg->initializeLinkage();

   _unlatchedRegisterList =
      (TR::RealRegister**)cg->trMemory()->allocateHeapMemory(sizeof(TR::RealRegister*)*(TR::RealRegister::NumRegisters + 1));

   _unlatchedRegisterList[0] = 0; // mark that list is empty

   _linkageProperties = &cg->getLinkage()->getProperties();
   _linkageProperties->setEndianness(comp->target().cpu.isBigEndian());

   if (!comp->getOption(TR_FullSpeedDebug))
      cg->setSupportsDirectJNICalls();
   cg->setSupportsVirtualGuardNOPing();

   if(comp->target().isLinux())
      {
      // only hardhat linux-arm builds have the required gcc soft libraries
      // that allow the vm to be compiled with -msoft-float.
      // With -msoft-float any floating point return value is placed in gr0 for single precision and
      // gr0 and gr1 for double precision.  This is where the jit expects the result following a directToJNI call.
      // The floating point return value on non-hardhat linux-arm builds is through the coprocessor register f0.
      // Therefore the HasHardFloatReturn flag must be set to force generation of instructions to move the result
      // from f0 to gr0/gr1 following a directToJNI dispatch.

      #if !defined(HARDHAT) && !defined(__VFP_FP__) // gcc does not support VFP with -mfloat-abi=hard yet
      cg->setHasHardFloatReturn();
      #endif
      }

   if (!debug("hasFramePointer"))
      cg->setFrameRegister(cg->machine()->getRealRegister(_linkageProperties->getStackPointerRegister()));
   else
      cg->setFrameRegister(cg->machine()->getRealRegister(_linkageProperties->getFramePointerRegister()));

   cg->setMethodMetaDataRegister(cg->machine()->getRealRegister(_linkageProperties->getMethodMetaDataRegister()));

   // Tactical GRA settings
#if 1 // PPC enables below, but seem no longer used?
   cg->setGlobalGPRPartitionLimit(TR::Machine::getGlobalGPRPartitionLimit());
   cg->setGlobalFPRPartitionLimit(TR::Machine::getGlobalFPRPartitionLimit());
#endif
   cg->setGlobalRegisterTable(TR::Machine::getGlobalRegisterTable());
   _numGPR = _linkageProperties->getNumAllocatableIntegerRegisters();
   cg->setLastGlobalGPR(TR::Machine::getLastGlobalGPRRegisterNumber());
   cg->setLast8BitGlobalGPR(TR::Machine::getLast8BitGlobalGPRRegisterNumber());
   cg->setLastGlobalFPR(TR::Machine::getLastGlobalFPRRegisterNumber());
   _numFPR = _linkageProperties->getNumAllocatableFloatRegisters();

   // TODO: Disable FP-GRA since current GRA does not work well with ARM linkage (where Float register usage is limited).
   cg->setDisableFloatingPointGRA();

   cg->setSupportsRecompilation();

   cg->setSupportsGlRegDeps();
   cg->setSupportsGlRegDepOnFirstBlock();
   cg->setPerformsChecksExplicitly();
   cg->setConsiderAllAutosAsTacticalGlobalRegisterCandidates();
   cg->setSupportsScaledIndexAddressing();
   cg->setSupportsAlignedAccessOnly();
   cg->setSupportsPrimitiveArrayCopy();
   cg->setSupportsReferenceArrayCopy();
   cg->setSupportsLoweringConstIDiv();
   cg->setSupportsNewInstanceImplOpt();

#ifdef J9_PROJECT_SPECIFIC
   cg->setAheadOfTimeCompile(new (cg->trHeapMemory()) TR::AheadOfTimeCompile(cg));
#endif
   cg->getLinkage()->initARMRealRegisterLinkage();
   //To enable this, we must change OMR::ARM::Linkage::saveArguments to support GRA registers
   //cg->getLinkage()->setParameterLinkageRegisterIndex(comp->getJittedMethodSymbol());

   _numberBytesReadInaccessible = 0;
   _numberBytesWriteInaccessible = 0;

   cg->setSupportsJavaFloatSemantics();
   cg->setSupportsInliningOfTypeCoersionMethods();

   if (comp->target().isLinux())
      {
      // On AIX and Linux, we are very far away from address
      // wrapping-around.
      _maxObjectSizeGuaranteedNotToOverflow = 0x10000000;
      cg->setSupportsDivCheck();
      if (!comp->getOption(TR_DisableTraps))
         cg->setHasResumableTrapHandler();
      }
   else
      {
      TR_ASSERT(0, "unknown target");
      }

   // Tactical GRA
   if (!comp->getOption(TR_DisableRegisterPressureSimulation))
      {
      for (int32_t i = 0; i < TR_numSpillKinds; i++)
         _globalRegisterBitVectors[i].init(cg->getNumberOfGlobalRegisters(), cg->trMemory());

      for (TR_GlobalRegisterNumber grn=0; grn < cg->getNumberOfGlobalRegisters(); grn++)
         {
         TR::RealRegister::RegNum reg = (TR::RealRegister::RegNum)cg->getGlobalRegister(grn);
         if (cg->getFirstGlobalGPR() <= grn && grn <= cg->getLastGlobalGPR())
            _globalRegisterBitVectors[ TR_gprSpill ].set(grn);
         else if (cg->getFirstGlobalFPR() <= grn && grn <= cg->getLastGlobalFPR())
            _globalRegisterBitVectors[ TR_fprSpill ].set(grn);

         if (!cg->getProperties().getPreserved(reg))
            _globalRegisterBitVectors[ TR_volatileSpill ].set(grn);
         if (cg->getProperties().getIntegerArgument(reg) || cg->getProperties().getFloatArgument(reg))
            _globalRegisterBitVectors[ TR_linkageSpill  ].set(grn);
         }

      }

   // Calculate inverse of getGlobalRegister function
   //
   TR_GlobalRegisterNumber grn;
   int i;

   TR_GlobalRegisterNumber globalRegNumbers[TR::RealRegister::NumRegisters];
   for (i=0; i < cg->getNumberOfGlobalGPRs(); i++)
     {
     grn = cg->getFirstGlobalGPR() + i;
     globalRegNumbers[cg->getGlobalRegister(grn)] = grn;
     }
   for (i=0; i < cg->getNumberOfGlobalFPRs(); i++)
     {
     grn = cg->getFirstGlobalFPR() + i;
     globalRegNumbers[cg->getGlobalRegister(grn)] = grn;
     }

   // Initialize linkage reg arrays
   TR::ARMLinkageProperties linkageProperties = cg->getProperties();
   for (i=0; i < linkageProperties.getNumIntArgRegs(); i++)
     _gprLinkageGlobalRegisterNumbers[i] = globalRegNumbers[linkageProperties.getIntegerArgumentRegister(i)];
   for (i=0; i < linkageProperties.getNumFloatArgRegs(); i++)
     _fprLinkageGlobalRegisterNumbers[i] = globalRegNumbers[linkageProperties.getFloatArgumentRegister(i)];

   if (comp->getOption(TR_TraceRA))
      {
      cg->setGPRegisterIterator(new (cg->trHeapMemory()) TR::RegisterIterator(cg->machine(), TR::RealRegister::FirstGPR, TR::RealRegister::LastGPR));
      cg->setFPRegisterIterator(new (cg->trHeapMemory()) TR::RegisterIterator(cg->machine(), TR::RealRegister::FirstFPR, TR::RealRegister::LastFPR));
      }

   if (!comp->compileRelocatableCode())
      {
      cg->setSupportsProfiledInlining();
      }

   // Disable optimizations for max min by default on arm
   comp->setOption(TR_DisableMaxMinOptimization);
   }

#if 0 // to be enabled
void
OMR::ARM::CodeGenerator::armCGOnClassUnloading(void *loaderPtr)
   {
   TR_ARMTableOfConstants::onClassUnloading(loaderPtr);
   }
#endif

TR_RuntimeHelper
directToInterpreterHelper(TR::ResolvedMethodSymbol *methodSymbol, TR::CodeGenerator *cg)
   {
   // Virtual/Interface methods need to be dispatched to static helper
   // since we don't know who the caller may be.
   bool   sync = methodSymbol->isSynchronised();

   if (methodSymbol->isVMInternalNative() || methodSymbol->isJITInternalNative())
      return TR_ARMicallVMprJavaSendNativeStatic;

   switch (methodSymbol->getMethod()->returnType())
      {
      case TR::NoType:
         return sync?TR_ARMicallVMprJavaSendStaticSync0:TR_ARMicallVMprJavaSendStatic0;
      case TR::Int8:
      case TR::Int16:
      case TR::Int32:
         return sync?TR_ARMicallVMprJavaSendStaticSync1:TR_ARMicallVMprJavaSendStatic1;
      case TR::Address:
         if (cg->comp()->target().is64Bit())
            return sync?TR_ARMicallVMprJavaSendStaticSyncJ:TR_ARMicallVMprJavaSendStaticJ;
         else
            return sync?TR_ARMicallVMprJavaSendStaticSync1:TR_ARMicallVMprJavaSendStatic1;
      case TR::Int64:
         return sync?TR_ARMicallVMprJavaSendStaticSyncJ:TR_ARMicallVMprJavaSendStaticJ;
      case TR::Float:
         return sync?TR_ARMicallVMprJavaSendStaticSyncF:TR_ARMicallVMprJavaSendStaticF;
      case TR::Double:
         return sync?TR_ARMicallVMprJavaSendStaticSyncD:TR_ARMicallVMprJavaSendStaticD;
      default:
         TR_ASSERT(0, "Unknown return type of a method.\n");
         return (TR_RuntimeHelper)0;
      }
   }

TR::Instruction *OMR::ARM::CodeGenerator::generateSwitchToInterpreterPrePrologue(TR::Instruction *cursor, TR::Node *node)
   {
   TR::Compilation *comp = self()->comp();
   TR::Register   *gr4 = self()->machine()->getRealRegister(TR::RealRegister::gr4);
   TR::Register   *lr = self()->machine()->getRealRegister(TR::RealRegister::gr14); // link register
   TR::ResolvedMethodSymbol *methodSymbol = comp->getJittedMethodSymbol();
   TR::SymbolReference    *revertToInterpreterSymRef = self()->symRefTab()->findOrCreateRuntimeHelper(TR_ARMrevertToInterpreterGlue);
   uintptr_t             ramMethod = (uintptr_t)methodSymbol->getResolvedMethod()->resolvedMethodAddress();
   TR::SymbolReference    *helperSymRef = self()->symRefTab()->findOrCreateRuntimeHelper(directToInterpreterHelper(methodSymbol, self()));
   uintptr_t             helperAddr = (uintptr_t)helperSymRef->getMethodAddress();

   // gr4 must contain the saved LR; see Recompilation.s
   cursor = new (self()->trHeapMemory()) TR::ARMTrg1Src1Instruction(cursor, TR::InstOpCode::mov, node, gr4, lr, self());
   cursor = self()->getLinkage()->flushArguments(cursor);
   cursor = generateImmSymInstruction(self(), TR::InstOpCode::bl, node, (uintptr_t)revertToInterpreterSymRef->getMethodAddress(), new (self()->trHeapMemory()) TR::RegisterDependencyConditions((uint8_t)0,0, self()->trMemory()), revertToInterpreterSymRef, NULL, cursor);
   cursor = generateImmInstruction(self(), TR::InstOpCode::dd, node, (int32_t)ramMethod, TR_RamMethod, cursor);

   if (comp->getOption(TR_EnableHCR))
      comp->getStaticHCRPICSites()->push_front(cursor);

   cursor = generateImmInstruction(self(), TR::InstOpCode::dd, node, (int32_t)helperAddr, TR_AbsoluteHelperAddress, helperSymRef, cursor);
   // Used in FSD to store an  instruction
   cursor = generateImmInstruction(self(), TR::InstOpCode::dd, node, 0, cursor);

   return cursor;
   }

void OMR::ARM::CodeGenerator::beginInstructionSelection()
   {
   TR::Compilation *comp = self()->comp();
   TR::Node *startNode = comp->getStartTree()->getNode();
   if (comp->getMethodSymbol()->getLinkageConvention() == TR_Private)
      {
      TR::Instruction *cursor = NULL;
      /* save the original JNI native address if a JNI thunk is generated */
      TR::ResolvedMethodSymbol *methodSymbol  = comp->getMethodSymbol();
      if (methodSymbol->isJNI())
         {
         uintptr_t JNIMethodAddress = (uintptr_t) methodSymbol->getResolvedMethod()->startAddressForJNIMethod(comp);
         cursor = new (self()->trHeapMemory()) TR::ARMImmInstruction(cursor, TR::InstOpCode::dd, startNode, (int32_t)JNIMethodAddress, self());
         }

      _returnTypeInfoInstruction = new (self()->trHeapMemory()) TR::ARMImmInstruction(cursor, TR::InstOpCode::dd, startNode, 0, self());
      new (self()->trHeapMemory()) TR::ARMAdminInstruction(_returnTypeInfoInstruction, TR::InstOpCode::proc, startNode, NULL, self());

      }
   else
      {
      _returnTypeInfoInstruction = NULL;
      new (self()->trHeapMemory()) TR::ARMAdminInstruction((TR::Instruction *)NULL, TR::InstOpCode::proc, startNode, NULL, self());
      }
   }

void OMR::ARM::CodeGenerator::endInstructionSelection()
   {
   if (_returnTypeInfoInstruction != NULL)
      {
      _returnTypeInfoInstruction->setSourceImmediate(static_cast<uint32_t>(self()->comp()->getReturnInfo()));
      }
   }

void OMR::ARM::CodeGenerator::doBinaryEncoding()
   {
   TR::Compilation *comp = self()->comp();
   int32_t estimate = 0;
   TR::Instruction *cursorInstruction = self()->getFirstInstruction();

   self()->getLinkage()->createPrologue(cursorInstruction);

   bool skipOneReturn = false;
   while (cursorInstruction)
      {
      if (cursorInstruction->getOpCodeValue() == TR::InstOpCode::retn)
         {
         if (skipOneReturn == false)
            {
            TR::Instruction *temp = cursorInstruction->getPrev();
            self()->getLinkage()->createEpilogue(temp);
            cursorInstruction = temp->getNext();
            skipOneReturn     = true;
            }
         else
            {
            skipOneReturn = false;
            }
         }
      estimate          = cursorInstruction->estimateBinaryLength(estimate);
      cursorInstruction = cursorInstruction->getNext();
      }

   estimate = self()->setEstimatedLocationsForSnippetLabels(estimate);

   if (estimate > 32768)
      {
      estimate = identifyFarConditionalBranches(estimate, self());
      }

   self()->setEstimatedCodeLength(estimate);

   cursorInstruction = self()->getFirstInstruction();
   uint8_t *coldCode = NULL;
   uint8_t *temp = self()->allocateCodeMemory(self()->getEstimatedCodeLength(), 0, &coldCode);

   self()->setBinaryBufferStart(temp);
   self()->setBinaryBufferCursor(temp);
   self()->alignBinaryBufferCursor();

   while (cursorInstruction)
      {
      self()->setBinaryBufferCursor(cursorInstruction->generateBinaryEncoding());
      cursorInstruction = cursorInstruction->getNext();
      }

   self()->getLinkage()->performPostBinaryEncoding();

   }

bool OMR::ARM::CodeGenerator::hasDataSnippets()
   {
   return (_constantData==NULL) ? false : true;
   }

void OMR::ARM::CodeGenerator::emitDataSnippets()
   {
   self()->setBinaryBufferCursor(_constantData->emitSnippetBody());
   }

int32_t OMR::ARM::CodeGenerator::setEstimatedLocationsForDataSnippetLabels(int32_t estimatedSnippetStart)
   {
   return estimatedSnippetStart+_constantData->getLength();
   }

#ifdef DEBUG
void OMR::ARM::CodeGenerator::dumpDataSnippets(TR::FILE *outFile)
   {
   if (outFile == NULL)
      return;
   _constantData->print(outFile);
   }
#endif

int32_t OMR::ARM::CodeGenerator::findOrCreateAddressConstant(void *v, TR::DataType t,
                             TR::Instruction *n0, TR::Instruction *n1,
                             TR::Instruction *n2, TR::Instruction *n3,
                             TR::Instruction *n4,
                             TR::Node *node, bool isUnloadablePicSite)
   {
   if (_constantData == NULL)
      _constantData = new (self()->trHeapMemory()) TR::ARMConstantDataSnippet(self());
   return(_constantData->addConstantRequest(v, t, n0, n1, n2, n3, n4, node, isUnloadablePicSite));
   }

// different from evaluate in that it returns a clobberable register
TR::Register *OMR::ARM::CodeGenerator::gprClobberEvaluate(TR::Node *node)
   {
   if (node->getReferenceCount() > 1)
      {
      if (node->getOpCode().isLong())
         {
         TR::Register     *lowReg  = self()->allocateRegister();
         TR::Register     *highReg = self()->allocateRegister();
         TR::RegisterPair *longReg = self()->allocateRegisterPair(lowReg, highReg);
         TR::Register     *temp    = self()->evaluate(node);

         generateTrg1Src1Instruction(self(), TR::InstOpCode::mov, node, lowReg, temp->getLowOrder());
         generateTrg1Src1Instruction(self(), TR::InstOpCode::mov, node, highReg, temp->getHighOrder());

         return longReg;
         }
      else
         {
         TR::Register *targetRegister = self()->allocateRegister();
         generateTrg1Src1Instruction(self(), TR::InstOpCode::mov, node, targetRegister, self()->evaluate(node));
         return targetRegister;
         }
      }
   else
      {
      return self()->evaluate(node);
      }
   }


static int32_t identifyFarConditionalBranches(int32_t estimate, TR::CodeGenerator *cg)
   {
   TR_Array<TR::ARMConditionalBranchInstruction *> candidateBranches(cg->trMemory(), 256);
   TR::Instruction *cursorInstruction = cg->getFirstInstruction();

   while (cursorInstruction)
      {
      TR::ARMConditionalBranchInstruction *branch = cursorInstruction->getARMConditionalBranchInstruction();
      if (branch != NULL)
         {
         if (abs(branch->getEstimatedBinaryLocation() - branch->getLabelSymbol()->getEstimatedCodeLocation()) > 16384)
            {
            candidateBranches.add(branch);
            }
         }
      cursorInstruction = cursorInstruction->getNext();
      }

   // The following heuristic algorithm penalizes backward branches in
   // estimation, since it should be rare that a backward branch needs
   // a far relocation.

   for (int32_t i=candidateBranches.lastIndex(); i>=0; i--)
      {
      int32_t myLocation=candidateBranches[i]->getEstimatedBinaryLocation();
      int32_t targetLocation=candidateBranches[i]->getLabelSymbol()->getEstimatedCodeLocation();
      int32_t  j;

      if (targetLocation >= myLocation)
         {
         for (j=i+1; j<candidateBranches.size() &&
                 targetLocation>
                 candidateBranches[j]->getEstimatedBinaryLocation();
              j++)
            ;
         if ((targetLocation-myLocation + (j-i-1)*4) >= 32768)
            {
            candidateBranches[i]->setFarRelocation(true);
            }
         else
            {
            candidateBranches.remove(i);
            }
         }
      else    // backward branch
         {
         for (j=i-1; j>=0 && targetLocation<=
                 candidateBranches[j]->getEstimatedBinaryLocation();
              j--)
            ;
         if ((myLocation-targetLocation + (i-j-1)*4) > 32768)
            {
            candidateBranches[i]->setFarRelocation(true);
            }
         else
            {
            candidateBranches.remove(i);
            }
         }
      }
      return(estimate+4*candidateBranches.size());
   }

void OMR::ARM::CodeGenerator::buildRegisterMapForInstruction(TR_GCStackMap *map)
   {
   // Build the register map
   //
   for (int i=TR::RealRegister::FirstGPR;
            i<=TR::RealRegister::LastGPR; i++)
      {
      TR::RealRegister *realReg = self()->machine()->getRealRegister(
                             (TR::RealRegister::RegNum)i);
      if (realReg->getHasBeenAssignedInMethod())
         {
         TR::Register *virtReg = realReg->getAssignedRegister();
         if (virtReg!=NULL && virtReg->containsCollectedReference())
            {
            map->setRegisterBits(TR::CodeGenerator::registerBitMask(i));
            }
         }
      }
   }

/* @@
bool OMR::ARM::CodeGenerator::canNullChkBeImplicit(TR::Node *node)
   {
   return self()->canNullChkBeImplicit(node, true);
   }
*/

void OMR::ARM::CodeGenerator::apply24BitLabelRelativeRelocation(int32_t * cursor, TR::LabelSymbol * label)
   {
   int32_t delta = (intptr_t)label->getCodeLocation() - ((intptr_t)cursor + 8);
   *cursor = *cursor | ((delta >> 2) & 0x00FFFFFF);
   }

void OMR::ARM::CodeGenerator::apply8BitLabelRelativeRelocation(int32_t * cursor, TR::LabelSymbol * label)
   {
   int32_t delta = (intptr_t)label->getCodeLocation() - ((intptr_t)cursor + 8);
   TR_ASSERT((delta & ~0x0ff) == 0, "assertion failure");
   *cursor |= delta;
   }

TR_ARMOutOfLineCodeSection *OMR::ARM::CodeGenerator::findOutLinedInstructionsFromLabel(TR::LabelSymbol *label)
   {
   ListIterator<TR_ARMOutOfLineCodeSection> oiIterator(&self()->getARMOutOfLineCodeSectionList());
   TR_ARMOutOfLineCodeSection *oiCursor = oiIterator.getFirst();

   while (oiCursor)
      {
      if (oiCursor->getEntryLabel() == label)
         return oiCursor;
      oiCursor = oiIterator.getNext();
      }

   return NULL;
   }

TR_GlobalRegisterNumber OMR::ARM::CodeGenerator::pickRegister(TR::RegisterCandidate            *regCan,
                                                          TR::Block                          ** barr,
                                                          TR_BitVector                      &  availRegs,
                                                          TR_GlobalRegisterNumber           &  highRegisterNumber,
                                                          TR_LinkHead<TR::RegisterCandidate> *  candidates)
   {
   // Tactical GRA
   // We delegate the decision to use register pressure simulation to common code.
   // if (!comp()->getOption(TR_DisableRegisterPressureSimulation))
   return OMR::CodeGenerator::pickRegister(regCan, barr, availRegs, highRegisterNumber, candidates);
   // }
   }

bool OMR::ARM::CodeGenerator::allowGlobalRegisterAcrossBranch(TR::RegisterCandidate *rc, TR::Node * branchNode)
   {
   // If return false, processLiveOnEntryBlocks has to dis-qualify any candidates which are referenced
   // within any CASE of a SWITCH statement.
   return true;
   }

int32_t OMR::ARM::CodeGenerator::getMaximumNumberOfGPRsAllowedAcrossEdge(TR::Node * node)
   {
   if (node->getOpCode().isSwitch())
      {
      // This code should be separated out in a function in TreeEvaluatore.cpp
      // for better coordination with the evaluator code.
      if (node->getOpCodeValue() == TR::table)
         {
          // The constant below comes from ControlFlowEvaluator.cpp#tableEvaluator(),
    	  // number of dependencies required.
         return _numGPR - 2;
         }
      else
         {
         int32_t total = node->getCaseIndexUpperBound();
         int i;
         // The constants below (total and number of registers to leave free) comes
         // from ControlFlowEvaluator.cpp#switchDispatch() and lookupScheme implementations.
         if (total <= 15)
            {
            uint32_t base, rotate;
            for (i = 2; i < total && constantIsImmed8r(node->getChild(i)->getCaseConstant(), &base, &rotate); i++)
               ;
            if (i == total)
               return _numGPR - 1;
            }

         if (total <= 9)
            {
            int32_t preInt = node->getChild(2)->getCaseConstant();
            for (i = 3; i < total; i++)
               {
               int32_t diff = node->getChild(i)->getCaseConstant() - preInt;
               uint32_t base, rotate;
               preInt += diff;
               if (diff < 0 || !constantIsImmed8r(diff, &base, &rotate))
                  break;
               }
            if (i >= total)
               return _numGPR - 2;
            }
#if 0 // As noted above, these checks are synched with ControlFlowEvaluator.cpp#switchDispatch(), and this section is unused as of now.
         if (total <= 8)
            return _numGPR - 3;
#endif
         return _numGPR - 6;  // Be conservative when we don't know whether it is balanced.
         }
      }
   else if (node->getOpCode().isIf() && node->getFirstChild()->getType().isInt64())
      {
      return ( _numGPR - 4 ); // 4 GPRs are needed for long-compare arguments themselves.
      }

   return _numGPR; // pickRegister will ensure that the number of free registers isn't exceeded
   }

int32_t OMR::ARM::CodeGenerator::getMaximumNumberOfFPRsAllowedAcrossEdge(TR::Node * node)
   {
   return _numFPR; // pickRegister will ensure that the number of free registers isn't exceeded
   }

#if 1
TR_GlobalRegisterNumber OMR::ARM::CodeGenerator::getLinkageGlobalRegisterNumber(int8_t linkageRegisterIndex, TR::DataType type)
   {
   TR_GlobalRegisterNumber result;
   if (type == TR::Float || type == TR::Double)
      {
      if (linkageRegisterIndex >= self()->getProperties()._numFloatArgumentRegisters)
         result = -1;
      else
         result = _fprLinkageGlobalRegisterNumbers[linkageRegisterIndex];
      }
   else
      {
      if (linkageRegisterIndex >= self()->getProperties()._numIntegerArgumentRegisters)
         result = -1;
      else
         result = _gprLinkageGlobalRegisterNumbers[linkageRegisterIndex];
      }
   diagnostic("getLinkageGlobalRegisterNumber(%d, %s) = %d\n", linkageRegisterIndex, self()->comp()->getDebug()->getName(type), result);
   return result;
   }
#endif
int32_t OMR::ARM::CodeGenerator::getMaximumNumbersOfAssignableGPRs()
   {
   return TR::RealRegister::LastGPR - TR::RealRegister::FirstGPR  + 1;
   }

int32_t OMR::ARM::CodeGenerator::getMaximumNumbersOfAssignableFPRs()
   {
   return TR::RealRegister::LastFPR - TR::RealRegister::FirstFPR  + 1;
   }


/**
 * \brief
 * Determine if value fits in the immediate field (if any) of instructions that machine provides.
 *
 * \details
 *
 * A node with a large constant can be materialized and left as commoned nodes.
 * Smaller constants can be uncommoned so that they re-materialize every time when needed as a call
 * parameter. This query is platform specific as constant loading can be expensive on some platforms
 * but cheap on others, depending on their magnitude.
 */
bool OMR::ARM::CodeGenerator::shouldValueBeInACommonedNode(int64_t value)
   {
   int64_t smallestPos = self()->getSmallestPosConstThatMustBeMaterialized();
   int64_t largestNeg = self()->getLargestNegConstThatMustBeMaterialized();

   return ((value >= smallestPos) || (value <= largestNeg));
   }

bool OMR::ARM::CodeGenerator::isGlobalRegisterAvailable(TR_GlobalRegisterNumber i, TR::DataType dt)
   {
   return self()->machine()->getRealRegister((TR::RealRegister::RegNum)self()->getGlobalRegister(i))->getState() == TR::RealRegister::Free;
   }

bool
OMR::ARM::CodeGenerator::directCallRequiresTrampoline(intptr_t targetAddress, intptr_t sourceAddress)
   {
   return
      !self()->comp()->target().cpu.isTargetWithinBranchImmediateRange(targetAddress, sourceAddress) ||
      self()->comp()->getOption(TR_StressTrampolines);
   }
