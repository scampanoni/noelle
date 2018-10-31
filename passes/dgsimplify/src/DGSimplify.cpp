#include "DGSimplify.hpp"

/*
 * Options of the dependence graph simplifier pass.
 */
static cl::opt<bool> ForceInlineToLoop("dgsimplify-inline-to-loop", cl::ZeroOrMore, cl::Hidden, cl::desc("Force inlining along the call graph from main to the loops being parallelized"));

DGSimplify::~DGSimplify () {
  for (auto orderedLoops : preOrderedLoops) {
    delete orderedLoops.second;
  }
  for (auto l : loopSummaries) {
    delete l;
  }
}

bool llvm::DGSimplify::doInitialization (Module &M) {
  errs() << "DGSimplify at \"doInitialization\"\n" ;

  return false;
}

bool llvm::DGSimplify::runOnModule (Module &M) {
  errs() << "DGSimplify at \"runOnModule\"\n";

  /*
   * Collect function and loop ordering to track inlining progress
   */
  auto main = M.getFunction("main");
  collectFnGraph(main);
  collectInDepthOrderFns(main);

  // OPTIMIZATION(angelo): Do this lazily, depending on what functions are considered in algorithms
  for (auto func : depthOrderedFns) {
    collectPreOrderedLoopsFor(func);
  }

  printFnCallGraph();
  printFnOrder();

  auto writeToContinueFile = []() -> void {
    ofstream continuefile("dgsimplify_continue.txt");
    continuefile << "1\n";
    continuefile.close();
  };

  /*
   * Inline calls within large SCCs of targeted loops
   */
  ifstream doCallInlineFile("dgsimplify_do_scc_call_inline.txt");
  bool doInline = doCallInlineFile.good();
  doCallInlineFile.close();
  if (doInline) {
    std::string filename = "dgsimplify_scc_call_inlining.txt";
    getLoopsToInline(filename);
    bool inlined = inlineCallsInMassiveSCCsOfLoops();
    bool remaining = registerRemainingLoops(filename);
    if (remaining) writeToContinueFile();
    return inlined;
  }

  /*
   * Inline functions containing targeted loops so the loop is in main
   */
  ifstream doHoistFile("dgsimplify_do_hoist.txt");
  bool doHoist = doHoistFile.good();
  doHoistFile.close();
  if (doHoist) {
    loopsToCheck.clear();
    std::string filename = "dgsimplify_loop_hoisting.txt";
    getLoopsToInline(filename);
    // bool inlined = inlineFnsOfLoopsToCGRoot();
    loopsToCheck.clear();
    bool remaining = registerRemainingLoops(filename);
    if (remaining) writeToContinueFile();
    // return inlined;
  }

  return false;
}

void llvm::DGSimplify::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<PDGAnalysis>();
  AU.addRequired<PostDominatorTreeWrapperPass>();
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.setPreservesAll();
  return ;
}

void llvm::DGSimplify::getLoopsToInline (std::string filename) {
  loopsToCheck.clear();
  ifstream infile(filename);
  if (infile.good()) {
    std::string line;
    std::string delimiter = ",";
    while(getline(infile, line)) {
      size_t i = line.find(delimiter);
      int fnInd = std::stoi(line.substr(0, i));
      int loopInd = std::stoi(line.substr(i + delimiter.length()));
      auto F = depthOrderedFns[fnInd];
      auto &loops = *preOrderedLoops[F];
      assert(loopInd < loops.size());
      loopsToCheck[F].insert(loops[loopInd]);
      errs() << "DGSimplify:   Checking: FN index: "
        << fnInd << " " << F->getName()
        << ", LOOP: " << loopInd << "\n";
      // loops[loopInd]->header->print(errs() << "Header:\n"); errs() << "\n";
    }
    return;
  }

  // NOTE(angelo): Default to selecting all loops in the program
  for (auto funcLoops : preOrderedLoops) {
    auto F = funcLoops.first;
    for (auto summary : *funcLoops.second) {
      loopsToCheck[F].insert(summary);
      errs() << "DGSimplify:   Checking: FN index: "
        << fnOrders[F] << " " << F->getName()
        << ", LOOP: " << summary->id << "\n";
      // summary->header->print(errs() << "Header:\n"); errs() << "\n";
    }
  }
}

