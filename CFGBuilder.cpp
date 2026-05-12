#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Tooling/CommonOptionsParser.h" 
#include "clang/Analysis/CFG.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"          
#include <map>
#include <set>
#include <queue>
#include <algorithm> 
#include <iterator>  

using namespace clang;
using namespace clang::tooling;
using namespace std;

static llvm::cl::OptionCategory MyToolCategory("Static Analyzer Options");

struct Definition {
    int id;
    string varName;
    int blockID;
};

class MyASTVisitor : public RecursiveASTVisitor<MyASTVisitor> {
private:
    ASTContext *Context;
    int defCounter = 0;
    vector<Definition> allDefinitions;

    string getVarName(Expr *E) {
        if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenCasts())) {
            return DRE->getDecl()->getNameAsString();
        }
        return "unknown";
    }

public:
    explicit MyASTVisitor(ASTContext *Context) : Context(Context) {}

    bool VisitFunctionDecl(FunctionDecl *Declaration) {
        if (!Declaration->hasBody()) return true;

        llvm::outs() << "Analyzing Function: " << Declaration->getNameInfo().getAsString() << "\n";        

        CFG::BuildOptions Options;
        
        unique_ptr<CFG> functionCFG = CFG::buildCFG(Declaration, Declaration->getBody(), Context, Options);

        if (!functionCFG) return true;

        llvm::outs() << "Phase 3 - Graph Optimizations\n";
        set<int> visitedBlocks;
        queue<CFGBlock*> q;
        
        q.push(&functionCFG->getEntry());
        while (!q.empty()) {
            CFGBlock *curr = q.front();
            q.pop();
            int currID = curr->getBlockID();
            if (visitedBlocks.find(currID) == visitedBlocks.end()) {
                visitedBlocks.insert(currID);
                for (CFGBlock::succ_iterator succIt = curr->succ_begin(); succIt != curr->succ_end(); ++succIt) {
                    if (CFGBlock *succ = *succIt) q.push(succ);
                }
            }
        }

        for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) {
            int blockID = (*it)->getBlockID();
            if (visitedBlocks.find(blockID) == visitedBlocks.end()) {
                llvm::outs() << "  [DEAD CODE] Block " << blockID << " is Unreachable.\n";
            }
        }
    
        map<int, set<int>> dominators;
        set<int> allBlocks;
        for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) {
            allBlocks.insert((*it)->getBlockID());
        }

        int entryID = functionCFG->getEntry().getBlockID();
        for (int blockID : allBlocks) {
            if (blockID == entryID) dominators[blockID].insert(entryID);
            else dominators[blockID] = allBlocks;
        }

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
                if (newDom != dominators[B]) {
                    dominators[B] = newDom;
                    changed = true;
                }
            }
        }

        for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) {
            CFGBlock *block = *it;
            int A = block->getBlockID();
            for (CFGBlock::succ_iterator succIt = block->succ_begin(); succIt != block->succ_end(); ++succIt) {
                if (CFGBlock *succ = *succIt) {
                    int B = succ->getBlockID();
                    if (dominators[A].count(B)) {
                        llvm::outs() << "  [LOOP DETECTED] Back-edge from Block " << A << " to " << B << " (Suggest Loop-Invariant Code Motion)\n";
                    }
                }
            }
        }

        map<int, set<int>> genSets; 
        llvm::outs() << "Phase 2 & 3 - Instruction-Level Analysis \n";

        for (CFG::iterator blockIt = functionCFG->begin(); blockIt != functionCFG->end(); ++blockIt) {
            CFGBlock *block = *blockIt;
            int blockID = block->getBlockID();
            if (visitedBlocks.find(blockID) == visitedBlocks.end()) continue; // Skip unreachable

            for (CFGBlock::iterator elemIt = block->begin(); elemIt != block->end(); ++elemIt) {
                if (optional<CFGStmt> cfgStmt = elemIt->getAs<CFGStmt>()) {
                    const Stmt *stmt = cfgStmt->getStmt();

                    if (const BinaryOperator *binOp = dyn_cast<BinaryOperator>(stmt)) {
                        if (!binOp->isAssignmentOp() && isa<IntegerLiteral>(binOp->getLHS()->IgnoreParenCasts()) && isa<IntegerLiteral>(binOp->getRHS()->IgnoreParenCasts())) {
                            llvm::outs() << "  [FOLDING] Constant Folding possible in Block " << blockID << "\n";
                        }
                        
                        if (binOp->isAssignmentOp()) {
                            string varName = getVarName(binOp->getLHS());
                            defCounter++;
                            allDefinitions.push_back({defCounter, varName, blockID});
                            genSets[blockID].insert(defCounter);
                            if (isa<IntegerLiteral>(binOp->getRHS()->IgnoreParenCasts())) {
                                llvm::outs() << "  [PROPAGATION] Constant Propagation for '" << varName << "'\n";
                            }
                        }
                    }
                    
                    if (const DeclStmt *declStmt = dyn_cast<DeclStmt>(stmt)) {
                        for (auto *decl : declStmt->decls()) {
                            if (VarDecl *varDecl = dyn_cast<VarDecl>(decl)) {
                                if (varDecl->hasInit()) {
                                    string varName = varDecl->getNameAsString();
                                    defCounter++;
                                    allDefinitions.push_back({defCounter, varName, blockID});
                                    genSets[blockID].insert(defCounter);
                                }
                            }
                        }
                    }

                    if (const CallExpr *call = dyn_cast<CallExpr>(stmt)) {
                        if (const FunctionDecl *func = call->getDirectCallee()) {
                            string funcName = func->getNameAsString();
                            if (funcName == "scanf" || funcName == "gets") llvm::outs() << "  [TAINT SOURCE] Untrusted input: " << funcName << "\n";
                            if (funcName == "printf" || funcName == "system") llvm::outs() << "  [TAINT SINK] Sensitive function: " << funcName << "\n";
                        }
                    }
                }
            }
        }

        map<int, set<int>> killSets, inSets, outSets;
        for (const auto& pair : genSets) {
            int blockID = pair.first;
            for (int defID : pair.second) {
                string varName = "";
                for (const auto& def : allDefinitions) if (def.id == defID) varName = def.varName;
                for (const auto& def : allDefinitions) if (def.varName == varName && def.id != defID) killSets[blockID].insert(def.id);
            }
        }

        for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) {
            outSets[(*it)->getBlockID()] = genSets[(*it)->getBlockID()];
        }

        changed = true;
        while (changed) {
            changed = false;
            for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) {
                CFGBlock *block = *it;
                int b = block->getBlockID();

                set<int> newIn;
                for (CFGBlock::pred_iterator predIt = block->pred_begin(); predIt != block->pred_end(); ++predIt) {
                    if (CFGBlock *pred = *predIt) newIn.insert(outSets[pred->getBlockID()].begin(), outSets[pred->getBlockID()].end());
                }
                inSets[b] = newIn;

                set<int> newOut = genSets[b];
                set<int> inMinusKill;
                set_difference(inSets[b].begin(), inSets[b].end(), killSets[b].begin(), killSets[b].end(), inserter(inMinusKill, inMinusKill.begin()));
                newOut.insert(inMinusKill.begin(), inMinusKill.end());

                if (newOut != outSets[b]) {
                    outSets[b] = newOut;
                    changed = true;
                }
            }
        }
        
        llvm::outs() << "Reaching Definitions Results\n";
        for (CFG::iterator it = functionCFG->begin(); it != functionCFG->end(); ++it) {
            int b = (*it)->getBlockID();
            llvm::outs() << "Block " << b << " | IN: {";
            for (int d : inSets[b]) llvm::outs() << "D" << d << " ";
            llvm::outs() << "} | OUT: {";
            for (int d : outSets[b]) llvm::outs() << "D" << d << " ";
            llvm::outs() << "}\n";
        }
        return true; 
    }
};

class MyASTConsumer : public ASTConsumer {
private:
    MyASTVisitor Visitor;
public:
    explicit MyASTConsumer(ASTContext *Context) : Visitor(Context) {}
    virtual void HandleTranslationUnit(ASTContext &Context) override { Visitor.TraverseDecl(Context.getTranslationUnitDecl()); }
};

class MyFrontendAction : public ASTFrontendAction {
public:
    virtual unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &Compiler, llvm::StringRef InFile) override {
        return make_unique<MyASTConsumer>(&Compiler.getASTContext());
    }
};

int main(int argc, const char **argv) {
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
    if (!ExpectedParser) { llvm::errs() << ExpectedParser.takeError(); return 1; }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    return Tool.run(newFrontendActionFactory<MyFrontendAction>().get());
}