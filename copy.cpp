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

static llvm::cl::OptionCategory MyToolCategory("Static Analyzer Options");

class MyASTVisitor : public RecursiveASTVisitor<MyASTVisitor> {
private:
    ASTContext *Context;
    Rewriter &TheRewriter;
    set<const FunctionDecl*> allFunctions;
    set<string> calledFunctions;

    // --- PHASE 2 FIX: Stop Dataflow Pollution from Pointers/Arrays ---
    string getVarName(Expr *E) {
        if (!E) return "";
        if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts())) {
            return DRE->getDecl()->getNameAsString();
        }
        return ""; // Return empty string instead of "unknown" to prevent alias pollution
    }

    // --- PHASE 2/3 FIX: Protect I/O, System Calls, and Increments ---
    bool hasSideEffects(const Stmt *S) {
        if (!S) return false;
        if (isa<CallExpr>(S)) return true;
        
        // NEW: Catch state-mutating unary operators (x++, --y)
        if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(S)) {
            if (UO->isIncrementDecrementOp()) return true;
        }
        
        for (const Stmt *child : S->children()) {
            if (hasSideEffects(child)) return true;
        }
        return false;
    }

    // Helper: Evaluates arithmetic utilizing our dynamically tracked constants
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
                    // Standard Arithmetic
                    case BO_Add: return lVal + rVal;
                    case BO_Sub: return lVal - rVal;
                    case BO_Mul: return lVal * rVal;
                    case BO_Div: return rVal != 0 ? lVal / rVal : 0;
                    case BO_Rem: return rVal != 0 ? lVal % rVal : 0;
                    
                    // Bitwise Logical
                    case BO_And: return lVal & rVal;
                    case BO_Or:  return lVal | rVal;
                    case BO_Xor: return lVal ^ rVal;
                    
                    // Bitwise Shifts (Guarded against undefined behavior)
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

    // Helper: Extracts variables that aren't already folded into constants
    void extractActualUses(const Stmt *S, map<string, long long> &currentVals, set<string> &uses) {
        if (!S) return;
        if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(S)) {
            string name = DRE->getDecl()->getNameAsString();
            if (currentVals.find(name) == currentVals.end()) {
                uses.insert(name);
            }
        }
        for (const Stmt *child : S->children()) extractActualUses(child, currentVals, uses);
    }

    // Helper: Replaces propagated constants dynamically on the right hand sides
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

    bool VisitCallExpr(CallExpr *CE) {
        if (FunctionDecl *FD = CE->getDirectCallee()) {
            calledFunctions.insert(FD->getNameAsString());
        }
        return true;
    }

    bool VisitFunctionDecl(FunctionDecl *Declaration) {
        if (!Declaration->hasBody()) return true;
        // --- NEW CODE: System Header Filter ---
        SourceManager &SM = Context->getSourceManager();
        if (SM.isInSystemHeader(Declaration->getLocation()) || 
            !SM.isInMainFile(Declaration->getLocation())) {
            return true; // Skip standard library and external header functions
        }
        // --------------------------------------
        if (Declaration->isThisDeclarationADefinition()) {
            allFunctions.insert(Declaration);
        }

        string funcName = Declaration->getNameInfo().getAsString();
        llvm::outs() << "Analyzing Function: " << funcName << "\n";

        // --- IMPROVEMENT 2: Maximize CFG Detail ---
        CFG::BuildOptions Options;
        Options.AddImplicitDtors = true;
        Options.AddInitializers = true;
        Options.setAllAlwaysAdd(); // Forces Clang to include all sub-expressions
        unique_ptr<CFG> functionCFG = CFG::buildCFG(Declaration, Declaration->getBody(), Context, Options);
        if (!functionCFG) return true;

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

        map<int, set<int>> dominators;
        set<int> allBlocks;
        for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) allBlocks.insert((*it)->getBlockID());
        int entryID = functionCFG->getEntry().getBlockID();
        for (int blockID : allBlocks) dominators[blockID] = (blockID == entryID) ? set<int>{entryID} : allBlocks;

        bool changed = true;
        while (changed) {
            changed = false;
            for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) {
                CFGBlock *block = *it;
                int B = block->getBlockID();
                if (B == entryID) continue;

                set<int> newDom = allBlocks;
                for (CFGBlock::pred_iterator predIt = block->pred_begin(); predIt != block->pred_end(); ++predIt) {
                    if (CFGBlock *pred = *predIt) {
                        int P = pred->getBlockID();
                        set<int> intersection;
                        set_intersection(newDom.begin(), newDom.end(), dominators[P].begin(), dominators[P].end(), inserter(intersection, intersection.begin()));
                        newDom = intersection;
                    }
                }
                newDom.insert(B);
                if (newDom != dominators[B]) { dominators[B] = newDom; changed = true; }
            }
        }

        map<int, set<string>> useLiveSets, defLiveSets;
        map<const Stmt*, set<string>> stmtUsesMap; 

        // PASS 1: FORWARD - Constant Folding and Local Constant Propagation
        // Note: This is strictly Local Constant Propagation (resets per basic block).
        // Safely implementing Global Constant Propagation across branches/loops requires 
        // converting the graph to SSA (Static Single Assignment) form first.
        // but Constant Folding still works everywhere without exceptions unlike Constant Propogation
        for (CFG::iterator blockIt = functionCFG->begin(); blockIt != functionCFG->end(); ++blockIt) {
            CFGBlock *block = *blockIt;
            if (visitedBlocks.find(block->getBlockID()) == visitedBlocks.end()) continue;

            // --- THE FIX: Isolate the constants to the current block ---
            map<string, long long> localConsts;

            for (CFGBlock::iterator elemIt = block->begin(); elemIt != block->end(); ++elemIt) {
                if (optional<CFGStmt> cfgStmt = elemIt->getAs<CFGStmt>()) {
                    const Stmt *stmt = cfgStmt->getStmt();
                    set<string> aUses;
                    
                    // --- THE FIX: Wipe local knowledge if state mutates unpredictably ---
                    if (hasSideEffects(stmt)) {
                        localConsts.clear(); 
                    }
                    
                    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(stmt)) {
                        if (BO->isAssignmentOp()) {
                            string lhsName = getVarName(BO->getLHS());
                            
                            // --- THE FIX: Strictly evaluate standard '=' assignments ---
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
                            } else {
                                // It is a compound assignment (+=, -=). The value changed, so we must erase it!
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
                    stmtUsesMap[stmt] = aUses; // Save accurate un-folded uses for DCE later
                }
            }
        }

        // PASS 2: BACKWARD - Setup Block-Level Gen/Kill Sets strictly respecting ordering
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
                            if (!defVar.empty()) sDefs.insert(defVar); // Guard against aliases
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
                    for (const string &use : sUses) useLiveSets[blockID].insert(use);
                }
            }
        }

        // PASS 3: Fixed-Point Iteration (Dataflow Analysis)
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

        // PASS 4: BACKWARD - Statement-Level Dead Code Elimination
        for (CFG::iterator blockIt = functionCFG->begin(); blockIt != functionCFG->end(); ++blockIt) {
            CFGBlock *block = *blockIt;
            int b = block->getBlockID();
            if (visitedBlocks.find(b) == visitedBlocks.end()) continue;

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
                            if (!defVar.empty()) { // Guard against aliases
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
                                    if (live.find(defVar) != live.end()) allDead = false;
                                }
                            }
                        }
                        if (allDead && !sDefs.empty()) isDead = true;
                    }
                    
                    // --- PHASE 2/3 FIX: Protect Side Effects ---
                    if (isDead && hasSideEffects(stmt)) {
                        isDead = false; // Never kill statements with function calls
                    }
                    
                    if (isDead) {
                        TheRewriter.ReplaceText(stmt->getSourceRange(), "/* [DEAD CODE REMOVED] */;");
                    } else {
                        for (const string &def : sDefs) live.erase(def);
                        for (const string &use : sUses) live.insert(use);
                    }
                }
            }
        }

       // --- DOT Graph Generation (Improvements 3, 5, & 6) ---
        std::error_code EC;
        string dotFileName = funcName + "_cfg.dot"; 
        llvm::raw_fd_ostream dotFile(dotFileName, EC, llvm::sys::fs::OF_None);

        if (!EC) {
            dotFile << "digraph CFG {\n";
            dotFile << "  ranksep=0.4;\n  nodesep=0.4;\n";
            dotFile << "  node [fontname=\"Helvetica\", fontsize=10, margin=0.05];\n"; 

            PrintingPolicy Policy(Context->getLangOpts()); // Required for Improvement 5

            for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) {
                CFGBlock *block = *it;
                int blockID = block->getBlockID();
                
                string shape = "box"; 
                string blockType = "Statement";
                string fillColor = "\"#f8f9fa\""; 
                
                if (block == &functionCFG->getEntry()) { shape = "ellipse"; blockType = "Entry"; fillColor = "\"#d4edda\""; } 
                else if (block == &functionCFG->getExit()) { shape = "ellipse"; blockType = "Exit"; fillColor = "\"#d4edda\""; }
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

                // --- IMPROVEMENT 5: Extract actual C code for the block ---
                string blockCode = "";
                for (CFGBlock::iterator elemIt = block->begin(); elemIt != block->end(); ++elemIt) {
                    if (optional<CFGStmt> cfgStmt = elemIt->getAs<CFGStmt>()) {
                        string stmtStr;
                        llvm::raw_string_ostream rso(stmtStr);
                        cfgStmt->getStmt()->printPretty(rso, nullptr, Policy);
                        
                        // Sanitize string for DOT format (escape quotes and newlines)
                        string sanitized = rso.str();
                        size_t pos = 0;
                        while ((pos = sanitized.find("\"", pos)) != string::npos) { sanitized.replace(pos, 1, "\\\""); pos += 2; }
                        while ((pos = sanitized.find("\n", pos)) != string::npos) { sanitized.replace(pos, 1, "\\l"); pos += 2; }
                        
                        blockCode += sanitized + "\\l";
                    }
                }

                // Write the node using dotFile instead of llvm::outs()
                dotFile << "  Block" << blockID << " [shape=\"" << shape 
                        << "\", style=filled, fillcolor=" << fillColor 
                        << ", label=\"[" << blockType << "]\\nBlock " << blockID << "\\l";
                
                if (!blockCode.empty()) {
                    dotFile << "----------\\l" << blockCode;
                }
                
                dotFile << "----------\\lLive IN: {";
                for (const string &v : inLive[blockID]) dotFile << v << " ";
                dotFile << "}\\lLive OUT: {";
                for (const string &v : outLive[blockID]) dotFile << v << " ";
                dotFile << "}\"];\n"; 
            }

            // --- IMPROVEMENT 6: Draw Edges with True/False Labels ---
            for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) {
                CFGBlock *block = *it;
                int A = block->getBlockID();
                
                // Check if this block evaluates a condition
                bool isConditional = (block->getTerminator().getStmt() != nullptr) && (block->succ_size() == 2);
                int succIndex = 0;

                for (CFGBlock::succ_iterator succIt = block->succ_begin(); succIt != block->succ_end(); ++succIt, ++succIndex) {
                    if (CFGBlock *succ = *succIt) {
                        int B = succ->getBlockID();
                        dotFile << "  Block" << A << " -> Block" << B;
                        
                        // Store edge attributes to prevent syntax clashes
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

    void RemoveUnreachableFunctions() {
        for (const FunctionDecl* FD : allFunctions) {
            string name = FD->getNameAsString();
            if (name != "main" && calledFunctions.find(name) == calledFunctions.end()) {
                TheRewriter.ReplaceText(FD->getSourceRange(), "/* [UNREACHABLE FUNCTION REMOVED] */");
            }
        }
    }
};

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

int main(int argc, const char **argv) {
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
    if (!ExpectedParser) { llvm::errs() << ExpectedParser.takeError(); return 1; }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    return Tool.run(newFrontendActionFactory<MyFrontendAction>().get());
}