bool llvm::DGSimplify::registerRemainingLoops (std::string filename) {
  remove(filename.c_str());
  if (loopsToCheck.size() == 0) return false;

  ofstream outfile(filename);
  for (auto funcLoops : loopsToCheck) {
    auto F = funcLoops.first;
    int fnInd = fnOrders[F];
    for (auto summary : funcLoops.second) {
      int loopInd = summary->id;
      errs() << "DGSimplify:   Remaining: FN index: "
        << fnInd << " " << F->getName()
        << ", LOOP index: " << loopInd << "\n";
      outfile << fnInd << "," << loopInd << "\n";
    }
  }
  outfile.close();
  return true;
}

bool llvm::DGSimplify::inlineCallsInMassiveSCCsOfLoops () {
  auto &PDGA = getAnalysis<PDGAnalysis>();
  bool anyInlined = false;

  // NOTE(angelo): Order these functions to prevent duplicating loops yet to be checked
  std::vector<Function *> fnsToCheck;
  for (auto fnLoops : loopsToCheck) fnsToCheck.push_back(fnLoops.first);
  std::sort(fnsToCheck.begin(), fnsToCheck.end(), [this](Function *a, Function *b) {
    // NOTE(angelo): Sort functions deepest first
    return fnOrders[a] > fnOrders[b];
  });

  std::set<Function *> fnsToAvoid;
  for (auto F : fnsToCheck) {
    if (fnsToAvoid.find(F) != fnsToAvoid.end()) continue;
    auto& PDT = getAnalysis<PostDominatorTreeWrapperPass>(*F).getPostDomTree();
    auto& LI = getAnalysis<LoopInfoWrapperPass>(*F).getLoopInfo();
    auto& SE = getAnalysis<ScalarEvolutionWrapperPass>(*F).getSE();
    auto fdg = PDGA.getFunctionPDG(*F);
    auto loopsPreorder = LI.getLoopsInPreorder();

    bool inlined = false;
    std::set<LoopSummary *> removeSummaries;
    for (auto summary : loopsToCheck[F]) {
      auto loop = loopsPreorder[summary->id];
      auto LDI = new LoopDependenceInfo(F, fdg, loop, LI, PDT);
      auto &attrs = LDI->sccdagAttrs;
      attrs.populate(LDI->loopSCCDAG, LDI->liSummary, SE);
      bool inlinedCall = inlineCallsInMassiveSCCs(F, LDI);
      if (!inlinedCall) removeSummaries.insert(summary);

      inlined |= inlinedCall;
      delete LDI;
      if (inlined) break;
    }

    // NOTE(angelo): Avoid affected function parents as we will revisit them next simplify pass
    if (inlined) {
      for (auto parentF : parentFns[F]) fnsToAvoid.insert(parentF);
    }
    for (auto summary : removeSummaries) loopsToCheck[F].erase(summary);
    anyInlined |= inlined;
    delete fdg;
  }

  return anyInlined;
}

/*
 * GOAL: Go through loops in function
 * If there is only one non-clonable/reducable SCC,
 * try inlining the function call in that SCC with the
 * most memory edges to other internal/external values
 */
bool llvm::DGSimplify::inlineCallsInMassiveSCCs (Function *F, LoopDependenceInfo *LDI) {
  std::set<SCC *> sccsToCheck;
  for (auto sccNode : LDI->loopSCCDAG->getNodes()) {
    auto scc = sccNode->getT();
    if (!LDI->sccdagAttrs.executesCommutatively(scc)
        && !LDI->sccdagAttrs.executesIndependently(scc)
        && !LDI->sccdagAttrs.canBeCloned(scc)) {
      sccsToCheck.insert(scc);
    }
  }

  /*
   * NOTE: if there are more than two non-trivial SCCs, then
   * there is less incentive to continue trying to inline.
   * Why 2? Because 2 is always a simple non-trivial number
   * to start a heuristic at.
   */
  if (sccsToCheck.size() > 2) return false;

  int64_t maxMemEdges = 0;
  CallInst *inlineCall = nullptr;
  for (auto scc : sccsToCheck) {
    for (auto valNode : scc->getNodes()) {
      auto val = valNode->getT();
      if (auto call = dyn_cast<CallInst>(val)) {
        auto callF = call->getCalledFunction();
        if (!callF || callF->empty()) continue;

        // NOTE(angelo): Do not consider inlining a recursive function call
        if (callF == F) continue;

        // NOTE(angelo): Do not consider inlining calls to functions of lower depth
        if (fnOrders[callF] < fnOrders[F]) continue;

        auto memEdgeCount = 0;
        for (auto edge : valNode->getAllConnectedEdges()) {
          if (edge->isMemoryDependence()) memEdgeCount++;
        }
        if (memEdgeCount > maxMemEdges) {
          maxMemEdges = memEdgeCount;
          inlineCall = call;
        }
      }
    }
  }

  return inlineCall && inlineFunctionCall(F, inlineCall);
}

