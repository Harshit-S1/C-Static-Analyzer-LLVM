#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Tooling/CommonOptionsParser.h" 
#include "clang/Analysis/CFG.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"          
#include <map>
#include <set>
#include <queue>
#include <algorithm> 
#include <iterator>
#include <system_error>
#include <string>

using namespace clang;
using namespace clang::tooling;
using namespace std;

// a command line option category is setup for our tool
static llvm::cl::OptionCategory MyToolCategory("Static Analyzer Options");

// This is the main worker that walks through the C code's AST (Abstract Syntax Tree)
class MyASTVisitor : public RecursiveASTVisitor<MyASTVisitor> {
private:
    ASTContext *Context;
    Rewriter &TheRewriter;
    set<const FunctionDecl*> allFunctions;
    set<string> calledFunctions;

    //    To Stop Dataflow Pollution from Pointers/Arrays
    // Tries to safely pull the variable name out of an expression
    string getVarName(Expr *E) {
        if (!E) return "";
        if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts())) {
            return DRE->getDecl()->getNameAsString();
        }
        return ""; // returns empty string instead of "unknown" to prevent alias pollution
    }

    //    Protecting I/O, System Calls, and Increments
    // Checks if a statement actually does something we can't safely delete
    bool hasSideEffects(const Stmt *S) {
        if (!S) return false;
        if (isa<CallExpr>(S)) return true;
        
        // catches state-mutating unary operators (x++, --y)
        if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(S)) {
            if (UO->isIncrementDecrementOp()) return true;
        }
        
        for (const Stmt *child : S->children()) {
            if (hasSideEffects(child)) return true;
        }
        return false;
    }

    // Helper to Evaluate arithmetic utilizing our dynamically tracked constants
    // basically a mini-calculator to crunch the numbers if we know what the variables hold
    long long evaluateExpr(const Expr *E, map<string, long long> &vals, bool &isConst) {
        if (!E) { isConst = false; return 0; }
        E = E->IgnoreParenImpCasts();
        
        if (const IntegerLiteral *IL = dyn_cast<IntegerLiteral>(E)) {
            isConst = true; return IL->getValue().getSExtValue();
        }
        if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
            string name = DRE->getDecl()->getNameAsString();
            if (vals.count(name)) { isConst = true; return vals[name]; }
        }
        if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(E)) {
            bool lConst = false, rConst = false;
            long long lVal = evaluateExpr(BO->getLHS(), vals, lConst);
            long long rVal = evaluateExpr(BO->getRHS(), vals, rConst);
            if (lConst && rConst) {
                isConst = true;
                switch (BO->getOpcode()) {
                    // standard arithmetic
                    case BO_Add: return lVal + rVal;
                    case BO_Sub: return lVal - rVal;
                    case BO_Mul: return lVal * rVal;
                    case BO_Div: return rVal != 0 ? lVal / rVal : 0;
                    case BO_Rem: return rVal != 0 ? lVal % rVal : 0;
                    
                    // bitwise logical
                    case BO_And: return lVal & rVal;
                    case BO_Or:  return lVal | rVal;
                    case BO_Xor: return lVal ^ rVal;
                    
                    // bitwise shifts (guarded against undefined behavior)
                    case BO_Shl: return (rVal >= 0 && rVal < 64) ? (lVal << rVal) : 0;
                    case BO_Shr: return (rVal >= 0 && rVal < 64) ? (lVal >> rVal) : 0;
                    
                    default: isConst = false; return 0;
                }
            }
        }
        if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(E)) {
            if (UO->getOpcode() == UO_Minus) {
                bool innerConst = false;
                long long val = evaluateExpr(UO->getSubExpr(), vals, innerConst);
                if (innerConst) { isConst = true; return -val; }
            }
        }
        isConst = false; return 0;
    }

    // Helper which extracts variables that aren't already folded into constants
    // digs out all the real variables actively being used in a statement
    void extractActualUses(const Stmt *S, map<string, long long> &currentVals, set<string> &uses) {
        if (!S) return;
        
        if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(S)) {
            //    ensures it is a variable, not a function (stops printf in Live Sets)
            if (isa<VarDecl>(DRE->getDecl())) {
                string name = DRE->getDecl()->getNameAsString();
                if (currentVals.find(name) == currentVals.end()) {
                    uses.insert(name);
                }
            }
        }
        for (const Stmt *child : S->children()) extractActualUses(child, currentVals, uses);
    }

    // Helper to replace propagated constants dynamically on the right hand sides
    // swaps out variable names with their actual hardcoded numbers in the source code
    void replaceDREs(const Stmt *S, map<string, long long> &vals, Rewriter &R) {
        if (!S) return;
        
        if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(S)) {
            if (BO->isAssignmentOp()) {
                replaceDREs(BO->getRHS(), vals, R);
                return;
            }
        }
        if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(S)) {
            string name = DRE->getDecl()->getNameAsString();
            if (vals.count(name)) {
                R.ReplaceText(DRE->getSourceRange(), std::to_string(vals[name]));
            }
        }
        for (const Stmt *child : S->children()) replaceDREs(child, vals, R);
    }

