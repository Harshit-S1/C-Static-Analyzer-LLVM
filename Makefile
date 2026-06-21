CXX = clang++
CXXFLAGS = -O3 -fno-rtti -std=c++17
LLVM_CXXFLAGS = $(shell llvm-config --cxxflags)
LLVM_LDFLAGS = $(shell llvm-config --ldflags --libs --system-libs)

CLANG_LIBS = -Wl,--start-group \
	-lclangTooling -lclangFrontendTool -lclangFrontend -lclangDriver \
	-lclangSerialization -lclangCodeGen -lclangParse -lclangSema \
	-lclangStaticAnalyzerFrontend -lclangStaticAnalyzerCheckers \
	-lclangStaticAnalyzerCore -lclangAnalysis -lclangARCMigrate \
	-lclangRewrite -lclangRewriteFrontend -lclangEdit -lclangASTMatchers \
	-lclangAST -lclangLex -lclangAPINotes -lclangSupport -lclangBasic \
	-Wl,--end-group

all: analyzer

# linking the object file to create the final executable
analyzer: CFGBuilder.o
	$(CXX) CFGBuilder.o -o analyzer $(LLVM_LDFLAGS) $(CLANG_LIBS)

# compiling the C++ file into an object file (the separation prevents recompilation)
CFGBuilder.o: CFGBuilder.cpp
	$(CXX) $(CXXFLAGS) $(LLVM_CXXFLAGS) -c CFGBuilder.cpp -o CFGBuilder.o

# cleanup now includes the .o file
clean:
	rm -f analyzer CFGBuilder.o temp.c optimized.c *_cfg.dot