bool llvm::DGSimplify::inlineFnsOfLoopsToCGRoot () {
  std::vector<Function *> fnsToCheck;
  for (auto fnLoops : loopsToCheck) fnsToCheck.push_back(fnLoops.first);
  std::sort(fnsToCheck.begin(), fnsToCheck.end(), [this](Function *a, Function *b) {
    // NOTE(angelo): Sort functions deepest first
    return fnOrders[a] > fnOrders[b];
  });

  int fnIndex = 0;
  std::set<Function *> fnsWillCheck(fnsToCheck.begin(), fnsToCheck.end());
  while (fnIndex < fnsToCheck.size()) {
    auto childF = fnsToCheck[fnIndex];
    bool inlinedFully = true;
    for (auto parentF : parentFns[childF]) {
      // NOTE(angelo): Do not inline from less deep to more deep (to avoid recursive chains)
      if (fnOrders[parentF] > fnOrders[childF]) continue;

      // NOTE(angelo): Since only one inline per function is permitted, this for loop
      //  isn't entirely necessary, but it better expresses conceptual intent
      for (auto call : childrenFns[parentF][childF]) {
        inlinedFully &= inlineFunctionCall(parentF, call);
      }

      // NOTE(angelo): Insert parent to inline up the CG if it isn't already inserted
      if (fnsWillCheck.find(parentF) != fnsWillCheck.end()) continue;
      int insertIndex = -1;
      while (fnOrders[fnsToCheck[++insertIndex]] > fnOrders[parentF]);
      fnsToCheck.insert(fnsToCheck.begin() + insertIndex, parentF);
      fnsWillCheck.insert(parentF);
    }
    if (!inlinedFully) break;
    loopsToCheck.erase(childF);
    fnsWillCheck.erase(childF);
    ++fnIndex;
  }

  // TODO(angelo): Handle case where we list functions to check without loops
  // TODO(angelo): Add fnsWillCheck to loopsToCheck, and remaining fnsToCheck
}

bool llvm::DGSimplify::inlineFunctionCall (Function *F, CallInst *call) {
  // NOTE(angelo): Prevent inlining a call within a function already altered by inlining
  if (fnsAffected.find(F) != fnsAffected.end()) return false ;

  // NOTE(angelo): Prevent inlining a call to the entry of a recursive chain of functions
  Function *callF = call->getCalledFunction();
  if (recursiveChainEntranceFns.find(callF) != recursiveChainEntranceFns.end()) return false ;

  InlineFunctionInfo IFI;
  call->print(errs() << "DGSimplify:   Inlining in: " << F->getName() << ", "); errs() << "\n";
  if (InlineFunction(call, IFI)) {
    fnsAffected.insert(F); 
    adjustOrdersAfterInline(F, call);
    return true;
  }
  return false;
}


void llvm::DGSimplify::adjustOrdersAfterInline (Function *parentF, CallInst *call) {
  auto childF = call->getCalledFunction();
  removeFnPairInstance(parentF, childF, call);
  for (auto newChild : childrenFns[childF]) {
    for (auto call : newChild.second) {
      addFnPairInstance(parentF, newChild.first, call);
    }
  }

  bool parentHasLoops = preOrderedLoops.find(parentF) != preOrderedLoops.end();
  bool childHasLoops = preOrderedLoops.find(childF) != preOrderedLoops.end();
  if (!childHasLoops) return ;
  if (!parentHasLoops) preOrderedLoops[parentF] = new std::vector<LoopSummary *>();

  /*
   * NOTE(angelo): Starting after the loop in the parent function, index all loops in the
   * child function as being now in the parent function and adjust the indices of loops
   * after the call site by the number of loops inserted
   */
  auto &parentLoops = *preOrderedLoops[parentF];
  auto &childLoops = *preOrderedLoops[childF];
  auto nextLoopInParent = getNextPreorderLoopAfter(parentF, call);
  auto startInd = nextLoopInParent ? nextLoopInParent->id : parentLoops.size();
  auto childLoopCount = childLoops.size();
  auto endInd = startInd + childLoopCount;

  // NOTE(angelo): Adjust parent loops after the call site
  parentLoops.resize(parentLoops.size() + childLoopCount);
  for (auto shiftIndex = parentLoops.size() - 1; shiftIndex >= endInd; --shiftIndex) {
    parentLoops[shiftIndex] = parentLoops[shiftIndex - childLoopCount];
  }

  // NOTE(angelo): Insert inlined loops from child function
  for (auto childIndex = startInd; childIndex < endInd; ++childIndex) {
    parentLoops[childIndex] = childLoops[childIndex - startInd];
  }

  // DEBUG(angelo): loop order after inlining
  printFnLoopOrder(parentF);
}