public:
    explicit MyASTVisitor(ASTContext *Context, Rewriter &R) : Context(Context), TheRewriter(R) {}

    // keeps track of what functions are being called so we can delete unused ones later
    bool VisitCallExpr(CallExpr *CE) {
        if (FunctionDecl *FD = CE->getDirectCallee()) {
            calledFunctions.insert(FD->getNameAsString());
        }
        return true;
    }

    // The main worker, this runs for every function definition it finds
    bool VisitFunctionDecl(FunctionDecl *Declaration) {
        if (!Declaration->hasBody()) return true;
        SourceManager &SM = Context->getSourceManager();
        if (SM.isInSystemHeader(Declaration->getLocation()) || 
            !SM.isInMainFile(Declaration->getLocation())) {
            return true; // to skip standard library and external header functions
        }
        
        if (Declaration->isThisDeclarationADefinition()) {
            allFunctions.insert(Declaration);
        }

        string funcName = Declaration->getNameInfo().getAsString();
        llvm::outs() << "Analyzing Function: " << funcName << "\n";

        // Maximizing CFG Detail 
        // tells Clang to build the Control Flow Graph and give us all the main details
        CFG::BuildOptions Options;
        Options.AddImplicitDtors = true;
        Options.AddInitializers = true;
        // Options.setAllAlwaysAdd(); // Forces Clang to include all sub-expressions
        unique_ptr<CFG> functionCFG = CFG::buildCFG(Declaration, Declaration->getBody(), Context, Options);
        if (!functionCFG) return true;

        // figures out which blocks can actually be reached (through standard BFS traversal)
        set<int> visitedBlocks;
        queue<CFGBlock*> q;
        q.push(&functionCFG->getEntry());
        while (!q.empty()) {
            CFGBlock *curr = q.front(); q.pop();
            int currID = curr->getBlockID();
            if (visitedBlocks.find(currID) == visitedBlocks.end()) {
                visitedBlocks.insert(currID);
                for (CFGBlock::succ_iterator succIt = curr->succ_begin(); succIt != curr->succ_end(); ++succIt) {
                    if (CFGBlock *succ = *succIt) q.push(succ);
                }
            }
        }

        //    Restricting Dominators strictly to Reachable Blocks
        // Figures out Dominators (which block must run before another block)
        map<int, set<int>> dominators;
        set<int> allBlocks = visitedBlocks; 
        
        int entryID = functionCFG->getEntry().getBlockID();
        for (int blockID : allBlocks) dominators[blockID] = (blockID == entryID) ? set<int>{entryID} : allBlocks;

        bool changed = true;
        while (changed) {
            changed = false;
            for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) {
                CFGBlock *block = *it;
                int B = block->getBlockID();
                
                // Skips the entry block and completely ignores unreachable blocks
                if (B == entryID || visitedBlocks.find(B) == visitedBlocks.end()) continue;

                set<int> newDom = allBlocks;
                for (CFGBlock::pred_iterator predIt = block->pred_begin(); predIt != block->pred_end(); ++predIt) {
                    if (CFGBlock *pred = *predIt) {
                        int P = pred->getBlockID();
                        // Only intersect with reachable predecessors
                        if (visitedBlocks.find(P) != visitedBlocks.end()) {
                            set<int> intersection;
                            set_intersection(newDom.begin(), newDom.end(), dominators[P].begin(), dominators[P].end(), inserter(intersection, intersection.begin()));
                            newDom = intersection;
                        }
                    }
                }
                newDom.insert(B);
                if (newDom != dominators[B]) { dominators[B] = newDom; changed = true; }
            }
        }

        map<int, set<string>> useLiveSets, defLiveSets;
        map<const Stmt*, set<string>> stmtUsesMap; 

        //    Track global usage
        set<string> everUsedVars;

        // pass 1: (forward) - Constant Folding and Local Constant Propagation
        // Note: This is strictly Local Constant Propagation (resets per basic block).
        // Safely implementing Global Constant Propagation across branches/loops requires 
        // converting the graph to SSA (Static Single Assignment) form first.
        // but Constant Folding still works everywhere without exceptions unlike Constant Propogation
        for (CFG::iterator blockIt = functionCFG->begin(); blockIt != functionCFG->end(); ++blockIt) {
            CFGBlock *block = *blockIt;
            if (visitedBlocks.find(block->getBlockID()) == visitedBlocks.end()) continue;

            //    isolating the constants to the current block
            map<string, long long> localConsts;

            for (CFGBlock::iterator elemIt = block->begin(); elemIt != block->end(); ++elemIt) {
                if (optional<CFGStmt> cfgStmt = elemIt->getAs<CFGStmt>()) {
                    const Stmt *stmt = cfgStmt->getStmt();
                    set<string> aUses;
                    
                    //    wiping local knowledge if state mutates unpredictably
                    if (hasSideEffects(stmt)) {
                        localConsts.clear(); 
                    }
                    
                    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(stmt)) {
                        if (BO->isAssignmentOp()) {
                            string lhsName = getVarName(BO->getLHS());
                            
                            //    strictly evaluating the standard '=' assignments
                            if (BO->getOpcode() == BO_Assign) {
                                bool isConst = false;
                                long long val = evaluateExpr(BO->getRHS(), localConsts, isConst);
                                if (isConst) {
                                    localConsts[lhsName] = val;
                                    TheRewriter.ReplaceText(BO->getRHS()->getSourceRange(), std::to_string(val));
                                } else {
                                    localConsts.erase(lhsName);
                                    extractActualUses(BO->getRHS(), localConsts, aUses);
                                    replaceDREs(BO->getRHS(), localConsts, TheRewriter);
                                }
                                //    Protecting Arrays from DCE 
                                // If lhsName is empty, it means this is NOT a simple scalar assignment (like x = 5).
                                // It is an ArraySubscript (arr[y] = 5) or Pointer Dereference (*p = 5).
                                // In these cases, the variables on the left side are actively used to compute the memory address!
                                if (lhsName.empty()) {
                                    extractActualUses(BO->getLHS(), localConsts, aUses);
                                }
                            } else {
                                // It is a compound assignment (+=, -=), The value changed, so it must be erased
                                localConsts.erase(lhsName);
                                extractActualUses(stmt, localConsts, aUses);
                                replaceDREs(stmt, localConsts, TheRewriter);
                            }
                        } else {
                            extractActualUses(stmt, localConsts, aUses);
                            replaceDREs(stmt, localConsts, TheRewriter);
                        }
                    } else if (const DeclStmt *DS = dyn_cast<DeclStmt>(stmt)) {
                        for (auto *decl : DS->decls()) {
                            if (VarDecl *VD = dyn_cast<VarDecl>(decl)) {
                                if (VD->hasInit()) {
                                    string name = VD->getNameAsString();
                                    bool isConst = false;
                                    long long val = evaluateExpr(VD->getInit(), localConsts, isConst);
                                    if (isConst) {
                                        localConsts[name] = val;
                                        TheRewriter.ReplaceText(VD->getInit()->getSourceRange(), std::to_string(val));
                                    } else {
                                        localConsts.erase(name);
                                        extractActualUses(VD->getInit(), localConsts, aUses);
                                        replaceDREs(VD->getInit(), localConsts, TheRewriter);
                                    }
                                }
                            }
                        }
                    } else {
                        extractActualUses(stmt, localConsts, aUses);
                        replaceDREs(stmt, localConsts, TheRewriter);
                    }
                    stmtUsesMap[stmt] = aUses; // saving accurate un-folded uses for DCE later
                }
            }
        }

        // pass 2: (backward) - Setup of Block-Level Gen/Kill sets which strictly respect ordering
        // figuring out exactly where variables are created (def) and used in each block
        for (CFG::iterator blockIt = functionCFG->begin(); blockIt != functionCFG->end(); ++blockIt) {
            CFGBlock *block = *blockIt;
            int blockID = block->getBlockID();
            if (visitedBlocks.find(blockID) == visitedBlocks.end()) continue;

            for (auto it = block->rbegin(); it != block->rend(); ++it) {
                if (optional<CFGStmt> cfgStmt = it->getAs<CFGStmt>()) {
                    const Stmt *stmt = cfgStmt->getStmt();
                    set<string> sDefs;
                    set<string> sUses = stmtUsesMap[stmt];
                    
                    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(stmt)) {
                        if (BO->isAssignmentOp()) {
                            string defVar = getVarName(BO->getLHS());
                            if (!defVar.empty()) sDefs.insert(defVar); 
                        }
                    } else if (const DeclStmt *DS = dyn_cast<DeclStmt>(stmt)) {
                        for (auto *decl : DS->decls()) {
                            if (VarDecl *VD = dyn_cast<VarDecl>(decl)) {
                                string defVar = VD->getNameAsString();
                                if (!defVar.empty()) sDefs.insert(defVar);
                            }
                        }
                    }
                    
                    for (const string &def : sDefs) {
                        useLiveSets[blockID].erase(def);
                        defLiveSets[blockID].insert(def);
                    }
                    for (const string &use : sUses) {
                        useLiveSets[blockID].insert(use);
                        //    Log that this variable is active with help of everUsedVars
                        everUsedVars.insert(use); 
                    }
                }
            }
        }

        // pass 3: Fixed-Point Iteration (Dataflow Analysis)
        // Looping, kept updating the live variable sets until the math stops changing
        map<int, set<string>> inLive, outLive;
        changed = true;
        while (changed) {
            changed = false;
            for (CFG::reverse_iterator it = functionCFG->rbegin(); it != functionCFG->rend(); ++it) {
                CFGBlock *block = *it;
                int b = block->getBlockID();
                if (visitedBlocks.find(b) == visitedBlocks.end()) continue;

                set<string> newOut;
                for (CFGBlock::succ_iterator succIt = block->succ_begin(); succIt != block->succ_end(); ++succIt) {
                    if (CFGBlock *succ = *succIt) newOut.insert(inLive[succ->getBlockID()].begin(), inLive[succ->getBlockID()].end());
                }
                outLive[b] = newOut;
                
                set<string> newIn = useLiveSets[b];
                set<string> outMinusDef;
                set_difference(outLive[b].begin(), outLive[b].end(), defLiveSets[b].begin(), defLiveSets[b].end(), inserter(outMinusDef, outMinusDef.begin()));
                newIn.insert(outMinusDef.begin(), outMinusDef.end());
                
                if (newIn != inLive[b]) { inLive[b] = newIn; changed = true; }
            }
        }

        // pass 4: (backward) - Statement-Level DCE & Unreachable Code Removal
        // time to actually chop out the dead code based on what we found above
        for (CFG::iterator blockIt = functionCFG->begin(); blockIt != functionCFG->end(); ++blockIt) {
            CFGBlock *block = *blockIt;
            int b = block->getBlockID();
            
            //    Phase 3 Unreachable Code Removal
            if (visitedBlocks.find(b) == visitedBlocks.end()) {
                // This block is completely disconnected so deleting all its contents
                for (auto elemIt = block->rbegin(); elemIt != block->rend(); ++elemIt) {
                    if (optional<CFGStmt> cfgStmt = elemIt->getAs<CFGStmt>()) {
                        const Stmt *stmt = cfgStmt->getStmt();
                        
                        // trying to cleanly grab the semicolon too so the C code doesn't break
                        SourceRange rangeToRemove = stmt->getSourceRange();
                        SourceLocation endLoc = rangeToRemove.getEnd();
                        if (endLoc.isValid()) {
                            const char* charData = TheRewriter.getSourceMgr().getCharacterData(endLoc);
                            int offset = 0;
                            while (offset < 5 && charData[offset] != ';' && charData[offset] != '\0') offset++;
                            if (charData[offset] == ';') endLoc = endLoc.getLocWithOffset(offset + 1);
                        }
                        TheRewriter.RemoveText(SourceRange(rangeToRemove.getBegin(), endLoc));
                        llvm::outs() << "[Unreachable Code Removed]\n";
                    }
                }
                continue; // now safely skipping the Live Variable Dataflow math
            }

            set<string> live = outLive[b];
            
            for (auto elemIt = block->rbegin(); elemIt != block->rend(); ++elemIt) {
                if (optional<CFGStmt> cfgStmt = elemIt->getAs<CFGStmt>()) {
                    const Stmt *stmt = cfgStmt->getStmt();
                    set<string> sDefs;
                    set<string> sUses = stmtUsesMap[stmt];
                    bool isDead = false;
                    
                    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(stmt)) {
                        if (BO->isAssignmentOp()) {
                            string defVar = getVarName(BO->getLHS());
                            if (!defVar.empty()) { // guard against aliases
                                sDefs.insert(defVar);
                                if (live.find(defVar) == live.end()) isDead = true;
                            }
                        }
                    } else if (const DeclStmt *DS = dyn_cast<DeclStmt>(stmt)) {
                        bool allDead = true;
                        for (auto *decl : DS->decls()) {
                            if (VarDecl *VD = dyn_cast<VarDecl>(decl)) {
                                string defVar = VD->getNameAsString();
                                if (!defVar.empty()) {
                                    sDefs.insert(defVar);
                                    
                                    //    Protecting the Type Signature
                                    // It is only truly dead if the current value is dead and it's never used anywhere else
                                    if (live.find(defVar) != live.end() || everUsedVars.count(defVar)) {
                                        allDead = false;
                                    }
                                }
                            }
                        }
                        if (allDead && !sDefs.empty()) isDead = true;
                    }
                    
                    //    Protecting Side Effects 
                    if (isDead && hasSideEffects(stmt)) {
                        isDead = false; // prevents killing statements with function calls
                    }
                    
                    if (isDead) {
                        // aggressive Removal: attempt to get the extended source range to catch semicolons
                        SourceRange rangeToRemove = stmt->getSourceRange();
                        
                        // extending the end location slightly to try and grab the semicolon
                        SourceLocation endLoc = rangeToRemove.getEnd();
                        if (endLoc.isValid()) {
                            // find the character after the end location
                            const char* charData = TheRewriter.getSourceMgr().getCharacterData(endLoc);
                            int offset = 0;
                            // look ahead up to 5 chars for a semicolon
                            while (offset < 5 && charData[offset] != ';' && charData[offset] != '\0') {
                                offset++;
                            }
                            if (charData[offset] == ';') {
                                endLoc = endLoc.getLocWithOffset(offset + 1); // include the semicolon
                            }
                        }
                        
                        TheRewriter.RemoveText(SourceRange(rangeToRemove.getBegin(), endLoc));
                        
                        llvm::outs() << "[Dead Code Removed]\n";
                    } else {
                        for (const string &def : sDefs) live.erase(def);
                        for (const string &use : sUses) live.insert(use);
                    }
                }
            }
        }

        // Pass 5 - Forward Security Taint Analysis
        // security check, tracking bad input to make sure it doesn't cause damage
        map<int, set<string>> taintIn, taintOut;
        set<int> vulnerableBlocks; 
        map<int, string> vulnReasons;

        bool taintChanged = true;
        while (taintChanged) {
            taintChanged = false;
            for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) {
                CFGBlock *block = *it;
                int b = block->getBlockID();
                if (visitedBlocks.find(b) == visitedBlocks.end()) continue;

                // 1.calculating IN set (union of predecessors' OUT sets)
                set<string> newIn;
                for (CFGBlock::pred_iterator predIt = block->pred_begin(); predIt != block->pred_end(); ++predIt) {
                    if (CFGBlock *pred = *predIt) {
                        newIn.insert(taintOut[pred->getBlockID()].begin(), taintOut[pred->getBlockID()].end());
                    }
                }
                taintIn[b] = newIn;

                // 2.process block instructions to generate OUT set
                set<string> currentTaint = newIn;
                map<string, long long> emptyVals; // dummy map for extractActualUses
                
                for (CFGBlock::iterator elemIt = block->begin(); elemIt != block->end(); ++elemIt) {
                    if (optional<CFGStmt> cfgStmt = elemIt->getAs<CFGStmt>()) {
                        const Stmt *stmt = cfgStmt->getStmt();
                        
                        // A.checking for SOURCES (Input introduces taint)
                        // this is where bad data sneaks in (like user input from scanf)
                        if (const CallExpr *CE = dyn_cast<CallExpr>(stmt)) {
                            if (const FunctionDecl *FD = CE->getDirectCallee()) {
                                string fName = FD->getNameAsString();
                                if (fName == "scanf" || fName == "gets" || fName == "fgets" || fName == "read") {
                                    for (unsigned i = 0; i < CE->getNumArgs(); ++i) {
                                        set<string> argUses;
                                        extractActualUses(CE->getArg(i), emptyVals, argUses);
                                        for (const string &use : argUses) currentTaint.insert(use);
                                    }
                                }
                            }
                        }
                        
                        // B.checking for PROPAGATION
                        // how does the bad data spread through regular math and assignments?
                        // B1. standard assignments (y=x+1)
                        if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(stmt)) {
                            if (BO->isAssignmentOp()) {
                                string lhs = getVarName(BO->getLHS());
                                set<string> rhsUses;
                                extractActualUses(BO->getRHS(), emptyVals, rhsUses);
                                
                                bool isTainted = false;
                                for (const string &use : rhsUses) {
                                    if (currentTaint.count(use)) { isTainted = true; break; }
                                }
                                
                                if (isTainted && !lhs.empty()) {
                                    currentTaint.insert(lhs);
                                } else if (!lhs.empty() && BO->getOpcode() == BO_Assign) {
                                    // Fix: Only erase on direct '=' assignment. 
                                    // Compound assignments (+=) retain their previous taint!
                                    currentTaint.erase(lhs);
                                }
                            }
                        }
                        // B2.declarations with initialization (int y = x;)
                        else if (const DeclStmt *DS = dyn_cast<DeclStmt>(stmt)) {
                            for (auto *decl : DS->decls()) {
                                if (VarDecl *VD = dyn_cast<VarDecl>(decl)) {
                                    if (VD->hasInit()) {
                                        string lhs = VD->getNameAsString();
                                        set<string> rhsUses;
                                        extractActualUses(VD->getInit(), emptyVals, rhsUses);
                                        
                                        bool isTainted = false;
                                        for (const string &use : rhsUses) {
                                            if (currentTaint.count(use)) { isTainted = true; break; }
                                        }
                                        
                                        if (isTainted && !lhs.empty()) currentTaint.insert(lhs);
                                        // Note: No need to erase here, as it's a brand new variable
                                    }
                                }
                            }
                        }
                        
                       // C.checking for SINKS (vulnerability detected)
                       // Where does the bad data cause damage?
                        
                        // Sink 1: Array Indexing (Buffer Overflow)
                        //    Deep scan the entire statement tree for array accesses
                        queue<const Stmt*> stmtQ;
                        stmtQ.push(stmt);
                        
                        while (!stmtQ.empty()) {
                            const Stmt *curr = stmtQ.front();
                            stmtQ.pop();
                            
                            if (const ArraySubscriptExpr *ASE = dyn_cast<ArraySubscriptExpr>(curr)) {
                                set<string> idxUses;
                                extractActualUses(ASE->getIdx(), emptyVals, idxUses);
                                for (const string &use : idxUses) {
                                    if (currentTaint.count(use)) {
                                        vulnerableBlocks.insert(b);
                                        vulnReasons[b] = "Buffer Overflow Risk: Tainted index '" + use + "'";
                                    }
                                }
                            }
                            
                            // pushing all children into the queue to continue digging
                            for (const Stmt *child : curr->children()) {
                                if (child) stmtQ.push(child);
                            }
                        }
                        
                        // Sink 2 & 3: Dangerous Function Calls
                        if (const CallExpr *CE = dyn_cast<CallExpr>(stmt)) {
                            if (const FunctionDecl *FD = CE->getDirectCallee()) {
                                string fName = FD->getNameAsString();
                                
                                // Command Injection (system, popen)
                                if (fName == "system" || fName == "popen") {
                                    if (CE->getNumArgs() > 0) {
                                        set<string> argUses;
                                        extractActualUses(CE->getArg(0), emptyVals, argUses);
                                        for (const string &use : argUses) {
                                            if (currentTaint.count(use)) {
                                                vulnerableBlocks.insert(b);
                                                vulnReasons[b] = "Command Injection Risk: Tainted arg '" + use + "'";
                                            }
                                        }
                                    }
                                }
                                // format string Vulnerability like with printf
                                else if (fName == "printf") {
                                    if (CE->getNumArgs() > 0) {
                                        set<string> argUses;
                                        extractActualUses(CE->getArg(0), emptyVals, argUses);
                                        for (const string &use : argUses) {
                                            if (currentTaint.count(use)) {
                                                vulnerableBlocks.insert(b);
                                                vulnReasons[b] = "Format String Risk: Tainted format '" + use + "'";
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                
                if (currentTaint != taintOut[b]) {
                    taintOut[b] = currentTaint;
                    taintChanged = true;
                }
            }
        }

        // Natural loop detection
        // finding those while/for loops by looking for edges that point backward
        map<int, set<int>> naturalLoops; // maps header ID to all block IDs in that loop

        for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) {
            CFGBlock *header = *it;
            int H = header->getBlockID();
            
            // scanning all blocks to see if any have a back-edge pointing to H
            for (CFG::iterator tailIt = functionCFG->begin(); tailIt != functionCFG->end(); ++tailIt) {
                CFGBlock *tail = *tailIt;
                int T = tail->getBlockID();
                
                // checking if an edge exists from T -> H
                bool hasEdge = false;
                for (CFGBlock::succ_iterator succIt = tail->succ_begin(); succIt != tail->succ_end(); ++succIt) {
                    if (*succIt && (*succIt)->getBlockID() == H) { hasEdge = true; break; }
                }
                
                // If T -> H is an edge, AND H dominates T, it is a guaranteed Loop Back-edge
                if (hasEdge && dominators[T].count(H)) {
                    set<int> loopBlocks;
                    loopBlocks.insert(H);
                    loopBlocks.insert(T);
                    
                    // traverse backwards from Tail to Header to find all blocks trapped in the loop
                    if (H != T) {
                        queue<CFGBlock*> worklist; //    storing pointers, not just IDs
                        worklist.push(tail);
                        
                        while (!worklist.empty()) {
                            CFGBlock *currBlock = worklist.front(); 
                            worklist.pop();
                            
                            //    direct predecessor traversal (Instant lookup instead of CFG-wide scan)
                            for (CFGBlock::pred_iterator pIt = currBlock->pred_begin(); pIt != currBlock->pred_end(); ++pIt) {
                                if (CFGBlock *pred = *pIt) {
                                    int P = pred->getBlockID();
                                    // If we haven't seen this block yet (and it's not the header, which is already in the set)
                                    if (loopBlocks.find(P) == loopBlocks.end()) {
                                        loopBlocks.insert(P);
                                        worklist.push(pred); 
                                    }
                                }
                            }
                        }
                    }
                    
                    // Store the discovered loop
                    naturalLoops[H].insert(loopBlocks.begin(), loopBlocks.end());
                    
                    // Print findings to the console for verification
                    llvm::outs() << ">> Detected Loop with Header Block " << H << ". Loop Blocks: { ";
                    for (int lb : naturalLoops[H]) llvm::outs() << lb << " ";
                    llvm::outs() << "}\n";
                }
            }
        }

       // DOT Graph Generation
       // now have to draw the pictures for Graphviz
        std::error_code EC;
        string dotFileName = funcName + "_cfg.dot"; 
        llvm::raw_fd_ostream dotFile(dotFileName, EC, llvm::sys::fs::OF_None);

        if (!EC) {
            dotFile << "digraph CFG {\n";
            dotFile << "  ranksep=0.4;\n  nodesep=0.4;\n";
            //    adding width=0 and height=0 to force tight wrapping around text
            dotFile << "  node [fontname=\"Helvetica\", fontsize=10, margin=\"0.1,0.05\", width=0, height=0];\n";

            PrintingPolicy Policy(Context->getLangOpts());

            // doing the setup of node shapes and colors based on what the block actually does
            for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) {
                CFGBlock *block = *it;
                int blockID = block->getBlockID();
                
                string shape = "box"; 
                string blockType = "Statement";
                string fillColor = "\"#f8f9fa\""; 
                
                if (block == &functionCFG->getEntry()) { shape = "ellipse"; blockType = "Entry"; fillColor = "\"#d4edda\""; } 
                else if (block == &functionCFG->getExit()) { shape = "ellipse"; blockType = "Exit"; fillColor = "\"#d4edda\""; }
                //    highlights vulnerable sinks
                else if (vulnerableBlocks.count(blockID)) { 
                    shape = "octagon"; 
                    blockType = "SECURITY RISK"; 
                    fillColor = "\"#ffb3b3\""; // Light Red
                }
                else if (block->getTerminator().getStmt() != nullptr) { shape = "diamond"; blockType = "Control Flow"; fillColor = "\"#cce5ff\""; } 
                else {
                    for (CFGBlock::iterator elemIt = block->begin(); elemIt != block->end(); ++elemIt) {
                        if (optional<CFGStmt> cfgStmt = elemIt->getAs<CFGStmt>()) {
                            if (const CallExpr *call = dyn_cast<CallExpr>(cfgStmt->getStmt())) {
                                if (const FunctionDecl *func = call->getDirectCallee()) {
                                    string calleeName = func->getNameAsString();
                                    if (calleeName == "printf" || calleeName == "scanf" || calleeName == "puts" || calleeName == "gets") {
                                        shape = "parallelogram"; blockType = "I/O"; fillColor = "\"#fff3cd\""; break; 
                                    }
                                }
                            }
                        }
                    }
                }

                if (visitedBlocks.find(blockID) == visitedBlocks.end()) { blockType = "Dead Code"; fillColor = "\"#f8d7da\""; }

                //    Identifying Loop Headers
                string penWidth = "1.0"; // Default border thickness
                string loopLabel = "";
                if (naturalLoops.count(blockID)) {
                    penWidth = "3.0"; // Make the border thick
                    loopLabel = "\\l[LOOP HEADER]";
                }

            // extracting and formating the actual C code for the block
            // cleaning up the raw C code so it doesn't totally break the Graphviz parser
                string blockCode = "";
                for (CFGBlock::iterator elemIt = block->begin(); elemIt != block->end(); ++elemIt) {
                    if (optional<CFGStmt> cfgStmt = elemIt->getAs<CFGStmt>()) {
                        const Stmt *S = cfgStmt->getStmt();
                        
                        // to hide floating AST evaluation nodes (like the ghost '0;') 
                        if (isa<Expr>(S) && !isa<BinaryOperator>(S) && !isa<CallExpr>(S) && !isa<UnaryOperator>(S)) {
                            continue; 
                        }
                        
                        string stmtStr;
                        llvm::raw_string_ostream rso(stmtStr);
                        cfgStmt->getStmt()->printPretty(rso, nullptr, Policy);
                        
                        string sanitized = rso.str();
                        
                        // 1. erasing carriage returns which break Graphviz completely
                        sanitized.erase(std::remove(sanitized.begin(), sanitized.end(), '\r'), sanitized.end());
                        
                        // 2. safely removing leading/trailing whitespace
                        size_t first = sanitized.find_first_not_of(" \n\t");
                        if (first == string::npos) continue; 
                        size_t last = sanitized.find_last_not_of(" \n\t");
                        sanitized = sanitized.substr(first, (last - first + 1));
                        
                        if (!sanitized.empty()) {
                            //    escaping literal backslashes to stop \n from breaking Graphviz
                            size_t pos = 0;
                            while ((pos = sanitized.find("\\", pos)) != string::npos) { sanitized.replace(pos, 1, "\\\\"); pos += 2; }
                            
                            // 3.escaping quotes for Graphviz
                            pos = 0;
                            while ((pos = sanitized.find("\"", pos)) != string::npos) { sanitized.replace(pos, 1, "\\\""); pos += 2; }
                            
                            // 4.using standard centered newlines
                            pos = 0;
                            while ((pos = sanitized.find("\n", pos)) != string::npos) { sanitized.replace(pos, 1, "\\n"); pos += 2; }
                            
                            // 5.ensuring statements end with a semicolon
                            if (sanitized.back() != ';' && sanitized.back() != '}') {
                               sanitized += ";";
                            }
                            
                            blockCode += sanitized + "\\n";
                        }
                    }
                }

                //    preventing the Graphviz boundary collapse by safely building the label string
                // building the text label for the block, showing the code and the live variables
                string extraLabel = vulnerableBlocks.count(blockID) ? "\\n! " + vulnReasons[blockID] + " !" : "";
                string labelStr = "[" + blockType + "] Block " + std::to_string(blockID) + extraLabel;
                
                // only add the separator if there is going to be content below it
                if (!blockCode.empty() || !inLive[blockID].empty() || !outLive[blockID].empty()) {
                    labelStr += "\\n   \\n";
                }
                
                if (!blockCode.empty()) {
                    labelStr += blockCode;
                }
                
                if (!inLive[blockID].empty()) {
                    labelStr += "Live IN: {";
                    bool isFirst = true;
                    for (const string &var : inLive[blockID]) {
                        if (!isFirst) labelStr += ", ";
                        labelStr += var;
                        isFirst = false;
                    }
                    labelStr += "}\\n";
                }
                
                if (!outLive[blockID].empty()) {
                    labelStr += "Live OUT: {";
                    bool isFirst = true;
                    for (const string &var : outLive[blockID]) {
                        if (!isFirst) labelStr += ", ";
                        labelStr += var;
                        isFirst = false;
                    }
                    labelStr += "}\\n";
                }
                
                // stripping the absolute final hanging newline to ensure the bounding box wraps perfectly
                if (labelStr.length() >= 2 && labelStr.substr(labelStr.length() - 2) == "\\n") {
                    labelStr = labelStr.substr(0, labelStr.length() - 2);
                }

                // writing the cleanly formatted string to the file
                dotFile << "  Block" << blockID << " [shape=\"" << shape 
                        << "\", style=\"filled\", color=\"black\", fillcolor=" << fillColor 
                        << ", penwidth=" << penWidth 
                        << ", label=\"" << labelStr << "\"];\n";
                
            } 

            //    Drawing Edges with True/False Labels
            // drawing the arrows connecting the blocks and colour-coding the true/false branches and loop back-edges
            for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) {
                CFGBlock *block = *it;
                int A = block->getBlockID();
                
                // checking if this block evaluates a condition
                bool isConditional = (block->getTerminator().getStmt() != nullptr) && (block->succ_size() == 2);
                int succIndex = 0;

                for (CFGBlock::succ_iterator succIt = block->succ_begin(); succIt != block->succ_end(); ++succIt, ++succIndex) {
                    if (CFGBlock *succ = *succIt) {
                        int B = succ->getBlockID();
                        dotFile << "  Block" << A << " -> Block" << B;
                        
                        // storing edge attributes to prevent syntax clashes
                        std::vector<string> attrs;
                        
                        if (dominators[A].count(B)) {
                            attrs.push_back("penwidth=2.0");
                            attrs.push_back("label=\"Loop Back-edge\"");
                            attrs.push_back("fontname=\"Helvetica\"");
                            attrs.push_back("fontsize=9");
                            attrs.push_back("color=\"blue\"");
                        } else if (isConditional) {
                            if (succIndex == 0) {
                                attrs.push_back("label=\" True\"");
                                attrs.push_back("fontcolor=\"#28a745\"");
                                attrs.push_back("color=\"#28a745\"");
                            } else if (succIndex == 1) {
                                attrs.push_back("label=\" False\"");
                                attrs.push_back("fontcolor=\"#dc3545\"");
                                attrs.push_back("color=\"#dc3545\"");
                            }
                        }
                        
                        if (!attrs.empty()) {
                            dotFile << " [";
                            for (size_t i = 0; i < attrs.size(); ++i) {
                                dotFile << attrs[i] << (i < attrs.size() - 1 ? ", " : "");
                            }
                            dotFile << "]";
                        }
                        dotFile << ";\n";
                    }
                }
            }
            dotFile << "}\n";
            dotFile.close();
            llvm::outs() << "Successfully generated CFG visualization: " << dotFileName << "\n";
        } else {
            llvm::errs() << "Error opening file " << dotFileName << ": " << EC.message() << "\n";
        }

        return true; 
    }

    // destroy any functions that aren't 'main' and never get called
    void RemoveUnreachableFunctions() {
        for (const FunctionDecl* FD : allFunctions) {
            string name = FD->getNameAsString();
            if (name != "main" && calledFunctions.find(name) == calledFunctions.end()) {
                TheRewriter.ReplaceText(FD->getSourceRange(), "/* [Unreachable Function Removed] */");
            }
        }
    }
};

// These just hook our visitor into Clang's main parsing pipeline
class MyASTConsumer : public ASTConsumer {
private:
    ASTContext *Ctx;
    Rewriter TheRewriter;
public:
    virtual void Initialize(ASTContext &Context) override {
        Ctx = &Context;
        TheRewriter.setSourceMgr(Context.getSourceManager(), Context.getLangOpts());
    }

    virtual void HandleTranslationUnit(ASTContext &Context) override { 
        MyASTVisitor Visitor(&Context, TheRewriter);
        Visitor.TraverseDecl(Context.getTranslationUnitDecl()); 
        
        Visitor.RemoveUnreachableFunctions();
        
        // to save the newly optimized C code into a temp file
        const RewriteBuffer *RewriteBuf = TheRewriter.getRewriteBufferFor(Context.getSourceManager().getMainFileID());
        if (RewriteBuf) {
            std::error_code EC;
            llvm::raw_fd_ostream outFile("optimized.c", EC, llvm::sys::fs::OF_None);
            if (!EC) {
                outFile << string(RewriteBuf->begin(), RewriteBuf->end());
                outFile.close();
            }
        }
    }
};

class MyFrontendAction : public ASTFrontendAction {
public:
    virtual unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &Compiler, llvm::StringRef InFile) override {
        return make_unique<MyASTConsumer>();
    }
};

// The standard entry point to fire up the Clang tool
int main(int argc, const char **argv) {
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
    if (!ExpectedParser) { llvm::errs() << ExpectedParser.takeError(); return 1; }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    return Tool.run(newFrontendActionFactory<MyFrontendAction>().get());
}