LoopSummary *llvm::DGSimplify::getNextPreorderLoopAfter (Function *F, CallInst *call) {
  // NOTE(angelo): Mimic getLoopFor, getLoopDepth, and isLoopHeader of llvm LoopInfo API
  auto getSummaryFor = [&](BasicBlock *BB) -> LoopSummary * {
    LoopSummary *deepestSummary = nullptr;
    for (auto summary : *preOrderedLoops[F]) {
      if (summary->bbs.find(BB) == summary->bbs.end()) continue;
      if (summary->depth < deepestSummary->depth) continue;
      assert(summary->depth != deepestSummary->depth);
      deepestSummary = summary;
    }
    return deepestSummary;
  };
  auto getSummaryIfHeader = [&](BasicBlock *BB) -> LoopSummary * {
    for (auto summary : *preOrderedLoops[F]) {
      if (summary->header == BB) return summary;
    }
    return nullptr;
  };

  auto callBB = call->getParent();
  auto callLoop = getSummaryFor(callBB);
  bool startSearch = false;
  LoopSummary *prev = nullptr;
  LoopSummary *next = nullptr;
  // NOTE(angelo): Search in forward program order for next loop header
  for (auto &B : *F) {
    if (!startSearch) {
      auto l = getSummaryIfHeader(&B);
      if (l) prev = l;
    }
    if (callBB == &B) {
      startSearch = true;
      continue;
    }
    if (startSearch) {
      auto l = getSummaryIfHeader(&B);
      if (!l) continue;

      /*
       * NOTE(angelo): Next loop header must either be:
       * 1) a direct child of the inner-most loop the call resides in
       * 2) a loop with a smaller depth than the call's inner-most loop
       */
      if (l->depth > callLoop->depth + 1) continue;
      next = l;
      break;
    }
  }

  assert(prev != nullptr);
  if (!next) return nullptr;
  errs() << "DGSimplify:   Previous summary id: " << prev->id << " Next << " << next->id << "\n";
  assert(prev->id + 1 == next->id);
  return next;
}

void llvm::DGSimplify::collectFnGraph (Function *main) {
  auto &callGraph = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  std::queue<Function *> funcToTraverse;
  std::set<Function *> reached;

  /*
   * NOTE(angelo): Traverse call graph, collecting function "parents":
   *  Parent functions are those encountered before their children in a
   *  breadth-first traversal of the call graph
   */
  funcToTraverse.push(main);
  reached.insert(main);
  while (!funcToTraverse.empty()) {
    auto func = funcToTraverse.front();
    funcToTraverse.pop();

    auto funcCGNode = callGraph[func];
    for (auto &callRecord : make_range(funcCGNode->begin(), funcCGNode->end())) {
      auto weakVH = callRecord.first;
      if (!weakVH.pointsToAliveValue() || !isa<CallInst>(*&weakVH)) continue;
      auto F = callRecord.second->getFunction();
      if (!F || F->empty()) continue;

      addFnPairInstance(func, F, (CallInst *)(&*weakVH));
      if (reached.find(F) != reached.end()) continue;
      reached.insert(F);
      funcToTraverse.push(F);
    }
  }
}

/*
 * NOTE(angelo): Determine the depth of functions in the call graph:
 *  next-depth functions are those where every parent function
 *  has already been assigned a previous depth
 * Obviously, recursive loops by this definition have undefined depth.
 *  These groups, each with a chain of recursive functions, are ordered
 *  by their entry points' relative depths. They are assigned depths
 *  after all other directed acyclic portions of the call graph (starting
 *  from their common ancestor) is traversed.
 */
void llvm::DGSimplify::collectInDepthOrderFns (Function *main) {
  auto &callGraph = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  std::queue<Function *> funcToTraverse;
  std::set<Function *> reached;
  std::vector<Function *> *deferred = new std::vector<Function *>();

  funcToTraverse.push(main);
  fnOrders[main] = 0;
  depthOrderedFns.push_back(main);
  reached.insert(main);
  // NOTE(angelo): Check to see whether any functions remain to be traversed
  while (!funcToTraverse.empty()) {
    // NOTE(angelo): Check to see whether any order-able functions remain
    while (!funcToTraverse.empty()) {
      auto func = funcToTraverse.front();
      funcToTraverse.pop();

      auto funcCGNode = callGraph[func];
      for (auto &callRecord : make_range(funcCGNode->begin(), funcCGNode->end())) {
        auto F = callRecord.second->getFunction();
        if (!F || F->empty()) continue;
        if (reached.find(F) != reached.end()) continue;
        
        bool allParentsOrdered = true;
        for (auto parent : parentFns[F]) {
          if (reached.find(parent) == reached.end()) {
            allParentsOrdered = false;
            break;
          }
        }
        if (allParentsOrdered) {
          funcToTraverse.push(F);
          fnOrders[F] = depthOrderedFns.size();
          depthOrderedFns.push_back(F);
          reached.insert(F);
        } else {
          deferred->push_back(F);
        }
      }
    }

    /*
     * NOTE(angelo): Collect all deferred functions that never got ordered.
     * By definition of the ordering, they must all be parts of recursive chains.
     * Order their entry points, add them to the queue to traverse.
     */
    auto remaining = new std::vector<Function *>();
    for (auto left : *deferred) {
      if (fnOrders.find(left) == fnOrders.end()) {
        recursiveChainEntranceFns.insert(left);
        remaining->push_back(left);
        funcToTraverse.push(left);
        fnOrders[left] = depthOrderedFns.size();
        depthOrderedFns.push_back(left);
        reached.insert(left);
      }
    }
    delete deferred;
    deferred = remaining;
  }

  delete deferred;
}

void llvm::DGSimplify::collectPreOrderedLoopsFor (Function *F) {
  // NOTE(angelo): Enforce managing order instead of recalculating it entirely
  if (preOrderedLoops.find(F) != preOrderedLoops.end()) {
    errs() << "DGSimplify:   Misuse! Do not collect ordered loops more than once. Manage current ordering.\n";
  }
  auto& LI = getAnalysis<LoopInfoWrapperPass>(*F).getLoopInfo();
  if (LI.empty()) return;

  int count = 0;
  auto *orderedLoops = new std::vector<LoopSummary *>();
  auto liOrdered = LI.getLoopsInPreorder();
  std::unordered_map<Loop *, LoopSummary *> summaryMap;
  for (auto loop : liOrdered) {
    auto summary = new LoopSummary(count++, loop);
    loopSummaries.insert(summary);
    orderedLoops->push_back(summary);
    summaryMap[loop] = summary;
  }
  for (auto i = 0; i < count; ++i) {
    auto parent = liOrdered[i]->getParentLoop();
    (*orderedLoops)[i]->parent = parent ? summaryMap[parent] : nullptr;
    for (auto childLoop : liOrdered[i]->getSubLoops()) {
      (*orderedLoops)[i]->children.insert(summaryMap[childLoop]);
    }
  }
  preOrderedLoops[F] = orderedLoops;
}

void llvm::DGSimplify::addFnPairInstance (Function *parentF, Function *childF, CallInst *call) {
  auto &children = childrenFns[parentF];
  parentFns[childF].insert(parentF);
  children[childF].insert(call);
}

void llvm::DGSimplify::removeFnPairInstance (Function *parentF, Function *childF, CallInst *call) {
  auto &children = childrenFns[parentF];
  children[childF].erase(call);
  if (children[childF].size() == 0) {
    children.erase(childF);
  }
}

void llvm::DGSimplify::printFnCallGraph () {
  for (auto fns : parentFns) {
    errs() << "DGSimplify:   Child function: " << fns.first->getName() << "\n";
    for (auto f : fns.second) {
      errs() << "DGSimplify:   \tParent: " << f->getName() << "\n";
    }
  }
}

void llvm::DGSimplify::printFnOrder () {
  int count = 0;
  for (auto fn : depthOrderedFns) {
    errs() << "DGSimplify:   Function: " << count++ << " " << fn->getName() << "\n";
  }
}

void llvm::DGSimplify::printFnLoopOrder (Function *F) {
  for (auto summary : *preOrderedLoops[F]) {
    auto headerBB = summary->header;
    errs() << "DGSimplify:   Loop " << summary->id << "\n";
    headerBB->print(errs()); errs() << "\n";
  }
}
